// Phase 2 test suite -- the Database API (SET / GET / DEL / EXISTS / KEYS).
//
// These tests deliberately go through the public Database surface only. The
// hash table underneath already has its own suite; what is being checked here is
// the contract the command parser and server will code against in Phases 3-4.

#include <cstddef>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Database.h"
#include "test_framework.h"

using vcache::Database;

// ------------------------------------------------------------- empty state ----

VCACHE_TEST(NewDatabaseIsEmpty) {
    Database db;
    CHECK_EQ(db.size(), std::size_t{0});
    CHECK(db.empty());
    CHECK(db.keys().empty());
    CHECK(!db.get("anything").has_value());
    CHECK(!db.exists("anything"));
    CHECK(!db.del("anything"));
}

// -------------------------------------------------------------- SET / GET ----

VCACHE_TEST(SetThenGetReturnsTheValue) {
    Database db;
    CHECK(db.set("name", "Vansh") == vcache::SetOutcome::Inserted);

    const std::optional<std::string> value = db.get("name");
    CHECK(value.has_value());
    CHECK_EQ(*value, std::string("Vansh"));
    CHECK_EQ(db.size(), std::size_t{1});
}

VCACHE_TEST(SetReportsWhetherTheKeyIsNew) {
    // Phase 8 turned this into an enum so a write rejected by the memory limit
    // is distinguishable from a plain overwrite.
    Database db;
    CHECK(db.set("key", "first") == vcache::SetOutcome::Inserted);
    CHECK(db.set("key", "second") == vcache::SetOutcome::Updated);

    CHECK_EQ(*db.get("key"), std::string("second"));
    CHECK_EQ(db.size(), std::size_t{1});  // overwrite must not grow the keyspace
}

VCACHE_TEST(GetMissingKeyReturnsNullopt) {
    Database db;
    db.set("present", "yes");

    CHECK(!db.get("absent").has_value());
    CHECK(!db.get("").has_value());
    CHECK(!db.get("PRESENT").has_value());  // keys are byte-exact
}

VCACHE_TEST(GetReturnsACopyNotAView) {
    // The whole point of returning by value: a caller holding the result cannot
    // reach back into the keyspace, and the result stays valid after the entry
    // is overwritten or deleted. Phase 5 depends on this.
    Database db;
    db.set("key", "original");

    std::optional<std::string> snapshot = db.get("key");
    CHECK(snapshot.has_value());

    *snapshot = "mutated by the caller";
    CHECK_EQ(*db.get("key"), std::string("original"));  // store is untouched

    db.del("key");
    CHECK_EQ(*snapshot, std::string("mutated by the caller"));  // snapshot survives
}

// -------------------------------------------------------------------- DEL ----

VCACHE_TEST(DelRemovesTheKey) {
    Database db;
    db.set("doomed", "value");

    CHECK(db.del("doomed"));
    CHECK_EQ(db.size(), std::size_t{0});
    CHECK(!db.exists("doomed"));
    CHECK(!db.get("doomed").has_value());
}

VCACHE_TEST(DelIsIdempotentAndReportsMisses) {
    // DEL replies 1 or 0, never an error -- deleting a missing key is normal.
    Database db;
    db.set("key", "value");

    CHECK(db.del("key"));
    CHECK(!db.del("key"));       // second delete: 0
    CHECK(!db.del("never-set"));
    CHECK_EQ(db.size(), std::size_t{0});
}

VCACHE_TEST(SetAfterDelCreatesAFreshKey) {
    Database db;
    db.set("key", "old");
    db.del("key");

    CHECK(db.set("key", "new") == vcache::SetOutcome::Inserted);  // not an overwrite
    CHECK_EQ(*db.get("key"), std::string("new"));
    CHECK_EQ(db.size(), std::size_t{1});
}

// ----------------------------------------------------------------- EXISTS ----

VCACHE_TEST(ExistsReflectsMembership) {
    Database db;
    db.set("a", "1");
    db.set("b", "");  // an empty value is still a present key

    CHECK(db.exists("a"));
    CHECK(db.exists("b"));
    CHECK(!db.exists("c"));

    db.del("a");
    CHECK(!db.exists("a"));
}

// ------------------------------------------------------------------- KEYS ----

VCACHE_TEST(KeysListsEveryKeyExactlyOnce) {
    Database db;
    const std::vector<std::string> expected = {"user:1", "user:2", "session:abc", ""};
    for (const std::string& key : expected) {
        db.set(key, "v");
    }

    const std::vector<std::string> actual = db.keys();
    CHECK_EQ(actual.size(), expected.size());

    const std::unordered_set<std::string> actualSet(actual.begin(), actual.end());
    CHECK_EQ(actualSet.size(), expected.size());  // no duplicates
    for (const std::string& key : expected) {
        CHECK(actualSet.count(key) == 1);
    }
}

VCACHE_TEST(KeysReflectsOverwritesAndDeletions) {
    Database db;
    db.set("a", "1");
    db.set("b", "2");
    db.set("a", "overwritten");  // must not produce a duplicate key
    db.del("b");

    const std::vector<std::string> keys = db.keys();
    CHECK_EQ(keys.size(), std::size_t{1});
    CHECK_EQ(keys[0], std::string("a"));
}

// -------------------------------------------------------- size / clear ----

VCACHE_TEST(SizeTracksInsertionsAndDeletions) {
    Database db;
    for (int i = 0; i < 10; ++i) {
        db.set("key" + std::to_string(i), "v");
    }
    CHECK_EQ(db.size(), std::size_t{10});

    for (int i = 0; i < 4; ++i) {
        db.del("key" + std::to_string(i));
    }
    CHECK_EQ(db.size(), std::size_t{6});
    CHECK(!db.empty());
}

VCACHE_TEST(ClearEmptiesTheKeyspaceButKeepsTheDatabaseUsable) {
    Database db;
    for (int i = 0; i < 100; ++i) {
        db.set("key" + std::to_string(i), "v");
    }

    db.clear();
    CHECK_EQ(db.size(), std::size_t{0});
    CHECK(db.empty());
    CHECK(db.keys().empty());
    CHECK(!db.get("key0").has_value());

    db.set("fresh", "value");
    CHECK_EQ(*db.get("fresh"), std::string("value"));
}

// ------------------------------------------------------------ value shapes ----

VCACHE_TEST(EmptyKeysAndEmptyValuesAreValidStorage) {
    // The Database layer stores what it is given. Deciding that a command is
    // malformed belongs to the parser in Phase 3.
    Database db;
    db.set("", "value for the empty key");
    db.set("key", "");

    CHECK(db.exists(""));
    CHECK_EQ(*db.get(""), std::string("value for the empty key"));
    CHECK(db.get("key").has_value());
    CHECK(db.get("key")->empty());  // present, but empty -- not the same as (nil)
    CHECK_EQ(db.size(), std::size_t{2});
}

VCACHE_TEST(ValuesAreBinarySafe) {
    // Values arrive off a socket in Phase 4 and may contain any byte.
    Database db;
    const std::string binaryKey("k\0ey", 4);
    const std::string binaryValue("va\0lue\xff", 7);

    db.set(binaryKey, binaryValue);

    const std::optional<std::string> stored = db.get(binaryKey);
    CHECK(stored.has_value());
    CHECK_EQ(stored->size(), std::size_t{7});
    CHECK(*stored == binaryValue);
    CHECK(!db.exists("k"));  // must not truncate at the NUL
}

VCACHE_TEST(LargeValuesRoundTripIntact) {
    Database db;
    const std::string large(1024 * 1024, 'x');  // 1 MiB
    db.set("big", large);

    const std::optional<std::string> stored = db.get("big");
    CHECK(stored.has_value());
    CHECK_EQ(stored->size(), large.size());
    CHECK(*stored == large);
}

// -------------------------------------------------------- scale & lifetime ----

VCACHE_TEST(HandlesFiftyThousandKeys) {
    constexpr int kCount = 50000;
    Database db;

    for (int i = 0; i < kCount; ++i) {
        db.set("user:" + std::to_string(i), "payload-" + std::to_string(i));
    }
    CHECK_EQ(db.size(), static_cast<std::size_t>(kCount));
    CHECK_EQ(db.keys().size(), static_cast<std::size_t>(kCount));

    for (int i = 0; i < kCount; ++i) {
        const std::optional<std::string> value = db.get("user:" + std::to_string(i));
        CHECK(value.has_value());
        CHECK_EQ(*value, "payload-" + std::to_string(i));
    }

    for (int i = 0; i < kCount; i += 2) {
        CHECK(db.del("user:" + std::to_string(i)));
    }
    CHECK_EQ(db.size(), static_cast<std::size_t>(kCount / 2));

    for (int i = 0; i < kCount; ++i) {
        CHECK_EQ(db.exists("user:" + std::to_string(i)), (i % 2 == 1));
    }
}

VCACHE_TEST(SeparateDatabasesDoNotShareState) {
    Database first;
    Database second;

    first.set("key", "from-first");
    second.set("key", "from-second");

    CHECK_EQ(*first.get("key"), std::string("from-first"));
    CHECK_EQ(*second.get("key"), std::string("from-second"));

    first.clear();
    CHECK_EQ(second.size(), std::size_t{1});
}

VCACHE_TEST(DatabaseIsNeitherCopyableNorMovable) {
    // Phase 5 gave Database a std::shared_mutex, which is neither copyable nor
    // movable -- and an object holding a lock should not be able to slide out
    // from under the threads using it. Callers pass Database& instead.
    CHECK(!std::is_copy_constructible<Database>::value);
    CHECK(!std::is_move_constructible<Database>::value);
    CHECK(!std::is_copy_assignable<Database>::value);
    CHECK(!std::is_move_assignable<Database>::value);
}

VCACHE_TEST(PreSizedDatabaseBehavesIdentically) {
    // The bucket-count hint is a performance knob only; it must not change
    // observable behaviour.
    Database db(4096);
    for (int i = 0; i < 200; ++i) {
        db.set("key" + std::to_string(i), "v");
    }
    CHECK_EQ(db.size(), std::size_t{200});
    CHECK(db.exists("key199"));
    CHECK(!db.exists("key200"));
}

int main() {
    return testing::runAll();
}
