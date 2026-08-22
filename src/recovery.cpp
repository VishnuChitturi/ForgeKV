// =============================================================================
// ForgeKV — Stage 10: Recovery implementation (updated for TTL opcodes)
// =============================================================================
//
// Recovery::run() is still used as a fallback from KeyValueStore::recover()
// when no snapshot exists. However, Stage 10 now handles recovery inline in
// kv_store.cpp::recover() with full expiry awareness.
//
// Recovery::run() is retained for:
//   1. Backward compatibility with tests that use it directly.
//   2. As a simple fallback that handles all 4 opcodes.
//
// For kOpSetWithExpiry: calls storage_.set_with_expiry() with the timestamp.
// Already-expired entries are skipped (storage_.set_with_expiry() is a no-op
// for them if the timestamp is in the past — the InMemoryStorage will simply
// treat them as expired on the next get() call; they will be removed on the
// next cleanup pass).
// =============================================================================

#include "forgekv/recovery.h"
#include <chrono>

namespace forgekv {

static std::uint64_t recovery_current_time_us() noexcept {
    const auto now = std::chrono::system_clock::now();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            now.time_since_epoch())
            .count());
}

Recovery::Result Recovery::run()
{
    WAL::ReplayResult wal_result = wal_.replay(
        [this](const WalRecord& rec) {
            const std::uint64_t now = recovery_current_time_us();
            switch (rec.opcode) {
                case kOpSet:
                    storage_.set(rec.key, rec.value);
                    break;
                case kOpSetWithExpiry:
                    // Skip already-expired entries during recovery.
                    if (rec.expires_at_us > 0 && rec.expires_at_us <= now) {
                        break;
                    }
                    storage_.set_with_expiry(rec.key, rec.value,
                                              rec.expires_at_us);
                    break;
                case kOpDel:
                    storage_.del(rec.key);
                    break;
                case kOpClear:
                    storage_.clear();
                    break;
                default:
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
