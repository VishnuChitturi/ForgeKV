// =============================================================================
// ForgeKV — Stage 5: Recovery implementation
// =============================================================================
//
// Recovery::run() delegates record reading to WAL::replay() (which handles
// all binary parsing and corruption detection), then applies each record to
// the Storage abstraction.
//
// Write ordering during normal operation:
//
//   KeyValueStore::set() → WAL::append_set() → Storage::set()
//
// Write ordering during recovery:
//
//   WAL::replay() → Recovery::run() → Storage::set/del/clear()
//
// Recovery bypasses KeyValueStore::set/del/clear entirely.  This is
// intentional: those methods write a WAL record BEFORE touching Storage.
// During recovery we must apply operations to Storage WITHOUT producing
// any new WAL records.
//
// After run() returns, the WAL's append-mode stream is still positioned at
// the end of the file.  Any new mutations written via KeyValueStore will be
// appended after the existing records — the log is never truncated or
// rewritten by recovery.
// =============================================================================

#include "forgekv/recovery.h"

namespace forgekv {

// -----------------------------------------------------------------------------
// run
// -----------------------------------------------------------------------------
Recovery::Result Recovery::run()
{
    WAL::ReplayResult wal_result = wal_.replay(
        [this](const WalRecord& rec) {
            switch (rec.opcode) {
                case kOpSet:
                    storage_.set(rec.key, rec.value);
                    break;
                case kOpDel:
                    // del() returns false if the key was already absent.
                    // During replay that is not an error — the WAL faithfully
                    // records operations as they were requested, and the Storage
                    // may have had the key removed by an earlier CLEAR or DEL.
                    storage_.del(rec.key);
                    break;
                case kOpClear:
                    storage_.clear();
                    break;
                default:
                    // read_record() already validates the opcode before calling
                    // the callback, so this branch should be unreachable.
                    throw std::runtime_error(
                        "Recovery: unexpected opcode in callback: "
                        + std::to_string(static_cast<int>(rec.opcode)));
            }
        });

    return Result{
        wal_result.records_replayed,
        wal_result.incomplete_tail
    };
}

} // namespace forgekv
