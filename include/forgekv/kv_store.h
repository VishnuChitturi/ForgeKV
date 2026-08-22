#pragma once
// =============================================================================
// ForgeKV — Stage 10: KeyValueStore (TTL / Expiration)
// =============================================================================
//
// Stage 10 adds:
//
//   1. set_with_ttl(key, value, ttl_seconds) — set a key with an optional
//      time-to-live in seconds.  The absolute expiration timestamp is stored
//      internally as microseconds since Unix epoch (wall clock).
//
//   2. ttl(key) — query the remaining TTL for a key.
//      Returns:
//        > 0  — seconds remaining (fractional)
//        0.0  — key is expired (or will expire imminently)
//       -1.0  — key exists but is permanent (no TTL set)
//       -2.0  — key does not exist (or has already expired and been removed)
//
//   3. Background cleanup thread — periodically removes expired keys from
//      in-memory storage and writes WAL DEL records so expiration is durable.
//      The thread wakes every cleanup_interval_ms milliseconds or immediately
//      on shutdown.  It is joined in the destructor.
//
//   4. Destructor cleanup — the destructor signals the cleanup thread to stop
//      and joins it before destroying WAL/storage.
//
// TTL semantics:
//
//   - set(key, value)               → permanent; removes any prior TTL.
//   - set_with_ttl(key, value, ttl) → sets expiry = now + ttl.
//   - ttl <= 0                      → key is immediately expired/not stored.
//   - Updating a key with set()     → clears the TTL; key becomes permanent.
//   - Updating with set_with_ttl()  → replaces the expiry with new value.
//
// Read-time expiration (safe with shared_mutex):
//
//   get() and exists() use Storage::get/exists which check the current time
//   and return "absent" for expired keys WITHOUT mutating storage.
//   Physical removal only happens under exclusive lock (background thread or
//   write operations that encounter an expired key).
//
// Background cleanup and WAL durability:
//
//   When the background thread removes expired keys, it:
//   1. Acquires the exclusive lock.
//   2. Calls storage_->expire_keys(now_us) to remove expired entries.
//   3. For each removed key, calls wal_->append_del(key) to write a WAL
//      DEL record.  This ensures that a restart will NOT resurrect the key.
//   4. Releases the lock.
//
// Concurrency model (unchanged from Stage 7):
//
//   READ operations (get, exists, size, empty, ttl):
//     → shared_lock (multiple concurrent readers)
//
//   WRITE operations (set, set_with_ttl, del, clear, compact, snapshot):
//     → exclusive lock (single writer, all readers blocked)
//
//   Background cleanup:
//     → exclusive lock (short critical section per cleanup cycle)
//
// Thread lifecycle:
//
//   The cleanup thread is started in the constructor (after recover()).
//   It runs until stop_cleanup_ is set.
//   The destructor: sets stop_cleanup_, notifies the condition variable,
//   joins the thread. The WAL and storage are destroyed AFTER the thread
//   has exited, so there is no use-after-free risk.
//
// =============================================================================

#include "forgekv/recovery.h"
#include "forgekv/snapshot.h"
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
    bool snapshot();

    // -------------------------------------------------------------------------
    // Stage 10: Cleanup control (primarily for testing)
    // -------------------------------------------------------------------------

    // Trigger an immediate cleanup cycle (synchronously, under exclusive lock).
    // Useful in tests that need to flush expired keys without waiting for the
    // background thread's next wakeup.
    void run_cleanup_now();

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
};

} // namespace forgekv

