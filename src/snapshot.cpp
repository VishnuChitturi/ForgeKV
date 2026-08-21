// =============================================================================
// ForgeKV — Stage 9: SnapshotManager implementation
// =============================================================================
//
// See include/forgekv/snapshot.h for the full design, binary format, and API
// documentation.
//
// BINARY LAYOUT (recap)
// ---------------------
//
//  Offset  Size  Type       Field
//  ------  ----  ---------  -----------------------------------------------
//       0     4  uint32_t   Magic  (0x464B534E, little-endian)
//       4     1  uint8_t    Version (0x01)
//       5     8  uint64_t   WAL byte offset (little-endian)
//      13     4  uint32_t   Record count (little-endian)
//      17     …  records    [key_len(4) + key + val_len(4) + val] × count
//       ?     4  uint32_t   CRC32 over all preceding bytes (little-endian)
//
// ENCODING
// ---------
// All multi-byte integers are explicitly serialised little-endian using
// encode_u32 / encode_u64 helpers (same approach as WAL::encode_u32).
//
// CHECKSUM
// ---------
// CRC32 with reflected polynomial 0xEDB88320 — same algorithm as the WAL.
// The checksum covers every byte from offset 0 through the last payload byte.
// The checksum field itself is excluded from the computation.
//
// ATOMIC WRITE
// -------------
// save() writes to <snapshot_path_>.tmp in the same directory, then renames.
// The rename is atomic on POSIX.  If anything fails before rename, the temp
// file is cleaned up and the old snapshot (if any) is preserved.
//
// =============================================================================

#include "forgekv/snapshot.h"

#include <array>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace forgekv {

// =============================================================================
// CRC32 lookup table (same polynomial as WAL — ISO 3309 / 0xEDB88320)
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

void SnapshotManager::encode_u8(std::vector<std::uint8_t>& buf,
                                 std::uint8_t v)
{
    buf.push_back(v);
}

void SnapshotManager::encode_u32(std::vector<std::uint8_t>& buf,
                                  std::uint32_t v)
{
    buf.push_back(static_cast<std::uint8_t>( v        & 0xFFu));
    buf.push_back(static_cast<std::uint8_t>((v >>  8) & 0xFFu));
    buf.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFFu));
    buf.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFFu));
}

void SnapshotManager::encode_u64(std::vector<std::uint8_t>& buf,
                                  std::uint64_t v)
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

std::uint32_t SnapshotManager::crc32(const std::uint8_t* data,
                                      std::size_t len)
{
    const auto& table = snapshot_crc32_table();
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < len; ++i) {
        crc = (crc >> 8) ^ table[(crc ^ data[i]) & 0xFFu];
    }
    return crc ^ 0xFFFFFFFFu;
}

// =============================================================================
// save — write snapshot atomically
// =============================================================================
//
// Serialisation strategy:
//
//   Build the entire snapshot payload into a single in-memory buffer, compute
//   the CRC32, append the checksum, then write the buffer to a temp file in
//   one call.  This minimises partial-write risk before the rename.
//
// The buffer layout mirrors the documented format exactly:
//
//   [magic 4B][version 1B][wal_offset 8B][count 4B]
//   [[key_len 4B][key ...][val_len 4B][val ...]] × count
//   [crc32 4B]

void SnapshotManager::save(
    std::uint64_t wal_offset,
    const std::vector<std::pair<std::string, std::string>>& records)
{
    // Guard against overflow in record count.
    if (records.size() > static_cast<std::size_t>(
            std::numeric_limits<std::uint32_t>::max())) {
        throw std::runtime_error("SnapshotManager::save: too many records");
    }
    const auto record_count = static_cast<std::uint32_t>(records.size());

    // -------------------------------------------------------------------------
    // 1. Build the payload buffer (everything the CRC covers).
    // -------------------------------------------------------------------------
    std::vector<std::uint8_t> buf;

    // Reserve an approximate size to avoid many reallocations.
    // Header = 17 bytes; each record ~= 8 + key.size() + val.size()
    buf.reserve(kSnapshotHeaderSize + records.size() * 16);

    // Header
    encode_u32(buf, kSnapshotMagic);          // magic (4 bytes)
    encode_u8 (buf, kSnapshotVersion);         // version (1 byte)
    encode_u64(buf, wal_offset);               // wal offset (8 bytes)
    encode_u32(buf, record_count);             // record count (4 bytes)

    // Records
    for (const auto& [key, value] : records) {
        if (key.size() > static_cast<std::size_t>(
                std::numeric_limits<std::uint32_t>::max())) {
            throw std::runtime_error(
                "SnapshotManager::save: key too large: " + key);
        }
        if (value.size() > static_cast<std::size_t>(
                std::numeric_limits<std::uint32_t>::max())) {
            throw std::runtime_error(
                "SnapshotManager::save: value too large for key: " + key);
        }

        const auto key_len = static_cast<std::uint32_t>(key.size());
        const auto val_len = static_cast<std::uint32_t>(value.size());

        encode_u32(buf, key_len);
        for (const char c : key) {
            buf.push_back(static_cast<std::uint8_t>(c));
        }
        encode_u32(buf, val_len);
        for (const char c : value) {
            buf.push_back(static_cast<std::uint8_t>(c));
        }
    }

    // -------------------------------------------------------------------------
    // 2. Compute CRC32 over the entire buffer (header + records).
    // -------------------------------------------------------------------------
    const std::uint32_t checksum = crc32(buf.data(), buf.size());
    encode_u32(buf, checksum);  // append checksum (4 bytes)

    // -------------------------------------------------------------------------
    // 3. Write to a temporary file in the same directory as the snapshot.
    // -------------------------------------------------------------------------
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
        // tmp closes here via destructor — flushes OS buffer.
    }

    // -------------------------------------------------------------------------
    // 4. Atomically rename temp → final snapshot path.
    // -------------------------------------------------------------------------
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
// Returns a SnapshotLoadResult:
//   - !exists              → no file on disk
//   - exists && !corrupt   → valid snapshot; data populated
//   - exists && corrupt    → file present but invalid; data empty; error set
//
// All errors are captured in the result — this function never throws.

SnapshotManager::SnapshotLoadResult
SnapshotManager::load() const noexcept
{
    SnapshotLoadResult result;

    // Check whether the file exists at all.
    std::error_code ec;
    const bool file_exists =
        std::filesystem::exists(snapshot_path_, ec);
    if (ec || !file_exists) {
        return result; // exists == false, corrupt == false
    }
    result.exists = true;

    // Try to read and validate the file.
    try {
        // Read entire file into memory.
        std::ifstream in(snapshot_path_, std::ios::binary);
        if (!in.is_open()) {
            result.corrupt = true;
            result.error_msg = "SnapshotManager::load: cannot open file: "
                               + snapshot_path_;
            return result;
        }

        std::vector<std::uint8_t> raw(
            (std::istreambuf_iterator<char>(in)),
            std::istreambuf_iterator<char>());
        in.close();

        // Must have at least header (17 bytes) + checksum (4 bytes).
        const std::size_t minimum_size = kSnapshotHeaderSize + kSnapshotChecksumSize;
        if (raw.size() < minimum_size) {
            result.corrupt = true;
            result.error_msg =
                "SnapshotManager::load: file too small ("
                + std::to_string(raw.size()) + " bytes, need at least "
                + std::to_string(minimum_size) + ")";
            return result;
        }

        // ----------------------------------------------------------------
        // Verify magic.
        // ----------------------------------------------------------------
        const std::uint32_t magic = decode_u32(raw, 0);
        if (magic != kSnapshotMagic) {
            result.corrupt = true;
            char hex[9];
            std::snprintf(hex, sizeof(hex), "%08X", magic);
            result.error_msg =
                "SnapshotManager::load: invalid magic (got 0x"
                + std::string(hex) + ", expected 0x464B534E)";
            return result;
        }

        // ----------------------------------------------------------------
        // Verify version.
        // ----------------------------------------------------------------
        const std::uint8_t version = raw[4];
        if (version != kSnapshotVersion) {
            result.corrupt = true;
            result.error_msg =
                "SnapshotManager::load: unknown version: "
                + std::to_string(static_cast<int>(version));
            return result;
        }

        // ----------------------------------------------------------------
        // Decode wal_offset and record_count from header.
        // ----------------------------------------------------------------
        const std::uint64_t wal_offset   = decode_u64(raw, 5);
        const std::uint32_t record_count = decode_u32(raw, 13);

        // ----------------------------------------------------------------
        // Verify CRC32.
        // The checksum covers raw[0 .. raw.size()-5] (all but last 4 bytes).
        // ----------------------------------------------------------------
        const std::size_t checksum_offset = raw.size() - kSnapshotChecksumSize;
        const std::uint32_t stored_crc    = decode_u32(raw, checksum_offset);
        const std::uint32_t computed_crc  = crc32(raw.data(), checksum_offset);

        if (computed_crc != stored_crc) {
            result.corrupt = true;
            result.error_msg =
                "SnapshotManager::load: CRC32 mismatch — snapshot is corrupted";
            return result;
        }

        // ----------------------------------------------------------------
        // Decode key-value records.
        // ----------------------------------------------------------------
        std::size_t pos = kSnapshotHeaderSize; // start of records
        std::vector<std::pair<std::string, std::string>> kv_records;
        kv_records.reserve(record_count);

        for (std::uint32_t i = 0; i < record_count; ++i) {
            // Need at least 4 bytes for key_len.
            if (pos + 4 > checksum_offset) {
                result.corrupt = true;
                result.error_msg =
                    "SnapshotManager::load: truncated at record "
                    + std::to_string(i) + " key_len";
                return result;
            }
            const std::uint32_t key_len = decode_u32(raw, pos);
            pos += 4;

            // Need key_len bytes for key.
            if (pos + key_len > checksum_offset) {
                result.corrupt = true;
                result.error_msg =
                    "SnapshotManager::load: truncated at record "
                    + std::to_string(i) + " key data";
                return result;
            }
            std::string key(reinterpret_cast<const char*>(raw.data() + pos),
                            key_len);
            pos += key_len;

            // Need 4 bytes for val_len.
            if (pos + 4 > checksum_offset) {
                result.corrupt = true;
                result.error_msg =
                    "SnapshotManager::load: truncated at record "
                    + std::to_string(i) + " val_len";
                return result;
            }
            const std::uint32_t val_len = decode_u32(raw, pos);
            pos += 4;

            // Need val_len bytes for value.
            if (pos + val_len > checksum_offset) {
                result.corrupt = true;
                result.error_msg =
                    "SnapshotManager::load: truncated at record "
                    + std::to_string(i) + " val data";
                return result;
            }
            std::string value(reinterpret_cast<const char*>(raw.data() + pos),
                              val_len);
            pos += val_len;

            kv_records.emplace_back(std::move(key), std::move(value));
        }

        // pos must now point exactly to the checksum field (no stray bytes).
        if (pos != checksum_offset) {
            result.corrupt = true;
            result.error_msg =
                "SnapshotManager::load: extra bytes before checksum";
            return result;
        }

        // ----------------------------------------------------------------
        // Success.
        // ----------------------------------------------------------------
        result.data.wal_offset = wal_offset;
        result.data.records    = std::move(kv_records);

    } catch (const std::exception& e) {
        result.corrupt   = true;
        result.error_msg = std::string("SnapshotManager::load: exception: ")
                           + e.what();
    } catch (...) {
        result.corrupt   = true;
        result.error_msg = "SnapshotManager::load: unknown exception";
    }

    return result;
}

// =============================================================================
// remove — delete the snapshot file
// =============================================================================

bool SnapshotManager::remove() noexcept
{
    std::error_code ec;
    // remove() returns true if the file was deleted, false if it did not exist.
    // std::filesystem::remove() sets ec on other errors.
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
