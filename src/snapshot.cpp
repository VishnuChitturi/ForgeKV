// =============================================================================
// ForgeKV — Stage 10: SnapshotManager implementation (v2 format)
// =============================================================================
//
// Stage 10 extends the snapshot format to v2 which stores per-record expiry.
//
// v1 RECORD LAYOUT (Stage 9):
//   [key_len(4)] [key] [val_len(4)] [val]
//
// v2 RECORD LAYOUT (Stage 10):
//   [key_len(4)] [key] [val_len(4)] [val]
//   [has_expiry(1)]
//   [expires_at_us(8)] ← only present when has_expiry == 0x01
//
// BACKWARD COMPATIBILITY:
//   - save() always writes v2 (kSnapshotVersionV2 = 0x02).
//   - load() handles both v1 and v2:
//       v1 → all entries are permanent (expires_at_us = 0).
//       v2 → entries carry has_expiry + optional expires_at_us.
//
// EXPIRED ENTRIES:
//   - save() must receive only live (non-expired) entries from the caller.
//     Expired entries must be filtered by the caller before calling save().
//   - load() checks if a recovered expiry is already in the past at load time
//     and excludes those entries from the result (they are considered as if
//     they were never written), so they will not be restored to live storage.
//
// HEADER (17 bytes, same as v1):
//   [magic(4)] [version(1)] [wal_offset(8)] [count(4)]
//
// CHECKSUM:
//   CRC32 over ALL bytes from offset 0 through the last record byte.
// =============================================================================

#include "forgekv/snapshot.h"

#include <array>
#include <chrono>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace forgekv {

// =============================================================================
// CRC32
// =============================================================================

namespace {

const std::array<std::uint32_t, 256>& snapshot_crc32_table() {
    static const auto table = []() {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256u; ++i) {
            std::uint32_t c = i;
            for (int bit = 0; bit < 8; ++bit) {
                if (c & 1u) {
                    c = (c >> 1) ^ 0xEDB88320u;
                } else {
                    c >>= 1;
                }
            }
            t[i] = c;
        }
        return t;
    }();
    return table;
}

// Return current time in microseconds since Unix epoch (for expiry checks at
// load time).
std::uint64_t current_time_us() noexcept {
    const auto now = std::chrono::system_clock::now();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            now.time_since_epoch())
            .count());
}

} // anonymous namespace

// =============================================================================
// Construction
// =============================================================================

SnapshotManager::SnapshotManager(const std::string& wal_path)
    : wal_path_(wal_path),
      snapshot_path_(wal_path + ".snapshot")
{}

// =============================================================================
// Encoding helpers
// =============================================================================

void SnapshotManager::encode_u8(std::vector<std::uint8_t>& buf, std::uint8_t v)
{
    buf.push_back(v);
}

void SnapshotManager::encode_u32(std::vector<std::uint8_t>& buf, std::uint32_t v)
{
    buf.push_back(static_cast<std::uint8_t>( v        & 0xFFu));
    buf.push_back(static_cast<std::uint8_t>((v >>  8) & 0xFFu));
    buf.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFFu));
    buf.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFFu));
}

void SnapshotManager::encode_u64(std::vector<std::uint8_t>& buf, std::uint64_t v)
{
    buf.push_back(static_cast<std::uint8_t>( v        & 0xFFu));
    buf.push_back(static_cast<std::uint8_t>((v >>  8) & 0xFFu));
    buf.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFFu));
    buf.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFFu));
    buf.push_back(static_cast<std::uint8_t>((v >> 32) & 0xFFu));
    buf.push_back(static_cast<std::uint8_t>((v >> 40) & 0xFFu));
    buf.push_back(static_cast<std::uint8_t>((v >> 48) & 0xFFu));
    buf.push_back(static_cast<std::uint8_t>((v >> 56) & 0xFFu));
}

std::uint32_t SnapshotManager::decode_u32(const std::vector<std::uint8_t>& buf,
                                           std::size_t offset)
{
    return  static_cast<std::uint32_t>(buf[offset])
         | (static_cast<std::uint32_t>(buf[offset + 1]) <<  8)
         | (static_cast<std::uint32_t>(buf[offset + 2]) << 16)
         | (static_cast<std::uint32_t>(buf[offset + 3]) << 24);
}

std::uint64_t SnapshotManager::decode_u64(const std::vector<std::uint8_t>& buf,
                                           std::size_t offset)
{
    return  static_cast<std::uint64_t>(buf[offset])
         | (static_cast<std::uint64_t>(buf[offset + 1]) <<  8)
         | (static_cast<std::uint64_t>(buf[offset + 2]) << 16)
         | (static_cast<std::uint64_t>(buf[offset + 3]) << 24)
         | (static_cast<std::uint64_t>(buf[offset + 4]) << 32)
         | (static_cast<std::uint64_t>(buf[offset + 5]) << 40)
         | (static_cast<std::uint64_t>(buf[offset + 6]) << 48)
         | (static_cast<std::uint64_t>(buf[offset + 7]) << 56);
}

// =============================================================================
// CRC32
// =============================================================================

std::uint32_t SnapshotManager::crc32(const std::uint8_t* data, std::size_t len)
{
    const auto& table = snapshot_crc32_table();
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < len; ++i) {
        crc = (crc >> 8) ^ table[(crc ^ data[i]) & 0xFFu];
    }
    return crc ^ 0xFFFFFFFFu;
}

// =============================================================================
// save — write snapshot v2 atomically
// =============================================================================
//
// v2 per-record format:
//   [key_len(4)] [key] [val_len(4)] [val] [has_expiry(1)] [expires_at_us(8)?]

void SnapshotManager::save(
    std::uint64_t wal_offset,
    const std::vector<std::pair<std::string, StoreEntry>>& records)
{
    if (records.size() > static_cast<std::size_t>(
            std::numeric_limits<std::uint32_t>::max())) {
        throw std::runtime_error("SnapshotManager::save: too many records");
    }
    const auto record_count = static_cast<std::uint32_t>(records.size());

    std::vector<std::uint8_t> buf;
    buf.reserve(kSnapshotHeaderSize + records.size() * 24);

    // Header (same as v1 except version byte = 0x02).
    encode_u32(buf, kSnapshotMagic);         // magic    (4)
    encode_u8 (buf, kSnapshotVersionV2);      // version  (1) ← v2
    encode_u64(buf, wal_offset);              // wal offs (8)
    encode_u32(buf, record_count);            // count    (4)

    // Records.
    for (const auto& [key, entry] : records) {
        if (key.size() > static_cast<std::size_t>(
                std::numeric_limits<std::uint32_t>::max())) {
            throw std::runtime_error(
                "SnapshotManager::save: key too large: " + key);
        }
        if (entry.value.size() > static_cast<std::size_t>(
                std::numeric_limits<std::uint32_t>::max())) {
            throw std::runtime_error(
                "SnapshotManager::save: value too large for key: " + key);
        }

        const auto key_len = static_cast<std::uint32_t>(key.size());
        const auto val_len = static_cast<std::uint32_t>(entry.value.size());

        encode_u32(buf, key_len);
        for (const char c : key) { buf.push_back(static_cast<std::uint8_t>(c)); }
        encode_u32(buf, val_len);
        for (const char c : entry.value) { buf.push_back(static_cast<std::uint8_t>(c)); }

        // has_expiry flag (1 byte) + expires_at_us (8 bytes, only if flag == 1).
        if (entry.has_expiry()) {
            encode_u8 (buf, 0x01u);
            encode_u64(buf, entry.expires_at_us);
        } else {
            encode_u8 (buf, 0x00u);
            // No expires_at_us bytes for permanent entries.
        }
    }

    // CRC32 over everything before this field.
    const std::uint32_t checksum = crc32(buf.data(), buf.size());
    encode_u32(buf, checksum);

    // Write to temp file, then atomic rename.
    const std::filesystem::path snap_path(snapshot_path_);
    const std::filesystem::path dir = snap_path.parent_path().empty()
                                      ? std::filesystem::path(".")
                                      : snap_path.parent_path();
    const std::string tmp_str = (dir / (snap_path.filename().string() + ".tmp")).string();

    {
        std::ofstream tmp(tmp_str, std::ios::binary | std::ios::trunc);
        if (!tmp.is_open()) {
            throw std::runtime_error(
                "SnapshotManager::save: failed to create temp file: " + tmp_str);
        }

        tmp.write(reinterpret_cast<const char*>(buf.data()),
                  static_cast<std::streamsize>(buf.size()));
        tmp.flush();

        if (tmp.fail()) {
            tmp.close();
            std::error_code ec;
            std::filesystem::remove(tmp_str, ec);
            throw std::runtime_error(
                "SnapshotManager::save: write failed to temp file: " + tmp_str);
        }
    }

    {
        std::error_code ec;
        std::filesystem::rename(tmp_str, snap_path, ec);
        if (ec) {
            std::error_code rm_ec;
            std::filesystem::remove(tmp_str, rm_ec);
            throw std::runtime_error(
                "SnapshotManager::save: rename failed: " + ec.message());
        }
    }
}

// =============================================================================
// load — read and validate the snapshot file
// =============================================================================
//
// Supports both v1 (0x01) and v2 (0x02).
// v1: records have no expiry data; all entries are permanent.
// v2: each record has a has_expiry byte + optional expires_at_us.
//
// Expired entries (expires_at_us <= now) are excluded from the result data
// so they do not get restored to live storage.

SnapshotManager::SnapshotLoadResult
SnapshotManager::load() const noexcept
{
    SnapshotLoadResult result;

    std::error_code ec;
    const bool file_exists = std::filesystem::exists(snapshot_path_, ec);
    if (ec || !file_exists) {
        return result;
    }
    result.exists = true;

    try {
        std::ifstream in(snapshot_path_, std::ios::binary);
        if (!in.is_open()) {
            result.corrupt   = true;
            result.error_msg = "SnapshotManager::load: cannot open file: "
                               + snapshot_path_;
            return result;
        }

        std::vector<std::uint8_t> raw(
            (std::istreambuf_iterator<char>(in)),
            std::istreambuf_iterator<char>());
        in.close();

        const std::size_t minimum_size = kSnapshotHeaderSize + kSnapshotChecksumSize;
        if (raw.size() < minimum_size) {
            result.corrupt   = true;
            result.error_msg = "SnapshotManager::load: file too small ("
                               + std::to_string(raw.size()) + " bytes)";
            return result;
        }

        // Validate magic.
        const std::uint32_t magic = decode_u32(raw, 0);
        if (magic != kSnapshotMagic) {
            result.corrupt = true;
            char hex[9];
            std::snprintf(hex, sizeof(hex), "%08X", magic);
            result.error_msg = "SnapshotManager::load: invalid magic (got 0x"
                               + std::string(hex) + ")";
            return result;
        }

        // Validate version (accept v1 and v2).
        const std::uint8_t version = raw[4];
        if (version != kSnapshotVersion && version != kSnapshotVersionV2) {
            result.corrupt   = true;
            result.error_msg = "SnapshotManager::load: unknown version: "
                               + std::to_string(static_cast<int>(version));
            return result;
        }

        const bool is_v2 = (version == kSnapshotVersionV2);

        const std::uint64_t wal_offset   = decode_u64(raw, 5);
        const std::uint32_t record_count = decode_u32(raw, 13);

        // Verify CRC32 (covers everything except last 4 bytes).
        const std::size_t checksum_offset = raw.size() - kSnapshotChecksumSize;
        const std::uint32_t stored_crc    = decode_u32(raw, checksum_offset);
        const std::uint32_t computed_crc  = crc32(raw.data(), checksum_offset);

        if (computed_crc != stored_crc) {
            result.corrupt   = true;
            result.error_msg = "SnapshotManager::load: CRC32 mismatch — snapshot is corrupted";
            return result;
        }

        // Decode records.
        const std::uint64_t now = current_time_us();
        std::size_t pos = kSnapshotHeaderSize;
        std::vector<std::pair<std::string, StoreEntry>> kv_records;
        kv_records.reserve(record_count);

        for (std::uint32_t i = 0; i < record_count; ++i) {
            // key_len
            if (pos + 4 > checksum_offset) {
                result.corrupt   = true;
                result.error_msg = "SnapshotManager::load: truncated at record "
                                   + std::to_string(i) + " key_len";
                return result;
            }
            const std::uint32_t key_len = decode_u32(raw, pos);
            pos += 4;

            // key
            if (pos + key_len > checksum_offset) {
                result.corrupt   = true;
                result.error_msg = "SnapshotManager::load: truncated at record "
                                   + std::to_string(i) + " key data";
                return result;
            }
            std::string key(reinterpret_cast<const char*>(raw.data() + pos), key_len);
            pos += key_len;

            // val_len
            if (pos + 4 > checksum_offset) {
                result.corrupt   = true;
                result.error_msg = "SnapshotManager::load: truncated at record "
                                   + std::to_string(i) + " val_len";
                return result;
            }
            const std::uint32_t val_len = decode_u32(raw, pos);
            pos += 4;

            // val
            if (pos + val_len > checksum_offset) {
                result.corrupt   = true;
                result.error_msg = "SnapshotManager::load: truncated at record "
                                   + std::to_string(i) + " val data";
                return result;
            }
            std::string value(reinterpret_cast<const char*>(raw.data() + pos), val_len);
            pos += val_len;

            // Expiry (v2 only).
            std::uint64_t expires_at_us = 0;
            if (is_v2) {
                // has_expiry flag (1 byte)
                if (pos + 1 > checksum_offset) {
                    result.corrupt   = true;
                    result.error_msg = "SnapshotManager::load: truncated at record "
                                       + std::to_string(i) + " has_expiry";
                    return result;
                }
                const std::uint8_t has_expiry = raw[pos];
                pos += 1;

                if (has_expiry == 0x01u) {
                    // expires_at_us (8 bytes)
                    if (pos + 8 > checksum_offset) {
                        result.corrupt   = true;
                        result.error_msg = "SnapshotManager::load: truncated at record "
                                           + std::to_string(i) + " expires_at_us";
                        return result;
                    }
                    expires_at_us = decode_u64(raw, pos);
                    pos += 8;
                } else if (has_expiry != 0x00u) {
                    result.corrupt   = true;
                    result.error_msg = "SnapshotManager::load: invalid has_expiry byte at record "
                                       + std::to_string(i);
                    return result;
                }
            }

            // Skip already-expired entries during load.
            // An already-expired entry must NOT be restored to live storage.
            if (expires_at_us != 0 && expires_at_us <= now) {
                continue; // drop this entry — it is expired
            }

            kv_records.emplace_back(std::move(key),
                                    StoreEntry(std::move(value), expires_at_us));
        }

        // pos must now be at checksum_offset (no stray bytes).
        if (pos != checksum_offset) {
            result.corrupt   = true;
            result.error_msg = "SnapshotManager::load: extra bytes before checksum";
            return result;
        }

        result.data.wal_offset = wal_offset;
        result.data.records    = std::move(kv_records);

    } catch (const std::exception& e) {
        result.corrupt   = true;
        result.error_msg = std::string("SnapshotManager::load: exception: ") + e.what();
    } catch (...) {
        result.corrupt   = true;
        result.error_msg = "SnapshotManager::load: unknown exception";
    }

    return result;
}

// =============================================================================
// remove
// =============================================================================

bool SnapshotManager::remove() noexcept
{
    std::error_code ec;
    std::filesystem::remove(snapshot_path_, ec);
    return !ec;
}

// =============================================================================
// Accessors
// =============================================================================

const std::string& SnapshotManager::snapshot_path() const noexcept
{
    return snapshot_path_;
}

bool SnapshotManager::exists() const noexcept
{
    std::error_code ec;
    return std::filesystem::exists(snapshot_path_, ec) && !ec;
}

} // namespace forgekv
