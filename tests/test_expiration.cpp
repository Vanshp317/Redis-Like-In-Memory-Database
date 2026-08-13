// Phase 6 test suite -- TTL and expiration.
//
// Time-dependent tests are a classic source of flakiness, so the design here is
// deliberate:
//
//   * Expiry is driven by short, real TTLs with generous margins, never by
//     sleeping right up to a boundary and hoping the scheduler cooperates.
//   * Reclamation is driven by calling removeExpired() directly rather than by
//     waiting on the background sweeper. The sweeper gets its own tests, where
//     the thing under test is the thread, not the expiry rule.

#include <chrono>
#include <cstddef>
#include <string>
#include <thread>
#include <vector>

#include "Database.h"
#include "ExpirationSweeper.h"
#include "test_framework.h"

using vcache::Database;
using vcache::ExpirationSweeper;
using vcache::SweeperConfig;
using namespace std::chrono_literals;

namespace {

// Sweeps the whole table regardless of size, for tests that care about the
// result rather than the incremental behaviour.
std::size_t sweepEverything(Database& db) {
    return db.removeExpired(db.bucketCount());
}

}  // namespace

// ------------------------------------------------------------ basic expiry ----

VCACHE_TEST(AKeyWithATtlIsReadableBeforeItExpires) {
    Database db;
    db.set("session", "abc123", 60s);

    const auto value = db.get("session");
    CHECK(value.has_value());
    CHECK_EQ(*value, std::string("abc123"));
    CHECK(db.exists("session"));
}

VCACHE_TEST(AKeyDisappearsOnceItsTtlElapses) {
    Database db;
    db.set("short", "value", 1s);

    CHECK(db.exists("short"));
    std::this_thread::sleep_for(1200ms);

    CHECK(!db.get("short").has_value());
    CHECK(!db.exists("short"));
}

VCACHE_TEST(KeysWithoutATtlNeverExpire) {
    Database db;
    db.set("permanent", "value");

    std::this_thread::sleep_for(50ms);
    CHECK(db.exists("permanent"));
    CHECK(!db.ttl("permanent").has_value());  // no expiration set
    CHECK_EQ(sweepEverything(db), std::size_t{0});
    CHECK(db.exists("permanent"));
}

VCACHE_TEST(ExpiryIsHiddenFromReadsImmediately) {
    // The guarantee that lets reclamation be lazy: a client can never observe
    // an expired key, however long the sweeper takes to get to it.
    Database db;
    db.set("key", "value", 1s);
    std::this_thread::sleep_for(1200ms);

    CHECK(!db.get("key").has_value());
    CHECK(!db.exists("key"));
    CHECK(db.keys().empty());
    CHECK(!db.ttl("key").has_value());

    // Still physically present -- nothing has reclaimed it yet.
    CHECK_EQ(db.size(), std::size_t{1});
}

// ----------------------------------------------------------- SET semantics ----

VCACHE_TEST(SettingWithoutATtlClearsAnExistingOne) {
    // Matches Redis: a plain SET makes a key persistent again. Without this, a
    // key that once had a TTL would keep vanishing after being rewritten.
    Database db;
    db.set("key", "first", 1s);
    CHECK(db.ttl("key").has_value());

    db.set("key", "second");
    CHECK(!db.ttl("key").has_value());

    std::this_thread::sleep_for(1200ms);
    CHECK(db.exists("key"));
    CHECK_EQ(*db.get("key"), std::string("second"));
}

VCACHE_TEST(SettingWithATtlReplacesTheOldOne) {
    Database db;
    db.set("key", "value", 100s);

    db.set("key", "value", 1s);
    const auto remaining = db.ttl("key");
    CHECK(remaining.has_value());
    CHECK(*remaining <= 1s);  // the shorter TTL won, not the longer
}

VCACHE_TEST(AddingATtlToAPersistentKeyWorks) {
    Database db;
    db.set("key", "value");
    CHECK(!db.ttl("key").has_value());

    db.set("key", "value", 60s);
    CHECK(db.ttl("key").has_value());
}

VCACHE_TEST(TtlIsReportedRoundedUp) {
    // A key set a moment ago with EX 60 should read as 60, not 59 -- the
    // fractional millisecond already elapsed must not lose a whole second.
    Database db;
    db.set("key", "value", 60s);

    const auto remaining = db.ttl("key");
    CHECK(remaining.has_value());
    CHECK_EQ(remaining->count(), std::int64_t{60});
}

VCACHE_TEST(TtlCountsDown) {
    Database db;
    db.set("key", "value", 3s);

    const auto first = db.ttl("key");
    std::this_thread::sleep_for(1100ms);
    const auto second = db.ttl("key");

    CHECK(first.has_value());
    CHECK(second.has_value());
    CHECK(*second < *first);
}

VCACHE_TEST(TtlOfAnAbsentKeyIsEmpty) {
    Database db;
    CHECK(!db.ttl("never-set").has_value());
}

// ------------------------------------------------------------------- DEL ----

VCACHE_TEST(DeletingAnExpiredKeyReportsAMiss) {
    // DEL must agree with EXISTS. Reporting 1 for a key EXISTS just called
    // absent would be incoherent, even though the entry is physically there.
    Database db;
    db.set("key", "value", 1s);
    std::this_thread::sleep_for(1200ms);

    CHECK(!db.exists("key"));
    CHECK(!db.del("key"));          // reports the miss
    CHECK_EQ(db.size(), std::size_t{0});  // but still reclaims the memory
}

VCACHE_TEST(DeletingALiveKeyStillReportsAHit) {
    Database db;
    db.set("key", "value", 60s);
    CHECK(db.del("key"));
    CHECK_EQ(db.size(), std::size_t{0});
}

// -------------------------------------------------------------- KEYS view ----

VCACHE_TEST(KeysOmitsExpiredEntries) {
    Database db;
    db.set("live1", "v");
    db.set("live2", "v", 60s);
    db.set("doomed1", "v", 1s);
    db.set("doomed2", "v", 1s);

    CHECK_EQ(db.keys().size(), std::size_t{4});
    std::this_thread::sleep_for(1200ms);

    const std::vector<std::string> keys = db.keys();
    CHECK_EQ(keys.size(), std::size_t{2});
    for (const std::string& key : keys) {
        CHECK(key.rfind("live", 0) == 0);
    }

    // size() deliberately still counts the unreclaimed entries, like DBSIZE.
    CHECK_EQ(db.size(), std::size_t{4});
}

// ---------------------------------------------------------- reclamation ----

VCACHE_TEST(RemoveExpiredFreesOnlyExpiredEntries) {
    Database db;
    for (int i = 0; i < 50; ++i) {
        db.set("live" + std::to_string(i), "v");
        db.set("doomed" + std::to_string(i), "v", 1s);
    }
    CHECK_EQ(db.size(), std::size_t{100});

    CHECK_EQ(sweepEverything(db), std::size_t{0});  // nothing expired yet
    std::this_thread::sleep_for(1200ms);

    CHECK_EQ(sweepEverything(db), std::size_t{50});
    CHECK_EQ(db.size(), std::size_t{50});
    for (int i = 0; i < 50; ++i) {
        CHECK(db.exists("live" + std::to_string(i)));
    }
}

VCACHE_TEST(SweepingIsIncrementalAndResumes) {
    // The property that keeps the write lock from being held for a long time on
    // a big keyspace: one pass covers a slice, and successive passes advance
    // through the table rather than restarting.
    Database db(1024);
    for (int i = 0; i < 500; ++i) {
        db.set("key" + std::to_string(i), "v", 1s);
    }
    std::this_thread::sleep_for(1200ms);

    const std::size_t buckets = db.bucketCount();
    const std::size_t slice = buckets / 8;

    const std::size_t firstPass = db.removeExpired(slice);
    CHECK(firstPass < std::size_t{500});   // only a slice was touched
    CHECK(db.size() > std::size_t{0});     // the rest is still there

    // Enough further passes to cover the whole table.
    std::size_t reclaimed = firstPass;
    for (std::size_t pass = 0; pass < 8; ++pass) {
        reclaimed += db.removeExpired(slice);
    }

    CHECK_EQ(reclaimed, std::size_t{500});
    CHECK_EQ(db.size(), std::size_t{0});
}

VCACHE_TEST(SweepingAnEmptyDatabaseIsHarmless) {
    Database db;
    CHECK_EQ(db.removeExpired(100), std::size_t{0});
    CHECK_EQ(db.removeExpired(0), std::size_t{0});
}

VCACHE_TEST(TheCursorSurvivesATableResize) {
    // The table grows underneath the sweeper as keys are added, which leaves
    // the saved cursor pointing past the end. Wrapping must not lose entries.
    Database db(16);
    for (int i = 0; i < 200; ++i) {
        db.set("key" + std::to_string(i), "v", 1s);
    }
    db.removeExpired(8);  // leaves the cursor mid-table

    for (int i = 200; i < 2000; ++i) {  // forces several resizes
        db.set("key" + std::to_string(i), "v", 1s);
    }
    std::this_thread::sleep_for(1200ms);

    for (int pass = 0; pass < 200; ++pass) {
        db.removeExpired(db.bucketCount());
    }
    CHECK_EQ(db.size(), std::size_t{0});
}

VCACHE_TEST(ExpiredKeysDoNotComeBack) {
    Database db;
    db.set("key", "old", 1s);
    std::this_thread::sleep_for(1200ms);
    CHECK(!db.exists("key"));

    db.set("key", "new");
    CHECK(db.exists("key"));
    CHECK_EQ(*db.get("key"), std::string("new"));

    sweepEverything(db);
    CHECK(db.exists("key"));  // the sweeper must not take the fresh value
}

VCACHE_TEST(ClearResetsTheSweepCursor) {
    Database db(256);
    for (int i = 0; i < 100; ++i) {
        db.set("key" + std::to_string(i), "v", 1s);
    }
    db.removeExpired(64);
    db.clear();

    CHECK_EQ(db.size(), std::size_t{0});
    db.set("fresh", "v", 1s);
    std::this_thread::sleep_for(1200ms);
    CHECK_EQ(sweepEverything(db), std::size_t{1});
}

// ------------------------------------------------------ background sweeper ----

VCACHE_TEST(TheSweeperReclaimsWithoutBeingAsked) {
    Database db;
    for (int i = 0; i < 100; ++i) {
        db.set("key" + std::to_string(i), "v", 1s);
    }

    SweeperConfig config;
    config.interval = 20ms;
    config.bucketsPerPass = 1024;  // whole table per pass, for a prompt test

    ExpirationSweeper sweeper(db, config);
    sweeper.start();
    CHECK(sweeper.running());

    // Well past the 1s TTL plus several sweep intervals.
    std::this_thread::sleep_for(1500ms);
    sweeper.stop();

    CHECK_EQ(db.size(), std::size_t{0});
    CHECK_EQ(sweeper.totalReclaimed(), std::size_t{100});
    CHECK(sweeper.passes() > std::size_t{1});
}

VCACHE_TEST(TheSweeperLeavesLiveKeysAlone) {
    Database db;
    db.set("permanent", "v");
    db.set("long-lived", "v", 3600s);

    SweeperConfig config;
    config.interval = 10ms;
    ExpirationSweeper sweeper(db, config);
    sweeper.start();
    std::this_thread::sleep_for(150ms);
    sweeper.stop();

    CHECK_EQ(db.size(), std::size_t{2});
    CHECK_EQ(sweeper.totalReclaimed(), std::size_t{0});
    CHECK(sweeper.passes() > std::size_t{1});
}

VCACHE_TEST(StoppingTheSweeperIsPromptAndIdempotent) {
    // A one-hour interval: if stop() waited for the current interval to elapse,
    // this test would never finish.
    Database db;
    SweeperConfig config;
    config.interval = 3600s;

    ExpirationSweeper sweeper(db, config);
    sweeper.start();
    std::this_thread::sleep_for(20ms);

    const auto start = std::chrono::steady_clock::now();
    sweeper.stop();
    sweeper.stop();  // must not double-join
    const auto elapsed = std::chrono::steady_clock::now() - start;

    CHECK(elapsed < 2s);
    CHECK(!sweeper.running());
}

VCACHE_TEST(TheSweeperDestructorStopsTheThread) {
    Database db;
    {
        SweeperConfig config;
        config.interval = 10ms;
        ExpirationSweeper sweeper(db, config);
        sweeper.start();
        std::this_thread::sleep_for(30ms);
        // Destructor runs here and must join.
    }
    CHECK(true);
}

VCACHE_TEST(StartingTwiceIsHarmless) {
    Database db;
    SweeperConfig config;
    config.interval = 10ms;

    ExpirationSweeper sweeper(db, config);
    sweeper.start();
    sweeper.start();  // must not spawn a second thread over the first
    std::this_thread::sleep_for(30ms);
    sweeper.stop();

    CHECK(!sweeper.running());
}

int main() {
    return testing::runAll();
}
