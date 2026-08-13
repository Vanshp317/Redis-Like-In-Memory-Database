#include "Persistence.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <utility>

namespace vcache {

namespace {

using SystemClock = std::chrono::system_clock;
using Milliseconds = std::chrono::milliseconds;

// FNV-1a over the payload. A checksum, not a hash-table hash, so it skips the
// avalanche step HashTable uses -- all that matters here is that a flipped bit
// changes the result.
std::uint64_t checksumOf(const char* data, std::size_t length) noexcept {
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (std::size_t i = 0; i < length; ++i) {
        hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(data[i]));
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

// Explicit little-endian encoding rather than memcpy of the native
// representation, so a snapshot written on one architecture reads on another.
void appendLE(std::string& out, std::uint64_t value, std::size_t byteCount) {
    for (std::size_t i = 0; i < byteCount; ++i) {
        out.push_back(static_cast<char>((value >> (8 * i)) & 0xff));
    }
}

bool readLE(const std::string& data, std::size_t& offset, std::size_t byteCount,
            std::uint64_t& value) noexcept {
    if (offset + byteCount > data.size()) {
        return false;  // truncated
    }
    value = 0;
    for (std::size_t i = 0; i < byteCount; ++i) {
        const auto byte = static_cast<unsigned char>(data[offset + i]);
        value |= static_cast<std::uint64_t>(byte) << (8 * i);
    }
    offset += byteCount;
    return true;
}

// steady_clock expiry -> milliseconds since the Unix epoch.
//
// The two clocks do not share a tick period -- steady_clock counts nanoseconds
// and system_clock microseconds on this platform -- so the remaining duration
// is converted explicitly rather than relying on an implicit conversion that
// would not compile anyway.
std::int64_t toWallClockMillis(Clock::time_point expiry) {
    const Clock::duration remaining = expiry - Clock::now();
    const SystemClock::time_point wall =
        SystemClock::now() + std::chrono::duration_cast<SystemClock::duration>(remaining);
    return std::chrono::duration_cast<Milliseconds>(wall.time_since_epoch()).count();
}

// The inverse. Returns false if the moment has already passed, which is how a
// key that lapsed while the server was down gets dropped.
bool fromWallClockMillis(std::int64_t millis, Clock::time_point& expiry) {
    const SystemClock::time_point wall{Milliseconds(millis)};
    const SystemClock::duration remaining = wall - SystemClock::now();
    if (remaining <= SystemClock::duration::zero()) {
        return false;
    }
    expiry = Clock::now() + std::chrono::duration_cast<Clock::duration>(remaining);
    return true;
}

std::string describeErrno(const char* what, const std::string& path) {
    return std::string(what) + " '" + path + "': " + std::strerror(errno);
}

bool writeAll(int fd, const char* data, std::size_t length) {
    std::size_t written = 0;
    while (written < length) {
        // write() may accept fewer bytes than offered, exactly like send().
        const ssize_t result = ::write(fd, data + written, length - written);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        written += static_cast<std::size_t>(result);
    }
    return true;
}

bool readWholeFile(int fd, std::string& out) {
    char buffer[64 * 1024];
    while (true) {
        const ssize_t result = ::read(fd, buffer, sizeof(buffer));
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (result == 0) {
            return true;
        }
        out.append(buffer, static_cast<std::size_t>(result));
    }
}

}  // namespace

namespace snapshot {

std::string encode(const std::vector<Entry>& entries) {
    std::string out;
    out.append(kMagic, kMagicLength);
    appendLE(out, kFormatVersion, 2);

    // The count is written before the entries are known to be writable, so the
    // live ones are gathered first.
    const Clock::time_point now = Clock::now();
    std::vector<const Entry*> live;
    live.reserve(entries.size());
    for (const Entry& entry : entries) {
        if (!entry.isExpiredAt(now)) {
            live.push_back(&entry);
        }
    }

    appendLE(out, static_cast<std::uint64_t>(live.size()), 8);

    for (const Entry* entry : live) {
        appendLE(out, static_cast<std::uint64_t>(entry->key.size()), 4);
        out.append(entry->key);
        appendLE(out, static_cast<std::uint64_t>(entry->value.size()), 4);
        out.append(entry->value);

        const std::int64_t expiresAt =
            entry->hasExpiration() ? toWallClockMillis(*entry->expiration) : 0;
        appendLE(out, static_cast<std::uint64_t>(expiresAt), 8);
    }

    appendLE(out, checksumOf(out.data(), out.size()), 8);
    return out;
}

bool decode(const std::string& data, std::vector<Entry>& entries, std::string& error) {
    entries.clear();

    constexpr std::size_t kHeaderLength = kMagicLength + 2 + 8;
    constexpr std::size_t kChecksumLength = 8;

    if (data.size() < kHeaderLength + kChecksumLength) {
        error = "snapshot is too short to be valid";
        return false;
    }

    if (std::memcmp(data.data(), kMagic, kMagicLength) != 0) {
        error = "snapshot has the wrong magic bytes -- not a VCache snapshot";
        return false;
    }

    // The checksum covers everything before it, so it is verified before any
    // length field is trusted enough to allocate against.
    const std::size_t payloadLength = data.size() - kChecksumLength;
    std::size_t checksumOffset = payloadLength;
    std::uint64_t storedChecksum = 0;
    if (!readLE(data, checksumOffset, 8, storedChecksum)) {
        error = "snapshot is missing its checksum";
        return false;
    }
    if (storedChecksum != checksumOf(data.data(), payloadLength)) {
        error = "snapshot checksum mismatch -- the file is corrupt";
        return false;
    }

    std::size_t offset = kMagicLength;

    std::uint64_t version = 0;
    if (!readLE(data, offset, 2, version)) {
        error = "snapshot is truncated in its header";
        return false;
    }
    if (version != kFormatVersion) {
        error = "snapshot format version " + std::to_string(version) +
                " is not supported (expected " + std::to_string(kFormatVersion) + ")";
        return false;
    }

    std::uint64_t entryCount = 0;
    if (!readLE(data, offset, 8, entryCount)) {
        error = "snapshot is truncated in its header";
        return false;
    }

    const Clock::time_point now = Clock::now();
    entries.reserve(static_cast<std::size_t>(
        entryCount < 1000000 ? entryCount : 1000000));  // bounded: the count is untrusted

    for (std::uint64_t i = 0; i < entryCount; ++i) {
        std::uint64_t keyLength = 0;
        if (!readLE(data, offset, 4, keyLength) || keyLength > kMaxFieldLength) {
            error = "snapshot entry " + std::to_string(i) + " has an invalid key length";
            return false;
        }
        if (offset + keyLength > payloadLength) {
            error = "snapshot entry " + std::to_string(i) + " is truncated";
            return false;
        }
        std::string key = data.substr(offset, static_cast<std::size_t>(keyLength));
        offset += static_cast<std::size_t>(keyLength);

        std::uint64_t valueLength = 0;
        if (!readLE(data, offset, 4, valueLength) || valueLength > kMaxFieldLength) {
            error = "snapshot entry " + std::to_string(i) + " has an invalid value length";
            return false;
        }
        if (offset + valueLength > payloadLength) {
            error = "snapshot entry " + std::to_string(i) + " is truncated";
            return false;
        }
        std::string value = data.substr(offset, static_cast<std::size_t>(valueLength));
        offset += static_cast<std::size_t>(valueLength);

        std::uint64_t rawExpiry = 0;
        if (!readLE(data, offset, 8, rawExpiry)) {
            error = "snapshot entry " + std::to_string(i) + " is missing its expiry";
            return false;
        }

        Entry entry(std::move(key), std::move(value));

        const auto expiresAtMillis = static_cast<std::int64_t>(rawExpiry);
        if (expiresAtMillis != 0) {
            Clock::time_point expiry;
            if (!fromWallClockMillis(expiresAtMillis, expiry)) {
                continue;  // lapsed while the server was down
            }
            entry.expiration = expiry;
        }

        if (!entry.isExpiredAt(now)) {
            entries.push_back(std::move(entry));
        }
    }

    if (offset != payloadLength) {
        error = "snapshot has trailing bytes after the last entry";
        return false;
    }

    return true;
}

}  // namespace snapshot

SnapshotStore::SnapshotStore(std::string path) : path_(std::move(path)) {}

std::size_t SnapshotStore::saveCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return saveCount_;
}

SaveOutcome SnapshotStore::save(Database& database) {
    SaveOutcome outcome;

    // Serialises a manual SAVE against a scheduled one, so they cannot both be
    // writing the temporary file at once.
    std::lock_guard<std::mutex> lock(mutex_);

    // The database lock is taken and released inside snapshot(); the file work
    // below happens with no database lock held, so clients keep running while
    // the disk write is in flight.
    const std::vector<Entry> entries = database.snapshot();
    const std::string encoded = snapshot::encode(entries);

    const std::string tempPath = path_ + ".tmp";

    const int fd = ::open(tempPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        outcome.error = describeErrno("cannot open", tempPath);
        return outcome;
    }

    if (!writeAll(fd, encoded.data(), encoded.size())) {
        outcome.error = describeErrno("cannot write", tempPath);
        ::close(fd);
        ::unlink(tempPath.c_str());
        return outcome;
    }

    // Without fsync the bytes may still be sitting in the kernel's page cache,
    // so the rename below could publish a file whose contents have not reached
    // the disk yet -- a crash would then expose an empty or partial snapshot.
    if (::fsync(fd) < 0) {
        outcome.error = describeErrno("cannot flush", tempPath);
        ::close(fd);
        ::unlink(tempPath.c_str());
        return outcome;
    }
    ::close(fd);

    // The atomic step. Either the old snapshot is intact or the new one is;
    // there is no moment where the file on disk is half-written.
    if (::rename(tempPath.c_str(), path_.c_str()) < 0) {
        outcome.error = describeErrno("cannot replace", path_);
        ::unlink(tempPath.c_str());
        return outcome;
    }

    outcome.ok = true;
    outcome.entriesWritten = entries.size();
    outcome.bytesWritten = encoded.size();
    ++saveCount_;
    return outcome;
}

LoadOutcome SnapshotStore::load(Database& database) {
    LoadOutcome outcome;

    std::lock_guard<std::mutex> lock(mutex_);

    const int fd = ::open(path_.c_str(), O_RDONLY);
    if (fd < 0) {
        if (errno == ENOENT) {
            outcome.status = LoadStatus::NotFound;  // first run: nothing to restore
            return outcome;
        }
        outcome.status = LoadStatus::Failed;
        outcome.error = describeErrno("cannot open", path_);
        return outcome;
    }

    std::string data;
    const bool readOk = readWholeFile(fd, data);
    ::close(fd);

    if (!readOk) {
        outcome.status = LoadStatus::Failed;
        outcome.error = describeErrno("cannot read", path_);
        return outcome;
    }

    std::vector<Entry> entries;
    std::string error;
    if (!snapshot::decode(data, entries, error)) {
        outcome.status = LoadStatus::Failed;
        outcome.error = error;
        return outcome;
    }

    outcome.status = LoadStatus::Loaded;
    outcome.entriesLoaded = database.restore(entries);
    return outcome;
}

}  // namespace vcache
