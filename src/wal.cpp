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

} // namespace forgekv
