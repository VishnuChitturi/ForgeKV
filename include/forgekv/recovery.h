#pragma once
// =============================================================================
// ForgeKV — Stage 5: Recovery
// =============================================================================
//
// Recovery replays a WAL into a Storage instance, reconstructing the
// key-value state that existed at the time of the last clean (or unclean)
// shutdown.
//
// Responsibilities:
//
//   WAL      — owns the binary file format; validates records; exposes replay()
//   Recovery — owns the application logic: SET → storage.set(),
//              DEL → storage.del(), CLEAR → storage.clear()
//   Storage  — the destination; Recovery depends only on the abstract
//              Storage interface, not on InMemoryStorage.
//
// Recovery does NOT write to the WAL.  It applies operations directly to
// Storage, bypassing KeyValueStore::set/del/clear entirely.  This prevents
// WAL record duplication.
//
// Usage:
//
//   forgekv::Recovery recovery(wal, storage);
//   forgekv::Recovery::Result r = recovery.run();
//   // r.records_applied — number of records replayed
//   // r.incomplete_tail  — true if a truncated final record was found
//
// Failure:
//
//   If recovery encounters corrupted data (bad magic, version, opcode,
//   checksum mismatch, or a truncated record that is NOT the final entry),
//   run() throws std::runtime_error.  The Storage may contain partial state
//   and should not be used.
//
// =============================================================================

#include "forgekv/storage.h"
#include "forgekv/wal.h"

#include <cstddef>
#include <stdexcept>

namespace forgekv {

class Recovery {
public:
    // -------------------------------------------------------------------------
    // Construction
    // -------------------------------------------------------------------------

    // Takes non-owning references to WAL and Storage.
    // Both must remain valid for the lifetime of this Recovery object.
    explicit Recovery(const WAL& wal, Storage& storage) noexcept
        : wal_(wal), storage_(storage) {}

    // Non-copyable, non-movable — holds references.
    Recovery(const Recovery&)            = delete;
    Recovery& operator=(const Recovery&) = delete;
    Recovery(Recovery&&)                 = delete;
    Recovery& operator=(Recovery&&)      = delete;

    ~Recovery() = default;

    // -------------------------------------------------------------------------
    // Result
    // -------------------------------------------------------------------------

    struct Result {
        std::size_t records_applied{0};   // number of complete records applied
        bool        incomplete_tail{false}; // true if a truncated final record
                                             // was skipped
    };

    // -------------------------------------------------------------------------
    // run() — replay the WAL into Storage
    // -------------------------------------------------------------------------
    //
    // Reads every complete, valid WAL record in file order and applies it
    // to storage_:
    //
    //   kOpSet   → storage_.set(key, value)
    //   kOpDel   → storage_.del(key)   (missing-key is tolerated)
    //   kOpClear → storage_.clear()
    //
    // A truncated FINAL record is treated as a non-fatal crash tail:
    // all prior records are applied and run() returns with
    // result.incomplete_tail == true.
    //
    // Any other corruption causes run() to throw std::runtime_error.
    //
    // Throws std::runtime_error on:
    //   - Corrupted complete record (bad magic/version/opcode/checksum)
    //   - Truncated record that is NOT the final entry (mid-log corruption)
    [[nodiscard]] Result run();

private:
    const WAL& wal_;      // WAL to read from (non-owning)
    Storage&   storage_;  // Storage to apply records into (non-owning)
};

} // namespace forgekv
