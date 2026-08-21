// =============================================================================
// ForgeKV — Stage 4: WAL implementation — Binary serialisation + CRC32
// =============================================================================
//
// See include/forgekv/wal.h for the full design, record layout, and field
// documentation. This file implements:
//
//   1. encode_u8 / encode_u32 — explicit little-endian serialisation helpers.
//   2. crc32 — lookup-table CRC32 (ISO 3309 / 0xEDB88320 reflected).
//   3. write_record — assemble a complete binary WAL record and write it.
//   4. append_set / append_del / append_clear — public mutation loggers.
//   5. read_record — deserialise + validate one record from an input stream.
//
// ENDIANNESS
// ----------
// All multi-byte integers written to disk are little-endian. encode_u32
// serialises byte-by-byte in increasing address order (LSB first) regardless
// of the host CPU's native byte order. This guarantees a deterministic on-disk
// format across x86, ARM, RISC-V, and any future target.
//
// CRC32 ALGORITHM
// ---------------
// Standard CRC32 with the reflected polynomial 0xEDB88320 (same polynomial
// used in zlib, Ethernet, PKZIP — widely tested). The lookup table is
// computed once at program start via a lambda initialiser. No external
// libraries are used. The algorithm processes bytes left-to-right.
//
// The checksum covers every byte in [magic .. end-of-payload], i.e., the
// header (14 bytes) plus all payload bytes (key_len + val_len bytes). The
// checksum field itself is excluded from the calculation.
//
// READ_RECORD
// -----------
// Reads a record header (14 bytes), validates magic/version/opcode, reads the
// payload (key + value), reads the 4-byte checksum, then re-computes the CRC32
// over the header+payload bytes and compares. If anything is missing or
// mismatched, a std::runtime_error is thrown. The stream position advances by
// exactly one full record on success.
// =============================================================================

#include "forgekv/wal.h"

#include <array>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <cstring>  // std::memcpy

namespace forgekv {

// =============================================================================
// CRC32 lookup table (computed at startup)
// =============================================================================

namespace {

// Build a 256-entry CRC32 lookup table for the reflected polynomial 0xEDB88320.
// This is the same polynomial used by zlib, Ethernet CRC, PKZIP, and others.
// The table is computed once and is effectively a compile-time constant from
// the program's perspective.
const std::array<std::uint32_t, 256>& crc32_table() {
    static const auto table = []() {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256u; ++i) {
            std::uint32_t crc = i;
            for (int bit = 0; bit < 8; ++bit) {
                if (crc & 1u) {
                    crc = (crc >> 1) ^ 0xEDB88320u;
                } else {
                    crc >>= 1;
                }
            }
            t[i] = crc;
        }
        return t;
    }();
    return table;
}

} // anonymous namespace

// =============================================================================
// Construction
// =============================================================================

WAL::WAL(const std::string& path)
    : path_(path),
      stream_(path, std::ios::binary | std::ios::app)
{
    if (!stream_.is_open()) {
        throw std::runtime_error("WAL: failed to open log file: " + path);
    }
}

// =============================================================================
// Public append operations
// =============================================================================

void WAL::append_set(const std::string& key, const std::string& value) {
    write_record(kOpSet, key, value);
}

void WAL::append_del(const std::string& key) {
    write_record(kOpDel, key, /*value=*/"");
}

void WAL::append_clear() {
    write_record(kOpClear, /*key=*/"", /*value=*/"");
}

// =============================================================================
// path() accessor
// =============================================================================

const std::string& WAL::path() const noexcept {
    return path_;
}

// =============================================================================
// encode_u8 — serialise a uint8_t (trivial, but kept symmetric)
// =============================================================================

void WAL::encode_u8(std::vector<std::uint8_t>& buf, std::uint8_t v) {
    buf.push_back(v);
}

// =============================================================================
// encode_u32 — serialise a uint32_t as 4 bytes, little-endian
// =============================================================================
//
// The four bytes are appended LSB-first regardless of the host endianness:
//
//   value = 0x464B5741
//   bytes appended: 0x41, 0x57, 0x4B, 0x46   (i.e., byte[0] = v & 0xFF)
//
// We do NOT use reinterpret_cast or memcpy with an int because those are
// host-endian-dependent. Explicit shift + mask is platform-neutral.

void WAL::encode_u32(std::vector<std::uint8_t>& buf, std::uint32_t v) {
    buf.push_back(static_cast<std::uint8_t>( v        & 0xFFu));
    buf.push_back(static_cast<std::uint8_t>((v >>  8) & 0xFFu));
    buf.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFFu));
    buf.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFFu));
}

// =============================================================================
// crc32 — compute CRC32 over [data, data+len)
// =============================================================================

std::uint32_t WAL::crc32(const std::uint8_t* data, std::size_t len) {
    const auto& table = crc32_table();
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < len; ++i) {
        const std::uint8_t byte = data[i];
        crc = (crc >> 8) ^ table[(crc ^ byte) & 0xFFu];
    }
    return crc ^ 0xFFFFFFFFu;
}

// =============================================================================
// write_record — assemble and write one complete binary WAL record
// =============================================================================
//
// Record layout assembled into a single buffer:
//
//   [magic 4B][version 1B][opcode 1B][key_len 4B][val_len 4B]
//   [key bytes][value bytes]
//   [checksum 4B]
//
// The CRC32 is computed over the header + payload bytes (bytes before the
// checksum field). The checksum is appended last.
//
// The entire record is written in one stream_.write() call for atomicity
// at the std::ofstream level, then immediately flushed.

void WAL::write_record(std::uint8_t opcode,
                       const std::string& key,
                       const std::string& value) {
    // Safety: key_len and val_len must fit in uint32_t.
    // In practice string::size() returns size_t; guard against overflow.
    if (key.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
        throw std::runtime_error("WAL: key too large to encode");
    }
    if (value.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
        throw std::runtime_error("WAL: value too large to encode");
    }

    const auto key_len = static_cast<std::uint32_t>(key.size());
    const auto val_len = static_cast<std::uint32_t>(value.size());

    // Build header + payload bytes (everything the checksum covers).
    std::vector<std::uint8_t> body;
    body.reserve(kWalHeaderSize + key_len + val_len);

    encode_u32(body, kWalMagic);        // bytes 0-3:   magic
    encode_u8 (body, kWalVersion);      // byte  4:     version
    encode_u8 (body, opcode);           // byte  5:     opcode
    encode_u32(body, key_len);          // bytes 6-9:   key length
    encode_u32(body, val_len);          // bytes 10-13: value length

    // Payload: key bytes then value bytes.
    for (const char c : key) {
        body.push_back(static_cast<std::uint8_t>(c));
    }
    for (const char c : value) {
        body.push_back(static_cast<std::uint8_t>(c));
    }

    // Checksum covers the entire body assembled above.
    const std::uint32_t checksum = crc32(body.data(), body.size());

    // Append the 4-byte checksum (little-endian) to the buffer.
    encode_u32(body, checksum);

    // Write the full record in one call, then flush.
    stream_.write(reinterpret_cast<const char*>(body.data()),
                  static_cast<std::streamsize>(body.size()));
    stream_.flush();

    if (stream_.fail()) {
        throw std::runtime_error(
            "WAL: write failed (opcode=" + std::to_string(opcode) + ")");
    }
}

// =============================================================================
// read_record — deserialise + validate one WAL record from an input stream
// =============================================================================
//
// Reads exactly 14 header bytes, then key_len + val_len payload bytes, then
// 4 checksum bytes. Validates magic, version, opcode, and CRC32.
//
// Returns a WalRecord on success.
// Throws std::runtime_error for any truncation, invalid field, or checksum
// mismatch.

WalRecord WAL::read_record(std::istream& in) {
    // -------------------------------------------------------------------------
    // Helper: read exactly n bytes into a buffer. Throws on truncation.
    // -------------------------------------------------------------------------
    auto read_exact = [&](std::vector<std::uint8_t>& dest,
                          std::size_t n,
                          const char* context) {
        const std::size_t old_size = dest.size();
        dest.resize(old_size + n);
        in.read(reinterpret_cast<char*>(dest.data() + old_size),
                static_cast<std::streamsize>(n));
        if (static_cast<std::size_t>(in.gcount()) != n) {
            throw std::runtime_error(
                std::string("WAL: truncated record (") + context + ")");
        }
    };

    // -------------------------------------------------------------------------
    // Helper: decode a little-endian uint32_t from 4 bytes at buf[offset].
    // -------------------------------------------------------------------------
    auto decode_u32 = [](const std::vector<std::uint8_t>& buf,
                         std::size_t offset) -> std::uint32_t {
        return static_cast<std::uint32_t>(buf[offset])
             | (static_cast<std::uint32_t>(buf[offset + 1]) <<  8)
             | (static_cast<std::uint32_t>(buf[offset + 2]) << 16)
             | (static_cast<std::uint32_t>(buf[offset + 3]) << 24);
    };

    // -------------------------------------------------------------------------
    // 1. Read the fixed-size header (14 bytes).
    // -------------------------------------------------------------------------
    std::vector<std::uint8_t> record_bytes;
    record_bytes.reserve(kWalHeaderSize);
    read_exact(record_bytes, kWalHeaderSize, "header");

    // Validate magic.
    const std::uint32_t magic = decode_u32(record_bytes, 0);
    if (magic != kWalMagic) {
        throw std::runtime_error(
            "WAL: invalid magic number (expected 0x464B5741, got 0x"
            + [&]() {
                char hex[9];
                std::snprintf(hex, sizeof(hex), "%08X", magic);
                return std::string(hex);
              }() + ")");
    }

    // Validate version.
    const std::uint8_t version = record_bytes[4];
    if (version != kWalVersion) {
        throw std::runtime_error(
            "WAL: unknown format version: " + std::to_string(version));
    }

    // Validate opcode.
    const std::uint8_t opcode = record_bytes[5];
    if (opcode != kOpSet && opcode != kOpDel && opcode != kOpClear) {
        throw std::runtime_error(
            "WAL: unknown opcode: " + std::to_string(opcode));
    }

    // Decode lengths.
    const std::uint32_t key_len = decode_u32(record_bytes, 6);
    const std::uint32_t val_len = decode_u32(record_bytes, 10);

    // -------------------------------------------------------------------------
    // 2. Read the payload (key bytes + value bytes).
    // -------------------------------------------------------------------------
    const std::size_t payload_len = static_cast<std::size_t>(key_len)
                                  + static_cast<std::size_t>(val_len);
    if (payload_len > 0) {
        read_exact(record_bytes, payload_len, "payload");
    }

    // -------------------------------------------------------------------------
    // 3. Read the 4-byte checksum field (NOT part of what the CRC covers).
    // -------------------------------------------------------------------------
    std::vector<std::uint8_t> checksum_bytes;
    checksum_bytes.reserve(kWalChecksumSize);
    read_exact(checksum_bytes, kWalChecksumSize, "checksum");

    const std::uint32_t stored_checksum =
        static_cast<std::uint32_t>(checksum_bytes[0])
      | (static_cast<std::uint32_t>(checksum_bytes[1]) <<  8)
      | (static_cast<std::uint32_t>(checksum_bytes[2]) << 16)
      | (static_cast<std::uint32_t>(checksum_bytes[3]) << 24);

    // -------------------------------------------------------------------------
    // 4. Verify the checksum.
    // The CRC32 covers record_bytes (header + payload) = bytes[0..18+K+V-5].
    // -------------------------------------------------------------------------
    const std::uint32_t computed_checksum =
        crc32(record_bytes.data(), record_bytes.size());

    if (computed_checksum != stored_checksum) {
        throw std::runtime_error(
            "WAL: checksum mismatch — record is corrupted");
    }

    // -------------------------------------------------------------------------
    // 5. Decode payload into key and value strings.
    // -------------------------------------------------------------------------
    WalRecord rec;
    rec.opcode = opcode;

    if (key_len > 0) {
        rec.key.assign(
            reinterpret_cast<const char*>(record_bytes.data() + kWalHeaderSize),
            key_len);
    }
    if (val_len > 0) {
        rec.value.assign(
            reinterpret_cast<const char*>(
                record_bytes.data() + kWalHeaderSize + key_len),
            val_len);
    }

    return rec;
}

// =============================================================================
// replay — read all complete valid records from the WAL file and invoke
//           callback for each one, in strict file order.
// =============================================================================
//
// Truncation detection strategy:
//
//   read_record() throws std::runtime_error with a message starting with
//   "WAL: truncated record (" whenever it hits an unexpected EOF.  Any other
//   error message indicates a structural corruption (bad magic, bad version,
//   bad opcode, checksum mismatch) in an otherwise complete record.
//
//   When a truncation exception is caught, we inspect the input stream:
//
//     - If the stream's EOF flag is set (i.e., the read attempt reached the
//       actual end of the file), the incomplete record is the LAST entry in
//       the file — this is a normal crash tail.  We stop replay, set
//       incomplete_tail = true, and return successfully.
//
//     - If the EOF flag is NOT set (the truncated record is followed by more
//       bytes), the corruption is mid-log and is treated as fatal.  We rethrow
//       a descriptive error.  The caller must not trust the partial state.
//
//   In both truncation cases all previously replayed records are already in
//   Storage, but the current incomplete record is NOT applied.
//
// WAL duplication prevention:
//
//   replay() opens the file with a fresh std::ifstream for reading only.
//   It never touches the WAL's own append-mode stream_ and never calls any
//   append_* function.  This guarantees that recovery cannot write new records.

WAL::ReplayResult
WAL::replay(std::function<void(const WalRecord&)> callback) const
{
    // Open the WAL file for reading from the beginning.
    std::ifstream in(path_, std::ios::binary);
    if (!in.is_open()) {
        // If the file simply does not exist yet, replay is a no-op.
        // An empty / new WAL is not an error.
        return ReplayResult{};
    }

    ReplayResult result;

    while (true) {
        // Record the stream position before attempting to read.  This lets us
        // distinguish a clean EOF (no bytes consumed) from a truncated record
        // (some bytes consumed before EOF).
        const std::streampos pos_before = in.tellg();

        // Peek at the next byte.  If the stream is at EOF before we even begin
        // reading a record, we are done — clean termination.
        in.peek();
        if (in.eof()) {
            break; // clean EOF — no incomplete tail
        }

        // Attempt to read one complete record.
        try {
            WalRecord rec = read_record(in);
            callback(rec);
            ++result.records_replayed;
        }
        catch (const std::runtime_error& e) {
            const std::string msg = e.what();
            const bool is_truncation =
                (msg.rfind("WAL: truncated record (", 0) == 0);

            if (is_truncation) {
                // Check whether we are at EOF (the incomplete record is the
                // last thing in the file) or not (there are more bytes after
                // this broken record — mid-log corruption).
                if (in.eof()) {
                    // Normal crash tail: a partial final record.
                    // All prior records have already been applied.
                    result.incomplete_tail = true;
                    return result;
                }
                else {
                    // Mid-log: a truncated record with data after it.
                    // This is unrecoverable — stop and report.
                    (void)pos_before; // suppress unused-variable warning
                    throw std::runtime_error(
                        "WAL: recovery failed — truncated record in the "
                        "middle of the log (not at EOF); log may be corrupted");
                }
            }
            else {
                // Structural corruption in an otherwise complete record
                // (bad magic, bad version, bad opcode, checksum mismatch).
                // Always fatal regardless of position.
                throw std::runtime_error(
                    "WAL: recovery failed — " + msg);
            }
        }
    }

    return result;
}

// =============================================================================
// rewrite — atomically replace the WAL with a compacted snapshot
// =============================================================================
//
// Strategy:
//
//   1. Build a temporary path in the same directory as path_ so that
//      std::filesystem::rename() is guaranteed to be a same-partition
//      atomic replace on POSIX systems.
//
//   2. Open the temp file for binary writing (truncate mode — it must start
//      empty).  Write one SET record for every (key, value) pair in snapshot.
//      Flush after each record (matching the normal WAL durability contract).
//
//   3. Close the temp file stream before rename to flush OS buffers.
//
//   4. std::filesystem::rename() atomically replaces path_ with the temp
//      file.  If rename fails, the original path_ is untouched and the temp
//      file is removed (best-effort).
//
//   5. Reopen stream_ in binary append mode on the new path_.  The old inode
//      (which was atomically swapped out) is released.  All future
//      append_set/append_del/append_clear operations now write to the new,
//      compacted WAL.
//
// Note: this function is always called under KeyValueStore's exclusive
// mutex_, so no additional synchronization is needed here.

void WAL::rewrite(const std::vector<std::pair<std::string, std::string>>& snapshot)
{
    // -------------------------------------------------------------------------
    // 1. Derive a temp path in the same directory as the WAL file.
    //    Using the same directory is essential for atomic rename (same device).
    // -------------------------------------------------------------------------
    const std::filesystem::path wal_path(path_);
    const std::filesystem::path dir = wal_path.parent_path().empty()
                                      ? std::filesystem::path(".")
                                      : wal_path.parent_path();
    const std::filesystem::path tmp_path =
        dir / (wal_path.filename().string() + ".compact.tmp");
    const std::string tmp_str = tmp_path.string();

    // -------------------------------------------------------------------------
    // 2. Write all records to the temp file.
    //    On any failure: remove temp, throw — original WAL is untouched.
    // -------------------------------------------------------------------------
    {
        std::ofstream tmp(tmp_str, std::ios::binary | std::ios::trunc);
        if (!tmp.is_open()) {
            throw std::runtime_error(
                "WAL::rewrite: failed to create temporary file: " + tmp_str);
        }

        // Write one SET record per live key.
        // We build each record by temporarily redirecting stream_ to tmp,
        // but it is cleaner and safer to build the binary blob directly
        // via a local helper that reuses the existing write_record() logic
        // through a separate ofstream.
        //
        // We replicate the write_record logic inline here using the local
        // temp ofstream to avoid swapping stream_ prematurely.

        for (const auto& [key, value] : snapshot) {
            // Replicate write_record() — same binary layout, same CRC32 —
            // but targeting tmp instead of stream_.

            if (key.size() > static_cast<std::size_t>(
                    std::numeric_limits<std::uint32_t>::max())) {
                // Clean up and throw.
                tmp.close();
                std::error_code ec;
                std::filesystem::remove(tmp_path, ec);
                throw std::runtime_error("WAL::rewrite: key too large to encode");
            }
            if (value.size() > static_cast<std::size_t>(
                    std::numeric_limits<std::uint32_t>::max())) {
                tmp.close();
                std::error_code ec;
                std::filesystem::remove(tmp_path, ec);
                throw std::runtime_error("WAL::rewrite: value too large to encode");
            }

            const auto key_len = static_cast<std::uint32_t>(key.size());
            const auto val_len = static_cast<std::uint32_t>(value.size());

            std::vector<std::uint8_t> body;
            body.reserve(kWalHeaderSize + key_len + val_len);

            encode_u32(body, kWalMagic);
            encode_u8 (body, kWalVersion);
            encode_u8 (body, kOpSet);
            encode_u32(body, key_len);
            encode_u32(body, val_len);

            for (const char c : key)   { body.push_back(static_cast<std::uint8_t>(c)); }
            for (const char c : value) { body.push_back(static_cast<std::uint8_t>(c)); }

            const std::uint32_t checksum = crc32(body.data(), body.size());
            encode_u32(body, checksum);

            tmp.write(reinterpret_cast<const char*>(body.data()),
                      static_cast<std::streamsize>(body.size()));
            tmp.flush();

            if (tmp.fail()) {
                tmp.close();
                std::error_code ec;
                std::filesystem::remove(tmp_path, ec);
                throw std::runtime_error(
                    "WAL::rewrite: write failed for key: " + key);
            }
        }
        // tmp goes out of scope here → destructor closes and flushes.
    }

    // -------------------------------------------------------------------------
    // 3. Atomically replace the original WAL with the temp file.
    //    std::filesystem::rename() is atomic on POSIX (same device).
    //    On failure, remove the temp file and leave the original intact.
    // -------------------------------------------------------------------------
    {
        std::error_code ec;
        std::filesystem::rename(tmp_path, wal_path, ec);
        if (ec) {
            // Rename failed — clean up temp, preserve original.
            std::error_code rm_ec;
            std::filesystem::remove(tmp_path, rm_ec);
            throw std::runtime_error(
                "WAL::rewrite: atomic rename failed: " + ec.message());
        }
    }

    // -------------------------------------------------------------------------
    // 4. Reopen stream_ on the new WAL file (same path, new inode).
    //    This is critical: without this, stream_ still refers to the old
    //    inode and future appends would go to the replaced (now deleted) file.
    // -------------------------------------------------------------------------
    stream_.close();
    stream_.open(path_, std::ios::binary | std::ios::app);
    if (!stream_.is_open()) {
        // The compacted WAL is on disk at path_, but we cannot append to it.
        // This is a fatal state — callers should not continue using this WAL.
        throw std::runtime_error(
            "WAL::rewrite: compaction succeeded but failed to reopen WAL "
            "stream at: " + path_);
    }
}

// =============================================================================
// replay_from — partial WAL replay starting at a byte offset
// =============================================================================
//
// Seeks the input stream to `offset`, then delegates to the same loop logic
// as replay().
//
// Offset validation:
//
//   If the WAL file does not exist and offset == 0: return empty result (same
//   as replay() for a missing file).
//
//   If the WAL file does not exist and offset > 0: return empty result (the
//   caller's snapshot said the boundary was offset N, but after compaction or
//   if the file was deleted, there is nothing to replay).
//
//   If offset > file_size: throw std::runtime_error.
//
//   If the byte at offset is not the start of a valid record: read_record()
//   will throw with an "invalid magic" error, which propagates as a fatal
//   corruption error (consistent with replay() behaviour for corrupted records).

WAL::ReplayResult
WAL::replay_from(std::uint64_t offset,
                 std::function<void(const WalRecord&)> callback) const
{
    // Open for reading.
    std::ifstream in(path_, std::ios::binary);
    if (!in.is_open()) {
        // File does not exist — no records to replay from any offset.
        // (Caller's snapshot may have been created before the WAL was reset
        //  by some edge case; silently return empty result.)
        return ReplayResult{};
    }

    if (offset > 0) {
        // Determine file size to validate offset.
        in.seekg(0, std::ios::end);
        const std::uint64_t eof_pos = static_cast<std::uint64_t>(in.tellg());

        if (offset > eof_pos) {
            throw std::runtime_error(
                "WAL::replay_from: offset " + std::to_string(offset)
                + " is beyond end of file (" + std::to_string(eof_pos) + " bytes)");
        }

        if (offset == eof_pos) {
            // Offset is exactly at EOF — nothing to replay.
            return ReplayResult{};
        }

        // Seek to the requested offset.
        in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        if (!in) {
            throw std::runtime_error(
                "WAL::replay_from: seek to offset "
                + std::to_string(offset) + " failed");
        }
    }

    // -------------------------------------------------------------------------
    // Replay loop — identical logic to replay() from here on.
    // -------------------------------------------------------------------------
    ReplayResult result;

    while (true) {
        const std::streampos pos_before = in.tellg();

        in.peek();
        if (in.eof()) {
            break; // clean EOF
        }

        try {
            WalRecord rec = read_record(in);
            callback(rec);
            ++result.records_replayed;
        }
        catch (const std::runtime_error& e) {
            const std::string msg = e.what();
            const bool is_truncation =
                (msg.rfind("WAL: truncated record (", 0) == 0);

            if (is_truncation) {
                if (in.eof()) {
                    result.incomplete_tail = true;
                    return result;
                } else {
                    (void)pos_before;
                    throw std::runtime_error(
                        "WAL: recovery failed — truncated record in the "
                        "middle of the log (not at EOF); log may be corrupted");
                }
            } else {
                throw std::runtime_error(
                    "WAL: recovery failed — " + msg);
            }
        }
    }

    return result;
}

// =============================================================================
// file_size — return the current size of the WAL file in bytes
// =============================================================================
//
// Used by KeyValueStore::snapshot() to capture the WAL boundary position.
// Returns 0 if the file does not exist.

std::uint64_t WAL::file_size() const noexcept
{
    std::error_code ec;
    const auto sz = std::filesystem::file_size(path_, ec);
    if (ec) {
        return 0u;
    }
    return static_cast<std::uint64_t>(sz);
}

} // namespace forgekv
