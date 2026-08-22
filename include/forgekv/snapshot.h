#pragma once
// =============================================================================
// ForgeKV — Stage 9: Snapshot Manager
// =============================================================================
//
// A snapshot is a complete, binary checkpoint of the current key-value state.
// Together with a WAL boundary (byte offset), it lets recovery skip replaying
// the entire WAL: load snapshot → replay only WAL records written AFTER the
// snapshot was taken.
//
// =============================================================================
// SNAPSHOT BINARY FORMAT
// =============================================================================
//
// The snapshot file uses a purpose-built binary format — distinct from the WAL
// binary format. Do NOT conflate the two.
//
//  Byte offset  Size  Type       Field
//  -----------  ----  ---------  ------------------------------------------------
//          0     4   uint32_t   Magic number (0x464B534E — "FKSN" stored LE)
//          4     1   uint8_t    Format version (0x01)
//          5     8   uint64_t   WAL byte offset (little-endian)
//                               Records in the WAL at byte positions < this
//                               offset are already encoded in the snapshot;
//                               recovery replays only records at offset >= this.
//         13     4   uint32_t   Record count (number of key-value pairs, LE)
//         17     …   records    Each record:
//                                 key_len  (4 bytes, uint32_t, LE)
//                                 key      (key_len bytes)
//                                 val_len  (4 bytes, uint32_t, LE)
//                                 val      (val_len bytes)
//          ?     4   uint32_t   CRC32 checksum (LE)
//                               Covers ALL bytes from offset 0 through the last
//                               value byte (i.e., everything BEFORE this field).
//
// Total header size = 17 bytes (magic 4 + version 1 + wal_offset 8 + count 4).
// Checksum field is the last 4 bytes of the file.
//
// =============================================================================
// INTEGRITY PROTECTION
// =============================================================================
//
// The trailing CRC32 checksum covers the entire file minus the checksum field
// itself. The same CRC32 algorithm used by the WAL (ISO 3309, reflected
// polynomial 0xEDB88320) is reused here for consistency.
//
// Corruption detection covers:
//   - Truncated header (file too small for magic + version + wal_offset + count)
//   - Truncated key/value payload (file ends before expected record bytes)
//   - Missing or truncated checksum field
//   - Invalid magic number
//   - Unknown format version
//   - CRC32 mismatch (any bit flip anywhere in the file)
//
// =============================================================================
// SNAPSHOT PATH CONVENTION
// =============================================================================
//
// The snapshot file is always placed at:
//
//     <wal-path>.snapshot
//
// For example, if the WAL is at "forgekv.wal", the snapshot lives at
// "forgekv.wal.snapshot".  A single current snapshot is maintained.
// There are no timestamped or numbered snapshot files.
//
// =============================================================================
// ATOMIC SNAPSHOT WRITES
// =============================================================================
//
// A snapshot is never written directly to the final path.  Instead:
//
//   1. Write complete snapshot to a temporary file in the same directory.
//   2. Flush and close the temporary file.
//   3. std::filesystem::rename() atomically replaces the final path.
//   4. If anything fails before rename(), the old snapshot (if any) is intact.
//
// This ensures that a partially-written snapshot is never visible to recovery.
//
// =============================================================================
// CORRUPTION BEHAVIOR
// =============================================================================
//
// If the snapshot file exists but is corrupt or truncated:
//   - SnapshotManager::load() returns a SnapshotLoadResult with
//     corrupt = true.
//   - KeyValueStore::recover() falls back to full WAL replay (from offset 0).
//   - The corrupt snapshot is NOT silently deleted; the caller decides.
//
// This fallback is safe because the WAL always contains a complete history
// of all operations (compaction reduces but does not remove complete records).
// Falling back to WAL-only recovery produces the same logical state as loading
// a valid snapshot.
//
// =============================================================================
// SNAPSHOT / COMPACTION INTERACTION
// =============================================================================
//
// Compaction rewrites the WAL completely (WAL::rewrite()).  After compaction,
// the WAL starts at byte offset 0 with fresh SET records for all live keys.
// Any existing snapshot's wal_offset refers to a position in the OLD WAL
// that no longer exists — making the offset meaningless and recovery incorrect.
//
// CHOSEN BEHAVIOR:
//   KeyValueStore::compact() deletes the snapshot file (if it exists) before
//   calling WAL::rewrite().  This is explicit, documented, and prevents any
//   scenario where an invalidated snapshot boundary causes incorrect recovery.
//
//   After compaction, recovery always uses WAL-only replay (from offset 0),
//   which is correct because the compacted WAL already contains the full
//   current state.
//
// =============================================================================
// WAL BOUNDARY CONSISTENCY
// =============================================================================
//
// The snapshot() method in KeyValueStore acquires the EXCLUSIVE lock for its
// full duration.  Under this lock it:
//
//   1. Captures the current in-memory state (via storage_->get_all()).
//   2. Queries the current WAL file size to obtain the boundary offset.
//   3. Writes the snapshot.
//
// Because the exclusive lock blocks all concurrent SET/DEL/CLEAR operations,
// no WAL record can be appended between steps 1 and 2.  The captured state
// and the WAL offset represent the SAME logical point in time.
//
// =============================================================================
// WHAT THIS STAGE DOES NOT DO
// =============================================================================
//
// - No automatic periodic snapshots.
// - No background snapshot thread.
// - No HTTP endpoint for snapshot.
// - No fsync (same durability model as the WAL: buffered I/O + flush).
// - No multiple concurrent snapshot files.
//
// =============================================================================

#include "forgekv/storage.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace forgekv {

// =============================================================================
// Constants
// =============================================================================

// Magic number that identifies a ForgeKV snapshot file.
// Value: 0x464B534E ('F','K','S','N') stored little-endian.
// Bytes on disk: 0x4E, 0x53, 0x4B, 0x46 ('N','S','K','F').
inline constexpr std::uint32_t kSnapshotMagic   = 0x464B534Eu;

// Snapshot format versions.
// Version 0x01: Stage 9 format — no expiry metadata.
// Version 0x02: Stage 10 format — each record has a has_expiry flag and
//               optional expires_at_us field.
inline constexpr std::uint8_t  kSnapshotVersion   = 0x01u;  // legacy (Stage 9)
inline constexpr std::uint8_t  kSnapshotVersionV2 = 0x02u;  // Stage 10

// Fixed header size: magic(4) + version(1) + wal_offset(8) + count(4) = 17 bytes.
inline constexpr std::size_t   kSnapshotHeaderSize = 17u;

// Trailing checksum size.
inline constexpr std::size_t   kSnapshotChecksumSize = 4u;

// =============================================================================
// Stage 10: Snapshot format v2 per-record layout
// =============================================================================
//
// Each record in v2 extends the v1 layout:
//
//   v1 record: [key_len(4)] [key] [val_len(4)] [val]
//
//   v2 record: [key_len(4)] [key] [val_len(4)] [val]
//              [has_expiry(1)] [expires_at_us(8)] ← only if has_expiry == 1
//
// has_expiry:  0x00 = permanent, 0x01 = has expiration timestamp.
// expires_at_us: microseconds since Unix epoch (little-endian uint64_t).
//                Only present in the byte stream when has_expiry == 0x01.
//
// Backward compatibility:
//   - Writing: always write v2.
//   - Loading: v1 snapshots are read as permanent entries (has_expiry = 0).
//     v2 snapshots decode has_expiry + expires_at_us for each record.
// =============================================================================

// =============================================================================
// SnapshotData — in-memory representation of a loaded snapshot
// =============================================================================

struct SnapshotData {
    std::uint64_t wal_offset{0};   // WAL byte boundary: replay from here

    // Stage 10: records now carry full StoreEntry data (value + expiry).
    // For snapshots loaded from v1 (Stage 9) format, entries are permanent
    // (expires_at_us == 0).
    std::vector<std::pair<std::string, StoreEntry>> records;
};

// =============================================================================
// SnapshotManager class
// =============================================================================

class SnapshotManager {
public:
    // -------------------------------------------------------------------------
    // Construction
    // -------------------------------------------------------------------------

    // Construct a SnapshotManager associated with the WAL at wal_path.
    // The snapshot file will live at: wal_path + ".snapshot".
    //
    // Does not open or read any file on construction.  Use save() and load().
    explicit SnapshotManager(const std::string& wal_path);

    // Default constructor — creates an unusable placeholder SnapshotManager.
    // This exists only to satisfy the move constructor of KeyValueStore.
    // A default-constructed SnapshotManager must not be used for save/load.
    SnapshotManager() = default;

    ~SnapshotManager() = default;

    SnapshotManager(const SnapshotManager&)            = delete;
    SnapshotManager& operator=(const SnapshotManager&) = delete;
    SnapshotManager(SnapshotManager&&)                 = default;
    SnapshotManager& operator=(SnapshotManager&&)      = default;

    // -------------------------------------------------------------------------
    // save — write a snapshot atomically
    // -------------------------------------------------------------------------

    // Write the snapshot to disk atomically (write to temp, rename to final).
    //
    // Parameters:
    //   wal_offset — the WAL byte offset at the time of the snapshot.
    //                Recovery will replay WAL records starting from this offset.
    //   records    — all live key-StoreEntry pairs at the time of snapshot.
    //                Expired entries must be excluded by the caller.
    //                StoreEntry carries both the value and expiry metadata.
    //
    // Format written: v2 (kSnapshotVersionV2) with has_expiry flag per record.
    //
    // Throws std::runtime_error if:
    //   - The temporary file cannot be created.
    //   - Any write or flush fails.
    //   - The atomic rename fails.
    void save(std::uint64_t wal_offset,
              const std::vector<std::pair<std::string, StoreEntry>>& records);

    // -------------------------------------------------------------------------
    // SnapshotLoadResult — returned by load()
    // -------------------------------------------------------------------------

    struct SnapshotLoadResult {
        bool exists{false};    // false if no snapshot file found
        bool corrupt{false};   // true if file exists but is invalid / corrupt
        SnapshotData data;     // populated only if exists && !corrupt
        std::string error_msg; // diagnostic if corrupt == true
    };

    // -------------------------------------------------------------------------
    // load — read and validate the snapshot
    // -------------------------------------------------------------------------

    // Attempt to load the snapshot file.
    //
    // Returns:
    //   !exists                  — no snapshot file on disk
    //   exists && !corrupt       — snapshot loaded and valid; data is populated
    //   exists && corrupt        — file is present but invalid; data is empty;
    //                             error_msg contains a diagnostic
    //
    // Never throws (all errors are reported via the result struct).
    [[nodiscard]] SnapshotLoadResult load() const noexcept;

    // -------------------------------------------------------------------------
    // remove — delete the snapshot file if it exists
    // -------------------------------------------------------------------------

    // Remove the snapshot file.  Silently succeeds if no file exists.
    // Called by KeyValueStore::compact() to prevent a stale snapshot from
    // pointing into the now-replaced WAL.
    //
    // Returns true if the file was removed or did not exist.
    // Returns false on I/O error (failure is non-fatal; compact() may warn).
    bool remove() noexcept;

    // -------------------------------------------------------------------------
    // Accessors
    // -------------------------------------------------------------------------

    // Return the path to the snapshot file.
    [[nodiscard]] const std::string& snapshot_path() const noexcept;

    // Return true if a snapshot file exists on disk.
    [[nodiscard]] bool exists() const noexcept;

private:
    // -------------------------------------------------------------------------
    // Encoding / decoding helpers (little-endian, same style as WAL)
    // -------------------------------------------------------------------------

    static void encode_u8 (std::vector<std::uint8_t>& buf, std::uint8_t  v);
    static void encode_u32(std::vector<std::uint8_t>& buf, std::uint32_t v);
    static void encode_u64(std::vector<std::uint8_t>& buf, std::uint64_t v);

    static std::uint32_t decode_u32(const std::vector<std::uint8_t>& buf,
                                    std::size_t offset);
    static std::uint64_t decode_u64(const std::vector<std::uint8_t>& buf,
                                    std::size_t offset);

    // -------------------------------------------------------------------------
    // CRC32 — same algorithm as WAL (ISO 3309, 0xEDB88320 reflected)
    // -------------------------------------------------------------------------

    static std::uint32_t crc32(const std::uint8_t* data, std::size_t len);

    // -------------------------------------------------------------------------
    // State
    // -------------------------------------------------------------------------

    std::string wal_path_;      // WAL file path (stored for context)
    std::string snapshot_path_; // snapshot_path_ = wal_path_ + ".snapshot"
};

} // namespace forgekv
