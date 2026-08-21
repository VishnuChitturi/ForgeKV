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
#include <fstream>
#include <functional>
#include <string>
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
inline constexpr std::uint8_t  kOpSet      = 0x01u;  // SET key value
inline constexpr std::uint8_t  kOpDel      = 0x02u;  // DEL key
inline constexpr std::uint8_t  kOpClear    = 0x03u;  // CLEAR (no key/value)

// Fixed header size in bytes: magic(4) + version(1) + opcode(1) + key_len(4)
//                                      + val_len(4) = 14 bytes.
inline constexpr std::size_t   kWalHeaderSize     = 14u;

// Size of the trailing CRC32 checksum field.
inline constexpr std::size_t   kWalChecksumSize   = 4u;

// =============================================================================
// WalRecord — decoded, validated record returned by read_record()
// =============================================================================

struct WalRecord {
    std::uint8_t  opcode;   // kOpSet, kOpDel, or kOpClear
    std::string   key;      // Key bytes (empty for CLEAR)
    std::string   value;    // Value bytes (empty for DEL and CLEAR)
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
    // header + payload + checksum as a single binary blob.
    // Flushes after every write.
    // Throws std::runtime_error if the write or flush fails.
    void write_record(std::uint8_t opcode,
                      const std::string& key,
                      const std::string& value);

    // -------------------------------------------------------------------------
    // State
    // -------------------------------------------------------------------------

    std::string   path_;    // path to the log file
    std::ofstream stream_;  // binary append-mode file stream; owned by this WAL
};

} // namespace forgekv
