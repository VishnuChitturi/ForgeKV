# Stage 4 — Binary WAL + Checksums

## Overview

Stage 4 replaces the text-based Write-Ahead Log introduced in Stage 3 with a
structured **binary format** that includes a **CRC32 checksum** on every record.

The behavioral contract of the WAL is unchanged: every mutating operation
(`SET`, `DEL`, `CLEAR`) is logged before the in-memory state is updated.
What changes is how each record is represented on disk.

---

## Why Replace the Text WAL?

Stage 3 stored records as human-readable lines:

```
SET|name|Vishnu
SET|age|21
DEL|age
CLEAR
```

This format has several limitations that make it unsuitable for a production
storage engine:

| Problem | Impact |
|---|---|
| Uses `\|` as a delimiter | Keys and values cannot contain `\|` without escaping |
| Uses `\n` as a record terminator | Values cannot contain newlines |
| No explicit lengths | A reader must scan for delimiters to find field boundaries |
| No checksums | A single flipped bit is silently accepted as valid data |
| No record framing | Truncated records cannot be distinguished from valid short records |
| No version or magic | Any file looks like a valid WAL |

The binary format eliminates all of these problems. It stores field lengths
explicitly, covers every record with a CRC32 checksum, and begins each record
with a magic number and version byte that identify the format unambiguously.

---

## Binary Record Layout

Every WAL record — regardless of operation type — has the same fixed-size
header followed by a variable-length payload and a fixed-size checksum trailer.

```
Offset  Size  Type       Field
------  ----  ---------  --------------------------------------------------
     0     4  uint32_t   Magic number  (0x464B5741)
     4     1  uint8_t    Format version (0x01)
     5     1  uint8_t    Operation code (SET=0x01, DEL=0x02, CLEAR=0x03)
     6     4  uint32_t   Key length in bytes (little-endian)
    10     4  uint32_t   Value length in bytes (little-endian)
    14     K  bytes      Key bytes (no null terminator)
  14+K     V  bytes      Value bytes (no null terminator)
14+K+V     4  uint32_t   CRC32 checksum (little-endian)
```

Where `K = key_len` and `V = val_len`.

**Total record size = 18 + K + V bytes.**

The header is always 14 bytes. The checksum field is always 4 bytes.

---

## Field Sizes

| Field       | Size (bytes) | Type        | Notes                              |
|-------------|-------------|-------------|------------------------------------|
| magic       | 4           | uint32_t LE | Identifies a ForgeKV WAL record    |
| version     | 1           | uint8_t     | Format version; currently 0x01     |
| opcode      | 1           | uint8_t     | Operation type (SET/DEL/CLEAR)     |
| key_len     | 4           | uint32_t LE | Number of key bytes that follow    |
| val_len     | 4           | uint32_t LE | Number of value bytes that follow  |
| key bytes   | key_len     | raw bytes   | Key data; no null terminator       |
| value bytes | val_len     | raw bytes   | Value data; no null terminator     |
| checksum    | 4           | uint32_t LE | CRC32 of all preceding bytes       |

---

## Operation Codes

| Operation | Code  | key_len | val_len | Notes                    |
|-----------|-------|---------|---------|--------------------------|
| SET       | 0x01  | ≥ 0     | ≥ 0     | key and value are stored |
| DEL       | 0x02  | ≥ 0     | 0       | only key is stored       |
| CLEAR     | 0x03  | 0       | 0       | no payload               |

An empty key (`key_len = 0`) is valid for SET and DEL — ForgeKV supports
empty string keys. CLEAR always has both lengths set to zero.

---

## Magic Number

```
kWalMagic = 0x464B5741
```

Stored little-endian, the four bytes on disk are:

```
0x41  0x57  0x4B  0x46
 'A'   'W'   'K'   'F'
```

Reading them left-to-right as ASCII gives `AWKF`, which encodes `FKWA`
("ForgeKV WAL") as a 32-bit little-endian integer.

A reader that encounters any other 4-byte value at offset 0 of a record
immediately rejects it with an error. This distinguishes ForgeKV WAL files
from arbitrary binary files, older format versions, and random corruption.

---

## Format Version

```
kWalVersion = 0x01
```

The version byte is at offset 4 of every record. If Stage 4 needs to change
the binary layout in an incompatible way in a future revision, this byte is
incremented. A reader that encounters an unknown version rejects the record
rather than attempting to parse it with the wrong layout.

---

## Byte Order

All multi-byte integer fields — `magic`, `key_len`, `val_len`, and `checksum`
— are stored in **little-endian** byte order on disk.

The encoding is done explicitly via bit-shift and mask operations, never via
`reinterpret_cast` of a host integer. This means the on-disk format is
identical on x86, ARM, RISC-V, or any other architecture — a WAL file written
on a big-endian machine can be read on a little-endian machine and vice versa.

---

## Checksum Algorithm

**Algorithm:** CRC32 with the reflected polynomial `0xEDB88320`
(ISO 3309 / ITU-T V.42, same polynomial used by zlib, Ethernet, PKZIP).

**Implementation:** A 256-entry lookup table computed at program startup.
No external libraries are used.

**Bytes covered:** All bytes from the start of the record (magic at offset 0)
through the last byte of the payload (the byte at offset `14 + K + V - 1`).
The checksum field itself is **not** included in the calculation.

In pseudocode:

```
checksum = CRC32( record_bytes[0 .. 14 + K + V - 1] )
```

The checksum is then stored as a 4-byte little-endian integer immediately
after the payload.

---

## Corruption Detection

When `WAL::read_record()` is called, it:

1. Reads the 14-byte header.
2. Validates magic — throws if mismatch.
3. Validates version — throws if unknown.
4. Validates opcode — throws if unknown (not 0x01, 0x02, or 0x03).
5. Reads `key_len + val_len` payload bytes.
6. Reads the 4-byte checksum field.
7. Re-computes `CRC32(header + payload)`.
8. Compares computed vs. stored checksum — throws if mismatch.
9. Returns a `WalRecord` struct with the decoded opcode, key, and value.

Any single-byte flip in the header or payload changes the computed CRC32
and causes step 8 to fail. The probability of an undetected corruption
(two complementary errors that produce the same CRC32) is approximately
`1 / 2^32`.

Corruption detection errors:

| Condition                    | Error thrown                          |
|------------------------------|---------------------------------------|
| Magic mismatch               | `"WAL: invalid magic number: ..."`    |
| Unknown version              | `"WAL: unknown format version: ..."`  |
| Unknown opcode               | `"WAL: unknown opcode: ..."`          |
| CRC32 mismatch               | `"WAL: checksum mismatch — record is corrupted"` |

---

## Truncated Record Detection

If a process crashes or a disk write is interrupted mid-record, the WAL file
will contain an incomplete record at the end. The binary format detects this
explicitly.

`WAL::read_record()` uses a helper that reads exactly `n` bytes and checks
that `istream::gcount() == n`. If the stream reaches EOF before `n` bytes
are available, the helper throws:

```
"WAL: truncated record (header)"
"WAL: truncated record (payload)"
"WAL: truncated record (checksum)"
```

The message identifies which section of the record was incomplete when the
stream ran out of data.

Stage 4 **detects** truncation and reports it. Stage 5 (crash recovery) will
decide how to react — typically by discarding the incomplete tail record and
replaying only the complete records that precede it.

---

## Operation-Specific Payload Details

### SET

```
key_len  = len(key)    — number of bytes in the key string
val_len  = len(value)  — number of bytes in the value string
payload  = key_bytes + value_bytes
```

No delimiter separates key bytes from value bytes in the payload. The reader
uses `key_len` to know exactly where the key ends and the value begins.

### DEL

```
key_len  = len(key)
val_len  = 0
payload  = key_bytes
```

The value length field is always zero. No value bytes appear in the payload.

### CLEAR

```
key_len  = 0
val_len  = 0
payload  = (empty)
```

The total record size for CLEAR is always 18 bytes (14 header + 0 payload + 4
checksum).

---

## Special Characters

Because keys and values are stored as raw bytes with explicit lengths, there
are **no forbidden characters**. The following characters, which were
problematic in the Stage 3 text format, are fully supported:

- `|`  (was the field delimiter in Stage 3)
- `\n` (was the record terminator in Stage 3)
- `\r`
- Space
- Tab
- Null byte (`\0`)
- Backslash
- Any other byte value

---

## WAL Write Ordering

The Stage 3 write-ahead ordering is preserved unchanged in Stage 4:

```
KeyValueStore::set()
        |
        v
WAL::append_set()   ← binary record written and flushed
        |
        v
Storage::set()      ← in-memory update

KeyValueStore::del()
        |
        v
WAL::append_del()   ← only if key exists
        |
        v
Storage::del()

KeyValueStore::clear()
        |
        v
WAL::append_clear()
        |
        v
Storage::clear()
```

If the WAL write throws, the exception propagates to the caller and the
in-memory state is **not** changed.

Read operations (`get`, `exists`, `size`, `empty`) do not write any WAL records.

`del()` only writes a WAL record when the key actually exists. If the key is
absent, `del()` returns `false` immediately without touching the WAL.

---

## Append-Only Behavior

The WAL opens the log file with `std::ios::binary | std::ios::app`. This
guarantees:

- If the file does not exist, it is created.
- If the file already exists, new records are appended after any existing content.
- The file is **never truncated** on open.

---

## Flushing

After every `append_set`, `append_del`, or `append_clear` call, the
implementation calls `stream_.flush()`. This drains the C++ standard library
buffer so the bytes are handed to the OS kernel before the call returns.

Full `fsync`-level durability (guaranteeing bytes are on physical media) is not
performed in Stage 4. That is a later concern.

---

## Why Stage 4 Does NOT Perform Recovery

Stage 4 creates a WAL format that is **robust enough to be replayed**. It does
not perform the replay.

The design decision is deliberate: WAL writing and WAL replaying are separate
concerns. Stage 4 proves that the format can be written and validated correctly.
Stage 5 will add the startup logic that opens the WAL, iterates through every
record using `WAL::read_record()`, and applies each record to the in-memory
state to reconstruct the database.

Separating the stages makes each one smaller and easier to verify independently.

Specifically, Stage 4 does NOT:

- Call `WAL::read_record()` on startup
- Replay SET/DEL/CLEAR records into Storage
- Implement a recovery manager
- Reconstruct the in-memory state from the WAL file
- Truncate a corrupted tail record during startup

All of the above are Stage 5.

---

## API

The public WAL interface is unchanged from Stage 3:

```cpp
class WAL {
public:
    explicit WAL(const std::string& path);   // opens/creates in binary append mode

    void append_set(const std::string& key, const std::string& value);
    void append_del(const std::string& key);
    void append_clear();

    [[nodiscard]] const std::string& path() const noexcept;

    // New in Stage 4: deserialise + validate one record from any input stream.
    static WalRecord read_record(std::istream& in);
};
```

`read_record()` is the new addition. It is static so that any binary input
stream (including a `std::istringstream` for unit-testing) can be passed in.
Stage 5 will open the WAL file as a binary input stream and call this in a loop.

---

## Example: What a CLEAR Record Looks Like On Disk

```
Offset  Byte    Meaning
------  ------  ----------------------------------
     0  0x41    Magic byte 0 ('A')
     1  0x57    Magic byte 1 ('W')
     2  0x4B    Magic byte 2 ('K')
     3  0x46    Magic byte 3 ('F')
     4  0x01    Version = 1
     5  0x03    Opcode = CLEAR
     6  0x00    key_len byte 0 = 0
     7  0x00    key_len byte 1 = 0
     8  0x00    key_len byte 2 = 0
     9  0x00    key_len byte 3 = 0
    10  0x00    val_len byte 0 = 0
    11  0x00    val_len byte 1 = 0
    12  0x00    val_len byte 2 = 0
    13  0x00    val_len byte 3 = 0
    14  ??      checksum byte 0 (CRC32 of bytes 0..13, little-endian)
    15  ??      checksum byte 1
    16  ??      checksum byte 2
    17  ??      checksum byte 3
```

Total size: 18 bytes.

---

## Relationship to Other Stages

| Stage | What it does with the WAL |
|-------|---------------------------|
| 3     | Writes text records; no checksums |
| **4** | **Writes binary records; CRC32 checksums; corruption/truncation detection** |
| 5     | Replays the Stage 4 binary WAL on startup to reconstruct in-memory state |
| 8     | Compacts the WAL to reclaim disk space |
| 9     | Adds snapshots so recovery only needs to replay a suffix of the WAL |
