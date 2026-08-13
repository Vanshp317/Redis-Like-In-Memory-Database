// Measures three claims made in earlier phases, each of which was flagged at
// the time as "Phase 9 should check this":
//
//   1. Phase 5 chose std::shared_mutex over a plain std::mutex on the argument
//      that cache reads dominate. Does the reader/writer lock actually win?
//   2. Phase 2 chose to return values by COPY from Database::get(). What does
//      that copy cost against returning a pointer?
//   3. Phase 8 admitted that enabling eviction turns every GET into a write to
//      the recency list. How much does that cost?
//
// Any of these could come back saying the earlier choice was wrong. That is the
// point of measuring rather than asserting.

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

#include "Database.h"
#include "HashTable.h"
#include "benchmark.h"

using bench::doNotOptimize;
using bench::makeKeys;
using vcache::Database;
using vcache::HashTable;

namespace {

constexpr std::size_t kKeyCount = 100000;
constexpr int kOpsPerThread = 200000;
constexpr int kReadPercent = 90;  // a cache workload: overwhelmingly reads

// --- three lock strategies over the same container ---------------------------

class NoLockTable {
public:
    void set(const std::string& k, const std::string& v) { table_.put(k, v); }
    std::optional<std::string> get(const std::string& k) {
        const std::string* found = table_.get(k);
        return found != nullptr ? std::optional<std::string>(*found) : std::nullopt;
    }

private:
    HashTable table_;
};

class MutexTable {
public:
    void set(const std::string& k, const std::string& v) {
        std::lock_guard<std::mutex> lock(mutex_);
        table_.put(k, v);
    }
    std::optional<std::string> get(const std::string& k) {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string* found = table_.get(k);
        return found != nullptr ? std::optional<std::string>(*found) : std::nullopt;
    }

private:
    HashTable table_;
    std::mutex mutex_;
};

// What Database actually does: shared for reads, exclusive for writes.
class SharedMutexTable {
public:
    void set(const std::string& k, const std::string& v) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        table_.put(k, v);
    }
    std::optional<std::string> get(const std::string& k) {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        const std::string* found = table_.get(k);
        return found != nullptr ? std::optional<std::string>(*found) : std::nullopt;
    }

private:
    HashTable table_;
    std::shared_mutex mutex_;
};

// A shared_mutex used exclusively for everything. Isolates the lock's own
// overhead from the benefit of actually sharing.
class ExclusiveSharedMutexTable {
public:
    void set(const std::string& k, const std::string& v) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        table_.put(k, v);
    }
    std::optional<std::string> get(const std::string& k) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        const std::string* found = table_.get(k);
        return found != nullptr ? std::optional<std::string>(*found) : std::nullopt;
    }

private:
    HashTable table_;
    std::shared_mutex mutex_;
};

// One timed pass. Threaded numbers are noisy -- scheduler decisions and
// cache-line ownership vary run to run -- so callers take a median of several.
template <typename Table>
double mixedThroughputOnce(int threadCount, const std::vector<std::string>& keys) {
    Table table;
    const std::string value(64, 'v');
    for (const std::string& key : keys) {
        table.set(key, value);
    }

    const auto worker = [&](int threadIndex) {
        // Each thread walks a different stride through the keyspace so they are
        // not all hammering the same cache lines in lockstep.
        std::size_t cursor = static_cast<std::size_t>(threadIndex) * 7919;
        for (int i = 0; i < kOpsPerThread; ++i) {
            const std::string& key = keys[cursor % keys.size()];
            cursor += 31;

            if (i % 100 < kReadPercent) {
                auto found = table.get(key);
                doNotOptimize(found);
            } else {
                table.set(key, value);
            }
        }
    };

    const auto start = bench::Clock::now();
    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(threadCount));
    for (int t = 0; t < threadCount; ++t) {
        threads.emplace_back(worker, t);
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    const double elapsed = bench::secondsSince(start);

    return static_cast<double>(threadCount) * kOpsPerThread / elapsed;
}

template <typename Table>
double mixedThroughput(int threadCount, const std::vector<std::string>& keys) {
    std::vector<double> results;
    for (int i = 0; i < 3; ++i) {
        results.push_back(mixedThroughputOnce<Table>(threadCount, keys));
    }
    std::sort(results.begin(), results.end());
    return results[1];
}

void benchmarkLockStrategies(const std::vector<std::string>& keys) {
    std::cout << "  " << kReadPercent << "% reads / " << (100 - kReadPercent) << "% writes, "
              << kKeyCount << " keys, " << kOpsPerThread << " ops per thread\n\n";

    std::cout << std::left << std::setw(30) << "  strategy" << std::right << std::setw(13)
              << "1 thread" << std::setw(13) << "2 threads" << std::setw(13) << "4 threads"
              << std::setw(13) << "8 threads" << "\n";
    std::cout << "  " << std::string(80, '-') << "\n";

    const int threadCounts[] = {1, 2, 4, 8};

    const auto row = [&](const std::string& label, auto runner) {
        std::cout << std::left << std::setw(30) << ("  " + label) << std::right << std::fixed
                  << std::setprecision(0);
        for (const int threads : threadCounts) {
            std::cout << std::setw(13) << runner(threads);
        }
        std::cout << "\n";
    };

    // No lock is not thread-safe; it is run at one thread only, as the ceiling
    // any locking strategy is measured against.
    std::cout << std::left << std::setw(30) << "  no lock (unsafe, ceiling)" << std::right
              << std::fixed << std::setprecision(0) << std::setw(13)
              << mixedThroughput<NoLockTable>(1, keys) << std::setw(13) << "-" << std::setw(13)
              << "-" << std::setw(13) << "-" << "\n";

    row("std::mutex", [&](int t) { return mixedThroughput<MutexTable>(t, keys); });
    row("std::shared_mutex (shared)",
        [&](int t) { return mixedThroughput<SharedMutexTable>(t, keys); });
    row("std::shared_mutex (excl)",
        [&](int t) { return mixedThroughput<ExclusiveSharedMutexTable>(t, keys); });
}

// --- the cost of copying values out ------------------------------------------

void benchmarkCopyCost(const std::vector<std::string>& keys) {
    for (const std::size_t valueSize : {std::size_t{16}, std::size_t{256}, std::size_t{4096}}) {
        const std::string value(valueSize, 'v');

        HashTable table;
        Database db;
        for (const std::string& key : keys) {
            table.put(key, value);
            db.set(key, value);
        }

        // The pointer path takes the SAME shared lock Database::get takes, so
        // the only difference measured is the copy itself rather than the copy
        // plus the lock.
        std::shared_mutex mutex;
        const double pointerSeconds = bench::timeMedian(3, [&] {
            for (const std::string& key : keys) {
                std::shared_lock<std::shared_mutex> lock(mutex);
                const std::string* found = table.get(key);
                doNotOptimize(found);
            }
        });

        const double copySeconds = bench::timeMedian(3, [&] {
            for (const std::string& key : keys) {
                auto found = db.get(key);
                doNotOptimize(found);
            }
        });

        const double pointerOps = static_cast<double>(keys.size()) / pointerSeconds;
        const double copyOps = static_cast<double>(keys.size()) / copySeconds;

        bench::printThroughput(std::to_string(valueSize) + "B, pointer under lock", pointerOps);
        bench::printThroughput(std::to_string(valueSize) + "B, Database::get (copies)", copyOps);
        bench::printNote("copy costs " + std::to_string(100.0 * (1.0 - copyOps / pointerOps)).substr(0, 4) +
                         "% of throughput at this size\n");
    }
}

// --- the cost of eviction bookkeeping ----------------------------------------

void benchmarkEvictionOverhead(const std::vector<std::string>& keys) {
    const std::string value(64, 'v');

    const auto measureOnce = [&](std::size_t maxMemory, int threadCount) {
        Database db;
        db.setMaxMemory(maxMemory);
        for (const std::string& key : keys) {
            db.set(key, value);
        }

        const auto worker = [&](int threadIndex) {
            std::size_t cursor = static_cast<std::size_t>(threadIndex) * 7919;
            for (int i = 0; i < kOpsPerThread; ++i) {
                auto found = db.get(keys[cursor % keys.size()]);
                cursor += 31;
                doNotOptimize(found);
            }
        };

        const auto start = bench::Clock::now();
        std::vector<std::thread> threads;
        for (int t = 0; t < threadCount; ++t) {
            threads.emplace_back(worker, t);
        }
        for (std::thread& thread : threads) {
            thread.join();
        }
        return static_cast<double>(threadCount) * kOpsPerThread / bench::secondsSince(start);
    };

    const auto measure = [&](std::size_t maxMemory, int threadCount) {
        std::vector<double> results;
        for (int i = 0; i < 3; ++i) {
            results.push_back(measureOnce(maxMemory, threadCount));
        }
        std::sort(results.begin(), results.end());
        return results[1];
    };

    // A limit large enough that nothing is ever actually evicted, so this
    // isolates the bookkeeping from the eviction work itself.
    const std::size_t roomyLimit = 4ull * 1024 * 1024 * 1024;

    std::cout << std::left << std::setw(30) << "  configuration" << std::right << std::setw(13)
              << "1 thread" << std::setw(13) << "2 threads" << std::setw(13) << "4 threads"
              << std::setw(13) << "8 threads" << "\n";
    std::cout << "  " << std::string(80, '-') << "\n";

    for (const auto& setup : {std::make_pair("no limit (no tracking)", std::size_t{0}),
                              std::make_pair("limit set (LRU tracking)", roomyLimit)}) {
        std::cout << std::left << std::setw(30) << ("  " + std::string(setup.first)) << std::right
                  << std::fixed << std::setprecision(0);
        for (const int threads : {1, 2, 4, 8}) {
            std::cout << std::setw(13) << measure(setup.second, threads);
        }
        std::cout << "\n";
    }
}

// --- does sharding actually deliver scaling? ---------------------------------

double shardedThroughputOnce(std::size_t shardCount, int threadCount,
                             const std::vector<std::string>& keys) {
    Database db(1024, shardCount);
    const std::string value(64, 'v');
    for (const std::string& key : keys) {
        db.set(key, value);
    }

    const auto worker = [&](int threadIndex) {
        std::size_t cursor = static_cast<std::size_t>(threadIndex) * 7919;
        for (int i = 0; i < kOpsPerThread; ++i) {
            const std::string& key = keys[cursor % keys.size()];
            cursor += 31;
            if (i % 100 < kReadPercent) {
                auto found = db.get(key);
                doNotOptimize(found);
            } else {
                db.set(key, value);
            }
        }
    };

    const auto start = bench::Clock::now();
    std::vector<std::thread> threads;
    for (int t = 0; t < threadCount; ++t) {
        threads.emplace_back(worker, t);
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    return static_cast<double>(threadCount) * kOpsPerThread / bench::secondsSince(start);
}

double shardedThroughput(std::size_t shardCount, int threadCount,
                         const std::vector<std::string>& keys) {
    std::vector<double> results;
    for (int i = 0; i < 3; ++i) {
        results.push_back(shardedThroughputOnce(shardCount, threadCount, keys));
    }
    std::sort(results.begin(), results.end());
    return results[1];
}

void benchmarkSharding(const std::vector<std::string>& keys) {
    std::cout << "  Same workload as above, through the real Database. One shard is the\n"
                 "  old single-lock design; the rest show what splitting it bought.\n\n";

    std::cout << std::left << std::setw(30) << "  shards" << std::right << std::setw(13)
              << "1 thread" << std::setw(13) << "2 threads" << std::setw(13) << "4 threads"
              << std::setw(13) << "8 threads" << "\n";
    std::cout << "  " << std::string(80, '-') << "\n";

    for (const std::size_t shardCount : {std::size_t{1}, std::size_t{4}, std::size_t{16},
                                         std::size_t{64}}) {
        std::cout << std::left << std::setw(30)
                  << ("  " + std::to_string(shardCount) +
                      (shardCount == 1 ? " (was the design)" : ""))
                  << std::right << std::fixed << std::setprecision(0);
        for (const int threads : {1, 2, 4, 8}) {
            std::cout << std::setw(13) << shardedThroughput(shardCount, threads, keys);
        }
        std::cout << "\n";
    }
}

}  // namespace

int main() {
    std::cout << "VCache database-layer benchmarks\n";
    std::cout << "Apple M3 Pro (11 cores), Apple clang 17, -O2, Release\n";
    std::cout << "GET-only rows are pure reads; mixed rows are " << kReadPercent << "% reads\n";

    const std::vector<std::string> keys = makeKeys(kKeyCount);

    bench::printHeading("Lock strategy (ops/sec, higher is better)");
    benchmarkLockStrategies(keys);

    bench::printHeading("Cost of returning values by copy");
    bench::printThroughputHeader();
    benchmarkCopyCost(keys);

    bench::printHeading("Cost of LRU tracking on GET (ops/sec)");
    benchmarkEvictionOverhead(keys);

    bench::printHeading("Sharding: does the database scale now? (ops/sec)");
    benchmarkSharding(keys);

    return 0;
}
