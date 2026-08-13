#pragma once

#include <chrono>
#include <cstddef>
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
// expiry, memory limits or durability. Database is the policy layer above it,
// and it is where the remaining phases attach:
//
//   Phase 6  TTL: expiry checks on read, plus a background sweeper
//   Phase 7  persistence: snapshot on demand, restore at startup
//   Phase 8  LRU eviction once a memory limit is reached
//
// Keeping all of that here means the TCP server and command parser never touch
// the hash table directly, so those decisions stay swappable.
//
// EVICTION (Phase 8)
//
// With a memory limit set, a write that pushes usage over the limit evicts the
// least recently used entries until it is back under. A GET counts as a use and
// promotes the key; EXISTS and KEYS do not, being introspection rather than
// access.
//
// Enabling a limit costs read concurrency. Promoting a key on GET is a WRITE to
// the recency list, so a read can no longer be purely a read. The compromise:
// the table lock stays SHARED for the lookup and the value copy -- the
// expensive parts -- and only the handful of pointer writes on the recency list
// take a second, much narrower mutex inside HashTable. Making GET take the
// exclusive lock instead would serialise every read in the server.
//
// That narrow mutex is still a single point every GET touches, which is
// precisely why Redis uses sampled approximate LRU rather than a strict list.
// Phase 9 should measure the cost with the limit on and off.
//
// EXPIRATION (Phase 6)
//
// Expiry is split in two, and the split is deliberate:
//
//   * Reads HIDE expired keys. get/exists/keys treat an expired entry as
//     absent, but never delete it.
//   * removeExpired() RECLAIMS them, driven by ExpirationSweeper on a timer.
//
// The obvious alternative -- have a read delete the key it finds expired -- is
// what Redis does, but Redis is single-threaded so deletion during a read costs
// it nothing. Here a read holds the SHARED lock, and deleting under a shared
// lock is not allowed. Making reads exclusive would serialise every GET in the
// server to fix a case that only arises for already-dead data; upgrading the
// lock mid-read means dropping it, re-taking it exclusively and re-checking,
// which adds a race window to the hottest path in the database.
//
// So reads stay genuinely read-only and the sweeper does the freeing. The cost
// is that an expired key occupies memory until the next sweep touches its
// bucket. It is never visible to a client in the meantime.
//
// THREAD SAFETY (Phase 5)
//
// Every public method is safe to call from any number of threads. One
// std::shared_mutex guards the table: reads (get, exists, keys, size, empty)
// take it in shared mode and run concurrently with each other, while writes
// (set, del, clear) take it exclusively and run alone.
//
// A reader/writer lock rather than a plain mutex because cache workloads are
// overwhelmingly reads, and a plain mutex would serialise them for no reason.
// It is not free, though -- shared_mutex has more overhead per acquisition than
// a plain mutex, so on a write-heavy workload it can lose. Phase 9 should
// measure all three options: plain mutex, this, and per-shard locks.
//
// The lock lives HERE and not in HashTable. Locking the container would mean
// every operation takes the lock separately, so a read-modify-write across two
// calls would still race, and single-threaded users would pay for a lock they
// do not need.
//
// Not copyable and not movable: a std::shared_mutex is neither, and an object
// holding a lock should not be able to slide out from under threads using it.
class Database {
public:
    explicit Database(std::size_t initialBucketCount = HashTable::kDefaultBucketCount);

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    Database(Database&&) = delete;
    Database& operator=(Database&&) = delete;

    // SET key value. Rejected means the entry on its own is bigger than the
    // memory limit, so no amount of evicting could make room -- storing it and
    // then immediately evicting it would report success while losing the data.
    //
    // This overload also CLEARS any existing expiration, matching Redis, where a
    // plain SET makes a key persistent again.
    SetOutcome set(const std::string& key, const std::string& value);

    // SET key value EX <ttl>. The key disappears once `ttl` has elapsed.
    //
    // A ttl of zero or less would mean "already expired", which is a caller
    // error rather than a storage decision -- the command parser rejects it
    // before it gets here. Passing one anyway stores an entry that every read
    // will treat as absent.
    SetOutcome set(const std::string& key, const std::string& value, std::chrono::seconds ttl);

    // Remaining lifetime of a key, or empty if the key is absent, already
    // expired, or persistent. Those three are not distinguished because the
    // protocol has no TTL command to distinguish them for -- this exists for
    // Phase 7, which needs a remaining duration to persist (a steady_clock
    // timestamp is meaningless in a file), and for tests.
    //
    // Rounded up, so a key set with EX 60 reports 60 rather than 59.
    std::optional<std::chrono::seconds> ttl(const std::string& key);

    // GET key. Empty optional means "no such key" -- which the protocol renders
    // as (nil).
    //
    // Returns a COPY of the value, made while the lock is held. A pointer into
    // the table would dangle the moment the lock was released and another thread
    // overwrote or evicted the key. This is the signature choice from Phase 2
    // paying off: nothing here had to change to become thread-safe.
    //
    // Non-const because Phase 6 deletes the key when it finds it expired.
    std::optional<std::string> get(const std::string& key);

    // DEL key. True only if a LIVE key was removed -- deleting an expired key
    // reports 0, matching what EXISTS would have said a moment earlier. The
    // entry is reclaimed either way.
    bool del(const std::string& key);

    // EXISTS key. Expired keys report absent.
    bool exists(const std::string& key);

    // KEYS. Snapshot of the live keyspace in unspecified order; expired keys
    // are omitted.
    //
    // O(n) and it materialises every key at once, so it is a debugging and
    // demo command, not something to call on a hot path -- the same caveat
    // Redis puts on its own KEYS.
    std::vector<std::string> keys();

    // Raw stored count, INCLUDING expired keys that the sweeper has not yet
    // reclaimed -- the same behaviour as Redis DBSIZE. Filtering here would
    // make a cheap accessor O(n); use keys().size() when the live count is what
    // matters.
    //
    // No longer noexcept: taking the lock can throw std::system_error, and
    // claiming otherwise would turn a lock failure into a std::terminate.
    std::size_t size() const;
    bool empty() const;

    // Reclaims expired entries from at most `maxBuckets` buckets, resuming
    // where the previous call stopped, and returns how many were freed.
    //
    // Bounded on purpose. Sweeping the whole table would hold the exclusive
    // lock for as long as the scan takes, stalling every client on a large
    // keyspace; a slice per pass keeps each pause short and predictable.
    //
    // Normally driven by ExpirationSweeper. Tests call it directly to get
    // deterministic expiry without waiting on a timer.
    std::size_t removeExpired(std::size_t maxBuckets);

    // Buckets currently allocated. Lets a sweeper size its slice relative to
    // the table rather than guessing.
    std::size_t bucketCount() const;

    // A consistent point-in-time copy of every live entry, for persistence.
    // Expired entries are omitted -- dead data should not be written to disk.
    //
    // This copies the whole keyspace into memory while holding the read lock,
    // which briefly doubles memory use. The alternative -- writing to the file
    // while holding the lock -- would block every writer for the duration of a
    // disk write, which is far worse. Redis avoids both by forking and letting
    // copy-on-write do the work; that is the future-work path in section 20.
    std::vector<Entry> snapshot() const;

    // Replaces the entire keyspace with `entries`, returning how many were
    // stored. Entries already expired are skipped.
    //
    // Replaces rather than merges: restoring a snapshot means "the database is
    // now what this file says", not "add these on top of whatever is here".
    //
    // Recency is not recorded in a snapshot, so after a restore the file's
    // order stands in for it: the last entry read is treated as most recently
    // used. A restored database that immediately hits its memory limit will
    // therefore evict in file order rather than true historical order.
    std::size_t restore(const std::vector<Entry>& entries);

    // Zero means unlimited, and nothing is ever evicted. Lowering the limit
    // below current usage evicts immediately to get back under it.
    void setMaxMemory(std::size_t bytes);
    std::size_t maxMemory() const;

    // Estimated bytes held: the entries plus the bucket array. An estimate --
    // see kAllocationSlack in HashTable.cpp for exactly what it cannot see.
    std::size_t memoryUsage() const;

    // Keys evicted since construction, for diagnostics. A steadily climbing
    // count means the limit is too small for the working set.
    std::size_t evictedCount() const;

    // Drops the entire keyspace (FLUSHALL).
    void clear();

private:
    // mutable so the const accessors can take a read lock.
    mutable std::shared_mutex mutex_;
    HashTable table_;

    // Both assume the exclusive lock is already held.
    bool fitsWithinLimitLocked(const std::string& key, const std::string& value) const;
    void evictToFitLocked();

    // Where the next sweep resumes. Guarded by mutex_ like everything else.
    std::size_t sweepCursor_ = 0;

    std::size_t maxMemoryBytes_ = 0;  // 0 = unlimited
    std::size_t evictedCount_ = 0;
};

}  // namespace vcache
