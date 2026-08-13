// Phase 7 test suite -- snapshots, restore, and corruption handling.
//
// Discovery Document section 15 asks specifically for "save, restart, restore,
// and corrupted-file handling", and the corruption half is the larger one here:
// a snapshot is attacker-adjacent input in the same way a client request is,
// and a decoder that trusts its length fields is a decoder that can be made to
// allocate four gigabytes.

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "Database.h"
#include "Persistence.h"
#include "SnapshotScheduler.h"
#include "test_framework.h"

using vcache::Database;
using vcache::Entry;
using vcache::LoadStatus;
using vcache::SaveOutcome;
using vcache::SnapshotScheduler;
using vcache::SnapshotStore;
using namespace std::chrono_literals;

namespace {

// A snapshot path that deletes itself, so a failing test cannot leave debris
// that makes the next run pass or fail for the wrong reason.
class TempSnapshot {
public:
    TempSnapshot() {
        static std::atomic<int> counter{0};
        path_ = "/tmp/vcache-test-" + std::to_string(::getpid()) + "-" +
                std::to_string(counter.fetch_add(1)) + ".snapshot";
        remove();
    }

    ~TempSnapshot() { remove(); }

    TempSnapshot(const TempSnapshot&) = delete;
    TempSnapshot& operator=(const TempSnapshot&) = delete;

    const std::string& path() const { return path_; }

    void remove() const {
        std::remove(path_.c_str());
        std::remove((path_ + ".tmp").c_str());
    }

    bool exists() const {
        std::ifstream file(path_, std::ios::binary);
        return file.good();
    }

    std::string readBytes() const {
        std::ifstream file(path_, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
    }

    void writeBytes(const std::string& bytes) const {
        std::ofstream file(path_, std::ios::binary | std::ios::trunc);
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }

private:
    std::string path_;
};

}  // namespace

// ------------------------------------------------------------ round trips ----

VCACHE_TEST(SaveThenLoadRestoresEveryKey) {
    TempSnapshot file;
    SnapshotStore store(file.path());

    Database original;
    original.set("name", "Vansh");
    original.set("greeting", "hello world");
    original.set("number", "42");

    const SaveOutcome saved = store.save(original);
    CHECK(saved.ok);
    CHECK_EQ(saved.entriesWritten, std::size_t{3});
    CHECK(saved.bytesWritten > std::size_t{0});
    CHECK(file.exists());

    Database restored;
    const auto outcome = store.load(restored);
    CHECK(outcome.status == LoadStatus::Loaded);
    CHECK_EQ(outcome.entriesLoaded, std::size_t{3});

    CHECK_EQ(*restored.get("name"), std::string("Vansh"));
    CHECK_EQ(*restored.get("greeting"), std::string("hello world"));
    CHECK_EQ(*restored.get("number"), std::string("42"));
    CHECK_EQ(restored.size(), std::size_t{3});
}

VCACHE_TEST(AnEmptyDatabaseRoundTrips) {
    TempSnapshot file;
    SnapshotStore store(file.path());

    Database empty;
    CHECK(store.save(empty).ok);

    Database restored;
    restored.set("should-be-gone", "v");

    const auto outcome = store.load(restored);
    CHECK(outcome.status == LoadStatus::Loaded);
    CHECK_EQ(outcome.entriesLoaded, std::size_t{0});
    // Restore REPLACES: a snapshot of an empty database means empty, not
    // "leave whatever was there".
    CHECK_EQ(restored.size(), std::size_t{0});
}

VCACHE_TEST(BinaryKeysAndValuesSurvive) {
    // The reason the format is length-prefixed binary. Every one of these would
    // need escaping in a text format, and each escape is a chance to be wrong.
    TempSnapshot file;
    SnapshotStore store(file.path());

    const std::string binaryKey("k\0ey\xff", 5);
    const std::string binaryValue("va\0l\nue\r\t\xff", 10);

    Database original;
    original.set(binaryKey, binaryValue);
    original.set("", "empty key");
    original.set("empty value", "");
    CHECK(store.save(original).ok);

    Database restored;
    CHECK(store.load(restored).status == LoadStatus::Loaded);

    CHECK(*restored.get(binaryKey) == binaryValue);
    CHECK_EQ(restored.get(binaryKey)->size(), std::size_t{10});
    CHECK_EQ(*restored.get(""), std::string("empty key"));
    CHECK(restored.get("empty value")->empty());
}

VCACHE_TEST(LargeValuesSurvive) {
    TempSnapshot file;
    SnapshotStore store(file.path());

    const std::string large(2 * 1024 * 1024, 'x');  // 2 MiB
    Database original;
    original.set("big", large);
    CHECK(store.save(original).ok);

    Database restored;
    CHECK(store.load(restored).status == LoadStatus::Loaded);
    CHECK(*restored.get("big") == large);
}

VCACHE_TEST(ManyKeysSurvive) {
    TempSnapshot file;
    SnapshotStore store(file.path());

    constexpr int kCount = 10000;
    Database original;
    for (int i = 0; i < kCount; ++i) {
        original.set("user:" + std::to_string(i), "payload-" + std::to_string(i));
    }
    CHECK(store.save(original).ok);

    Database restored;
    const auto outcome = store.load(restored);
    CHECK_EQ(outcome.entriesLoaded, std::size_t{kCount});

    for (int i = 0; i < kCount; ++i) {
        const auto value = restored.get("user:" + std::to_string(i));
        CHECK(value.has_value());
        CHECK_EQ(*value, "payload-" + std::to_string(i));
    }
}

VCACHE_TEST(SavingTwiceOverwritesCleanly) {
    TempSnapshot file;
    SnapshotStore store(file.path());

    Database first;
    first.set("only", "first");
    CHECK(store.save(first).ok);

    Database second;
    second.set("only", "second");
    second.set("extra", "value");
    CHECK(store.save(second).ok);
    CHECK_EQ(store.saveCount(), std::size_t{2});

    Database restored;
    CHECK(store.load(restored).status == LoadStatus::Loaded);
    CHECK_EQ(*restored.get("only"), std::string("second"));
    CHECK_EQ(restored.size(), std::size_t{2});
}

VCACHE_TEST(TheTemporaryFileIsNotLeftBehind) {
    // The write goes to <path>.tmp and is renamed into place; a leftover temp
    // file would mean the rename never happened.
    TempSnapshot file;
    SnapshotStore store(file.path());

    Database db;
    db.set("key", "value");
    CHECK(store.save(db).ok);

    std::ifstream leftover(file.path() + ".tmp", std::ios::binary);
    CHECK(!leftover.good());
}

// -------------------------------------------------------------------- TTL ----

VCACHE_TEST(TtlsSurviveTheRoundTrip) {
    TempSnapshot file;
    SnapshotStore store(file.path());

    Database original;
    original.set("permanent", "v");
    original.set("expiring", "v", 3600s);
    CHECK(store.save(original).ok);

    Database restored;
    CHECK(store.load(restored).status == LoadStatus::Loaded);

    CHECK(!restored.ttl("permanent").has_value());  // still persistent

    const auto remaining = restored.ttl("expiring");
    CHECK(remaining.has_value());
    // Roughly the original hour, allowing for the time the test itself took.
    CHECK(*remaining > 3500s);
    CHECK(*remaining <= 3600s);
}

VCACHE_TEST(ExpiredKeysAreNotWritten) {
    TempSnapshot file;
    SnapshotStore store(file.path());

    Database original;
    original.set("live", "v");
    original.set("doomed", "v", 1s);
    std::this_thread::sleep_for(1200ms);

    const SaveOutcome saved = store.save(original);
    CHECK(saved.ok);
    CHECK_EQ(saved.entriesWritten, std::size_t{1});  // dead data stays out of the file

    Database restored;
    CHECK_EQ(store.load(restored).entriesLoaded, std::size_t{1});
    CHECK(restored.exists("live"));
    CHECK(!restored.exists("doomed"));
}

VCACHE_TEST(KeysThatLapseWhileTheServerIsDownAreDropped) {
    // The reason expiry is stored as an absolute wall-clock time rather than as
    // a remaining duration. With a duration, this key would come back with a
    // fresh two seconds every time the file was loaded, and would never die.
    TempSnapshot file;
    SnapshotStore store(file.path());

    Database original;
    original.set("session", "abc", 1s);
    CHECK(store.save(original).ok);

    // Stand in for the server being down longer than the TTL.
    std::this_thread::sleep_for(1300ms);

    Database restored;
    const auto outcome = store.load(restored);
    CHECK(outcome.status == LoadStatus::Loaded);
    CHECK_EQ(outcome.entriesLoaded, std::size_t{0});
    CHECK(!restored.exists("session"));
}

VCACHE_TEST(ARestoredTtlStillCountsDownAndExpires) {
    TempSnapshot file;
    SnapshotStore store(file.path());

    Database original;
    original.set("short", "v", 2s);
    CHECK(store.save(original).ok);

    Database restored;
    CHECK(store.load(restored).status == LoadStatus::Loaded);
    CHECK(restored.exists("short"));

    std::this_thread::sleep_for(2200ms);
    CHECK(!restored.exists("short"));  // the clock did not restart
}

// -------------------------------------------------------------- missing ----

VCACHE_TEST(AMissingSnapshotIsNotAnError) {
    // A first run has no file. That must be distinguishable from a corrupt one,
    // because the two deserve opposite responses.
    TempSnapshot file;
    SnapshotStore store(file.path());

    Database db;
    const auto outcome = store.load(db);
    CHECK(outcome.status == LoadStatus::NotFound);
    CHECK(outcome.error.empty());
    CHECK_EQ(db.size(), std::size_t{0});
}

// ------------------------------------------------- corruption (section 15) ----

VCACHE_TEST(AnEmptyFileIsRejected) {
    TempSnapshot file;
    file.writeBytes("");

    SnapshotStore store(file.path());
    Database db;
    const auto outcome = store.load(db);

    CHECK(outcome.status == LoadStatus::Failed);
    CHECK(!outcome.error.empty());
}

VCACHE_TEST(WrongMagicBytesAreRejected) {
    TempSnapshot file;
    file.writeBytes("NOTVCACHE" + std::string(64, '\0'));

    SnapshotStore store(file.path());
    Database db;
    const auto outcome = store.load(db);

    CHECK(outcome.status == LoadStatus::Failed);
    CHECK(outcome.error.find("magic") != std::string::npos);
}

VCACHE_TEST(ATruncatedFileIsRejected) {
    TempSnapshot file;
    SnapshotStore store(file.path());

    Database original;
    for (int i = 0; i < 100; ++i) {
        original.set("key" + std::to_string(i), "value" + std::to_string(i));
    }
    CHECK(store.save(original).ok);

    // Simulates a crash partway through a write -- which is exactly what the
    // write-to-temp-then-rename dance exists to prevent, but the reader must
    // still cope if it ever sees one.
    const std::string full = file.readBytes();
    file.writeBytes(full.substr(0, full.size() / 2));

    Database db;
    const auto outcome = store.load(db);
    CHECK(outcome.status == LoadStatus::Failed);
    CHECK_EQ(db.size(), std::size_t{0});  // nothing partial was applied
}

VCACHE_TEST(AFlippedBitIsCaughtByTheChecksum) {
    TempSnapshot file;
    SnapshotStore store(file.path());

    Database original;
    original.set("key", "value");
    original.set("other", "data");
    CHECK(store.save(original).ok);

    std::string bytes = file.readBytes();
    CHECK(bytes.size() > std::size_t{30});
    bytes[bytes.size() / 2] = static_cast<char>(bytes[bytes.size() / 2] ^ 0x01);  // one bit
    file.writeBytes(bytes);

    Database db;
    const auto outcome = store.load(db);
    CHECK(outcome.status == LoadStatus::Failed);
    CHECK(outcome.error.find("checksum") != std::string::npos);
}

VCACHE_TEST(AnUnknownFormatVersionIsRejected) {
    TempSnapshot file;
    SnapshotStore store(file.path());

    Database original;
    original.set("key", "value");
    CHECK(store.save(original).ok);

    // Bump the version field (bytes 6-7), then repair the checksum so the file
    // is well-formed apart from its version -- otherwise this would just be
    // another checksum test.
    std::string bytes = file.readBytes();
    bytes[6] = static_cast<char>(99);

    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (std::size_t i = 0; i + 8 < bytes.size() + 1 && i < bytes.size() - 8; ++i) {
        hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[i]));
        hash *= 0x100000001b3ULL;
    }
    for (std::size_t i = 0; i < 8; ++i) {
        bytes[bytes.size() - 8 + i] = static_cast<char>((hash >> (8 * i)) & 0xff);
    }
    file.writeBytes(bytes);

    Database db;
    const auto outcome = store.load(db);
    CHECK(outcome.status == LoadStatus::Failed);
    CHECK(outcome.error.find("version") != std::string::npos);
}

VCACHE_TEST(AnAbsurdLengthFieldIsRejectedWithoutAllocating) {
    // A hand-built snapshot claiming a 4 GB key. The decoder must refuse on the
    // length alone rather than trying to reserve the space first.
    std::string bytes;
    bytes.append("VCACHE", 6);
    bytes.push_back(1);           // version low byte
    bytes.push_back(0);           // version high byte
    for (int i = 0; i < 8; ++i) {  // entryCount = 1
        bytes.push_back(i == 0 ? 1 : 0);
    }
    for (int i = 0; i < 4; ++i) {  // keyLength = 0xffffffff
        bytes.push_back(static_cast<char>(0xff));
    }

    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (const char c : bytes) {
        hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
        hash *= 0x100000001b3ULL;
    }
    for (std::size_t i = 0; i < 8; ++i) {
        bytes.push_back(static_cast<char>((hash >> (8 * i)) & 0xff));
    }

    TempSnapshot file;
    file.writeBytes(bytes);

    SnapshotStore store(file.path());
    Database db;
    const auto outcome = store.load(db);

    CHECK(outcome.status == LoadStatus::Failed);
    CHECK(outcome.error.find("length") != std::string::npos);
}

VCACHE_TEST(RandomGarbageIsRejectedNotCrashed) {
    // Every one of these must produce a clean failure rather than a crash.
    const std::string inputs[] = {
        std::string(1, '\0'),
        std::string(15, '\xff'),
        std::string("VCACHE"),
        std::string("VCACHE\x01\x00", 8),
        std::string(1024, '\0'),
        std::string("VCACHE\x01\x00\xff\xff\xff\xff\xff\xff\xff\xff", 16),
    };

    for (const std::string& input : inputs) {
        TempSnapshot file;
        file.writeBytes(input);

        SnapshotStore store(file.path());
        Database db;
        const auto outcome = store.load(db);

        CHECK(outcome.status == LoadStatus::Failed);
        CHECK(!outcome.error.empty());
        CHECK_EQ(db.size(), std::size_t{0});
    }
}

VCACHE_TEST(AFailedLoadLeavesTheDatabaseUntouched) {
    // Refusing to apply a partial restore matters: half a keyspace looks like a
    // working server and is worse than no server.
    TempSnapshot file;
    file.writeBytes("VCACHE" + std::string(40, '\x7f'));

    Database db;
    db.set("existing", "value");

    SnapshotStore store(file.path());
    const auto outcome = store.load(db);

    CHECK(outcome.status == LoadStatus::Failed);
    CHECK_EQ(db.size(), std::size_t{1});
    CHECK_EQ(*db.get("existing"), std::string("value"));
}

// ---------------------------------------------------------- encode/decode ----

VCACHE_TEST(EncodeAndDecodeAgreeDirectly) {
    std::vector<Entry> entries;
    entries.emplace_back("alpha", "one");
    entries.emplace_back("beta", "two");

    const std::string encoded = vcache::snapshot::encode(entries);

    std::vector<Entry> decoded;
    std::string error;
    CHECK(vcache::snapshot::decode(encoded, decoded, error));
    CHECK(error.empty());
    CHECK_EQ(decoded.size(), std::size_t{2});
}

VCACHE_TEST(TrailingBytesAreRejected) {
    // Otherwise a file could be silently extended with junk that the reader
    // never looks at.
    std::vector<Entry> entries;
    entries.emplace_back("key", "value");

    std::string encoded = vcache::snapshot::encode(entries);
    encoded.insert(encoded.size() - 8, "EXTRA");

    std::vector<Entry> decoded;
    std::string error;
    CHECK(!vcache::snapshot::decode(encoded, decoded, error));
}

// ------------------------------------------------------------- scheduler ----

VCACHE_TEST(TheSchedulerSavesOnItsOwn) {
    TempSnapshot file;
    SnapshotStore store(file.path());

    Database db;
    db.set("key", "value");

    SnapshotScheduler scheduler(store, db, 30ms);
    scheduler.start();
    CHECK(scheduler.running());

    std::this_thread::sleep_for(200ms);
    scheduler.stop();

    CHECK(scheduler.attempts() > std::size_t{1});
    CHECK_EQ(scheduler.failures(), std::size_t{0});
    CHECK(file.exists());

    Database restored;
    CHECK(store.load(restored).status == LoadStatus::Loaded);
    CHECK_EQ(*restored.get("key"), std::string("value"));
}

VCACHE_TEST(TheSchedulerStopsPromptly) {
    // An hour-long interval: if stop() waited out the current interval, this
    // test would never finish.
    TempSnapshot file;
    SnapshotStore store(file.path());
    Database db;

    SnapshotScheduler scheduler(store, db, 3600s);
    scheduler.start();
    std::this_thread::sleep_for(20ms);

    const auto start = std::chrono::steady_clock::now();
    scheduler.stop();
    scheduler.stop();  // idempotent
    const auto elapsed = std::chrono::steady_clock::now() - start;

    CHECK(elapsed < 2s);
    CHECK(!scheduler.running());
}

VCACHE_TEST(AZeroIntervalDisablesTheScheduler) {
    TempSnapshot file;
    SnapshotStore store(file.path());
    Database db;
    db.set("key", "value");

    SnapshotScheduler scheduler(store, db, 0ms);
    scheduler.start();
    std::this_thread::sleep_for(100ms);
    scheduler.stop();

    CHECK(!scheduler.running());
    CHECK_EQ(scheduler.attempts(), std::size_t{0});
    CHECK(!file.exists());
}

VCACHE_TEST(AFailingScheduledSaveIsCountedNotFatal) {
    // A bad path stands in for a full disk. The thread must keep running and
    // record the problem rather than taking the process down.
    SnapshotStore store("/nonexistent-directory/vcache.snapshot");
    Database db;
    db.set("key", "value");

    SnapshotScheduler scheduler(store, db, 20ms);
    scheduler.start();
    std::this_thread::sleep_for(150ms);
    scheduler.stop();

    CHECK(scheduler.attempts() > std::size_t{0});
    CHECK_EQ(scheduler.failures(), scheduler.attempts());
    CHECK(!scheduler.lastError().empty());
}

VCACHE_TEST(SavingToAnUnwritablePathReportsAnError) {
    SnapshotStore store("/nonexistent-directory/vcache.snapshot");
    Database db;
    db.set("key", "value");

    const SaveOutcome outcome = store.save(db);
    CHECK(!outcome.ok);
    CHECK(!outcome.error.empty());
    CHECK_EQ(store.saveCount(), std::size_t{0});
}

int main() {
    return testing::runAll();
}
