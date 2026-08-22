#pragma once
// =============================================================================
// ForgeKV — Stage 11: Statistics / Observability
// =============================================================================
//
// Stats is a plain-value snapshot of the KeyValueStore's current operational
// metrics. It is returned by KeyValueStore::stats() and serialized to JSON by
// HttpServer for the GET /stats endpoint.
//
// All fields carry ordinary value types — no atomics, no locks. The struct is
// safe to copy and pass freely between threads after it has been populated.
//
// Field semantics
// ---------------
//
//  key_count
//      Number of currently live, non-expired keys in storage at the instant
//      stats() is called. Derived directly from Storage::size() under the
//      KeyValueStore shared lock — no separate counter is maintained.
//
//  get_hits
//      Number of get() calls that returned a non-empty value (live key found).
//      Incremented atomically inside KeyValueStore::get().
//
//  get_misses
//      Number of get() calls that returned nullopt (key absent or expired).
//      Incremented atomically inside KeyValueStore::get().
//
//  set_count
//      Number of successful KeyValueStore::set() calls (permanent upsert).
//      Does NOT include set_with_ttl() or recovery replays.
//
//  delete_count
//      Number of explicit KeyValueStore::del() calls where the key existed
//      and was successfully deleted. Does NOT include background expiration
//      (which is counted separately in expired_count).
//
//  ttl_set_count
//      Number of successful KeyValueStore::set_with_ttl() calls where
//      ttl_seconds > 0 (the key was actually stored). Calls where
//      ttl_seconds <= 0 (key not stored) are not counted.
//
//  expired_count
//      Number of keys that were removed by background expiration
//      (do_expire_pass()). This counts physical removals, not logical
//      expiries that are merely invisible through get/exists.
//
//  wal_size_bytes
//      Current WAL file size in bytes, obtained via WAL::file_size().
//      Reflects the file size at the moment stats() acquires the shared lock.
//
//  uptime_seconds
//      Elapsed seconds since the KeyValueStore instance was constructed,
//      measured using std::chrono::steady_clock (monotonic). Not persisted.
//      Never negative.
//
//  last_snapshot_time_us
//      Wall-clock time of the most recent SUCCESSFUL snapshot, as
//      microseconds since Unix epoch (UTC). Updated only on success.
//      Zero if no snapshot has ever succeeded in this process lifetime.
//      Not persisted.
//
// Recovery semantics
// ------------------
//
//  WAL replay and snapshot loading during construction are NOT counted as
//  client operations. set_count, delete_count, ttl_set_count are all zero
//  immediately after construction (before any client-initiated calls).
//  key_count reflects the number of live keys loaded from WAL/snapshot.
//
// Thread safety
// -------------
//
//  The Stats struct itself is not thread-safe — it is a value type.
//  KeyValueStore::stats() assembles a Stats snapshot under the shared lock
//  and returns it by value. The caller receives an ordinary struct copy.
//
// =============================================================================

#include <cstdint>

namespace forgekv {

struct Stats {
    // ---- Live state ----
    std::uint64_t key_count{0};             // live key count (not expired)

    // ---- Operation counters ----
    std::uint64_t get_hits{0};              // get() returned a value
    std::uint64_t get_misses{0};            // get() returned nullopt
    std::uint64_t set_count{0};             // set() calls
    std::uint64_t delete_count{0};          // del() calls (key existed)
    std::uint64_t ttl_set_count{0};         // set_with_ttl() calls (ttl > 0)
    std::uint64_t expired_count{0};         // keys removed by expiration

    // ---- Infrastructure metrics ----
    std::uint64_t wal_size_bytes{0};        // WAL file size
    double        uptime_seconds{0.0};      // seconds since KV construction
    std::uint64_t last_snapshot_time_us{0}; // wall-clock us since epoch; 0=never
};

} // namespace forgekv
