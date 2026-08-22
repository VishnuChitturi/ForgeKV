#pragma once
// =============================================================================
// ForgeKV — Stage 11: KeyValueStore (Statistics / Observability)
// =============================================================================
//
// Stage 11 adds:
//
//   1. Stats struct (see include/forgekv/stats.h) — plain value type returned
//      by stats(). Fields documented in stats.h.
//
//   2. Atomic operation counters — lock-free, independent of the storage mutex:
//        get_hits_, get_misses_, set_count_, delete_count_,
//        ttl_set_count_, expired_count_
//
//   3. Uptime tracking — steady_clock::time_point recorded at construction,
//      before recover() runs. Uptime is the elapsed time from construction
//      to the stats() call.
//
//   4. last_snapshot_time_ — std::atomic<uint64_t> updated only on successful
//      snapshot(). Stores wall-clock microseconds since Unix epoch. Zero
//      until the first successful snapshot.
//
//   5. stats() method — acquires shared lock, reads storage size + WAL size,
//      loads atomic counters into a Stats struct, and returns by value.
//
//   6. Recovery guard — the recovering_ flag is set during recover() so that
//      WAL replay and snapshot loading do NOT increment operation counters.
//
// Counter semantics (summary — full details in stats.h):
//
//   set_count      → incremented in set() only
//   ttl_set_count  → incremented in set_with_ttl() only (when ttl > 0)
//   delete_count   → incremented in del() only (when key existed)
//   get_hits       → incremented in get() when value returned
//   get_misses     → incremented in get() when nullopt returned
//   expired_count  → incremented in do_expire_pass() for each key expired
//   key_count      → derived from storage_->size() at stats() call time
//
//   Recovery does NOT increment any of the above counters.
//
// Uptime:
//   Uses std::chrono::steady_clock (monotonic). Not persisted.
//   Recorded at the top of the constructor body, before recover().
//
// Last snapshot time:
//   Updated atomically only when snapshot() returns true (success).
//   Stores wall-clock microseconds since Unix epoch (system_clock).
//   Zero if no successful snapshot has occurred this process lifetime.
//
// WAL size:
//   Obtained via wal_->file_size() inside stats() under the shared lock.
//   No separate counter is maintained — the WAL is the authoritative source.
//
// Concurrency:
//   - Atomic counters are updated without the storage mutex — they are
//     independent monotonically increasing values.
//   - stats() holds the shared lock only long enough to read storage size
//     and WAL size; it does not block writers.
//   - last_snapshot_time_ is a standalone atomic<uint64_t>; reading it does
//     not require the storage lock.
//   - All counter reads in stats() use memory_order_relaxed because a slightly
//     stale view is explicitly acceptable for observability data.
//
// All other semantics (locking model, TTL, background cleanup, recovery) are
// unchanged from Stage 10.
// =============================================================================

#include "forgekv/recovery.h"
#include "forgekv/snapshot.h"
#include "forgekv/stats.h"
#include "forgekv/storage.h"
#include "forgekv/wal.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <thread>

namespace forgekv {

// =============================================================================
// TTL query result constants (returned by ttl())
// =============================================================================

// Returned by ttl() when the key exists and is permanent (no TTL set).
inline constexpr double kTtlPermanent = -1.0;

// Returned by ttl() when the key does not exist or has already expired.
inline constexpr double kTtlNotFound  = -2.0;

// =============================================================================
// KeyValueStore
// =============================================================================

class KeyValueStore {
public:
    // -------------------------------------------------------------------------
    // Construction
    // -------------------------------------------------------------------------

    // Default cleanup interval: 1 second.
    static constexpr std::chrono::milliseconds kDefaultCleanupInterval{1000};

    // Default constructor — creates InMemoryStorage and opens WAL at
    // "forgekv.wal" in the current working directory.  Performs WAL replay
    // into Storage before the store is ready for use.
    KeyValueStore();

    // Full dependency-injection constructor — accepts any Storage and WAL.
    explicit KeyValueStore(std::unique_ptr<Storage> storage,
                           std::unique_ptr<WAL>     wal);

    // Storage-only injection (convenience overload).
    explicit KeyValueStore(std::unique_ptr<Storage> storage);

    // Destructor: stops and joins the cleanup thread before destroying
    // WAL and storage.  MUST be called before the WAL file or storage
    // objects go out of scope.
    ~KeyValueStore();

    // Not copyable.
    KeyValueStore(const KeyValueStore&)            = delete;
    KeyValueStore& operator=(const KeyValueStore&) = delete;

    // Movable.
    KeyValueStore(KeyValueStore&& other) noexcept;
    KeyValueStore& operator=(KeyValueStore&&) = delete;

    // -------------------------------------------------------------------------
    // Core operations (Stage 2 API — unchanged)
    // -------------------------------------------------------------------------

    // SET: WAL append_set (permanent), then Storage::set.  Removes any TTL.
    void set(const std::string& key, const std::string& value);

    // GET: Storage::get only.  Returns nullopt for missing or expired keys.
    [[nodiscard]] std::optional<std::string> get(const std::string& key) const;

    // DEL: WAL append_del + Storage::del if key exists. Returns true if found.
    bool del(const std::string& key);

    // EXISTS: Storage::exists.  Returns false for expired keys.
    [[nodiscard]] bool exists(const std::string& key) const;

    // SIZE: Returns the count of live (non-expired) keys.
    [[nodiscard]] std::size_t size() const;

    // EMPTY: Returns true if no live keys exist.
    [[nodiscard]] bool empty() const;

    // CLEAR: WAL append_clear + Storage::clear.
    void clear();

    // -------------------------------------------------------------------------
    // Stage 10: TTL API
    // -------------------------------------------------------------------------

    // SET_WITH_TTL: Store key with a time-to-live in seconds.
    //
    // ttl_seconds: duration in seconds until the key expires.
    //              Must be > 0.  If ttl_seconds <= 0, the key is NOT stored
    //              (it is considered immediately expired) and the function
    //              returns without writing to WAL or storage.
    //
    // The absolute expiration timestamp is computed as:
    //   expires_at = system_clock::now() + duration(ttl_seconds)
    // and stored as microseconds since Unix epoch.
    //
    // If the key already exists (with or without TTL), the value and
    // expiration are both replaced atomically.
    //
    // Throws std::runtime_error if the WAL write fails.
    void set_with_ttl(const std::string& key,
                      const std::string& value,
                      double             ttl_seconds);

    // TTL: Query the remaining time-to-live for a key.
    //
    // Returns:
    //   kTtlPermanent (-1.0) — key exists and is permanent (no TTL).
    //   kTtlNotFound  (-2.0) — key does not exist or has already expired.
    //   >= 0.0               — seconds remaining until expiration (may be 0
    //                          if expiring imminently but not yet cleaned up).
    [[nodiscard]] double ttl(const std::string& key) const;

    // -------------------------------------------------------------------------
    // Stage 8: Log Compaction
    // -------------------------------------------------------------------------

    // COMPACT: Rewrite the WAL to contain only the current live state.
    // Expired keys are excluded.  Expiring-but-live keys are preserved with
    // their SET_WITH_EXPIRY records.
    // Stage 9 note: compact() DELETES the snapshot file before rewriting.
    void compact();

    // -------------------------------------------------------------------------
    // Stage 9: Snapshots
    // -------------------------------------------------------------------------

    // SNAPSHOT: Create a full-state checkpoint.
    // Expired keys are excluded from the snapshot.
    // Live expiring keys are stored with their expiry metadata.
    // Returns true on success, false on failure.
    // Stage 11: updates last_snapshot_time_ on success.
    bool snapshot();

    // -------------------------------------------------------------------------
    // Stage 10: Cleanup control (primarily for testing)
    // -------------------------------------------------------------------------

    // Trigger an immediate cleanup cycle (synchronously, under exclusive lock).
    // Useful in tests that need to flush expired keys without waiting for the
    // background thread's next wakeup.
    void run_cleanup_now();

    // -------------------------------------------------------------------------
    // Stage 11: Statistics
    // -------------------------------------------------------------------------

    // STATS: Return a snapshot of current operational statistics.
    //
    // Acquires the shared lock momentarily to read storage size and WAL size.
    // Atomic counters are read without the storage lock (relaxed order).
    // Returns a Stats struct by value — safe to use after the lock is released.
    [[nodiscard]] Stats stats() const;

private:
    // -------------------------------------------------------------------------
    // Recovery helper
    // -------------------------------------------------------------------------
    void recover();

    // -------------------------------------------------------------------------
    // Background cleanup
    // -------------------------------------------------------------------------

    // Entry point for the background cleanup thread.
    // Runs until stop_cleanup_ is true.
    void cleanup_worker();

    // -------------------------------------------------------------------------
    // Internal helpers
    // -------------------------------------------------------------------------

    // Perform a single cleanup pass: expire keys and write WAL DEL records.
    // MUST be called under the exclusive lock (mutex_).
    // Returns the number of keys expired.
    std::size_t do_expire_pass();

    // ---- Stage 11: Statistics ----

    // Monotonic start time (steady_clock::time_point) recorded at construction
    // BEFORE recover() runs. Used for uptime_seconds in stats().
    // Declared first so it is initialized first in every constructor initializer list.
    std::chrono::steady_clock::time_point start_time_{
        std::chrono::steady_clock::now()};

    // -------------------------------------------------------------------------
    // State
    // -------------------------------------------------------------------------

    // Reader/writer lock protecting storage_ and wal_.
    mutable std::shared_mutex mutex_;

    // Backing storage.
    std::unique_ptr<Storage> storage_;

    // Write-ahead log.
    std::unique_ptr<WAL> wal_;

    // Snapshot manager.
    SnapshotManager snapshot_manager_;

    // ---- Background cleanup thread ----

    // Set to true before joining the thread.
    std::atomic<bool> stop_cleanup_{false};

    // Condition variable to wake the cleanup thread early (on shutdown or
    // explicit trigger).  Uses a separate plain mutex (cleanup_cv_mutex_)
    // because std::condition_variable requires std::unique_lock<std::mutex>.
    mutable std::mutex              cleanup_cv_mutex_;
    std::condition_variable         cleanup_cv_;

    // Cleanup interval.
    std::chrono::milliseconds cleanup_interval_{kDefaultCleanupInterval};

    // The cleanup thread.  Joined in the destructor.
    std::thread cleanup_thread_;

    // Atomic operation counters. Updated without the storage mutex.
    // All use relaxed memory order; stats() tolerates a slightly stale view.
    mutable std::atomic<std::uint64_t> stat_get_hits_{0};
    mutable std::atomic<std::uint64_t> stat_get_misses_{0};
    std::atomic<std::uint64_t>         stat_set_count_{0};
    std::atomic<std::uint64_t>         stat_delete_count_{0};
    std::atomic<std::uint64_t>         stat_ttl_set_count_{0};
    std::atomic<std::uint64_t>         stat_expired_count_{0};

    // Wall-clock timestamp of the last successful snapshot, as microseconds
    // since Unix epoch. Zero means no snapshot has ever succeeded.
    std::atomic<std::uint64_t> last_snapshot_time_us_{0};

    // Recovery guard: true while recover() is running.
    // When true, set/del/ttl_set counters are suppressed.
    // Not atomic — only written in the constructor before any threads start.
    bool recovering_{false};
};

} // namespace forgekv
