// =============================================================================
// ForgeKV — Stage 10: KeyValueStore implementation (TTL / Expiration)
// =============================================================================
//
// Stage 10 adds:
//
//   1. set_with_ttl(key, value, ttl_seconds)
//      - Computes absolute expiration: now + ttl_seconds (as microseconds).
//      - Writes kOpSetWithExpiry WAL record.
//      - Calls storage_->set_with_expiry().
//      - ttl_seconds <= 0: key is not stored (immediately expired).
//
//   2. ttl(key)
//      - Returns kTtlPermanent (-1.0) if key exists and is permanent.
//      - Returns kTtlNotFound  (-2.0) if key is missing or expired.
//      - Returns remaining seconds (>=0.0) if key is expiring.
//
//   3. Background cleanup thread.
//      - Wakes every cleanup_interval_ or on shutdown.
//      - Calls do_expire_pass() under the exclusive lock.
//      - do_expire_pass() removes expired keys from storage and writes WAL
//        DEL records so expirations are durable across restarts.
//      - Thread is started after recover() returns.
//      - Thread is stopped/joined in the destructor.
//
//   4. Updated recover():
//      - Handles kOpSetWithExpiry records from WAL.
//      - Skips already-expired records during recovery.
//      - Loads v2 snapshot records with expiry metadata.
//
//   5. Updated compact():
//      - Excludes expired keys from the compacted WAL.
//      - Writes SET_WITH_EXPIRY for live expiring keys.
//
//   6. Updated snapshot():
//      - Excludes expired keys.
//      - Preserves expiry metadata for live expiring keys.
//
// Locking model (unchanged from Stage 7):
//   READ ops:   shared_lock (get, exists, size, empty, ttl)
//   WRITE ops:  exclusive lock (set, set_with_ttl, del, clear, compact, snapshot)
//   CLEANUP:    exclusive lock (short pass, writes WAL DEL records)
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
// Time helper
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
    : storage_(std::make_unique<InMemoryStorage>()),
      wal_(std::make_unique<WAL>(kDefaultWalPath)),
      snapshot_manager_(kDefaultWalPath)
{
    recover();
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
    : storage_(std::move(storage)),
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
    recover();
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
    : storage_(std::move(storage)),
      wal_(std::make_unique<WAL>(kDefaultWalPath)),
      snapshot_manager_(kDefaultWalPath)
{
    if (!storage_) {
        throw std::invalid_argument("KeyValueStore: storage must not be null");
    }
    recover();
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
    : snapshot_manager_()
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
// recover — snapshot-aware startup recovery (Stage 10 extended)
// =============================================================================
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

void KeyValueStore::set(const std::string& key, const std::string& value)
{
    std::unique_lock lock(mutex_);
    wal_->append_set(key, value);
    storage_->set(key, value);
}

// =============================================================================
// set_with_ttl — expiring upsert
// =============================================================================

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
}

// =============================================================================
// get — return value for non-expired key
// =============================================================================

std::optional<std::string> KeyValueStore::get(const std::string& key) const
{
    std::shared_lock lock(mutex_);
    return storage_->get(key);
}

// =============================================================================
// del — delete key (including any TTL)
// =============================================================================

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
// Stage 10 changes:
//   - Uses get_all_with_expiry() to get live entries WITH expiry metadata.
//   - Writes SET_WITH_EXPIRY for expiring keys, SET for permanent keys.
//   - Expired keys are automatically excluded (get_all_with_expiry filters them).

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
// Stage 10 changes:
//   - Uses get_all_with_expiry() — excludes expired keys.
//   - Passes StoreEntry records to SnapshotManager::save() (v2 format).

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

        return true;

    } catch (const std::exception& e) {
        std::cerr << "[ForgeKV] ERROR: snapshot() failed: " << e.what() << "\n";
        return false;
    } catch (...) {
        std::cerr << "[ForgeKV] ERROR: snapshot() failed (unknown exception)\n";
        return false;
    }
}

} // namespace forgekv
