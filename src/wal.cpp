// =============================================================================
// ForgeKV — Stage 3: WAL implementation
// =============================================================================
//
// See include/forgekv/wal.h for full design documentation.
//
// The WAL opens the log file in append mode (std::ios::app) on construction.
// Every append_* call formats a one-line record and writes it immediately,
// followed by an explicit flush so the record is in the OS buffer before
// control returns to the caller.
//
// Error detection:
//   After every write/flush we check stream_.fail(). If the stream has entered
//   a failure state we throw std::runtime_error. The caller (KeyValueStore) is
//   responsible for NOT applying the corresponding in-memory mutation when a
//   WAL write throws.
// =============================================================================

#include "forgekv/wal.h"

#include <stdexcept>
#include <string>

namespace forgekv {

// -----------------------------------------------------------------------------
// Construction
// -----------------------------------------------------------------------------

WAL::WAL(const std::string& path)
    : path_(path),
      stream_(path, std::ios::app)   // append mode — never truncates
{
    if (!stream_.is_open()) {
        throw std::runtime_error(
            "WAL: failed to open log file: " + path);
    }
}

// -----------------------------------------------------------------------------
// append_set
// -----------------------------------------------------------------------------
// Record format: "SET|<key>|<value>\n"
void WAL::append_set(const std::string& key, const std::string& value) {
    write_record("SET|" + key + "|" + value);
}

// -----------------------------------------------------------------------------
// append_del
// -----------------------------------------------------------------------------
// Record format: "DEL|<key>\n"
void WAL::append_del(const std::string& key) {
    write_record("DEL|" + key);
}

// -----------------------------------------------------------------------------
// append_clear
// -----------------------------------------------------------------------------
// Record format: "CLEAR\n"
void WAL::append_clear() {
    write_record("CLEAR");
}

// -----------------------------------------------------------------------------
// path
// -----------------------------------------------------------------------------
const std::string& WAL::path() const noexcept {
    return path_;
}

// -----------------------------------------------------------------------------
// write_record (private)
// -----------------------------------------------------------------------------
// Writes a single record line ("<record>\n") and flushes the stream.
// Throws std::runtime_error if the stream is in a fail/bad state after the
// write. This is the single choke-point for all WAL I/O error detection.
void WAL::write_record(const std::string& record) {
    stream_ << record << '\n';
    stream_.flush();

    if (stream_.fail()) {
        throw std::runtime_error(
            "WAL: write failed for record: " + record);
    }
}

} // namespace forgekv
