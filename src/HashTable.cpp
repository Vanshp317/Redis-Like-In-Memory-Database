#include "HashTable.h"

#include <algorithm>
#include <utility>

namespace vcache {

namespace {

// FNV-1a, 64-bit. Chosen over std::hash because it is specified, stable across
// platforms and runs (std::hash for strings is allowed to be randomized per
// process), which keeps benchmark numbers reproducible.
constexpr std::uint64_t kFnvOffsetBasis = 0xcbf29ce484222325ULL;
constexpr std::uint64_t kFnvPrime = 0x100000001b3ULL;

// Per-entry cost that sizeof cannot see: the malloc header on the node
// allocation, plus the heap buffers behind the key and value strings once they
// outgrow the small-string optimisation.
//
// This makes the accounting an ESTIMATE, not a measurement. It ignores
// allocator size-class rounding, counts short strings as if they allocated when
// they did not, and has no visibility into fragmentation. It is a proxy for
// memory pressure good enough to drive eviction decisions -- not a figure to
// quote as the process's real footprint.
constexpr std::size_t kAllocationSlack = 48;

}  // namespace

std::uint64_t HashTable::hashKey(const std::string& key) noexcept {
    std::uint64_t hash = kFnvOffsetBasis;
    for (const char ch : key) {
        // Cast through unsigned char: char may be signed, and sign-extending
        // bytes >= 0x80 would fold distinct inputs together.
        hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(ch));
        hash *= kFnvPrime;
    }

    // Avalanche (the splitmix64 finalizer). Bucket selection masks off the low
    // bits, and plain FNV-1a mixes its high bits far better than its low ones;
    // this pushes entropy back down so the mask sees well-distributed bits.
    hash ^= hash >> 33;
    hash *= 0xff51afd7ed558ccdULL;
    hash ^= hash >> 33;
    hash *= 0xc4ceb9fe1a85ec53ULL;
    hash ^= hash >> 33;
    return hash;
}

std::size_t HashTable::roundUpToPowerOfTwo(std::size_t n) noexcept {
    if (n <= 1) {
        return 1;
    }
    --n;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    if constexpr (sizeof(std::size_t) > 4) {
        n |= n >> 32;
    }
    return n + 1;
}

HashTable::HashTable(std::size_t initialBucketCount)
    : buckets_(roundUpToPowerOfTwo(initialBucketCount)), size_(0) {}

HashTable::~HashTable() {
    clear();
}

HashTable::HashTable(HashTable&& other) noexcept
    : buckets_(std::move(other.buckets_)),
      size_(other.size_),
      entryBytes_(other.entryBytes_),
      lruHead_(other.lruHead_),
      lruTail_(other.lruTail_) {
    // The recency list is made of raw pointers INTO the nodes, and the nodes
    // themselves did not move -- only ownership of them did. So the head and
    // tail pointers stay valid and are simply handed over.
    //
    // A moved-from std::vector is "valid but unspecified"; clearing makes the
    // state definite. Every mutating method re-allocates buckets on demand, so
    // the moved-from table stays usable.
    other.buckets_.clear();
    other.size_ = 0;
    other.entryBytes_ = 0;
    other.lruHead_ = nullptr;
    other.lruTail_ = nullptr;
}

HashTable& HashTable::operator=(HashTable&& other) noexcept {
    if (this != &other) {
        clear();  // iterative teardown of our own chains before dropping them
        buckets_ = std::move(other.buckets_);
        size_ = other.size_;
        entryBytes_ = other.entryBytes_;
        lruHead_ = other.lruHead_;
        lruTail_ = other.lruTail_;

        other.buckets_.clear();
        other.size_ = 0;
        other.entryBytes_ = 0;
        other.lruHead_ = nullptr;
        other.lruTail_ = nullptr;
    }
    return *this;
}

std::size_t HashTable::footprintEstimate(const std::string& key,
                                        const std::string& value) noexcept {
    // sizeof(Node) is exact for the struct: both strings' headers, the optional
    // expiration, the chain pointer and the two recency pointers. Everything
    // beyond it is the estimate described at kAllocationSlack.
    return sizeof(Node) + kAllocationSlack + key.size() + value.size();
}

std::size_t HashTable::memoryUsage() const noexcept {
    return entryBytes_ + buckets_.size() * sizeof(std::unique_ptr<Node>);
}

HashTable::Node* HashTable::findNode(const std::string& key) noexcept {
    return const_cast<Node*>(static_cast<const HashTable*>(this)->findNode(key));
}

const HashTable::Node* HashTable::findNode(const std::string& key) const noexcept {
    if (buckets_.empty()) {
        return nullptr;
    }
    const std::size_t index = bucketIndexFor(key);
    for (const Node* node = buckets_[index].get(); node != nullptr; node = node->next.get()) {
        if (node->entry.key == key) {
            return node;
        }
    }
    return nullptr;
}

void HashTable::lruPushFront(Node* node) noexcept {
    node->lruPrev = nullptr;
    node->lruNext = lruHead_;

    if (lruHead_ != nullptr) {
        lruHead_->lruPrev = node;
    }
    lruHead_ = node;

    if (lruTail_ == nullptr) {
        lruTail_ = node;  // first entry is both ends of the list
    }
}

void HashTable::lruUnlink(Node* node) noexcept {
    if (node->lruPrev != nullptr) {
        node->lruPrev->lruNext = node->lruNext;
    } else if (lruHead_ == node) {
        lruHead_ = node->lruNext;
    }

    if (node->lruNext != nullptr) {
        node->lruNext->lruPrev = node->lruPrev;
    } else if (lruTail_ == node) {
        lruTail_ = node->lruPrev;
    }

    node->lruPrev = nullptr;
    node->lruNext = nullptr;
}

void HashTable::lruClear() noexcept {
    lruHead_ = nullptr;
    lruTail_ = nullptr;
}

bool HashTable::touch(const std::string& key) {
    Node* node = findNode(key);
    if (node == nullptr) {
        return false;
    }

    std::lock_guard<std::mutex> lock(lruMutex_);

    // Already at the front: the common case for a hot key, and skipping the
    // pointer writes keeps repeated reads of the same key cheap.
    if (lruHead_ == node) {
        return true;
    }

    lruUnlink(node);
    lruPushFront(node);
    return true;
}

std::optional<std::string> HashTable::evictOldest() {
    std::string victimKey;
    {
        std::lock_guard<std::mutex> lock(lruMutex_);
        if (lruTail_ == nullptr) {
            return std::nullopt;
        }
        victimKey = lruTail_->entry.key;
    }

    // remove() re-takes lruMutex_ to unlink, so the key is copied out and the
    // lock released first -- holding it across the call would deadlock on a
    // non-recursive mutex.
    remove(victimKey);
    return victimKey;
}

std::optional<std::string> HashTable::mostRecentKey() const {
    std::lock_guard<std::mutex> lock(lruMutex_);
    if (lruHead_ == nullptr) {
        return std::nullopt;
    }
    return lruHead_->entry.key;
}

std::optional<std::string> HashTable::leastRecentKey() const {
    std::lock_guard<std::mutex> lock(lruMutex_);
    if (lruTail_ == nullptr) {
        return std::nullopt;
    }
    return lruTail_->entry.key;
}

std::size_t HashTable::bucketIndexFor(const std::string& key) const noexcept {
    // Valid only when buckets_ is non-empty; every caller guarantees that.
    return static_cast<std::size_t>(hashKey(key)) & (buckets_.size() - 1);
}

bool HashTable::put(const std::string& key, const std::string& value) {
    if (buckets_.empty()) {
        buckets_.resize(kDefaultBucketCount);
    }

    const std::size_t index = bucketIndexFor(key);
    for (Node* node = buckets_[index].get(); node != nullptr; node = node->next.get()) {
        if (node->entry.key == key) {
            node->entry.value = value;
            node->entry.expiration.reset();

            // Adjust the running total by the difference rather than
            // recomputing it, so accounting stays O(1).
            const std::size_t updated = footprintEstimate(key, value);
            entryBytes_ = entryBytes_ - node->footprint + updated;
            node->footprint = updated;

            // A write counts as a use.
            {
                std::lock_guard<std::mutex> lock(lruMutex_);
                lruUnlink(node);
                lruPushFront(node);
            }
            return false;
        }
    }

    // New key: push onto the front of the chain. Front insertion is O(1) and
    // needs no tail pointer.
    auto node = std::make_unique<Node>(Entry(key, value));
    node->footprint = footprintEstimate(key, value);
    entryBytes_ += node->footprint;

    Node* raw = node.get();
    node->next = std::move(buckets_[index]);
    buckets_[index] = std::move(node);
    ++size_;

    {
        std::lock_guard<std::mutex> lock(lruMutex_);
        lruPushFront(raw);
    }

    growIfNeeded();
    return true;
}

Entry* HashTable::find(const std::string& key) {
    // Reuse the const version rather than duplicating the traversal.
    return const_cast<Entry*>(static_cast<const HashTable*>(this)->find(key));
}

const Entry* HashTable::find(const std::string& key) const {
    if (buckets_.empty()) {
        return nullptr;
    }

    const std::size_t index = bucketIndexFor(key);
    for (const Node* node = buckets_[index].get(); node != nullptr; node = node->next.get()) {
        if (node->entry.key == key) {
            return &node->entry;
        }
    }
    return nullptr;
}

const std::string* HashTable::get(const std::string& key) const {
    const Entry* entry = find(key);
    return entry != nullptr ? &entry->value : nullptr;
}

bool HashTable::contains(const std::string& key) const {
    return find(key) != nullptr;
}

bool HashTable::remove(const std::string& key) {
    if (buckets_.empty()) {
        return false;
    }

    const std::size_t index = bucketIndexFor(key);

    // `link` points at the owning pointer of the current node -- the bucket slot
    // for the head, or the previous node's `next` otherwise. This removes the
    // usual "is it the head?" special case entirely.
    std::unique_ptr<Node>* link = &buckets_[index];
    while (*link != nullptr) {
        if ((*link)->entry.key == key) {
            std::unique_ptr<Node> victim = std::move(*link);
            *link = std::move(victim->next);  // splice the chain shut
            --size_;
            entryBytes_ -= victim->footprint;
            {
                std::lock_guard<std::mutex> lock(lruMutex_);
                lruUnlink(victim.get());
            }
            return true;                      // victim dies here, already detached
        }
        link = &(*link)->next;
    }
    return false;
}

void HashTable::clear() {
    {
        // Unlinked first: destroying nodes that are still on the recency list
        // would leave lruHead_/lruTail_ pointing at freed memory.
        std::lock_guard<std::mutex> lock(lruMutex_);
        lruClear();
    }
    for (auto& head : buckets_) {
        destroyChain(head);
    }
    size_ = 0;
    entryBytes_ = 0;
}

std::vector<std::string> HashTable::keys() const {
    std::vector<std::string> result;
    result.reserve(size_);
    for (const auto& head : buckets_) {
        for (const Node* node = head.get(); node != nullptr; node = node->next.get()) {
            result.push_back(node->entry.key);
        }
    }
    return result;
}

std::vector<std::string> HashTable::keysWhere(
    const std::function<bool(const Entry&)>& predicate) const {
    std::vector<std::string> result;
    result.reserve(size_);
    for (const auto& head : buckets_) {
        for (const Node* node = head.get(); node != nullptr; node = node->next.get()) {
            if (predicate(node->entry)) {
                result.push_back(node->entry.key);
            }
        }
    }
    return result;
}

std::vector<Entry> HashTable::entriesWhere(
    const std::function<bool(const Entry&)>& predicate) const {
    std::vector<Entry> result;
    result.reserve(size_);
    for (const auto& head : buckets_) {
        for (const Node* node = head.get(); node != nullptr; node = node->next.get()) {
            if (predicate(node->entry)) {
                result.push_back(node->entry);
            }
        }
    }
    return result;
}

std::size_t HashTable::removeIf(std::size_t firstBucket,
                                std::size_t bucketsToScan,
                                const std::function<bool(const Entry&)>& predicate) {
    if (buckets_.empty() || bucketsToScan == 0) {
        return 0;
    }

    const std::size_t total = buckets_.size();
    const std::size_t toScan = std::min(bucketsToScan, total);  // never lap the table
    std::size_t removed = 0;

    for (std::size_t offset = 0; offset < toScan; ++offset) {
        const std::size_t index = (firstBucket + offset) % total;

        // Same pointer-to-pointer walk as remove(): `link` owns the current
        // node, so unlinking needs no special case for the head of the chain.
        std::unique_ptr<Node>* link = &buckets_[index];
        while (*link != nullptr) {
            if (predicate((*link)->entry)) {
                std::unique_ptr<Node> victim = std::move(*link);
                *link = std::move(victim->next);
                --size_;
                ++removed;
                entryBytes_ -= victim->footprint;
                {
                    std::lock_guard<std::mutex> lock(lruMutex_);
                    lruUnlink(victim.get());
                }
                // `link` deliberately does not advance: the next node has just
                // moved into the slot it points at.
            } else {
                link = &(*link)->next;
            }
        }
    }

    return removed;
}

void HashTable::resize(std::size_t newBucketCount) {
    std::size_t target = roundUpToPowerOfTwo(newBucketCount);

    // Never shrink so far that the table is immediately over its load factor.
    while (static_cast<double>(size_) > static_cast<double>(target) * kMaxLoadFactor) {
        target *= 2;
    }
    if (target == buckets_.size()) {
        return;
    }

    std::vector<std::unique_ptr<Node>> oldBuckets(target);
    oldBuckets.swap(buckets_);  // buckets_ is now the fresh empty table

    const std::size_t mask = target - 1;
    for (auto& head : oldBuckets) {
        std::unique_ptr<Node> node = std::move(head);
        while (node != nullptr) {
            std::unique_ptr<Node> next = std::move(node->next);

            // Relink the existing node instead of allocating a new one: rehashing
            // moves pointers, never key or value bytes.
            const std::size_t index = static_cast<std::size_t>(hashKey(node->entry.key)) & mask;
            node->next = std::move(buckets_[index]);
            buckets_[index] = std::move(node);

            node = std::move(next);
        }
    }
}

void HashTable::growIfNeeded() {
    if (static_cast<double>(size_) > static_cast<double>(buckets_.size()) * kMaxLoadFactor) {
        resize(buckets_.size() * 2);
    }
}

double HashTable::loadFactor() const noexcept {
    if (buckets_.empty()) {
        return 0.0;
    }
    return static_cast<double>(size_) / static_cast<double>(buckets_.size());
}

std::size_t HashTable::longestChain() const {
    std::size_t longest = 0;
    for (const auto& head : buckets_) {
        std::size_t length = 0;
        for (const Node* node = head.get(); node != nullptr; node = node->next.get()) {
            ++length;
        }
        longest = std::max(longest, length);
    }
    return longest;
}

void HashTable::destroyChain(std::unique_ptr<Node>& head) noexcept {
    std::unique_ptr<Node> node = std::move(head);
    while (node != nullptr) {
        std::unique_ptr<Node> next = std::move(node->next);
        node.reset();             // this node only: its successor is already detached
        node = std::move(next);
    }
}

}  // namespace vcache
