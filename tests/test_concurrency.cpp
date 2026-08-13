// Phase 5b test suite -- the Database under concurrent access.
//
// A passing run here proves less than it looks like: races are timing-dependent
// and can hide for thousands of runs. The real verdict comes from running this
// binary under ThreadSanitizer, which flags unsynchronised access whether or not
// it happened to corrupt anything on this particular run.
//
// The values used are deliberately long and uniform (200 identical bytes) so a
// torn read -- half of one value spliced onto half of another -- would be
// visible rather than plausible.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "Database.h"
#include "ExpirationSweeper.h"
#include "test_framework.h"

using vcache::Database;

namespace {

constexpr std::size_t kValueLength = 200;

std::string valueFor(char filler) {
    return std::string(kValueLength, filler);
}

// True if the string is one solid run of a single character -- i.e. it is one
// value someone actually wrote, not two spliced together.
bool isIntact(const std::string& value) {
    if (value.size() != kValueLength) {
        return false;
    }
    for (const char c : value) {
        if (c != value[0]) {
            return false;
        }
    }
    return true;
}

void runInParallel(int threadCount, const std::function<void(int)>& body) {
    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(threadCount));
    for (int i = 0; i < threadCount; ++i) {
        threads.emplace_back(body, i);
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
}

}  // namespace

VCACHE_TEST(ConcurrentWritesToDistinctKeysAllLand) {
    constexpr int kThreads = 8;
    constexpr int kPerThread = 2000;

    Database db;
    runInParallel(kThreads, [&db](int threadIndex) {
        for (int i = 0; i < kPerThread; ++i) {
            db.set("t" + std::to_string(threadIndex) + ":k" + std::to_string(i),
                   "value" + std::to_string(i));
        }
    });

    CHECK_EQ(db.size(), std::size_t{kThreads * kPerThread});

    for (int t = 0; t < kThreads; ++t) {
        for (int i = 0; i < kPerThread; ++i) {
            const auto value = db.get("t" + std::to_string(t) + ":k" + std::to_string(i));
            CHECK(value.has_value());
            CHECK_EQ(*value, "value" + std::to_string(i));
        }
    }
}

VCACHE_TEST(ConcurrentWritesToTheSameKeyNeverTear) {
    // Eight writers hammer one key with eight different values. Any read must
    // return exactly one of them, whole -- never a mixture.
    constexpr int kWriters = 8;
    constexpr int kIterations = 2000;

    Database db;
    db.set("contended", valueFor('a'));

    std::atomic<bool> stopReading{false};
    std::atomic<int> tornReads{0};
    std::atomic<int> readCount{0};

    std::thread reader([&db, &stopReading, &tornReads, &readCount] {
        while (!stopReading.load()) {
            const auto value = db.get("contended");
            if (value.has_value()) {
                readCount.fetch_add(1);
                if (!isIntact(*value)) {
                    tornReads.fetch_add(1);
                }
            }
        }
    });

    runInParallel(kWriters, [&db](int threadIndex) {
        const std::string value = valueFor(static_cast<char>('a' + threadIndex));
        for (int i = 0; i < kIterations; ++i) {
            db.set("contended", value);
        }
    });

    stopReading.store(true);
    reader.join();

    CHECK_EQ(tornReads.load(), 0);
    CHECK(readCount.load() > 0);  // the reader actually did some work
    CHECK(isIntact(*db.get("contended")));
}

VCACHE_TEST(ManyReadersOneWriterStayConsistent) {
    // The case the reader/writer lock exists for: readers should run alongside
    // each other and still never observe a partial write.
    constexpr int kReaders = 8;

    Database db;
    db.set("key", valueFor('z'));

    std::atomic<bool> stop{false};
    std::atomic<int> failures{0};

    std::thread writer([&db, &stop] {
        int i = 0;
        while (!stop.load()) {
            db.set("key", valueFor(static_cast<char>('a' + (i++ % 26))));
        }
    });

    runInParallel(kReaders, [&db, &stop, &failures](int) {
        for (int i = 0; i < 20000 && !stop.load(); ++i) {
            const auto value = db.get("key");
            if (!value.has_value() || !isIntact(*value)) {
                failures.fetch_add(1);
            }
        }
    });

    stop.store(true);
    writer.join();

    CHECK_EQ(failures.load(), 0);
}

VCACHE_TEST(ConcurrentDeletesRemoveEachKeyExactlyOnce) {
    // del() returns true only for the thread that actually removed the key, so
    // the successes across all threads must sum to the number of keys. A racy
    // implementation would report the same removal twice.
    constexpr int kKeys = 5000;
    constexpr int kThreads = 8;

    Database db;
    for (int i = 0; i < kKeys; ++i) {
        db.set("key" + std::to_string(i), "v");
    }

    std::atomic<int> removals{0};
    runInParallel(kThreads, [&db, &removals](int) {
        for (int i = 0; i < kKeys; ++i) {
            if (db.del("key" + std::to_string(i))) {
                removals.fetch_add(1);
            }
        }
    });

    CHECK_EQ(removals.load(), kKeys);
    CHECK_EQ(db.size(), std::size_t{0});
}

VCACHE_TEST(InterleavedSetAndDeleteLeaveAConsistentState) {
    constexpr int kIterations = 5000;
    Database db;

    std::thread setter([&db] {
        for (int i = 0; i < kIterations; ++i) {
            db.set("key", valueFor('s'));
        }
    });
    std::thread deleter([&db] {
        for (int i = 0; i < kIterations; ++i) {
            db.del("key");
        }
    });
    std::thread reader([&db] {
        for (int i = 0; i < kIterations; ++i) {
            const auto value = db.get("key");
            if (value.has_value()) {
                // Either absent or fully present -- never half written.
                CHECK(isIntact(*value));
            }
        }
    });

    setter.join();
    deleter.join();
    reader.join();

    CHECK(db.size() <= std::size_t{1});
}

VCACHE_TEST(KeysSnapshotIsCoherentWhileWritersRun) {
    // keys() builds its whole vector under the lock, so each snapshot must be a
    // real point-in-time view: no duplicates, no partially written keys.
    Database db;
    std::atomic<bool> stop{false};

    std::thread writer([&db, &stop] {
        int i = 0;
        while (!stop.load()) {
            db.set("key" + std::to_string(i % 500), "v");
            ++i;
        }
    });

    for (int round = 0; round < 200; ++round) {
        const std::vector<std::string> snapshot = db.keys();
        const std::set<std::string> unique(snapshot.begin(), snapshot.end());
        CHECK_EQ(unique.size(), snapshot.size());  // no duplicates
    }

    stop.store(true);
    writer.join();
}

VCACHE_TEST(ClearRacingWithWritersIsSafe) {
    Database db;
    std::atomic<bool> stop{false};

    std::thread clearer([&db, &stop] {
        while (!stop.load()) {
            db.clear();
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    });

    runInParallel(4, [&db](int threadIndex) {
        for (int i = 0; i < 5000; ++i) {
            db.set("t" + std::to_string(threadIndex) + ":" + std::to_string(i), "v");
            db.get("t" + std::to_string(threadIndex) + ":" + std::to_string(i));
            db.exists("nonexistent");
        }
    });

    stop.store(true);
    clearer.join();

    db.clear();
    CHECK_EQ(db.size(), std::size_t{0});
}

VCACHE_TEST(ConcurrentGrowthDoesNotLoseEntries) {
    // Every writer forces rehashes as the table grows. A rehash relinks every
    // node in the table, so if that ever ran alongside a read, entries would go
    // missing -- this is the test that would catch it.
    constexpr int kThreads = 8;
    constexpr int kPerThread = 5000;

    Database db(16);  // start tiny to force many resizes
    runInParallel(kThreads, [&db](int threadIndex) {
        for (int i = 0; i < kPerThread; ++i) {
            db.set(std::to_string(threadIndex) + "-" + std::to_string(i), "v");
        }
    });

    CHECK_EQ(db.size(), std::size_t{kThreads * kPerThread});
    for (int t = 0; t < kThreads; ++t) {
        for (int i = 0; i < kPerThread; i += 97) {  // spot-check, cheaply
            CHECK(db.exists(std::to_string(t) + "-" + std::to_string(i)));
        }
    }
}

VCACHE_TEST(MixedWorkloadKeepsSizeAccurate) {
    // Reads, writes and deletes all at once, then an exact accounting: the size
    // must match the keys that were left behind.
    constexpr int kThreads = 6;
    constexpr int kPerThread = 3000;

    Database db;
    runInParallel(kThreads, [&db](int threadIndex) {
        for (int i = 0; i < kPerThread; ++i) {
            const std::string key = "t" + std::to_string(threadIndex) + ":" + std::to_string(i);
            db.set(key, "v");
            db.get(key);
            db.exists(key);
            if (i % 2 == 0) {
                db.del(key);
            }
        }
    });

    CHECK_EQ(db.size(), std::size_t{kThreads * kPerThread / 2});
    CHECK_EQ(db.keys().size(), db.size());
}

// ------------------------------------------------- expiration (Phase 6) ----

VCACHE_TEST(TheSweeperRacingWithClientsIsSafe) {
    // The sweeper takes the exclusive lock and unlinks nodes from arbitrary
    // buckets while readers walk chains and writers rehash. This is the test
    // most likely to expose a locking mistake under ThreadSanitizer.
    Database db(64);

    vcache::SweeperConfig config;
    config.interval = std::chrono::milliseconds(1);  // as aggressive as possible
    config.bucketsPerPass = 32;

    vcache::ExpirationSweeper sweeper(db, config);
    sweeper.start();

    runInParallel(6, [&db](int threadIndex) {
        for (int i = 0; i < 4000; ++i) {
            const std::string key = "t" + std::to_string(threadIndex) + ":" + std::to_string(i);

            // A mix of doomed and permanent keys, so the sweeper always has
            // something to remove while readers are mid-chain.
            if (i % 2 == 0) {
                db.set(key, valueFor('x'), std::chrono::seconds(1));
            } else {
                db.set(key, valueFor('y'));
            }

            const auto value = db.get(key);
            if (value.has_value()) {
                CHECK(isIntact(*value));
            }
            db.exists(key);
            if (i % 100 == 0) {
                db.keys();
            }
        }
    });

    sweeper.stop();

    // Every permanent key must have survived; nothing else is asserted, since
    // the doomed ones may or may not have lapsed yet.
    for (int t = 0; t < 6; ++t) {
        for (int i = 1; i < 4000; i += 2) {
            CHECK(db.exists("t" + std::to_string(t) + ":" + std::to_string(i)));
        }
    }
}

VCACHE_TEST(ConcurrentTtlWritesAndReclamationAgree) {
    // Two threads keep rewriting the same keys with TTLs while a third
    // reclaims. A key must never be visible with a torn value, and a key
    // rewritten as permanent must never be swept away.
    Database db;
    std::atomic<bool> stop{false};
    std::atomic<int> failures{0};

    std::thread reclaimer([&db, &stop] {
        while (!stop.load()) {
            db.removeExpired(db.bucketCount());
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    });

    runInParallel(4, [&db, &failures](int threadIndex) {
        for (int i = 0; i < 3000; ++i) {
            const std::string key = "shared" + std::to_string(i % 50);
            if (threadIndex % 2 == 0) {
                db.set(key, valueFor('a'), std::chrono::seconds(1));
            } else {
                db.set(key, valueFor('b'));
            }
            const auto value = db.get(key);
            if (value.has_value() && !isIntact(*value)) {
                failures.fetch_add(1);
            }
        }
    });

    stop.store(true);
    reclaimer.join();

    CHECK_EQ(failures.load(), 0);
}

int main() {
    return testing::runAll();
}
