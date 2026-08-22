// =============================================================================
// ForgeKV — Stage 11: KeyValueStore implementation (Statistics / Observability)
// =============================================================================
//
// Stage 11 adds lightweight runtime statistics. See kv_store.h for the full
// design notes. Changes relative to Stage 10:
//
//   1. start_time_ captured at the top of every constructor (before recover()).
//
//   2. recovering_ flag set true during recover(), cleared after.
//      This suppresses counter increments for WAL replay / snapshot loading.
//
//   3. set() — increments stat_set_count_ (only when !recovering_).
//
//   4. set_with_ttl() — increments stat_ttl_set_count_ (only when ttl > 0
//      and !recovering_).
//
//   5. get() — increments stat_get_hits_ (value found) or stat_get_misses_
//      (nullopt). These are client reads; recovery does not call get().
//      get() is const and uses mutable atomics — relaxed increment is fine.
//
//   6. del() — increments stat_delete_count_ when the key existed and was
//      deleted. Does NOT count deletions of non-existent keys.
//      Does NOT count background expiration deletions (those go to
//      stat_expired_count_).
//
//   7. do_expire_pass() — increments stat_expired_count_ by the number of
//      keys actually removed by expiration.
//
//   8. snapshot() — on success, records current wall-clock time in
//      last_snapshot_time_us_.
//
//   9. stats() — assembles and returns a Stats struct. Acquires the shared
//      lock to read storage_.size() and wal_->file_size() atomically with
//      respect to writers, then reads atomic counters without the lock.
//
// All counter increments use memory_order_relaxed — the counters are
// independent and do not synchronise any other state. stats() reads them
// with memory_order_relaxed too; a slightly stale view is acceptable.
// =============================================================================

#include "forgekv/kv_store.h"
#include "forgekv/in_memory_storage.h"
#include "forgekv/recovery.h"
#include "forgekv/snapshot.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <shared_mutex>
#include <stdexcept>

namespace forgekv {

// Default WAL path.
static constexpr const char* kDefaultWalPath = "forgekv.wal";

// =============================================================================
// Time helpers
// =============================================================================

// Returns current wall-clock time as microseconds since Unix epoch.
static std::uint64_t current_time_us() noexcept {
    const auto now = std::chrono::system_clock::now();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            now.time_since_epoch())
            .count());
}

// =============================================================================
// Helper: start the cleanup thread
// =============================================================================

static void start_cleanup_thread(
    std::thread& th,
    std::atomic<bool>& stop_flag,
    std::mutex& cv_mutex,
    std::condition_variable& cv,
    std::chrono::milliseconds interval,
    std::function<void()> work)
{
    th = std::thread([&stop_flag, &cv_mutex, &cv, interval,
                      work = std::move(work)]() mutable {
        while (!stop_flag.load(std::memory_order_relaxed)) {
            // Wait for interval or until notified (shutdown).
            std::unique_lock<std::mutex> lk(cv_mutex);
            cv.wait_for(lk, interval,
                        [&stop_flag]() {
                            return stop_flag.load(std::memory_order_relaxed);
                        });
            lk.unlock();

            if (stop_flag.load(std::memory_order_relaxed)) {
                break;
            }

            // Run the cleanup pass.
            work();
        }
    });
}

// =============================================================================
// Default constructor
// =============================================================================

KeyValueStore::KeyValueStore()
    : start_time_(std::chrono::steady_clock::now()),  // Stage 11: uptime anchor
      storage_(std::make_unique<InMemoryStorage>()),
      wal_(std::make_unique<WAL>(kDefaultWalPath)),
      snapshot_manager_(kDefaultWalPath)
{
    recovering_ = true;
    recover();
    recovering_ = false;

    start_cleanup_thread(
        cleanup_thread_, stop_cleanup_,
        cleanup_cv_mutex_, cleanup_cv_,
        cleanup_interval_,
        [this]() {
            std::unique_lock lock(mutex_);
            (void)do_expire_pass();
        });
}

// =============================================================================
// Full DI constructor
// =============================================================================

KeyValueStore::KeyValueStore(std::unique_ptr<Storage> storage,
                             std::unique_ptr<WAL>     wal)
    : start_time_(std::chrono::steady_clock::now()),  // Stage 11: uptime anchor
      storage_(std::move(storage)),
      wal_(std::move(wal)),
      snapshot_manager_("")
{
    if (!storage_) {
        throw std::invalid_argument("KeyValueStore: storage must not be null");
    }
    if (!wal_) {
        throw std::invalid_argument("KeyValueStore: wal must not be null");
    }
    snapshot_manager_ = SnapshotManager(wal_->path());

    recovering_ = true;
    recover();
    recovering_ = false;

    start_cleanup_thread(
        cleanup_thread_, stop_cleanup_,
        cleanup_cv_mutex_, cleanup_cv_,
        cleanup_interval_,
        [this]() {
            std::unique_lock lock(mutex_);
            (void)do_expire_pass();
        });
}

// =============================================================================
// Storage-only constructor (backward compatibility)
// =============================================================================

KeyValueStore::KeyValueStore(std::unique_ptr<Storage> storage)
    : start_time_(std::chrono::steady_clock::now()),  // Stage 11: uptime anchor
      storage_(std::move(storage)),
      wal_(std::make_unique<WAL>(kDefaultWalPath)),
      snapshot_manager_(kDefaultWalPath)
{
    if (!storage_) {
        throw std::invalid_argument("KeyValueStore: storage must not be null");
    }

    recovering_ = true;
    recover();
    recovering_ = false;

    start_cleanup_thread(
        cleanup_thread_, stop_cleanup_,
        cleanup_cv_mutex_, cleanup_cv_,
        cleanup_interval_,
        [this]() {
            std::unique_lock lock(mutex_);
            (void)do_expire_pass();
        });
}

// =============================================================================
// Destructor — stop and join the cleanup thread
// =============================================================================

KeyValueStore::~KeyValueStore()
{
    // Signal the cleanup thread to stop.
    stop_cleanup_.store(true, std::memory_order_relaxed);

    // Wake it up so it doesn't wait the full interval.
    cleanup_cv_.notify_all();

    // Join — waits for the thread to finish.
    if (cleanup_thread_.joinable()) {
        cleanup_thread_.join();
    }
}

// =============================================================================
// Move constructor
// =============================================================================

KeyValueStore::KeyValueStore(KeyValueStore&& other) noexcept
    : start_time_(std::chrono::steady_clock::now()),  // new uptime for the moved-to object
      snapshot_manager_()
{
    // Stop the other's cleanup thread before moving its state.
    other.stop_cleanup_.store(true, std::memory_order_relaxed);
    other.cleanup_cv_.notify_all();
    if (other.cleanup_thread_.joinable()) {
        other.cleanup_thread_.join();
    }

    std::unique_lock lock(other.mutex_);
    storage_          = std::move(other.storage_);
    wal_              = std::move(other.wal_);
    snapshot_manager_ = std::move(other.snapshot_manager_);
    cleanup_interval_ = other.cleanup_interval_;

    // Transfer stats counters.
    stat_get_hits_.store(other.stat_get_hits_.load(std::memory_order_relaxed),
                         std::memory_order_relaxed);
    stat_get_misses_.store(other.stat_get_misses_.load(std::memory_order_relaxed),
                           std::memory_order_relaxed);
    stat_set_count_.store(other.stat_set_count_.load(std::memory_order_relaxed),
                          std::memory_order_relaxed);
    stat_delete_count_.store(other.stat_delete_count_.load(std::memory_order_relaxed),
                             std::memory_order_relaxed);
    stat_ttl_set_count_.store(other.stat_ttl_set_count_.load(std::memory_order_relaxed),
                              std::memory_order_relaxed);
    stat_expired_count_.store(other.stat_expired_count_.load(std::memory_order_relaxed),
                              std::memory_order_relaxed);
    last_snapshot_time_us_.store(
        other.last_snapshot_time_us_.load(std::memory_order_relaxed),
        std::memory_order_relaxed);

    // Start a new cleanup thread for the moved-to object.
    stop_cleanup_.store(false, std::memory_order_relaxed);
    start_cleanup_thread(
        cleanup_thread_, stop_cleanup_,
        cleanup_cv_mutex_, cleanup_cv_,
        cleanup_interval_,
        [this]() {
            std::unique_lock lk(mutex_);
            (void)do_expire_pass();
        });
}

// =============================================================================
// recover — snapshot-aware startup recovery (unchanged from Stage 10)
// =============================================================================
//
// NOTE (Stage 11): This method runs with recovering_ == true, set in the
// constructor before recover() is called.  The set/del/ttl_set operations
// inside the replay callbacks check recovering_ and do NOT increment client
// operation counters.
//
// Handles:
//   A. kOpSet               → permanent key
//   B. kOpSetWithExpiry     → expiring key; skip if already expired
//   C. kOpDel               → remove key
//   D. kOpClear             → clear all keys
//   E. Snapshot v1 records  → permanent keys
//   F. Snapshot v2 records  → expiry-aware; skip already-expired entries

void KeyValueStore::recover()
{
    const auto snap_result = snapshot_manager_.load();

    if (snap_result.exists && !snap_result.corrupt) {
        // Path A: valid snapshot — restore it, then replay WAL tail.
        const SnapshotData& snap = snap_result.data;

        // Restore snapshot state. load() already excluded expired entries.
        for (const auto& [key, entry] : snap.records) {
            if (entry.has_expiry()) {
                storage_->set_with_expiry(key, entry.value, entry.expires_at_us);
            } else {
                storage_->set(key, entry.value);
            }
        }

        // Replay WAL tail.
        (void)wal_->replay_from(
            snap.wal_offset,
            [this](const WalRecord& rec) {
                const std::uint64_t now = current_time_us();
                switch (rec.opcode) {
                    case kOpSet:
                        storage_->set(rec.key, rec.value);
                        break;
                    case kOpSetWithExpiry:
                        // Skip already-expired entries.
                        if (rec.expires_at_us > 0 && rec.expires_at_us <= now) {
                            break; // expired before recovery completed
                        }
                        storage_->set_with_expiry(rec.key, rec.value,
                                                   rec.expires_at_us);
                        break;
                    case kOpDel:
                        storage_->del(rec.key);
                        break;
                    case kOpClear:
                        storage_->clear();
                        break;
                    default:
                        throw std::runtime_error(
                            "Recovery: unexpected opcode in WAL tail: "
                            + std::to_string(static_cast<int>(rec.opcode)));
                }
            });

    } else {
        // Path B / C: no snapshot, or corrupt — full WAL replay.
        if (snap_result.exists && snap_result.corrupt) {
            std::cerr << "[ForgeKV] WARNING: snapshot is corrupt and will be "
                         "ignored. Falling back to full WAL recovery. ("
                      << snap_result.error_msg << ")\n";
        }

        // Full replay handles all opcodes including kOpSetWithExpiry.
        (void)wal_->replay(
            [this](const WalRecord& rec) {
                const std::uint64_t now = current_time_us();
                switch (rec.opcode) {
                    case kOpSet:
                        storage_->set(rec.key, rec.value);
                        break;
                    case kOpSetWithExpiry:
                        if (rec.expires_at_us > 0 && rec.expires_at_us <= now) {
                            break; // already expired
                        }
                        storage_->set_with_expiry(rec.key, rec.value,
                                                   rec.expires_at_us);
                        break;
                    case kOpDel:
                        storage_->del(rec.key);
                        break;
                    case kOpClear:
                        storage_->clear();
                        break;
                    default:
                        throw std::runtime_error(
                            "Recovery: unexpected opcode: "
                            + std::to_string(static_cast<int>(rec.opcode)));
                }
            });
    }
}

// =============================================================================
// do_expire_pass — internal: scan and remove expired keys, write WAL DELs
// =============================================================================
//
// MUST be called under the exclusive lock.
// Returns the number of keys expired.
// Stage 11: increments stat_expired_count_ by the number of keys removed.

std::size_t KeyValueStore::do_expire_pass()
{
    const std::uint64_t now = current_time_us();
    const auto expired_keys = storage_->expire_keys(now);

    for (const auto& key : expired_keys) {
        // Write WAL DEL so the expiration survives a restart.
        try {
            wal_->append_del(key);
        } catch (const std::exception& e) {
            std::cerr << "[ForgeKV] WARNING: cleanup WAL write failed for key '"
                      << key << "': " << e.what() << "\n";
        }
    }

    // Stage 11: count expired keys (NOT counted as explicit deletions).
    if (!expired_keys.empty()) {
        stat_expired_count_.fetch_add(
            static_cast<std::uint64_t>(expired_keys.size()),
            std::memory_order_relaxed);
    }

    return expired_keys.size();
}

// =============================================================================
// run_cleanup_now — trigger immediate cleanup (for tests / manual use)
// =============================================================================

void KeyValueStore::run_cleanup_now()
{
    std::unique_lock lock(mutex_);
    (void)do_expire_pass();
}

// =============================================================================
// set — permanent upsert
// =============================================================================
//
// Stage 11: increments stat_set_count_ unless recovering_.

void KeyValueStore::set(const std::string& key, const std::string& value)
{
    std::unique_lock lock(mutex_);
    wal_->append_set(key, value);
    storage_->set(key, value);

    // Count client-initiated sets only (not recovery replays).
    if (!recovering_) {
        stat_set_count_.fetch_add(1, std::memory_order_relaxed);
    }
}

// =============================================================================
// set_with_ttl — expiring upsert
// =============================================================================
//
// Stage 11: increments stat_ttl_set_count_ when the key is actually stored
// (ttl_seconds > 0) and not during recovery.

void KeyValueStore::set_with_ttl(const std::string& key,
                                  const std::string& value,
                                  double             ttl_seconds)
{
    // ttl <= 0: key is immediately expired — do not store it.
    if (ttl_seconds <= 0.0) {
        return;
    }

    // Compute absolute expiration timestamp.
    const std::uint64_t now = current_time_us();
    const auto ttl_us = static_cast<std::uint64_t>(ttl_seconds * 1'000'000.0);
    const std::uint64_t expires_at_us = now + ttl_us;

    std::unique_lock lock(mutex_);
    wal_->append_set_with_expiry(key, value, expires_at_us);
    storage_->set_with_expiry(key, value, expires_at_us);

    // Count client-initiated TTL sets only (not recovery replays).
    if (!recovering_) {
        stat_ttl_set_count_.fetch_add(1, std::memory_order_relaxed);
    }
}

// =============================================================================
// get — return value for non-expired key
// =============================================================================
//
// Stage 11: increments stat_get_hits_ on success, stat_get_misses_ on miss.
// get() is a read operation; recovery does not call get().

std::optional<std::string> KeyValueStore::get(const std::string& key) const
{
    std::shared_lock lock(mutex_);
    auto result = storage_->get(key);

    if (result.has_value()) {
        stat_get_hits_.fetch_add(1, std::memory_order_relaxed);
    } else {
        stat_get_misses_.fetch_add(1, std::memory_order_relaxed);
    }

    return result;
}

// =============================================================================
// del — delete key (including any TTL)
// =============================================================================
//
// Stage 11: increments stat_delete_count_ when the key existed and was
// deleted. Missing-key deletions are NOT counted.

bool KeyValueStore::del(const std::string& key)
{
    std::unique_lock lock(mutex_);
    // exists() returns false for expired keys — consistent with treating
    // expired keys as logically non-existent.
    if (!storage_->exists(key)) {
        return false;
    }
    wal_->append_del(key);
    storage_->del(key);

    // Count explicit client deletions only (not recovery or expiration).
    if (!recovering_) {
        stat_delete_count_.fetch_add(1, std::memory_order_relaxed);
    }

    return true;
}

// =============================================================================
// exists — returns false for expired keys
// =============================================================================

bool KeyValueStore::exists(const std::string& key) const
{
    std::shared_lock lock(mutex_);
    return storage_->exists(key);
}

// =============================================================================
// ttl — query remaining TTL
// =============================================================================

double KeyValueStore::ttl(const std::string& key) const
{
    std::shared_lock lock(mutex_);
    const auto entry = storage_->get_entry(key);
    if (!entry.has_value()) {
        return kTtlNotFound;
    }

    const StoreEntry& e = entry.value();

    // Permanent key (no TTL).
    if (!e.has_expiry()) {
        return kTtlPermanent;
    }

    // Expiring key.
    const std::uint64_t now = current_time_us();
    if (e.expires_at_us <= now) {
        // Already expired — behaves as not found.
        return kTtlNotFound;
    }

    const double remaining_us =
        static_cast<double>(e.expires_at_us - now);
    return remaining_us / 1'000'000.0; // convert to seconds
}

// =============================================================================
// size — count of live (non-expired) keys
// =============================================================================

std::size_t KeyValueStore::size() const
{
    std::shared_lock lock(mutex_);
    return storage_->size();
}

// =============================================================================
// empty — true if no live keys
// =============================================================================

bool KeyValueStore::empty() const
{
    std::shared_lock lock(mutex_);
    return storage_->empty();
}

// =============================================================================
// clear — remove all keys
// =============================================================================

void KeyValueStore::clear()
{
    std::unique_lock lock(mutex_);
    wal_->append_clear();
    storage_->clear();
}

// =============================================================================
// compact — rewrite WAL with only live state
// =============================================================================
//
// Stage 11: compact() does NOT reset any statistics counters.
// WAL size changes naturally after rewrite; stats() will reflect the new size.

void KeyValueStore::compact()
{
    std::unique_lock lock(mutex_);

    // 1. Get live state with expiry metadata. now_us ensures expired are excluded.
    const std::uint64_t now = current_time_us();
    auto entries = storage_->get_all_with_expiry(now);

    // 2. Sort by key for determinism.
    std::sort(entries.begin(), entries.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    // 3. Build SnapshotEntry list for rewrite().
    std::vector<WAL::SnapshotEntry> snap;
    snap.reserve(entries.size());
    for (const auto& [key, entry] : entries) {
        snap.push_back(WAL::SnapshotEntry{key, entry.value, entry.expires_at_us});
    }

    // 4. Delete existing snapshot (stale after WAL rewrite).
    if (snapshot_manager_.exists()) {
        if (!snapshot_manager_.remove()) {
            std::cerr << "[ForgeKV] WARNING: compact() could not remove "
                         "existing snapshot at "
                      << snapshot_manager_.snapshot_path() << "\n";
        }
    }

    // 5. Rewrite WAL atomically.
    wal_->rewrite(snap);
}

// =============================================================================
// snapshot — checkpoint full state to disk
// =============================================================================
//
// Stage 11: updates last_snapshot_time_us_ atomically on success.
// last_snapshot_time_us_ is NOT updated on failure.

bool KeyValueStore::snapshot()
{
    try {
        std::unique_lock lock(mutex_);

        // 1. Capture live state with expiry metadata.
        const std::uint64_t now = current_time_us();
        auto records = storage_->get_all_with_expiry(now);

        // 2. Capture WAL boundary.
        const std::uint64_t wal_offset = wal_->file_size();

        // 3. Write snapshot v2.
        snapshot_manager_.save(wal_offset, records);

        // Stage 11: record the wall-clock time of this successful snapshot.
        last_snapshot_time_us_.store(current_time_us(), std::memory_order_relaxed);

        return true;

    } catch (const std::exception& e) {
        std::cerr << "[ForgeKV] ERROR: snapshot() failed: " << e.what() << "\n";
        return false;
    } catch (...) {
        std::cerr << "[ForgeKV] ERROR: snapshot() failed (unknown exception)\n";
        return false;
    }
}

// =============================================================================
// stats — assemble and return a Stats snapshot
// =============================================================================
//
// Acquires the shared lock to read storage size and WAL size (both derived
// from live storage state). Atomic counters are read without the lock.
//
// The Stats struct is returned by value; the caller owns it entirely.

Stats KeyValueStore::stats() const
{
    Stats s;

    // Read storage-derived metrics under shared lock.
    {
        std::shared_lock lock(mutex_);
        s.key_count      = static_cast<std::uint64_t>(storage_->size());
        s.wal_size_bytes = wal_->file_size();
    }

    // Read atomic counters (relaxed — slightly stale values are acceptable).
    s.get_hits    = stat_get_hits_.load(std::memory_order_relaxed);
    s.get_misses  = stat_get_misses_.load(std::memory_order_relaxed);
    s.set_count   = stat_set_count_.load(std::memory_order_relaxed);
    s.delete_count= stat_delete_count_.load(std::memory_order_relaxed);
    s.ttl_set_count  = stat_ttl_set_count_.load(std::memory_order_relaxed);
    s.expired_count  = stat_expired_count_.load(std::memory_order_relaxed);
    s.last_snapshot_time_us =
        last_snapshot_time_us_.load(std::memory_order_relaxed);

    // Compute uptime from monotonic clock.
    const auto elapsed = std::chrono::steady_clock::now() - start_time_;
    s.uptime_seconds = std::chrono::duration<double>(elapsed).count();

    return s;
}

// =============================================================================
// list_keys  (Stage 16)
// =============================================================================
//
// Returns metadata for all live (non-expired) keys as a snapshot.
//
// Algorithm:
//   1. Acquire the shared lock.
//   2. Call storage_->get_all_with_expiry(now_us) to get all live entries
//      (already excludes expired keys based on now_us).
//   3. For each entry compute the remaining TTL in seconds:
//        - expires_at_us == 0  → permanent (-1.0)
//        - expires_at_us > 0   → (expires_at_us - now_us) / 1e6  (>= 0.0)
//   4. Release lock and return the vector.
//
// Caller (HttpServer) sorts lexicographically and applies pagination.

std::vector<KeyValueStore::KeyInfo>
KeyValueStore::list_keys(std::uint64_t now_us) const
{
    std::vector<KeyInfo> result;

    std::shared_lock lock(mutex_);

    auto raw = storage_->get_all_with_expiry(now_us);
    result.reserve(raw.size());

    for (auto& [k, entry] : raw) {
        double ttl_sec;
        if (entry.expires_at_us == 0) {
            // Permanent key — no TTL.
            ttl_sec = -1.0;
        } else if (entry.expires_at_us > now_us) {
            // Live expiring key — compute remaining seconds.
            const std::uint64_t remaining_us = entry.expires_at_us - now_us;
            ttl_sec = static_cast<double>(remaining_us) / 1'000'000.0;
        } else {
            // Already expired (get_all_with_expiry should not return these
            // when now_us > 0, but be defensive).
            ttl_sec = 0.0;
        }

        result.push_back(KeyInfo{
            std::move(k),
            std::move(entry.value),
            ttl_sec
        });
    }

    return result;
}

} // namespace forgekv
