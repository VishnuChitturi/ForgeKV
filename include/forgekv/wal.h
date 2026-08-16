#pragma once

// =============================================================================
// ForgeKV — Stage 3: Write-Ahead Log (WAL)
// =============================================================================
//
// WAL is a simple append-only text log that records every mutating operation
// before it is applied to in-memory storage.
//
// "Write-ahead" means: the log entry is durably written FIRST.
// The in-memory mutation happens SECOND, only after the WAL write succeeds.
// If the WAL write fails, the in-memory state is not changed.
//
// Record format (one record per line):
//
//   SET|<key>|<value>     — insert or overwrite key
//   DEL|<key>             — remove key
//   CLEAR                 — remove all keys
//
// The '|' character is used as a delimiter.
//
// Format constraints (Stage 3 limitation):
//   Keys and values MUST NOT contain:
//     - '|'  (the field delimiter)
//     - '\n' (the record terminator)
//     - '\r' (carriage return — would corrupt line parsing)
//   These constraints are documented, not enforced at runtime in Stage 3.
//   Binary WAL with proper serialization is introduced in Stage 4.
//
// File handling:
//   The WAL opens the log file in append mode on construction. If the file
//   does not exist it is created. Existing content is preserved — the WAL
//   never truncates or overwrites the file.
//
//   Each record is flushed immediately after being written, so the OS buffer
//   is drained after every operation. Full fsync is not performed in Stage 3;
//   that durability guarantee is added in a later stage.
//
// Error handling:
//   - Construction throws std::runtime_error if the log file cannot be opened.
//   - append_set / append_del / append_clear throw std::runtime_error if the
//     write fails (stream bad-bit set after the write attempt).
//   - WAL is not responsible for in-memory state. Callers (KeyValueStore) must
//     ensure they do NOT modify in-memory state if a WAL write throws.
//
// What Stage 3 WAL does NOT do:
//   - No crash recovery / replay — that is Stage 5.
//   - No binary encoding or checksums — that is Stage 4.
//   - No compaction — that is Stage 8.
//   - No concurrency protection — that is Stage 7.
//   - No TTL, HTTP, snapshots, statistics — later stages.
//
// Thread safety: NOT thread-safe at this stage.
// =============================================================================

#include <fstream>
#include <string>

namespace forgekv {

class WAL {
public:
    // -------------------------------------------------------------------------
    // Construction
    // -------------------------------------------------------------------------

    // Open (or create) the WAL file at the given path in append mode.
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

    // Append a SET record:  "SET|<key>|<value>\n"
    // Throws std::runtime_error if the write fails.
    void append_set(const std::string& key, const std::string& value);

    // Append a DEL record:  "DEL|<key>\n"
    // Throws std::runtime_error if the write fails.
    void append_del(const std::string& key);

    // Append a CLEAR record: "CLEAR\n"
    // Throws std::runtime_error if the write fails.
    void append_clear();

    // -------------------------------------------------------------------------
    // Accessors
    // -------------------------------------------------------------------------

    // Return the path this WAL was opened with.
    [[nodiscard]] const std::string& path() const noexcept;

private:
    // -------------------------------------------------------------------------
    // Implementation helpers
    // -------------------------------------------------------------------------

    // Write a single pre-formatted record line to the log file and flush.
    // Throws std::runtime_error if the stream enters a fail/bad state.
    void write_record(const std::string& record);

    // -------------------------------------------------------------------------
    // State
    // -------------------------------------------------------------------------
    std::string   path_;    // path to the log file
    std::ofstream stream_;  // append-mode file stream; owned by this WAL
};

} // namespace forgekv
