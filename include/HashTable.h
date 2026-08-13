#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "Entry.h"

namespace vcache {

// An open-hashing (separate chaining) hash table mapping std::string -> Entry.
//
// This is deliberately written from scratch instead of wrapping
// std::unordered_map: the custom table is the thing being demonstrated, and it
// doubles as the subject of the Phase 9 benchmark against the standard library.
//
// Design notes:
//   * Bucket count is always a power of two, so the bucket index is a bitmask
//     (`hash & (n - 1)`) instead of a modulo. Masking is roughly an order of
//     magnitude cheaper than a 64-bit division on the hot lookup path.
//   * Because masking only looks at the low bits, the hash function ends with an
//     avalanche step so weak low bits cannot cause pathological clustering.
//   * The table grows (doubles) when the load factor would exceed 0.75.
//
// Complexity: O(1) average for put/get/remove, O(chain length) worst case.
//
// RECENCY LIST (Phase 8)
//
// Every node is also threaded onto a doubly linked list ordered most-recently-
// used first, which is what makes LRU eviction O(1): the victim is always the
// tail, and promoting a node is a handful of pointer writes. The list is
// intrusive -- it runs through the existing nodes rather than living in a
// second container -- because a standalone LRU structure would need its own
// hash map, and every operation would then cost two lookups instead of one.
//
// This class is NOT thread-safe, with one deliberate exception: the recency
// list has its own mutex. Database needs to record a read as a "use" while
// holding only a SHARED lock, and two concurrent readers would otherwise race
// on the list pointers. Everything else still relies on the caller's exclusive
// lock.
class HashTable {
public:
    static constexpr std::size_t kDefaultBucketCount = 16;
    static constexpr double kMaxLoadFactor = 0.75;

    explicit HashTable(std::size_t initialBucketCount = kDefaultBucketCount);
    ~HashTable();

    // Copying a table would mean deep-copying every chain; nothing needs that
    // yet, so it is disabled rather than silently expensive.
    HashTable(const HashTable&) = delete;
    HashTable& operator=(const HashTable&) = delete;

    HashTable(HashTable&& other) noexcept;
    HashTable& operator=(HashTable&& other) noexcept;

    // Inserts or overwrites. Returns true if a new key was inserted, false if an
    // existing key was overwritten.
    //
    // Overwriting also clears any expiration, matching Redis, where a plain SET
    // drops the previous TTL. Callers that want to keep or set a TTL should use
    // find() afterwards.
    bool put(const std::string& key, const std::string& value);

    // Returns a pointer to the stored entry, or nullptr if the key is absent.
    // The pointer is invalidated by any later put/remove/resize/clear.
    //
    // Callers may modify entry.value and entry.expiration through this pointer
    // but must NOT modify entry.key -- doing so would leave the entry sitting in
    // the wrong bucket.
    Entry* find(const std::string& key);
    const Entry* find(const std::string& key) const;

    // Convenience read accessor: returns a pointer to the value, or nullptr.
    const std::string* get(const std::string& key) const;

    bool remove(const std::string& key);
    bool contains(const std::string& key) const;

    std::size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }
    void clear();

    // Snapshot of every key currently stored. Order is unspecified.
    std::vector<std::string> keys() const;

    // Snapshot of the keys whose entries satisfy `predicate`. Used by the
    // Database to hide keys that have expired but not yet been reclaimed.
    std::vector<std::string> keysWhere(const std::function<bool(const Entry&)>& predicate) const;

    // Full copies of the entries satisfying `predicate`, keys and values
    // included. Used by persistence, which needs the values too.
    std::vector<Entry> entriesWhere(const std::function<bool(const Entry&)>& predicate) const;

    // Removes every entry satisfying `predicate` from `bucketsToScan` buckets
    // starting at `firstBucket`, wrapping around the end of the table. Returns
    // how many were removed.
    //
    // The bucket range is what makes incremental expiry possible: a sweeper can
    // reclaim a slice of the table per pass instead of walking millions of
    // entries while holding a lock that blocks every client.
    std::size_t removeIf(std::size_t firstBucket,
                         std::size_t bucketsToScan,
                         const std::function<bool(const Entry&)>& predicate);

    // Rehashes into at least `newBucketCount` buckets (rounded up to a power of
    // two, and raised further if needed to respect the max load factor).
    // Growth is automatic; call this directly only to pre-size a table.
    void resize(std::size_t newBucketCount);

    // Marks a key as just used, moving it to the front of the recency list.
    // Returns false if the key is absent. Safe to call while holding only a
    // shared lock -- it takes the recency mutex internally.
    bool touch(const std::string& key);

    // Removes the least recently used entry and returns its key, or an empty
    // optional if the table is empty. Requires the caller's exclusive lock.
    std::optional<std::string> evictOldest();

    // Key at each end of the recency list. Diagnostics and tests.
    std::optional<std::string> mostRecentKey() const;
    std::optional<std::string> leastRecentKey() const;

    // Estimated bytes held by the entries themselves, maintained incrementally
    // so it costs nothing to read. See kAllocationSlack in the .cpp for what
    // this can and cannot account for.
    std::size_t entryBytes() const noexcept { return entryBytes_; }

    // entryBytes() plus the bucket array, which is real memory too and grows
    // with the table.
    std::size_t memoryUsage() const noexcept;

    std::size_t bucketCount() const noexcept { return buckets_.size(); }
    double loadFactor() const noexcept;

    // Length of the longest collision chain. Diagnostic only -- used by tests
    // and benchmarks to prove the hash spreads keys evenly.
    std::size_t longestChain() const;

    // Exposed so tests can construct deliberate collisions and so benchmarks can
    // measure the hash in isolation.
    static std::uint64_t hashKey(const std::string& key) noexcept;

    // Estimated bytes one entry would occupy. Public so Database can ask
    // "would this even fit?" before inserting something it would have to evict
    // the whole keyspace for and still fail.
    static std::size_t footprintEstimate(const std::string& key,
                                         const std::string& value) noexcept;

private:
    struct Node {
        Entry entry;

        // The bucket chain owns the node.
        std::unique_ptr<Node> next;

        // The recency list only refers to it, so these are raw pointers. A
        // node is always owned by exactly one bucket chain and is unlinked
        // from the recency list before it is destroyed.
        Node* lruPrev = nullptr;
        Node* lruNext = nullptr;

        // Cached so removal does not have to recompute it, and so an overwrite
        // can adjust the running total by the difference.
        std::size_t footprint = 0;

        explicit Node(Entry e) : entry(std::move(e)), next(nullptr) {}
    };

    std::size_t bucketIndexFor(const std::string& key) const noexcept;
    void growIfNeeded();

    Node* findNode(const std::string& key) noexcept;
    const Node* findNode(const std::string& key) const noexcept;

    // All four assume lruMutex_ is already held.
    void lruPushFront(Node* node) noexcept;
    void lruUnlink(Node* node) noexcept;
    void lruClear() noexcept;

    static std::size_t roundUpToPowerOfTwo(std::size_t n) noexcept;

    // Destroys a chain iteratively.
    //
    // The obvious `head.reset()` would recurse: ~Node destroys its unique_ptr
    // `next`, which destroys the next Node, and so on -- one stack frame per
    // element. A long collision chain would then blow the stack. Detaching
    // `next` before each node dies keeps destruction flat.
    static void destroyChain(std::unique_ptr<Node>& head) noexcept;

    std::vector<std::unique_ptr<Node>> buckets_;
    std::size_t size_ = 0;
    std::size_t entryBytes_ = 0;

    // Front is the most recently used entry; back is the eviction victim.
    // Guarded by lruMutex_ so a shared-lock reader may promote an entry.
    mutable std::mutex lruMutex_;
    Node* lruHead_ = nullptr;
    Node* lruTail_ = nullptr;
};

}  // namespace vcache
