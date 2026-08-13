// Custom HashTable versus std::unordered_map.
//
// The comparison Discovery Document section 16 asks for, and the one that
// decides whether writing the table by hand bought anything or merely cost
// time. Both containers see identical keys in identical order.
//
// The comparison is not entirely apples to apples, and that is worth stating
// rather than hiding: VCache's node also carries an expiration field and two
// recency pointers that std::unordered_map has no equivalent of. Those cost
// memory and cache locality, so a tie on speed would still mean the custom
// table is doing more work per byte.

#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "HashTable.h"
#include "benchmark.h"

using bench::doNotOptimize;
using bench::makeKeys;
using vcache::HashTable;

namespace {

constexpr int kRepetitions = 5;

void benchmarkInsertion(std::size_t count) {
    const std::vector<std::string> keys = makeKeys(count);
    const std::string value(64, 'v');

    const double custom = bench::timeMedian(kRepetitions, [&] {
        HashTable table;
        for (const std::string& key : keys) {
            table.put(key, value);
        }
        doNotOptimize(table);
    });

    const double standard = bench::timeMedian(kRepetitions, [&] {
        std::unordered_map<std::string, std::string> map;
        for (const std::string& key : keys) {
            map[key] = value;
        }
        doNotOptimize(map);
    });

    bench::printThroughput("VCache HashTable::put", static_cast<double>(count) / custom);
    bench::printThroughput("std::unordered_map insert", static_cast<double>(count) / standard);
    bench::printNote("ratio: VCache is " + std::to_string(standard / custom).substr(0, 4) +
                     "x the throughput of std::unordered_map");
}

void benchmarkLookupHit(std::size_t count) {
    const std::vector<std::string> keys = makeKeys(count);
    const std::string value(64, 'v');

    HashTable table;
    std::unordered_map<std::string, std::string> map;
    for (const std::string& key : keys) {
        table.put(key, value);
        map[key] = value;
    }

    const double custom = bench::timeMedian(kRepetitions, [&] {
        for (const std::string& key : keys) {
            const std::string* found = table.get(key);
            doNotOptimize(found);
        }
    });

    const double standard = bench::timeMedian(kRepetitions, [&] {
        for (const std::string& key : keys) {
            auto it = map.find(key);
            doNotOptimize(it);
        }
    });

    bench::printThroughput("VCache HashTable::get (hit)", static_cast<double>(count) / custom);
    bench::printThroughput("std::unordered_map find (hit)", static_cast<double>(count) / standard);
    bench::printNote("ratio: VCache is " + std::to_string(standard / custom).substr(0, 4) +
                     "x the throughput of std::unordered_map");
}

void benchmarkLookupMiss(std::size_t count) {
    const std::vector<std::string> present = makeKeys(count);
    const std::vector<std::string> absent = makeKeys(count, "absent:");
    const std::string value(64, 'v');

    HashTable table;
    std::unordered_map<std::string, std::string> map;
    for (const std::string& key : present) {
        table.put(key, value);
        map[key] = value;
    }

    const double custom = bench::timeMedian(kRepetitions, [&] {
        for (const std::string& key : absent) {
            const std::string* found = table.get(key);
            doNotOptimize(found);
        }
    });

    const double standard = bench::timeMedian(kRepetitions, [&] {
        for (const std::string& key : absent) {
            auto it = map.find(key);
            doNotOptimize(it);
        }
    });

    bench::printThroughput("VCache HashTable::get (miss)", static_cast<double>(count) / custom);
    bench::printThroughput("std::unordered_map find (miss)", static_cast<double>(count) / standard);
}

void benchmarkDeletion(std::size_t count) {
    const std::vector<std::string> keys = makeKeys(count);
    const std::string value(64, 'v');

    const double custom = bench::timeMedian(kRepetitions, [&] {
        HashTable table;
        for (const std::string& key : keys) {
            table.put(key, value);
        }
        for (const std::string& key : keys) {
            doNotOptimize(table.remove(key));
        }
    });

    const double standard = bench::timeMedian(kRepetitions, [&] {
        std::unordered_map<std::string, std::string> map;
        for (const std::string& key : keys) {
            map[key] = value;
        }
        for (const std::string& key : keys) {
            doNotOptimize(map.erase(key));
        }
    });

    // Each repetition does one insert and one erase per key.
    bench::printThroughput("VCache put+remove", 2.0 * static_cast<double>(count) / custom);
    bench::printThroughput("std::unordered_map insert+erase",
                           2.0 * static_cast<double>(count) / standard);
}

void benchmarkPreSized(std::size_t count) {
    // Both containers told up front how many keys are coming, which removes
    // rehashing from the measurement and isolates raw insert cost.
    const std::vector<std::string> keys = makeKeys(count);
    const std::string value(64, 'v');

    const double custom = bench::timeMedian(kRepetitions, [&] {
        HashTable table(count * 2);
        for (const std::string& key : keys) {
            table.put(key, value);
        }
        doNotOptimize(table);
    });

    const double standard = bench::timeMedian(kRepetitions, [&] {
        std::unordered_map<std::string, std::string> map;
        map.reserve(count);
        for (const std::string& key : keys) {
            map[key] = value;
        }
        doNotOptimize(map);
    });

    bench::printThroughput("VCache put (pre-sized)", static_cast<double>(count) / custom);
    bench::printThroughput("unordered_map insert (reserved)",
                           static_cast<double>(count) / standard);
}

void reportDistribution(std::size_t count) {
    const std::vector<std::string> keys = makeKeys(count);
    HashTable table;
    for (const std::string& key : keys) {
        table.put(key, "v");
    }

    std::cout << "  entries              " << table.size() << "\n";
    std::cout << "  buckets              " << table.bucketCount() << "\n";
    std::cout << "  load factor          " << std::fixed << std::setprecision(3)
              << table.loadFactor() << "\n";
    std::cout << "  longest chain        " << table.longestChain() << "\n";
    bench::printNote("A longest chain near 1 means lookups really are O(1) in practice;");
    bench::printNote("a long tail would mean the hash is clustering.");
}

void reportMemoryAccuracy(std::size_t count) {
    // Phase 8 called the memory accounting an estimate. This checks how far off
    // it is against the process's actual resident set.
    const std::vector<std::string> keys = makeKeys(count);
    const std::string value(200, 'v');

    const std::size_t before = bench::currentResidentBytes();

    auto table = std::make_unique<HashTable>();
    for (const std::string& key : keys) {
        table->put(key, value);
    }

    const std::size_t after = bench::currentResidentBytes();
    const std::size_t estimated = table->memoryUsage();
    const std::size_t measured = after > before ? after - before : 0;

    std::cout << "  entries              " << table->size() << "\n";
    std::cout << "  empty-entry estimate " << HashTable::footprintEstimate("", "")
              << " bytes  (sizeof(Node) + 48B allocator allowance)\n";
    std::cout << "  VCache estimate      " << estimated << " bytes\n";
    std::cout << "  RSS delta            " << measured << " bytes\n";
    if (measured > 0) {
        std::cout << "  estimate / actual    " << std::fixed << std::setprecision(2)
                  << (static_cast<double>(estimated) / static_cast<double>(measured)) << "\n";
    }
    bench::printNote("Current RSS, measured either side of building the table only.");

    doNotOptimize(table);
}

}  // namespace

int main() {
    std::cout << "VCache hash table benchmarks\n";
    std::cout << "Apple M3 Pro (11 cores), Apple clang 17, -O2, Release\n";

    constexpr std::size_t kCount = 500000;
    std::cout << "500,000 keys, 64-byte values, median of " << kRepetitions
              << " runs after a warmup\n";

    bench::printHeading("Insertion");
    bench::printThroughputHeader();
    benchmarkInsertion(kCount);

    bench::printHeading("Insertion, pre-sized (no rehashing)");
    bench::printThroughputHeader();
    benchmarkPreSized(kCount);

    bench::printHeading("Lookup, key present");
    bench::printThroughputHeader();
    benchmarkLookupHit(kCount);

    bench::printHeading("Lookup, key absent");
    bench::printThroughputHeader();
    benchmarkLookupMiss(kCount);

    bench::printHeading("Insert then delete");
    bench::printThroughputHeader();
    benchmarkDeletion(kCount);

    bench::printHeading("Hash distribution");
    reportDistribution(kCount);

    bench::printHeading("Memory accounting accuracy");
    reportMemoryAccuracy(200000);

    return 0;
}
