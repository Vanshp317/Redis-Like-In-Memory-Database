#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <vector>

#include "Entry.h"
#include "HashTable.h"

namespace vcache {

// What a write did.
enum class SetOutcome {
    Inserted,  // a new key
    Updated,   // an existing key was overwritten
    Rejected,  // the entry alone exceeds the memory limit; nothing was stored
};

// The database engine: the single place that owns the keyspace and decides what
// operations on it mean.
//
// HashTable is a dumb container -- it stores bytes and knows nothing about
// expiry, memory limits or durability. Database is the policy layer above it.
// Keeping all of that here means the TCP server and command parser never touch
// the hash table directly, so those decisions stay swappable.
//
// SHARDING
//
// The keyspace is split across N independently locked shards. A key's shard is
// fixed by its hash, so every single-key operation -- GET, SET, DEL, EXISTS,
// TTL -- locks exactly one shard and runs in genuine parallel with operations
// on the other N-1.
//
// This replaced a single std::shared_mutex over the whole keyspace, which three
// separate pieces of evidence condemned:
//
//   * Benchmarks: throughput peaked at TWO threads and fell after. By eight
//     threads every locking strategy was slower than single-threaded.
//   * The lock-strategy comparison: a reader/writer lock beat a plain mutex,
//     but neither scaled -- because even a shared lock atomically writes a
//     reader count, so every reader bounces the same cache line between cores.
//   * CI: on Linux, glibc's reader-preferring shared_mutex let eight readers
//     starve a writer indefinitely. A test deadlocked outright.
//
// Sharding fixes all three at once: N locks means N cache lines and N
// independent reader counts, and a writer only has to out-wait the readers on
// its own shard.
//
// Shard selection uses the HIGH bits of the hash while HashTable's bucket index
// uses the LOW bits. Using the same bits for both would be catastrophic: every
// key routed to a shard would also land in the same bucket of that shard's
// table, turning it into one long collision chain.
//
// WHAT SHARDING COSTS
//
// Eviction becomes APPROXIMATE global LRU. Each shard owns a slice of the
// memory budget and evicts its own least-recently-used entry, so a key in a
// crowded shard can be evicted while an older key in an emptier shard survives.
// Exact global LRU would need a global recency list, which means a global lock,
// which is the thing being removed. Construct with shardCount == 1 when exact
// ordering matters.
//
// There is also a floor on capacity: eviction always leaves at least one entry
// per shard, so a database with N shards holds at least N entries once every
// shard has been touched. A memory limit smaller than N typical entries cannot
// be honoured. Size limits well above shardCount * entrySize, or fewer shards.
//
// Whole-keyspace operations -- size(), keys(), snapshot(), memoryUsage() --
// visit shards one at a time rather than freezing the world. Each shard's
// contribution is internally consistent, but the total may not match any single
// instant. Locking every shard at once to fix that would reintroduce the global
// bottleneck for the sake of commands the documentation already describes as
// debugging tools.
//
// EXPIRATION
//
// Expiry is split in two, and the split is deliberate:
//
//   * Reads HIDE expired keys. get/exists/keys treat an expired entry as
//     absent, but never delete it.
//   * removeExpired() RECLAIMS them, driven by ExpirationSweeper on a timer.
//
// The obvious alternative -- have a read delete the key it finds expired -- is
// what Redis does, but Redis is single-threaded so deletion during a read costs
// it nothing. Here a read holds a SHARED lock, and deleting under a shared lock
// is not allowed. Making reads exclusive would serialise every GET to fix a
// case that only arises for already-dead data.
//
// EVICTION
//
// With a memory limit set, a write that pushes a shard over its slice of the
// budget evicts that shard's least recently used entries. A GET counts as a use
// and promotes the key; EXISTS and KEYS do not, being introspection rather than
// access.
//
// Promoting on GET is a WRITE to the recency list, so a read is not purely a
// read. The shard lock stays SHARED for the lookup and the value copy -- the
// expensive parts -- and only the pointer surgery takes a second, much narrower
// mutex inside HashTable.
//
// THREAD SAFETY
//
// Every public method is safe to call from any number of threads. Not copyable
// and not movable: it holds locks, and an object holding a lock should not be
// able to slide out from under the threads using it.
class Database {
public:
    // Power of two, rounded up. Matched to the server's default worker count,
    // because shards should be sized to expected concurrent writers rather than
    // to core count.
    //
    // Measured on an M3 Pro at 90% reads (see BENCHMARKS.md): at eight threads,
    // 1 shard managed 1.0M ops/sec, 16 shards 5.1M, and 64 shards 10.7M. The
    // cost is about 4% single-threaded and one mutex plus a small bucket array
    // per shard, which is a trivial price for ten times the throughput under
    // load.
    static constexpr std::size_t kDefaultShardCount = 64;

    // `initialBucketCount` is the TOTAL across all shards; each shard gets its
    // share. Pass shardCount == 1 for exact global LRU ordering.
    explicit Database(std::size_t initialBucketCount = HashTable::kDefaultBucketCount,
                      std::size_t shardCount = kDefaultShardCount);

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    Database(Database&&) = delete;
    Database& operator=(Database&&) = delete;

    // SET key value. Rejected means the entry on its own is bigger than the
    // shard's slice of the memory limit, so no amount of evicting could make
    // room -- storing it and then immediately evicting it would report success
    // while losing the data.
    //
    // Also CLEARS any existing expiration, matching Redis, where a plain SET
    // makes a key persistent again.
    SetOutcome set(const std::string& key, const std::string& value);

    // SET key value EX <ttl>. The key disappears once `ttl` has elapsed.
    SetOutcome set(const std::string& key, const std::string& value, std::chrono::seconds ttl);

    // Remaining lifetime, or empty if the key is absent, expired, or persistent.
    // Rounded up, so a key set with EX 60 reports 60 rather than 59.
    std::optional<std::chrono::seconds> ttl(const std::string& key);

    // GET key. Empty optional means "no such key".
    //
    // Returns a COPY of the value, made while the lock is held. A pointer into
    // the table would dangle the moment the lock was released and another
    // thread overwrote or evicted the key.
    std::optional<std::string> get(const std::string& key);

    // DEL key. True only if a LIVE key was removed -- deleting an expired key
    // reports false, matching what exists() would have said a moment earlier.
    // The entry is reclaimed either way.
    bool del(const std::string& key);

    // EXISTS key. Expired keys report absent.
    bool exists(const std::string& key);

    // KEYS. Live keys in unspecified order, gathered shard by shard.
    //
    // O(n) and it materialises every key at once, so it is a debugging and demo
    // command, not something to call on a hot path -- the same caveat Redis
    // puts on its own KEYS.
    std::vector<std::string> keys();

    // Raw stored count INCLUDING expired keys not yet reclaimed, like Redis
    // DBSIZE. Summed across shards; see the note on whole-keyspace operations.
    std::size_t size() const;
    bool empty() const;

    // Reclaims expired entries, spreading `maxBuckets` across the shards and
    // resuming each where its previous pass stopped.
    //
    // Bounded on purpose: sweeping everything would hold locks for as long as
    // the scan takes.
    std::size_t removeExpired(std::size_t maxBuckets);

    // Buckets allocated across all shards.
    std::size_t bucketCount() const;

    // Shards in use. Fixed at construction.
    std::size_t shardCount() const noexcept { return shards_.size(); }

    // Live entries, for persistence. Expired entries are omitted.
    std::vector<Entry> snapshot() const;

    // Replaces the entire keyspace, returning how many entries were stored.
    // Entries already expired are skipped.
    //
    // Recency is not recorded in a snapshot, so restore order stands in for it.
    std::size_t restore(const std::vector<Entry>& entries);

    // Zero means unlimited. The budget is divided evenly among the shards;
    // lowering it evicts immediately.
    void setMaxMemory(std::size_t bytes);
    std::size_t maxMemory() const noexcept { return maxMemoryBytes_.load(); }

    // Estimated bytes held, summed across shards. An estimate -- see
    // kAllocationSlack in HashTable.cpp for what it cannot see.
    std::size_t memoryUsage() const;

    // Keys evicted since construction, summed across shards.
    std::size_t evictedCount() const;

    // Drops the entire keyspace (FLUSHALL).
    void clear();

private:
    // One independently locked slice of the keyspace.
    //
    // Heap-allocated individually rather than laid out in a vector, which keeps
    // adjacent shards' mutexes off the same cache line. Two shards sharing a
    // line would contend in hardware even though they never contend in software
    // -- false sharing, and it would quietly undo the point of sharding.
    struct Shard {
        mutable std::shared_mutex mutex;
        HashTable table;

        std::size_t sweepCursor = 0;
        std::size_t evicted = 0;
        std::size_t maxMemoryBytes = 0;  // this shard's slice; 0 = unlimited

        explicit Shard(std::size_t buckets) : table(buckets) {}
    };

    std::size_t shardIndexFor(const std::string& key) const noexcept;
    Shard& shardFor(const std::string& key) noexcept;
    const Shard& shardFor(const std::string& key) const noexcept;

    // Rejection is judged against the GLOBAL limit, not the shard's slice: an
    // entry that fits the configured budget should be storable regardless of
    // which shard it hashes to. Judging it per-shard meant that with many
    // shards and a modest limit, every write was refused even though the
    // database had ample room.
    bool fitsWithinLimit(const std::string& key, const std::string& value) const;

    // Eviction, by contrast, is per-shard: a shard trims its own slice.
    static void evictToFit(Shard& shard);

    std::vector<std::unique_ptr<Shard>> shards_;

    // Zero when there is a single shard, in which case shardIndexFor short
    // circuits -- shifting a 64-bit value by 64 is undefined behaviour.
    std::size_t shardMask_ = 0;
    unsigned shardShift_ = 0;

    // The global limit as configured, kept for reporting. The figures that
    // actually drive eviction are the per-shard slices.
    std::atomic<std::size_t> maxMemoryBytes_{0};
};

}  // namespace vcache
