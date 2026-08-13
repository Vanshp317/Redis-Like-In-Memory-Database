// Phase 8 test suite -- memory accounting and LRU eviction.
//
// The tests assert on ORDER and on CONSISTENCY, never on an absolute byte
// count. The accounting is an estimate (see kAllocationSlack in HashTable.cpp),
// so a test demanding "usage == 4096" would be asserting the estimate's
// arithmetic rather than the behaviour that matters: that the right key gets
// evicted, and that memory returns to where it started.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <string>
#include <thread>
#include <vector>

#include "Database.h"
#include "test_framework.h"

using vcache::Database;
using vcache::SetOutcome;
using namespace std::chrono_literals;

namespace {

// Values large enough that the per-entry constant does not dominate, so a
// limit expressed in "about N entries" behaves predictably.
constexpr std::size_t kValueSize = 1000;

std::string valueOfSize(std::size_t bytes) {
    return std::string(bytes, 'v');
}

// A limit that holds exactly `entries` values of kValueSize and no more.
//
// Derived from the real footprint estimate rather than from a guessed constant.
// A hand-picked number would silently stop testing anything the moment the
// per-entry overhead changed -- which is exactly what happened on the first run
// of this suite, where a padded limit was never reached and six ordering tests
// passed vacuously.
std::size_t limitForEntries(std::size_t entries) {
    // A key at least as long as any used below, so the limit is never too tight.
    const std::size_t perEntry =
        vcache::HashTable::footprintEstimate("longest-key", valueOfSize(kValueSize));

    // Plus the initial bucket array, which counts toward usage.
    return entries * perEntry + vcache::HashTable::kDefaultBucketCount * sizeof(void*);
}

// Exact LRU ordering is a WITHIN-SHARD guarantee. With the keyspace split, the
// globally-oldest key may live in a shard that is under its own budget and so
// survives while a newer key elsewhere is evicted -- documented in Database.h
// as approximate global LRU.
//
// The ordering tests below therefore use a single shard deliberately: they are
// testing the eviction policy, not the sharding. Sharded behaviour has its own
// tests at the end of this file.
constexpr std::size_t kOneShard = 1;

std::size_t singleShardBuckets() {
    return vcache::HashTable::kDefaultBucketCount;
}

}  // namespace

// ---------------------------------------------------------- no limit set ----

VCACHE_TEST(WithoutALimitNothingIsEverEvicted) {
    Database db;
    CHECK_EQ(db.maxMemory(), std::size_t{0});

    for (int i = 0; i < 2000; ++i) {
        db.set("key" + std::to_string(i), valueOfSize(kValueSize));
    }

    CHECK_EQ(db.size(), std::size_t{2000});
    CHECK_EQ(db.evictedCount(), std::size_t{0});
}

VCACHE_TEST(MemoryUsageGrowsWithStoredData) {
    Database db;
    const std::size_t empty = db.memoryUsage();

    db.set("key", valueOfSize(10000));
    const std::size_t filled = db.memoryUsage();

    CHECK(filled > empty);
    CHECK(filled - empty >= std::size_t{10000});  // at least the value itself
}

VCACHE_TEST(MemoryUsageReturnsToBaselineAfterRemoval) {
    // The accounting is incremental, so a leak here would show as a total that
    // never comes back down.
    Database db;
    const std::size_t baseline = db.memoryUsage();

    for (int i = 0; i < 500; ++i) {
        db.set("key" + std::to_string(i), valueOfSize(kValueSize));
    }
    CHECK(db.memoryUsage() > baseline);

    for (int i = 0; i < 500; ++i) {
        db.del("key" + std::to_string(i));
    }

    // Not equal to the baseline: the bucket array grew and does not shrink.
    // What matters is that every entry's bytes were given back.
    CHECK_EQ(db.size(), std::size_t{0});
    CHECK(db.memoryUsage() < baseline + db.bucketCount() * sizeof(void*) + 64);
}

VCACHE_TEST(OverwritingAdjustsAccountingByTheDifference) {
    Database db;
    db.set("key", valueOfSize(10000));
    const std::size_t large = db.memoryUsage();

    db.set("key", valueOfSize(10));
    const std::size_t small = db.memoryUsage();

    CHECK(small < large);
    CHECK_EQ(db.size(), std::size_t{1});  // still one key, just a smaller one
}

VCACHE_TEST(ClearResetsAccounting) {
    Database db;
    for (int i = 0; i < 200; ++i) {
        db.set("key" + std::to_string(i), valueOfSize(kValueSize));
    }

    db.clear();
    CHECK_EQ(db.size(), std::size_t{0});
    CHECK_EQ(db.memoryUsage(), db.bucketCount() * sizeof(void*));
}

// -------------------------------------------------------------- eviction ----

VCACHE_TEST(ExceedingTheLimitEvicts) {
    Database db(singleShardBuckets(), kOneShard);
    db.setMaxMemory(limitForEntries(10));

    for (int i = 0; i < 100; ++i) {
        db.set("key" + std::to_string(i), valueOfSize(kValueSize));
    }

    CHECK(db.size() < std::size_t{100});           // some were evicted
    CHECK(db.size() > std::size_t{0});             // but not everything
    CHECK(db.evictedCount() > std::size_t{0});
    CHECK(db.memoryUsage() <= db.maxMemory());
}

VCACHE_TEST(TheLeastRecentlyUsedKeyIsTheOneEvicted) {
    // The core guarantee. Three keys, room for about two: writing a fourth must
    // take the oldest, not an arbitrary one.
    Database db(singleShardBuckets(), kOneShard);
    db.setMaxMemory(limitForEntries(3));

    db.set("first", valueOfSize(kValueSize));
    db.set("second", valueOfSize(kValueSize));
    db.set("third", valueOfSize(kValueSize));
    CHECK_EQ(db.size(), std::size_t{3});

    db.set("fourth", valueOfSize(kValueSize));

    CHECK(!db.exists("first"));   // oldest went
    CHECK(db.exists("second"));
    CHECK(db.exists("third"));
    CHECK(db.exists("fourth"));
    CHECK_EQ(db.evictedCount(), std::size_t{1});
}

VCACHE_TEST(ReadingAKeyProtectsItFromEviction) {
    // What separates LRU from FIFO. "first" was written earliest, but reading
    // it makes it the most recent, so "second" becomes the victim instead.
    Database db(singleShardBuckets(), kOneShard);
    db.setMaxMemory(limitForEntries(3));

    db.set("first", valueOfSize(kValueSize));
    db.set("second", valueOfSize(kValueSize));
    db.set("third", valueOfSize(kValueSize));

    CHECK(db.get("first").has_value());  // promotes "first"

    db.set("fourth", valueOfSize(kValueSize));

    CHECK(db.exists("first"));    // saved by the read
    CHECK(!db.exists("second"));  // now the oldest
    CHECK(db.exists("third"));
    CHECK(db.exists("fourth"));
}

VCACHE_TEST(OverwritingAKeyAlsoPromotesIt) {
    Database db(singleShardBuckets(), kOneShard);
    db.setMaxMemory(limitForEntries(3));

    db.set("first", valueOfSize(kValueSize));
    db.set("second", valueOfSize(kValueSize));
    db.set("third", valueOfSize(kValueSize));

    db.set("first", valueOfSize(kValueSize));  // a write is a use too

    db.set("fourth", valueOfSize(kValueSize));

    CHECK(db.exists("first"));
    CHECK(!db.exists("second"));
}

VCACHE_TEST(RepeatedReadsOfOneKeyKeepItAlive) {
    // A hot key must survive an arbitrary amount of churn around it.
    Database db(singleShardBuckets(), kOneShard);
    db.setMaxMemory(limitForEntries(5));
    db.set("hot", valueOfSize(kValueSize));

    for (int i = 0; i < 200; ++i) {
        db.set("cold" + std::to_string(i), valueOfSize(kValueSize));
        CHECK(db.get("hot").has_value());
    }

    CHECK(db.exists("hot"));
    CHECK(db.evictedCount() > std::size_t{100});
}

VCACHE_TEST(EvictionOrderFollowsAccessOrderNotInsertionOrder) {
    Database db(singleShardBuckets(), kOneShard);
    db.setMaxMemory(limitForEntries(4));

    db.set("a", valueOfSize(kValueSize));
    db.set("b", valueOfSize(kValueSize));
    db.set("c", valueOfSize(kValueSize));
    db.set("d", valueOfSize(kValueSize));

    // Access in reverse, making "d" the oldest.
    db.get("c");
    db.get("b");
    db.get("a");

    db.set("e", valueOfSize(kValueSize));
    CHECK(!db.exists("d"));

    db.set("f", valueOfSize(kValueSize));
    CHECK(!db.exists("c"));

    CHECK(db.exists("a"));
    CHECK(db.exists("b"));
}

VCACHE_TEST(ExistsAndKeysDoNotCountAsUse) {
    // Documented behaviour: introspection is not access. If EXISTS promoted a
    // key, a monitoring loop calling EXISTS on everything would flatten the
    // recency order and make eviction arbitrary.
    Database db(singleShardBuckets(), kOneShard);
    db.setMaxMemory(limitForEntries(3));

    db.set("first", valueOfSize(kValueSize));
    db.set("second", valueOfSize(kValueSize));
    db.set("third", valueOfSize(kValueSize));

    CHECK(db.exists("first"));
    db.keys();

    db.set("fourth", valueOfSize(kValueSize));
    CHECK(!db.exists("first"));  // EXISTS did not save it
}

VCACHE_TEST(DeletingFreesRoomSoNothingIsEvicted) {
    Database db(singleShardBuckets(), kOneShard);
    db.setMaxMemory(limitForEntries(3));

    db.set("a", valueOfSize(kValueSize));
    db.set("b", valueOfSize(kValueSize));
    db.set("c", valueOfSize(kValueSize));

    db.del("a");
    db.set("d", valueOfSize(kValueSize));

    CHECK_EQ(db.evictedCount(), std::size_t{0});
    CHECK(db.exists("b"));
    CHECK(db.exists("c"));
    CHECK(db.exists("d"));
}

// ------------------------------------------------------- limit management ----

VCACHE_TEST(LoweringTheLimitEvictsImmediately) {
    Database db(singleShardBuckets(), kOneShard);
    for (int i = 0; i < 100; ++i) {
        db.set("key" + std::to_string(i), valueOfSize(kValueSize));
    }
    CHECK_EQ(db.size(), std::size_t{100});

    db.setMaxMemory(limitForEntries(10));

    CHECK(db.size() < std::size_t{100});
    CHECK(db.memoryUsage() <= db.maxMemory());
    CHECK(db.exists("key99"));  // the most recent survives
}

VCACHE_TEST(RemovingTheLimitStopsEviction) {
    Database db(singleShardBuckets(), kOneShard);
    db.setMaxMemory(limitForEntries(5));
    for (int i = 0; i < 50; ++i) {
        db.set("key" + std::to_string(i), valueOfSize(kValueSize));
    }
    const std::size_t evictedSoFar = db.evictedCount();
    CHECK(evictedSoFar > std::size_t{0});

    db.setMaxMemory(0);
    for (int i = 50; i < 150; ++i) {
        db.set("key" + std::to_string(i), valueOfSize(kValueSize));
    }

    CHECK_EQ(db.evictedCount(), evictedSoFar);  // no further evictions
}

VCACHE_TEST(AnEntryLargerThanTheLimitIsRejected) {
    // Storing it and then evicting it immediately would report success while
    // losing the data. Rejecting says so.
    Database db(singleShardBuckets(), kOneShard);
    db.setMaxMemory(4096);

    db.set("small", "fits");
    const auto outcome = db.set("huge", valueOfSize(100000));

    CHECK(outcome == SetOutcome::Rejected);
    CHECK(!db.exists("huge"));
    CHECK(db.exists("small"));  // the existing keyspace was not disturbed
}

VCACHE_TEST(SetOutcomeDistinguishesInsertFromUpdate) {
    Database db;
    CHECK(db.set("key", "first") == SetOutcome::Inserted);
    CHECK(db.set("key", "second") == SetOutcome::Updated);
    db.del("key");
    CHECK(db.set("key", "third") == SetOutcome::Inserted);
}

VCACHE_TEST(ATinyLimitLeavesAtLeastOneKey) {
    // The bucket array counts toward usage and never shrinks, so a limit below
    // it would loop forever if eviction did not stop at one entry.
    Database db(singleShardBuckets(), kOneShard);
    db.setMaxMemory(1);

    db.set("key", "value");
    CHECK(db.size() <= std::size_t{1});
    db.set("other", "value");
    CHECK(db.size() <= std::size_t{1});  // terminates rather than spinning
}

// -------------------------------------------- interaction with other phases ----

VCACHE_TEST(ExpiredKeysAreNotPromotedByAFailedRead) {
    Database db(singleShardBuckets(), kOneShard);
    db.setMaxMemory(limitForEntries(3));

    db.set("doomed", valueOfSize(kValueSize), 1s);
    db.set("b", valueOfSize(kValueSize));
    db.set("c", valueOfSize(kValueSize));

    std::this_thread::sleep_for(1200ms);
    CHECK(!db.get("doomed").has_value());  // a miss, so not a use

    db.set("d", valueOfSize(kValueSize));
    CHECK(!db.exists("doomed"));
}

VCACHE_TEST(TheSweeperFreesMemoryForTheAccounting) {
    Database db;
    for (int i = 0; i < 100; ++i) {
        db.set("key" + std::to_string(i), valueOfSize(kValueSize), 1s);
    }
    const std::size_t beforeExpiry = db.memoryUsage();

    std::this_thread::sleep_for(1200ms);
    // Wide enough that each shard's share covers all of its own buckets.
    db.removeExpired(db.bucketCount() * db.shardCount());

    CHECK_EQ(db.size(), std::size_t{0});
    CHECK(db.memoryUsage() < beforeExpiry);
}

VCACHE_TEST(RestoringRebuildsRecencyInFileOrder) {
    // A snapshot does not record recency, so restore order stands in for it.
    // Documented behaviour, pinned down here so it cannot drift silently.
    Database source;
    source.set("oldest", valueOfSize(kValueSize));
    source.set("middle", valueOfSize(kValueSize));
    source.set("newest", valueOfSize(kValueSize));

    const std::vector<vcache::Entry> entries = source.snapshot();

    Database restored(singleShardBuckets(), kOneShard);
    restored.restore(entries);
    restored.setMaxMemory(limitForEntries(3));
    CHECK_EQ(restored.size(), std::size_t{3});

    restored.set("extra", valueOfSize(kValueSize));

    // Whatever was restored first is now the victim.
    CHECK_EQ(restored.size(), std::size_t{3});
    CHECK(restored.exists("extra"));
}

// ------------------------------------------------------------ concurrency ----

VCACHE_TEST(EvictionUnderConcurrentLoadStaysConsistent) {
    // Eviction unlinks nodes from the recency list under the exclusive lock
    // while readers promote other nodes under a shared one. This is the test
    // that would expose a mistake in that split, under ThreadSanitizer.
    Database db;
    // Scaled to the shard count: eviction leaves at least one entry per shard,
    // so a limit below shardCount entries cannot be met. See Database.h.
    db.setMaxMemory(limitForEntries(db.shardCount() * 4));

    std::vector<std::thread> threads;
    for (int t = 0; t < 6; ++t) {
        threads.emplace_back([&db, t] {
            for (int i = 0; i < 2000; ++i) {
                const std::string key = "t" + std::to_string(t) + ":" + std::to_string(i);
                db.set(key, valueOfSize(kValueSize));

                const auto value = db.get(key);
                if (value.has_value()) {
                    // Anything readable must be whole, never a partial value.
                    CHECK_EQ(value->size(), kValueSize);
                }
                db.get("t" + std::to_string(t) + ":" + std::to_string(i / 2));
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }

    CHECK(db.memoryUsage() <= db.maxMemory());
    CHECK(db.evictedCount() > std::size_t{0});
    CHECK_EQ(db.keys().size(), db.size());  // accounting and contents agree
}

VCACHE_TEST(ConcurrentReadsPromoteWithoutCorruptingTheList) {
    // Many threads promoting the same small set of keys at once: if the recency
    // list were unguarded, its pointers would tangle and eviction would either
    // loop or lose entries.
    //
    // Every thread does a BOUNDED amount of work, deliberately. An earlier
    // version ran the readers in `while (!stop)` loops and let the writer set
    // the flag when it finished. That deadlocked in practice on Linux: glibc's
    // std::shared_mutex prefers readers, so eight threads continuously taking
    // the shared lock starved the writer, the writer never reached the flag,
    // and the readers never stopped. It passed on macOS, where libc++ lets the
    // writer in, and CI caught it on the first Linux run.
    //
    // The starvation is a real property of a single reader/writer lock, not a
    // test artifact -- see the note in Database.h. Bounding the loops means
    // this test measures recency-list integrity rather than lock fairness.
    Database db;
    db.setMaxMemory(limitForEntries(db.shardCount() * 4));
    for (int i = 0; i < 10; ++i) {
        db.set("shared" + std::to_string(i), valueOfSize(kValueSize));
    }

    constexpr int kReadRounds = 500;

    std::vector<std::thread> readers;
    for (int t = 0; t < 8; ++t) {
        readers.emplace_back([&db] {
            for (int round = 0; round < kReadRounds; ++round) {
                for (int i = 0; i < 10; ++i) {
                    db.get("shared" + std::to_string(i));
                }
            }
        });
    }

    std::thread writer([&db] {
        for (int i = 0; i < 500; ++i) {
            db.set("churn" + std::to_string(i), valueOfSize(kValueSize));
        }
    });

    for (std::thread& reader : readers) {
        reader.join();
    }
    writer.join();

    CHECK(db.memoryUsage() <= db.maxMemory());
    CHECK_EQ(db.keys().size(), db.size());
}

// ------------------------------------------------------------- sharding ----

VCACHE_TEST(ShardCountIsRoundedUpToAPowerOfTwo) {
    // Shard selection masks bits, which is only a valid modulo on a power of
    // two. A request for 5 must become 8 rather than silently misrouting keys.
    CHECK_EQ(Database(16, 1).shardCount(), std::size_t{1});
    CHECK_EQ(Database(16, 5).shardCount(), std::size_t{8});
    CHECK_EQ(Database(16, 16).shardCount(), std::size_t{16});
    CHECK_EQ(Database(16, 0).shardCount(), std::size_t{1});  // zero is meaningless
}

VCACHE_TEST(EveryKeyIsReachableWhicheverShardItLandsIn) {
    // Routing must be a pure function of the key: written to one shard, found
    // in the same one. A hash mismatch between put and get would show up as
    // keys that vanish.
    Database db(1024, 16);
    constexpr int kCount = 20000;

    for (int i = 0; i < kCount; ++i) {
        db.set("key" + std::to_string(i), "value" + std::to_string(i));
    }
    CHECK_EQ(db.size(), std::size_t{kCount});

    for (int i = 0; i < kCount; ++i) {
        const auto value = db.get("key" + std::to_string(i));
        CHECK(value.has_value());
        CHECK_EQ(*value, "value" + std::to_string(i));
    }
    CHECK_EQ(db.keys().size(), std::size_t{kCount});
}

VCACHE_TEST(KeysSpreadEvenlyAcrossShards) {
    // Shard selection takes the HIGH bits of the hash and the bucket index
    // takes the LOW bits. If both came from the same end, every key in a shard
    // would also share a bucket -- one giant collision chain per shard.
    //
    // Measured through bucket counts: even spreading means shards grow their
    // tables to similar sizes.
    Database db(16, 16);
    for (int i = 0; i < 16000; ++i) {
        db.set("user:" + std::to_string(i), "v");
    }

    CHECK_EQ(db.size(), std::size_t{16000});

    // 16000 keys over 16 shards is 1000 each; at a 0.75 load factor that is
    // 2048 buckets per shard, so about 32768 in total. Wild imbalance would
    // push the total far above that.
    const std::size_t buckets = db.bucketCount();
    CHECK(buckets >= std::size_t{16 * 1024});
    CHECK(buckets <= std::size_t{16 * 8192});
}

VCACHE_TEST(ShardedEvictionKeepsTotalMemoryBounded) {
    // The guarantee that survives sharding: the total stays under the limit.
    // Which particular key goes is what becomes approximate.
    Database db(256, 16);
    db.setMaxMemory(limitForEntries(16 * 10));  // ten entries per shard

    for (int i = 0; i < 5000; ++i) {
        db.set("key" + std::to_string(i), valueOfSize(kValueSize));
    }

    CHECK(db.memoryUsage() <= db.maxMemory());
    CHECK(db.evictedCount() > std::size_t{0});
    CHECK_EQ(db.keys().size(), db.size());
}

VCACHE_TEST(ShardedEvictionIsApproximateNotExactLru) {
    // Documents the cost of sharding rather than pretending it away.
    //
    // Five keys over sixteen shards, with room for four. Each shard holds at
    // most one of them and is under its own slice of the budget, so NOTHING is
    // evicted -- the globally-oldest key survives, and the total sits above the
    // configured limit because eviction never takes a shard below one entry.
    Database sharded(16, 16);
    sharded.setMaxMemory(limitForEntries(4));

    for (const char* key : {"first", "second", "third", "fourth", "fifth"}) {
        sharded.set(key, valueOfSize(kValueSize));
    }

    CHECK_EQ(sharded.evictedCount(), std::size_t{0});   // no shard was over
    CHECK(sharded.exists("first"));                     // the oldest survived

    // The same sequence through one shard evicts exactly the oldest key.
    Database exact(16, 1);
    exact.setMaxMemory(limitForEntries(4));
    for (const char* key : {"first", "second", "third", "fourth", "fifth"}) {
        exact.set(key, valueOfSize(kValueSize));
    }

    CHECK(!exact.exists("first"));
    CHECK(exact.exists("fifth"));
    CHECK_EQ(exact.evictedCount(), std::size_t{1});
}

VCACHE_TEST(TheLimitIsHonouredOnceItExceedsTheShardFloor) {
    // The bound does hold, provided the budget is above the floor of one entry
    // per shard. Below that, eviction cannot get there -- see Database.h.
    Database db(256, 16);
    db.setMaxMemory(limitForEntries(16 * 8));  // eight entries per shard

    for (int i = 0; i < 4000; ++i) {
        db.set("key" + std::to_string(i), valueOfSize(kValueSize));
    }

    CHECK(db.memoryUsage() <= db.maxMemory());
    CHECK(db.evictedCount() > std::size_t{0});
}

VCACHE_TEST(ShardsAreWrittenInParallelWithoutLoss) {
    // The point of the whole exercise: many threads writing at once, each
    // touching whichever shard its key hashes to.
    Database db(1024, 16);
    constexpr int kThreads = 8;
    constexpr int kPerThread = 5000;

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&db, t] {
            for (int i = 0; i < kPerThread; ++i) {
                db.set("t" + std::to_string(t) + ":" + std::to_string(i), "v");
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }

    CHECK_EQ(db.size(), std::size_t{kThreads * kPerThread});
    for (int t = 0; t < kThreads; ++t) {
        for (int i = 0; i < kPerThread; i += 97) {
            CHECK(db.exists("t" + std::to_string(t) + ":" + std::to_string(i)));
        }
    }
}

VCACHE_TEST(ASingleShardBehavesLikeTheUnshardedDatabase) {
    // The escape hatch works: shardCount 1 is the old behaviour exactly.
    Database db(64, 1);
    CHECK_EQ(db.shardCount(), std::size_t{1});

    for (int i = 0; i < 1000; ++i) {
        db.set("key" + std::to_string(i), "v");
    }
    CHECK_EQ(db.size(), std::size_t{1000});
    CHECK_EQ(db.keys().size(), std::size_t{1000});

    db.clear();
    CHECK_EQ(db.size(), std::size_t{0});
}

int main() {
    return testing::runAll();
}
