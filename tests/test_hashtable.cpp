// Phase 1 test suite -- covers the hash-table requirements from Discovery
// Document section 15: insertion, retrieval, deletion, collisions, resizing and
// large datasets.

#include <cstddef>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "HashTable.h"
#include "test_framework.h"

using vcache::Entry;
using vcache::HashTable;

namespace {

// Builds `count` distinct keys that all land in the same bucket of a table with
// `bucketCount` buckets. This is how the collision tests force chains on purpose
// instead of hoping for an accidental collision.
std::vector<std::string> makeCollidingKeys(std::size_t bucketCount,
                                           std::size_t count,
                                           std::size_t targetBucket = 0) {
    const std::size_t mask = bucketCount - 1;
    std::vector<std::string> keys;
    keys.reserve(count);

    // Expected work is count * bucketCount candidates; the cap only exists so a
    // broken hash fails loudly instead of hanging the suite.
    const std::size_t maxCandidates = (count + 1) * bucketCount * 64;
    for (std::size_t i = 0; i < maxCandidates && keys.size() < count; ++i) {
        std::string candidate = "collide-" + std::to_string(i);
        if ((static_cast<std::size_t>(HashTable::hashKey(candidate)) & mask) == targetBucket) {
            keys.push_back(std::move(candidate));
        }
    }

    if (keys.size() < count) {
        throw std::runtime_error("could not generate enough colliding keys");
    }
    return keys;
}

}  // namespace

// ---------------------------------------------------------------- basics ----

VCACHE_TEST(NewTableIsEmpty) {
    HashTable table;
    CHECK_EQ(table.size(), std::size_t{0});
    CHECK(table.empty());
    CHECK(table.keys().empty());
    CHECK(table.get("anything") == nullptr);
    CHECK(!table.contains("anything"));
}

VCACHE_TEST(PutThenGetReturnsTheValue) {
    HashTable table;
    CHECK(table.put("name", "Vansh"));

    const std::string* value = table.get("name");
    CHECK(value != nullptr);
    CHECK_EQ(*value, std::string("Vansh"));
    CHECK_EQ(table.size(), std::size_t{1});
    CHECK(!table.empty());
}

VCACHE_TEST(PutReturnsFalseWhenOverwriting) {
    HashTable table;
    CHECK(table.put("key", "first"));
    CHECK(!table.put("key", "second"));  // false == overwrote, did not insert

    CHECK_EQ(*table.get("key"), std::string("second"));
    CHECK_EQ(table.size(), std::size_t{1});  // overwrite must not grow the table
}

VCACHE_TEST(GetMissingKeyReturnsNull) {
    HashTable table;
    table.put("present", "yes");
    CHECK(table.get("absent") == nullptr);
    CHECK(table.find("absent") == nullptr);
}

VCACHE_TEST(ContainsReflectsMembership) {
    HashTable table;
    table.put("a", "1");

    CHECK(table.contains("a"));
    CHECK(!table.contains("b"));
    CHECK(!table.contains("A"));  // keys are case-sensitive and byte-exact
}

VCACHE_TEST(FindGivesMutableAccessToTheEntry) {
    HashTable table;
    table.put("counter", "1");

    Entry* entry = table.find("counter");
    CHECK(entry != nullptr);
    CHECK_EQ(entry->key, std::string("counter"));

    entry->value = "2";  // in-place edit, no rehash
    CHECK_EQ(*table.get("counter"), std::string("2"));
}

VCACHE_TEST(PutClearsAnyExistingExpiration) {
    // Phase 6 relies on this: a plain SET drops the previous TTL, like Redis.
    HashTable table;
    table.put("session", "abc");
    table.find("session")->expiration = vcache::Clock::now();

    table.put("session", "def");
    CHECK(!table.find("session")->expiration.has_value());
}

// -------------------------------------------------------------- deletion ----

VCACHE_TEST(RemoveDeletesTheKey) {
    HashTable table;
    table.put("doomed", "value");

    CHECK(table.remove("doomed"));
    CHECK_EQ(table.size(), std::size_t{0});
    CHECK(!table.contains("doomed"));
    CHECK(table.get("doomed") == nullptr);
}

VCACHE_TEST(RemoveMissingKeyReturnsFalse) {
    HashTable table;
    table.put("kept", "value");

    CHECK(!table.remove("never-added"));
    CHECK(!table.remove(""));
    CHECK_EQ(table.size(), std::size_t{1});  // unchanged
}

VCACHE_TEST(RemoveWorksAtHeadMiddleAndTailOfAChain) {
    // The pointer-to-pointer splice in remove() has no head special case; this
    // proves all three positions behave the same.
    HashTable table(16);
    const std::vector<std::string> keys = makeCollidingKeys(16, 5);
    for (std::size_t i = 0; i < keys.size(); ++i) {
        table.put(keys[i], "v" + std::to_string(i));
    }
    CHECK_EQ(table.longestChain(), std::size_t{5});  // all five really did collide

    CHECK(table.remove(keys[0]));  // one end of the chain
    CHECK(table.remove(keys[2]));  // middle
    CHECK(table.remove(keys[4]));  // other end
    CHECK_EQ(table.size(), std::size_t{2});

    CHECK(table.contains(keys[1]));
    CHECK(table.contains(keys[3]));
    CHECK(!table.contains(keys[0]));
    CHECK(!table.contains(keys[2]));
    CHECK(!table.contains(keys[4]));
}

VCACHE_TEST(ReinsertAfterRemoveWorks) {
    HashTable table;
    table.put("key", "old");
    table.remove("key");
    CHECK(table.put("key", "new"));  // true == treated as a fresh insert

    CHECK_EQ(*table.get("key"), std::string("new"));
    CHECK_EQ(table.size(), std::size_t{1});
}

// ------------------------------------------------------------ collisions ----

VCACHE_TEST(LongCollisionChainStaysCorrect) {
    // 500 keys in one bucket of a 1024-bucket table. Load factor stays at 0.49,
    // so no resize fires and the chain is genuinely 500 long -- this exercises
    // chain traversal and the iterative destructor.
    constexpr std::size_t kBuckets = 1024;
    constexpr std::size_t kKeys = 500;

    HashTable table(kBuckets);
    const std::vector<std::string> keys = makeCollidingKeys(kBuckets, kKeys);
    for (std::size_t i = 0; i < kKeys; ++i) {
        table.put(keys[i], "value-" + std::to_string(i));
    }

    CHECK_EQ(table.bucketCount(), kBuckets);            // no growth happened
    CHECK_EQ(table.longestChain(), kKeys);              // all in one bucket
    CHECK_EQ(table.size(), kKeys);

    for (std::size_t i = 0; i < kKeys; ++i) {
        const std::string* value = table.get(keys[i]);
        CHECK(value != nullptr);
        CHECK_EQ(*value, "value-" + std::to_string(i));
    }
}

VCACHE_TEST(CollidingKeysSurviveARehash) {
    // Keys that collide at 16 buckets should scatter once the table grows.
    HashTable table(16);
    const std::vector<std::string> keys = makeCollidingKeys(16, 8);
    for (const std::string& key : keys) {
        table.put(key, key + "-value");
    }

    table.resize(4096);
    CHECK_EQ(table.size(), keys.size());
    for (const std::string& key : keys) {
        CHECK_EQ(*table.get(key), key + "-value");
    }
    CHECK(table.longestChain() < keys.size());  // they no longer share a bucket
}

// --------------------------------------------------------------- resizing ----

VCACHE_TEST(TableGrowsAutomaticallyAndRespectsLoadFactor) {
    HashTable table(16);
    const std::size_t initialBuckets = table.bucketCount();

    for (int i = 0; i < 1000; ++i) {
        table.put("key" + std::to_string(i), "value");
        CHECK(table.loadFactor() <= HashTable::kMaxLoadFactor);
    }

    CHECK(table.bucketCount() > initialBuckets);
    CHECK_EQ(table.size(), std::size_t{1000});
}

VCACHE_TEST(BucketCountIsAlwaysAPowerOfTwo) {
    // Bucket selection uses `hash & (n - 1)`, which is only a valid modulo when
    // n is a power of two.
    for (std::size_t requested : {std::size_t{1}, std::size_t{3}, std::size_t{17},
                                  std::size_t{1000}, std::size_t{4097}}) {
        HashTable table(requested);
        const std::size_t n = table.bucketCount();
        CHECK(n >= requested);
        CHECK((n & (n - 1)) == 0);
    }
}

VCACHE_TEST(ExplicitResizePreservesEveryEntry) {
    HashTable table;
    for (int i = 0; i < 200; ++i) {
        table.put("key" + std::to_string(i), "value" + std::to_string(i));
    }

    table.resize(8192);
    CHECK_EQ(table.bucketCount(), std::size_t{8192});
    CHECK_EQ(table.size(), std::size_t{200});
    for (int i = 0; i < 200; ++i) {
        CHECK_EQ(*table.get("key" + std::to_string(i)), "value" + std::to_string(i));
    }
}

VCACHE_TEST(ResizeRefusesToShrinkPastTheLoadFactor) {
    HashTable table;
    for (int i = 0; i < 100; ++i) {
        table.put("key" + std::to_string(i), "v");
    }

    table.resize(1);  // absurd request: must be clamped upward, not honoured
    CHECK(table.loadFactor() <= HashTable::kMaxLoadFactor);
    CHECK_EQ(table.size(), std::size_t{100});
    for (int i = 0; i < 100; ++i) {
        CHECK(table.contains("key" + std::to_string(i)));
    }
}

// ---------------------------------------------------------- large dataset ----

VCACHE_TEST(HandlesOneHundredThousandKeys) {
    constexpr int kCount = 100000;
    HashTable table;

    for (int i = 0; i < kCount; ++i) {
        table.put("user:" + std::to_string(i), "payload-" + std::to_string(i));
    }
    CHECK_EQ(table.size(), static_cast<std::size_t>(kCount));

    for (int i = 0; i < kCount; ++i) {
        const std::string* value = table.get("user:" + std::to_string(i));
        CHECK(value != nullptr);
        CHECK_EQ(*value, "payload-" + std::to_string(i));
    }

    // Delete every second key, then confirm exactly the right half survives.
    for (int i = 0; i < kCount; i += 2) {
        CHECK(table.remove("user:" + std::to_string(i)));
    }
    CHECK_EQ(table.size(), static_cast<std::size_t>(kCount / 2));

    for (int i = 0; i < kCount; ++i) {
        const bool shouldExist = (i % 2 == 1);
        CHECK_EQ(table.contains("user:" + std::to_string(i)), shouldExist);
    }
}

VCACHE_TEST(HashSpreadsKeysEvenly) {
    // Guards against a hash regression. With 10k keys at a load factor under
    // 0.75, a healthy hash gives a longest chain around 5; a chain of 12+ means
    // the hash or the masking has broken.
    HashTable table;
    for (int i = 0; i < 10000; ++i) {
        table.put("session:" + std::to_string(i), "v");
    }
    CHECK(table.longestChain() <= 12);
}

VCACHE_TEST(HashIsDeterministic) {
    // Benchmarks and (later) on-disk snapshots depend on a stable hash.
    CHECK_EQ(HashTable::hashKey("vcache"), HashTable::hashKey("vcache"));
    CHECK(HashTable::hashKey("vcache") != HashTable::hashKey("vcachf"));
    CHECK(HashTable::hashKey("") != 0);
}

// ------------------------------------------------------ keys/clear/moves ----

VCACHE_TEST(KeysReturnsEveryKeyExactlyOnce) {
    HashTable table;
    const std::vector<std::string> expected = {"alpha", "beta", "gamma", "delta"};
    for (const std::string& key : expected) {
        table.put(key, "v");
    }

    const std::vector<std::string> actual = table.keys();
    CHECK_EQ(actual.size(), expected.size());

    const std::unordered_set<std::string> actualSet(actual.begin(), actual.end());
    CHECK_EQ(actualSet.size(), expected.size());  // no duplicates
    for (const std::string& key : expected) {
        CHECK(actualSet.count(key) == 1);
    }
}

VCACHE_TEST(ClearEmptiesTheTableButKeepsItUsable) {
    HashTable table;
    for (int i = 0; i < 50; ++i) {
        table.put("key" + std::to_string(i), "v");
    }

    table.clear();
    CHECK_EQ(table.size(), std::size_t{0});
    CHECK(table.empty());
    CHECK(table.get("key0") == nullptr);

    table.put("fresh", "value");
    CHECK_EQ(*table.get("fresh"), std::string("value"));
}

VCACHE_TEST(HandlesEmptyKeysAndEmbeddedNulBytes) {
    // Keys arrive from the network in Phase 4 and are not guaranteed to be
    // printable, so std::string length -- not NUL termination -- must define them.
    HashTable table;
    const std::string binaryKey("bin\0ary", 7);
    const std::string binaryValue("val\0ue", 6);

    table.put("", "empty-key-value");
    table.put(binaryKey, binaryValue);

    CHECK_EQ(*table.get(""), std::string("empty-key-value"));
    CHECK_EQ(table.get(binaryKey)->size(), std::size_t{6});
    CHECK(*table.get(binaryKey) == binaryValue);
    CHECK(!table.contains("bin"));  // must not stop at the NUL
    CHECK_EQ(table.size(), std::size_t{2});
}

VCACHE_TEST(MoveTransfersOwnershipAndLeavesSourceUsable) {
    HashTable source;
    for (int i = 0; i < 100; ++i) {
        source.put("key" + std::to_string(i), "value" + std::to_string(i));
    }

    HashTable moved(std::move(source));
    CHECK_EQ(moved.size(), std::size_t{100});
    CHECK_EQ(*moved.get("key42"), std::string("value42"));

    // NOLINTNEXTLINE(bugprone-use-after-move) -- reuse after move is the point
    CHECK_EQ(source.size(), std::size_t{0});
    source.put("reborn", "ok");  // moved-from table must still work
    CHECK_EQ(*source.get("reborn"), std::string("ok"));

    HashTable assigned;
    assigned.put("discarded", "value");
    assigned = std::move(moved);
    CHECK_EQ(assigned.size(), std::size_t{100});
    CHECK(!assigned.contains("discarded"));  // old contents were released
    CHECK_EQ(*assigned.get("key7"), std::string("value7"));
}

int main() {
    return testing::runAll();
}
