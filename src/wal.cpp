// =============================================================================
// ForgeKV — Stage 10: WAL implementation — TTL / Expiration support
// =============================================================================
//
// Stage 10 adds:
//
//   - kOpSetWithExpiry (0x04): a new WAL opcode for SET_WITH_EXPIRY records.
//     These carry an 8-byte absolute expiration timestamp (microseconds since
//     Unix epoch) AFTER the value bytes but BEFORE the CRC32 checksum.
//
//   - append_set_with_expiry(): writes a kOpSetWithExpiry record.
//
//   - encode_u64(): 8-byte little-endian uint64 serialisation helper.
//
//   - read_record(): extended to handle opcode 0x04; sets WalRecord.expires_at_us.
//
//   - rewrite(): updated to accept SnapshotEntry (key, value, expires_at_us)
//     and write appropriate SET or SET_WITH_EXPIRY records.
//
// BACKWARD COMPATIBILITY:
//   Old WAL files contain only opcodes 0x01 (SET), 0x02 (DEL), 0x03 (CLEAR).
//   Those are handled identically.  The new opcode 0x04 is only present in
//   WALs written by Stage 10+.  Old recovery code that does not know about
//   0x04 would reject it as "unknown opcode" — but that code path is also
//   replaced by Stage 10.  Within Stage 10 the validator explicitly allows 0x04.
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
    write_record(kOpSet, key, value, 0);
}

void WAL::append_del(const std::string& key) {
    write_record(kOpDel, key, /*value=*/"", 0);
}

void WAL::append_clear() {
    write_record(kOpClear, /*key=*/"", /*value=*/"", 0);
}

void WAL::append_set_with_expiry(const std::string& key,
                                  const std::string& value,
                                  std::uint64_t      expires_at_us) {
    write_record(kOpSetWithExpiry, key, value, expires_at_us);
}

// =============================================================================
// path() accessor
// =============================================================================

const std::string& WAL::path() const noexcept {
    return path_;
}

// =============================================================================
// Encoding helpers
// =============================================================================

void WAL::encode_u8(std::vector<std::uint8_t>& buf, std::uint8_t v) {
    buf.push_back(v);
}

void WAL::encode_u32(std::vector<std::uint8_t>& buf, std::uint32_t v) {
    buf.push_back(static_cast<std::uint8_t>( v        & 0xFFu));
    buf.push_back(static_cast<std::uint8_t>((v >>  8) & 0xFFu));
    buf.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFFu));
    buf.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFFu));
}

void WAL::encode_u64(std::vector<std::uint8_t>& buf, std::uint64_t v) {
    buf.push_back(static_cast<std::uint8_t>( v        & 0xFFu));
    buf.push_back(static_cast<std::uint8_t>((v >>  8) & 0xFFu));
    buf.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFFu));
    buf.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFFu));
    buf.push_back(static_cast<std::uint8_t>((v >> 32) & 0xFFu));
    buf.push_back(static_cast<std::uint8_t>((v >> 40) & 0xFFu));
    buf.push_back(static_cast<std::uint8_t>((v >> 48) & 0xFFu));
    buf.push_back(static_cast<std::uint8_t>((v >> 56) & 0xFFu));
}

// =============================================================================
// CRC32
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
// For kOpSet, kOpDel, kOpClear: expires_at_us is ignored (should be 0).
// For kOpSetWithExpiry: expires_at_us is encoded as 8 bytes little-endian
// after the value payload but BEFORE the checksum.

void WAL::write_record(std::uint8_t opcode,
                       const std::string& key,
                       const std::string& value,
                       std::uint64_t expires_at_us) {
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
    const bool has_expiry = (opcode == kOpSetWithExpiry);
    body.reserve(kWalHeaderSize + key_len + val_len
                 + (has_expiry ? kWalExpirySize : 0));

    encode_u32(body, kWalMagic);        // bytes 0-3:   magic
    encode_u8 (body, kWalVersion);      // byte  4:     version
    encode_u8 (body, opcode);           // byte  5:     opcode
    encode_u32(body, key_len);          // bytes 6-9:   key length
    encode_u32(body, val_len);          // bytes 10-13: value length

    for (const char c : key)   { body.push_back(static_cast<std::uint8_t>(c)); }
    for (const char c : value) { body.push_back(static_cast<std::uint8_t>(c)); }

    // Stage 10: For SET_WITH_EXPIRY, append 8-byte expiration timestamp.
    if (has_expiry) {
        encode_u64(body, expires_at_us);
    }

    // Checksum covers everything in body so far.
    const std::uint32_t checksum = crc32(body.data(), body.size());
    encode_u32(body, checksum);

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
// Handles opcodes 0x01 (SET), 0x02 (DEL), 0x03 (CLEAR), 0x04 (SET_WITH_EXPIRY).
// For 0x04: reads 8 extra bytes of expires_at_us after the value payload,
//           includes them in the checksum coverage.

WalRecord WAL::read_record(std::istream& in) {
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

    auto decode_u32 = [](const std::vector<std::uint8_t>& buf,
                         std::size_t offset) -> std::uint32_t {
        return static_cast<std::uint32_t>(buf[offset])
             | (static_cast<std::uint32_t>(buf[offset + 1]) <<  8)
             | (static_cast<std::uint32_t>(buf[offset + 2]) << 16)
             | (static_cast<std::uint32_t>(buf[offset + 3]) << 24);
    };

    auto decode_u64 = [](const std::vector<std::uint8_t>& buf,
                         std::size_t offset) -> std::uint64_t {
        return  static_cast<std::uint64_t>(buf[offset])
             | (static_cast<std::uint64_t>(buf[offset + 1]) <<  8)
             | (static_cast<std::uint64_t>(buf[offset + 2]) << 16)
             | (static_cast<std::uint64_t>(buf[offset + 3]) << 24)
             | (static_cast<std::uint64_t>(buf[offset + 4]) << 32)
             | (static_cast<std::uint64_t>(buf[offset + 5]) << 40)
             | (static_cast<std::uint64_t>(buf[offset + 6]) << 48)
             | (static_cast<std::uint64_t>(buf[offset + 7]) << 56);
    };

    // 1. Read fixed-size header (14 bytes).
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

    // Validate opcode (now also allows kOpSetWithExpiry = 0x04).
    const std::uint8_t opcode = record_bytes[5];
    if (opcode != kOpSet && opcode != kOpDel &&
        opcode != kOpClear && opcode != kOpSetWithExpiry) {
        throw std::runtime_error(
            "WAL: unknown opcode: " + std::to_string(opcode));
    }

    const std::uint32_t key_len = decode_u32(record_bytes, 6);
    const std::uint32_t val_len = decode_u32(record_bytes, 10);

    // 2. Read payload (key + value).
    const std::size_t payload_len = static_cast<std::size_t>(key_len)
                                  + static_cast<std::size_t>(val_len);
    if (payload_len > 0) {
        read_exact(record_bytes, payload_len, "payload");
    }

    // 3. Stage 10: for kOpSetWithExpiry, read 8-byte expires_at_us.
    //    These bytes are PART of the checksum-covered data.
    if (opcode == kOpSetWithExpiry) {
        read_exact(record_bytes, kWalExpirySize, "expires_at");
    }

    // 4. Read 4-byte checksum (NOT part of what the CRC covers).
    std::vector<std::uint8_t> checksum_bytes;
    checksum_bytes.reserve(kWalChecksumSize);
    read_exact(checksum_bytes, kWalChecksumSize, "checksum");

    const std::uint32_t stored_checksum =
        static_cast<std::uint32_t>(checksum_bytes[0])
      | (static_cast<std::uint32_t>(checksum_bytes[1]) <<  8)
      | (static_cast<std::uint32_t>(checksum_bytes[2]) << 16)
      | (static_cast<std::uint32_t>(checksum_bytes[3]) << 24);

    // 5. Verify checksum.
    const std::uint32_t computed_checksum =
        crc32(record_bytes.data(), record_bytes.size());

    if (computed_checksum != stored_checksum) {
        throw std::runtime_error(
            "WAL: checksum mismatch — record is corrupted");
    }

    // 6. Decode into WalRecord.
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

    // Stage 10: decode expires_at_us for SET_WITH_EXPIRY.
    if (opcode == kOpSetWithExpiry) {
        const std::size_t expiry_offset = kWalHeaderSize + key_len + val_len;
        rec.expires_at_us = decode_u64(record_bytes, expiry_offset);
    }

    return rec;
}

// =============================================================================
// replay — read all complete valid records from the WAL file
// =============================================================================

WAL::ReplayResult
WAL::replay(std::function<void(const WalRecord&)> callback) const
{
    std::ifstream in(path_, std::ios::binary);
    if (!in.is_open()) {
        return ReplayResult{};
    }

    ReplayResult result;

    while (true) {
        const std::streampos pos_before = in.tellg();

        in.peek();
        if (in.eof()) {
            break;
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
// rewrite — atomically replace the WAL with a compacted snapshot
// =============================================================================
//
// Stage 10: accepts SnapshotEntry objects that carry expiry metadata.
// For entries with expires_at_us == 0, writes a permanent SET record.
// For entries with expires_at_us != 0, writes a SET_WITH_EXPIRY record.
// Expired entries should already be excluded by the caller.

void WAL::rewrite(const std::vector<SnapshotEntry>& snapshot)
{
    const std::filesystem::path wal_path(path_);
    const std::filesystem::path dir = wal_path.parent_path().empty()
                                      ? std::filesystem::path(".")
                                      : wal_path.parent_path();
    const std::filesystem::path tmp_path =
        dir / (wal_path.filename().string() + ".compact.tmp");
    const std::string tmp_str = tmp_path.string();

    {
        std::ofstream tmp(tmp_str, std::ios::binary | std::ios::trunc);
        if (!tmp.is_open()) {
            throw std::runtime_error(
                "WAL::rewrite: failed to create temporary file: " + tmp_str);
        }

        auto write_entry = [&](const SnapshotEntry& entry) {
            if (entry.key.size() > static_cast<std::size_t>(
                    std::numeric_limits<std::uint32_t>::max())) {
                tmp.close();
                std::error_code ec;
                std::filesystem::remove(tmp_path, ec);
                throw std::runtime_error("WAL::rewrite: key too large to encode");
            }
            if (entry.value.size() > static_cast<std::size_t>(
                    std::numeric_limits<std::uint32_t>::max())) {
                tmp.close();
                std::error_code ec;
                std::filesystem::remove(tmp_path, ec);
                throw std::runtime_error("WAL::rewrite: value too large to encode");
            }

            const auto key_len = static_cast<std::uint32_t>(entry.key.size());
            const auto val_len = static_cast<std::uint32_t>(entry.value.size());
            const bool has_expiry = (entry.expires_at_us != 0);
            const std::uint8_t opcode = has_expiry ? kOpSetWithExpiry : kOpSet;

            std::vector<std::uint8_t> body;
            body.reserve(kWalHeaderSize + key_len + val_len
                         + (has_expiry ? kWalExpirySize : 0));

            encode_u32(body, kWalMagic);
            encode_u8 (body, kWalVersion);
            encode_u8 (body, opcode);
            encode_u32(body, key_len);
            encode_u32(body, val_len);

            for (const char c : entry.key)   { body.push_back(static_cast<std::uint8_t>(c)); }
            for (const char c : entry.value) { body.push_back(static_cast<std::uint8_t>(c)); }

            if (has_expiry) {
                encode_u64(body, entry.expires_at_us);
            }

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
                    "WAL::rewrite: write failed for key: " + entry.key);
            }
        };

        for (const auto& entry : snapshot) {
            write_entry(entry);
        }
        // tmp closes here.
    }

    {
        std::error_code ec;
        std::filesystem::rename(tmp_path, wal_path, ec);
        if (ec) {
            std::error_code rm_ec;
            std::filesystem::remove(tmp_path, rm_ec);
            throw std::runtime_error(
                "WAL::rewrite: atomic rename failed: " + ec.message());
        }
    }

    stream_.close();
    stream_.open(path_, std::ios::binary | std::ios::app);
    if (!stream_.is_open()) {
        throw std::runtime_error(
            "WAL::rewrite: compaction succeeded but failed to reopen WAL "
            "stream at: " + path_);
    }
}

// =============================================================================
// replay_from — partial WAL replay starting at a byte offset
// =============================================================================

WAL::ReplayResult
WAL::replay_from(std::uint64_t offset,
                 std::function<void(const WalRecord&)> callback) const
{
    std::ifstream in(path_, std::ios::binary);
    if (!in.is_open()) {
        return ReplayResult{};
    }

    if (offset > 0) {
        in.seekg(0, std::ios::end);
        const std::uint64_t eof_pos = static_cast<std::uint64_t>(in.tellg());

        if (offset > eof_pos) {
            throw std::runtime_error(
                "WAL::replay_from: offset " + std::to_string(offset)
                + " is beyond end of file (" + std::to_string(eof_pos) + " bytes)");
        }

        if (offset == eof_pos) {
            return ReplayResult{};
        }

        in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        if (!in) {
            throw std::runtime_error(
                "WAL::replay_from: seek to offset "
                + std::to_string(offset) + " failed");
        }
    }

    ReplayResult result;

    while (true) {
        const std::streampos pos_before = in.tellg();

        in.peek();
        if (in.eof()) {
            break;
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
// file_size
// =============================================================================

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
