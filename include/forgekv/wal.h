#pragma once
// =============================================================================
// ForgeKV — Stage 4: Write-Ahead Log (WAL) — Binary Format with Checksums
// =============================================================================
//
// WAL is an append-only binary log that records every mutating operation
// before it is applied to in-memory storage.
//
// "Write-ahead" means: the log entry is written FIRST. The in-memory mutation
// happens SECOND, only after the WAL write succeeds. If the WAL write fails,
// the in-memory state is not changed.
//
// Stage 4 replaces the Stage 3 text WAL with a structured binary format. Each
// record is a self-describing binary blob with explicit field lengths and a
// CRC32 checksum. This enables robust corruption detection and prepares the
// format for Stage 5 crash recovery.
//
// =============================================================================
// BINARY RECORD LAYOUT
// =============================================================================
//
// Every WAL record has this exact layout on disk:
//
//  Offset  Size  Type       Field
//  ------  ----  ---------  -----------------------------------------------
//       0     4  uint32_t   Magic number  (0x464B5741 — "FKWA" in ASCII LE)
//       4     1  uint8_t    Format version (0x01)
//       5     1  uint8_t    Operation code (SET=0x01, DEL=0x02, CLEAR=0x03)
//       6     4  uint32_t   Key length in bytes (little-endian)
//      10     4  uint32_t   Value length in bytes (little-endian)
//      14     ?  bytes      Key bytes   (key_len bytes, no null terminator)
//   14+K      ?  bytes      Value bytes (val_len bytes, no null terminator)
//   14+K+V    4  uint32_t   CRC32 checksum (little-endian)
//
//  K = key_len, V = val_len
//  Total record size = 14 + K + V + 4 = 18 + K + V bytes
//
// Operation-specific payload:
//
//  SET:   key_len > 0 (or 0 for empty key), val_len >= 0. Payload = key + value.
//  DEL:   key_len > 0 (or 0 for empty key), val_len = 0.  Payload = key only.
//  CLEAR: key_len = 0, val_len = 0.                        Payload is empty.
//
// =============================================================================
// CHECKSUM
// =============================================================================
//
// Algorithm: CRC32 (ISO 3309 / ITU-T V.42 polynomial: 0xEDB88320 reflected)
// Implemented from scratch — no external dependencies.
//
// Bytes covered: ALL bytes from the start of the record (magic field, offset 0)
// through the end of the payload (last byte before the checksum field).
// Equivalently: bytes [0 .. 14 + key_len + val_len - 1] (14 + K + V bytes).
//
// The checksum field itself is NOT included in the checksum calculation.
//
// =============================================================================
// BYTE ORDER
// =============================================================================
//
// All multi-byte integer fields (magic, key_len, val_len, checksum) are stored
// in LITTLE-ENDIAN byte order on disk. This is explicit regardless of the host
// machine's native endianness — the encode/decode helpers enforce it.
//
// =============================================================================
// MAGIC AND VERSION
// =============================================================================
//
// Magic:   0x464B5741  (bytes on disk: 0x41, 0x57, 0x4B, 0x46 — "AWKF")
//   Interpretation: "ForgeKV WAL" signature, stored little-endian.
//   On disk the four bytes are: 'W', 'A', 'L', reading "FKWA" as uint32 LE.
//   Specifically, bytes[0..3] = { 0x41, 0x57, 0x4B, 0x46 }.
//
// Version: 0x01
//   Any record with an unknown version is rejected at validation time.
//
// =============================================================================
// VALIDATION
// =============================================================================
//
// WAL::read_record() reads one complete record from an open input stream.
// Returns a WalRecord struct on success.
// Throws std::runtime_error for any of the following conditions:
//
//   - End of file before a complete header can be read      (truncated)
//   - End of file before all payload bytes are read          (truncated)
//   - End of file before the checksum field is read          (truncated)
//   - Magic number does not match kWalMagic                  (invalid magic)
//   - Version byte does not match kWalVersion                (invalid version)
//   - Operation code is not 0x01, 0x02, or 0x03             (invalid opcode)
//   - Computed CRC32 does not match stored checksum          (corruption)
//
// WAL::read_record() does NOT replay the record into Storage. It only
// deserializes and validates. Stage 5 will add the replay logic.
//
// =============================================================================
// FILE HANDLING
// =============================================================================
//
// The WAL opens the log file in append mode (std::ios::binary | std::ios::app).
// Existing content is preserved — the WAL never truncates on open.
//
// Each record is flushed (stream.flush()) immediately after being written.
// This drains the standard library buffer. Full OS-level fsync is not
// performed in Stage 4; that durability guarantee is a later concern.
//
// =============================================================================
// ERROR HANDLING
// =============================================================================
//
// - Construction throws std::runtime_error if the file cannot be opened.
// - append_set / append_del / append_clear throw std::runtime_error if
//   the write or flush fails.
// - read_record throws std::runtime_error for any invalid or truncated record.
//
// =============================================================================
// WHAT THIS STAGE DOES NOT DO
// =============================================================================
//
// - No crash recovery / WAL replay — that is Stage 5.
// - No compaction — that is Stage 8.
// - No concurrency protection — that is Stage 7.
// - No TTL, HTTP, snapshots, statistics — later stages.
//
// Thread safety: NOT thread-safe at this stage.
// =============================================================================

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace forgekv {

// =============================================================================
// Constants
// =============================================================================

// Magic number that identifies a ForgeKV WAL file.
// Value: 0x464B5741 stored little-endian.
// Bytes on disk: 0x41, 0x57, 0x4B, 0x46 ('A','W','K','F').
inline constexpr std::uint32_t kWalMagic   = 0x464B5741u;

// Current WAL format version. Increment if the binary layout changes
// in an incompatible way.
inline constexpr std::uint8_t  kWalVersion = 0x01u;

// Operation codes for WAL records.
inline constexpr std::uint8_t  kOpSet              = 0x01u;  // SET key value (permanent)
inline constexpr std::uint8_t  kOpDel              = 0x02u;  // DEL key
inline constexpr std::uint8_t  kOpClear            = 0x03u;  // CLEAR (no key/value)
inline constexpr std::uint8_t  kOpSetWithExpiry    = 0x04u;  // SET key value expires_at

// =============================================================================
// Stage 10: kOpSetWithExpiry record layout
// =============================================================================
//
// The SET_WITH_EXPIRY record extends the basic SET layout by appending an
// 8-byte absolute expiration timestamp after the value payload but BEFORE the
// CRC32 checksum.
//
//  Offset  Size  Type       Field
//  ------  ----  ---------  -----------------------------------------------
//       0     4  uint32_t   Magic number  (0x464B5741)
//       4     1  uint8_t    Format version (0x01)
//       5     1  uint8_t    Operation code (SET_WITH_EXPIRY = 0x04)
//       6     4  uint32_t   Key length in bytes (little-endian)
//      10     4  uint32_t   Value length in bytes (little-endian)
//      14     ?  bytes      Key bytes   (key_len bytes)
//   14+K      ?  bytes      Value bytes (val_len bytes)
//   14+K+V    8  uint64_t   Absolute expiration time in microseconds since
//                           Unix epoch (little-endian, UTC wall clock)
//   14+K+V+8  4  uint32_t   CRC32 checksum (covers ALL preceding bytes)
//
// The expires_at field stores microseconds since Unix epoch (1970-01-01T00:00:00Z).
// Using microseconds gives sub-millisecond precision while fitting in uint64_t.
// Wall-clock time (system_clock) is used — NOT monotonic clock — because the
// value must be meaningful across process restarts.
//
// Old WAL files (pre-Stage 10) contain only SET, DEL, CLEAR records (opcodes
// 0x01-0x03). Recovery handles them as permanent (non-expiring) keys.
// The kOpSetWithExpiry opcode (0x04) is new and was never written before
// Stage 10, ensuring full backward compatibility.
// =============================================================================

// Fixed header size in bytes: magic(4) + version(1) + opcode(1) + key_len(4)
//                                      + val_len(4) = 14 bytes.
inline constexpr std::size_t   kWalHeaderSize     = 14u;

// Size of the trailing CRC32 checksum field.
inline constexpr std::size_t   kWalChecksumSize   = 4u;

// Size of the expires_at field appended for kOpSetWithExpiry records.
inline constexpr std::size_t   kWalExpirySize     = 8u;

// =============================================================================
// WalRecord — decoded, validated record returned by read_record()
// =============================================================================

struct WalRecord {
    std::uint8_t  opcode;   // kOpSet, kOpDel, kOpClear, or kOpSetWithExpiry
    std::string   key;      // Key bytes (empty for CLEAR)
    std::string   value;    // Value bytes (empty for DEL and CLEAR)

    // Stage 10: expiration timestamp (only valid when opcode == kOpSetWithExpiry).
    // Stored as microseconds since Unix epoch (UTC wall-clock time).
    // Zero when opcode is not kOpSetWithExpiry.
    std::uint64_t expires_at_us{0};
};

// =============================================================================
// WAL class
// =============================================================================

class WAL {
public:
    // -------------------------------------------------------------------------
    // Construction
    // -------------------------------------------------------------------------

    // Open (or create) the WAL file at the given path in binary append mode.
    // Existing content is preserved — the WAL never truncates on open.
    //
    // Throws std::runtime_error if the file cannot be opened.
    explicit WAL(const std::string& path);

    // Destructor closes the file stream (RAII).
    ~WAL() = default;

    // Not copyable — a WAL owns an open file handle.
    WAL(const WAL&)            = delete;
    WAL& operator=(const WAL&) = delete;

    // Movable — transfers file ownership.
    WAL(WAL&&)            = default;
    WAL& operator=(WAL&&) = default;

    // -------------------------------------------------------------------------
    // Append operations
    // -------------------------------------------------------------------------

    // Append a binary SET record for (key, value).
    // Record layout: header [magic|version|SET|key_len|val_len] + key bytes
    //                + value bytes + CRC32.
    // Throws std::runtime_error if the write or flush fails.
    void append_set(const std::string& key, const std::string& value);

    // Append a binary DEL record for key.
    // Record layout: header [magic|version|DEL|key_len|0] + key bytes + CRC32.
    // Throws std::runtime_error if the write or flush fails.
    void append_del(const std::string& key);

    // Append a binary CLEAR record (no key or value payload).
    // Record layout: header [magic|version|CLEAR|0|0] + CRC32.
    // Throws std::runtime_error if the write or flush fails.
    void append_clear();

    // Stage 10: Append a SET_WITH_EXPIRY record.
    // expires_at_us is microseconds since Unix epoch (UTC wall-clock).
    // Record layout: header + key + value + expires_at_us(8 bytes) + CRC32.
    // Throws std::runtime_error if the write or flush fails.
    void append_set_with_expiry(const std::string& key,
                                const std::string& value,
                                std::uint64_t      expires_at_us);

    // -------------------------------------------------------------------------
    // Validation / read
    // -------------------------------------------------------------------------

    // Read and validate one WAL record from an open binary input stream.
    //
    // Returns a WalRecord on success (magic, version, opcode, checksum all
    // valid; no truncation detected).
    //
    // Throws std::runtime_error for:
    //   - Truncated header (EOF mid-read)
    //   - Truncated payload (EOF mid-read)
    //   - Truncated checksum (EOF mid-read)
    //   - Invalid magic number
    //   - Unknown version
    //   - Unknown opcode
    //   - CRC32 mismatch (corruption detected)
    //
    // The stream position advances by exactly one record on success.
    // On failure the stream state is undefined.
    //
    // Stage 5 calls this function to replay records into Storage.
    // Stage 4 only validates; it does NOT modify any in-memory state.
    static WalRecord read_record(std::istream& in);

    // =========================================================================
    // Stage 5: WAL replay
    // =========================================================================
    //
    // replay() reads the WAL file from the beginning, invoking callback once
    // for each complete, valid record in strict file order.
    //
    // Termination rules:
    //
    //   CLEAN EOF — no bytes of a new record are pending:
    //       replay returns normally; incomplete_tail = false.
    //
    //   TRUNCATED FINAL RECORD — EOF occurs while reading the very last
    //       (incomplete) record, and no subsequent complete records exist:
    //       replay returns normally after applying all prior valid records;
    //       incomplete_tail = true.  The incomplete record is NOT applied.
    //
    //   MID-LOG CORRUPTION or truncation before the final record:
    //       std::runtime_error is thrown.  Storage is in a partially-
    //       replayed state and must not be used.
    //
    // The callback receives a const WalRecord& and applies the operation to
    // Storage (or any other target).  The callback must not throw.
    //
    // This function does NOT write to the WAL.  It opens the WAL file for
    // reading independently of the WAL's own append-mode stream.

    struct ReplayResult {
        std::size_t records_replayed{0};   // number of complete records applied
        bool        incomplete_tail{false}; // true if a truncated final record
                                            // was skipped at EOF
    };

    // Replay all complete, valid records from the WAL file.
    //
    // Throws std::runtime_error if:
    //   - The WAL file cannot be opened for reading.
    //   - Any complete record contains an invalid magic, version, opcode, or
    //     a checksum mismatch.
    //   - A truncated record is encountered that is NOT at the very end of the
    //     file (mid-log truncation is treated as fatal corruption).
    [[nodiscard]] ReplayResult
    replay(std::function<void(const WalRecord&)> callback) const;

    // =========================================================================
    // Stage 9: Partial WAL replay starting at a byte offset
    // =========================================================================
    //
    // replay_from() is identical to replay() except that it skips to the given
    // byte offset before reading any records.  This is used by snapshot-based
    // recovery to replay only the WAL tail that was written AFTER a snapshot.
    //
    // Offset rules:
    //
    //   offset == 0          — equivalent to replay() (replay from beginning)
    //
    //   0 < offset < EOF     — seek to offset; read from there.
    //                          The byte at `offset` must be the first byte of a
    //                          valid WAL record header (magic = 0x464B5741).
    //                          If it is not, std::runtime_error is thrown.
    //
    //   offset == EOF        — no records to replay; returns result with
    //                          records_replayed == 0, incomplete_tail == false.
    //
    //   offset >  EOF        — std::runtime_error: offset beyond end of file.
    //
    //   offset inside a record — std::runtime_error: the read will fail with
    //                            an invalid magic error from read_record().
    //
    // The existing replay() behaviour for truncated final records and mid-log
    // corruption applies identically to the tail starting at `offset`.
    //
    // Throws std::runtime_error if:
    //   - The WAL file cannot be opened for reading.
    //   - offset is beyond EOF.
    //   - Any complete record (from offset onward) has invalid magic / version /
    //     opcode / checksum, or a mid-log truncated record.
    [[nodiscard]] ReplayResult
    replay_from(std::uint64_t                              offset,
                std::function<void(const WalRecord&)>      callback) const;

    // =========================================================================
    // Stage 9: WAL file size query
    // =========================================================================
    //
    // Returns the current size of the WAL file in bytes, or 0 if the file
    // does not exist.
    //
    // Used by KeyValueStore::snapshot() to capture the WAL boundary at the
    // exact moment of snapshot creation.
    //
    // Called under the exclusive lock, so no concurrent appends can change
    // the size between the state capture and this query.
    [[nodiscard]] std::uint64_t file_size() const noexcept;

    // =========================================================================
    // Stage 8: WAL compaction — atomic rewrite
    // =========================================================================
    //
    // rewrite() atomically replaces the WAL file with a minimal compacted WAL
    // containing exactly one SET record per live key/value pair.
    //
    // The snapshot parameter must contain the complete authoritative current
    // state (provided by KeyValueStore::compact() under an exclusive lock).
    // Each pair in the snapshot becomes one SET record in the new WAL.
    // Deleted keys must NOT appear in the snapshot.
    //
    // Atomic replacement strategy:
    //
    //   1. Derive a temporary filename in the same directory as the WAL file
    //      (same filesystem partition → rename is atomic at OS level).
    //   2. Write all SET records to the temp file using write_record().
    //   3. Flush and close the temp file.
    //   4. Use std::filesystem::rename() to replace the original WAL file.
    //      On POSIX systems rename() is atomic: readers either see the old
    //      file or the new file, never a half-written state.
    //   5. Reopen stream_ in binary append mode pointing to the new WAL file.
    //      This is critical: the next append_set/append_del/append_clear must
    //      write to the compacted file, not the old (now-replaced) inode.
    //
    // Failure handling:
    //
    //   If any step before the rename fails (temp file write error), the
    //   original WAL is untouched.  The temp file is removed on best effort.
    //   stream_ continues pointing at the original WAL.
    //
    //   If rename() fails, same: original WAL is untouched, temp file cleaned
    //   up, stream_ unchanged.
    //
    //   If rename() succeeds but stream_ reopen fails (extremely unlikely):
    //   std::runtime_error is thrown with a diagnostic; the compacted WAL is
    //   on disk but the append stream is invalid.  The store must be
    //   considered unusable at that point.
    //
    // Durability note:
    //   This function does NOT fsync the WAL file or the directory entry.
    //   The same durability level applies here as for normal WAL appends:
    //   standard library buffered I/O is flushed via stream.flush().
    //   A power loss between rename() and the next OS-level sync could, in
    //   theory, leave the old or new WAL depending on filesystem journaling.
    //   This matches the existing WAL durability model.
    //
    // Throws std::runtime_error if:
    //   - The temporary file cannot be created.
    //   - Any write or flush to the temporary file fails.
    //   - std::filesystem::rename() fails (original WAL preserved).
    //   - Reopening stream_ on the new WAL file fails.
    //
    // Stage 10: Each SnapshotEntry carries an optional expires_at_us value.
    // When expires_at_us == 0, a permanent SET record is written.
    // When expires_at_us != 0, a SET_WITH_EXPIRY record is written.
    // Expired entries (expires_at_us <= now) are excluded from the snapshot
    // by the caller (KeyValueStore::compact()); rewrite() trusts the input.
    struct SnapshotEntry {
        std::string   key;
        std::string   value;
        std::uint64_t expires_at_us{0}; // 0 = permanent
    };
    void rewrite(const std::vector<SnapshotEntry>& snapshot);

    // -------------------------------------------------------------------------
    // Accessors
    // -------------------------------------------------------------------------

    // Return the file path this WAL was opened with.
    [[nodiscard]] const std::string& path() const noexcept;

private:
    // -------------------------------------------------------------------------
    // Encoding helpers (little-endian)
    // -------------------------------------------------------------------------

    // Encode a uint32_t as 4 bytes, little-endian, appended to buf.
    static void encode_u32(std::vector<std::uint8_t>& buf, std::uint32_t v);

    // Encode a uint8_t as 1 byte, appended to buf.
    static void encode_u8(std::vector<std::uint8_t>& buf, std::uint8_t v);

    // Encode a uint64_t as 8 bytes, little-endian, appended to buf.
    static void encode_u64(std::vector<std::uint8_t>& buf, std::uint64_t v);

    // -------------------------------------------------------------------------
    // CRC32
    // -------------------------------------------------------------------------

    // Compute CRC32 (ISO 3309 polynomial, 0xEDB88320 reflected) over the
    // given byte range [data, data + len).
    // Returns the final CRC32 value.
    static std::uint32_t crc32(const std::uint8_t* data, std::size_t len);

    // -------------------------------------------------------------------------
    // Core write helper
    // -------------------------------------------------------------------------

    // Serialise and write one complete WAL record to stream_.
    // Computes the checksum over the header + payload bytes, then writes
    // header + payload + [expires_at_us?] + checksum as a single binary blob.
    // Flushes after every write.
    // For kOpSetWithExpiry: expires_at_us is encoded as 8 bytes LE after
    // the value payload and before the checksum.
    // For other opcodes: expires_at_us is ignored (pass 0).
    // Throws std::runtime_error if the write or flush fails.
    void write_record(std::uint8_t opcode,
                      const std::string& key,
                      const std::string& value,
                      std::uint64_t expires_at_us);

    // -------------------------------------------------------------------------
    // State
    // -------------------------------------------------------------------------

    std::string   path_;    // path to the log file
    std::ofstream stream_;  // binary append-mode file stream; owned by this WAL
};

} // namespace forgekv
