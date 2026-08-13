#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "Database.h"
#include "Entry.h"

namespace vcache {

// On-disk snapshot format, version 1. Little-endian throughout, so a file
// written on one machine reads correctly on another.
//
//   header    "VCACHE"           6 bytes
//             formatVersion      uint16
//             entryCount         uint64
//   entry     keyLength          uint32
//             key                keyLength bytes
//             valueLength        uint32
//             value              valueLength bytes
//             expiresAtMillis    int64   (0 = never expires)
//   footer    checksum           uint64  (FNV-1a over everything above)
//
// BINARY, not text. Keys and values are binary-safe -- they may contain NULs,
// newlines, any byte at all -- so a text format would need an escaping layer
// with its own bugs, and would then need parsing back. Length-prefixed binary
// sidesteps the question entirely: the length says exactly how many bytes to
// read, and no byte is special. The Discovery Document suggests starting with
// text and moving to binary later; skipping the text step avoids building an
// escaping scheme only to delete it.
//
// EXPIRY IS STORED AS AN ABSOLUTE WALL-CLOCK TIME, in milliseconds since the
// Unix epoch, even though the running database tracks expiry on a steady_clock.
// A steady_clock value is measured from an arbitrary boot-relative origin and
// is meaningless in a file. Storing the remaining duration instead would be
// worse than useless: a server down for an hour would resurrect every
// 60-second key with a fresh 60 seconds. An absolute time means keys that
// lapsed while the process was gone are correctly dropped on load. The
// conversion between clocks happens here, so nothing else has to know.
namespace snapshot {

constexpr char kMagic[] = {'V', 'C', 'A', 'C', 'H', 'E'};
constexpr std::size_t kMagicLength = sizeof(kMagic);
constexpr std::uint16_t kFormatVersion = 1;

// Refuses absurd length fields early. A corrupt file claiming a 4 GB key would
// otherwise cause a huge allocation before anything noticed.
constexpr std::uint32_t kMaxFieldLength = 512u * 1024u * 1024u;

// Serialises entries into the snapshot format. Expired entries are skipped.
std::string encode(const std::vector<Entry>& entries);

// Parses a snapshot. Returns false and fills `error` on any malformed input:
// wrong magic, unknown version, truncated data, impossible lengths, or a
// checksum mismatch. Entries already expired are dropped rather than loaded.
bool decode(const std::string& data, std::vector<Entry>& entries, std::string& error);

}  // namespace snapshot

struct SaveOutcome {
    bool ok = false;
    std::size_t entriesWritten = 0;
    std::size_t bytesWritten = 0;
    std::string error;
};

enum class LoadStatus {
    Loaded,
    NotFound,  // no snapshot yet -- a normal first run, not a failure
    Failed,    // the file exists but could not be read or trusted
};

struct LoadOutcome {
    LoadStatus status = LoadStatus::NotFound;
    std::size_t entriesLoaded = 0;
    std::string error;
};

// Owns a snapshot file and the rules for reading and writing it safely.
//
// Writes go to a temporary file which is flushed and then renamed over the
// target. rename() is atomic on POSIX, so a crash leaves either the complete
// old snapshot or the complete new one -- never a half-written file. Writing
// in place would mean a crash mid-write destroys the only copy of the data.
//
// Saves are serialised by an internal mutex, so a SAVE command and a scheduled
// snapshot cannot fight over the temporary file.
class SnapshotStore {
public:
    explicit SnapshotStore(std::string path);

    SnapshotStore(const SnapshotStore&) = delete;
    SnapshotStore& operator=(const SnapshotStore&) = delete;

    SaveOutcome save(Database& database);
    LoadOutcome load(Database& database);

    const std::string& path() const noexcept { return path_; }

    // Successful saves since construction. Diagnostics, and how tests confirm
    // the scheduler is doing anything.
    std::size_t saveCount() const;

private:
    std::string path_;
    mutable std::mutex mutex_;
    std::size_t saveCount_ = 0;
};

}  // namespace vcache
