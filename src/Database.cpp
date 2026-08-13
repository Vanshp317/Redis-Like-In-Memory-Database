#include "Database.h"

#include <algorithm>
#include <mutex>
#include <shared_mutex>

namespace vcache {

namespace {

// Naming the two lock kinds makes each method's intent obvious at a glance:
// ReadLock means "others may read this shard alongside me", WriteLock means "I
// am alone in this shard".
using ReadLock = std::shared_lock<std::shared_mutex>;
using WriteLock = std::unique_lock<std::shared_mutex>;

std::size_t roundUpToPowerOfTwo(std::size_t n) noexcept {
    std::size_t result = 1;
    while (result < n) {
        result *= 2;
    }
    return result;
}

unsigned log2OfPowerOfTwo(std::size_t n) noexcept {
    unsigned bits = 0;
    while ((std::size_t{1} << bits) < n) {
        ++bits;
    }
    return bits;
}

}  // namespace

Database::Database(std::size_t initialBucketCount, std::size_t shardCount) {
    const std::size_t shards = roundUpToPowerOfTwo(std::max<std::size_t>(shardCount, 1));

    // The caller's bucket count is the total, so each shard starts with its
    // share. HashTable rounds its own count up to a power of two and enforces a
    // floor, so a small share is harmless.
    const std::size_t bucketsPerShard = std::max<std::size_t>(initialBucketCount / shards, 1);

    shards_.reserve(shards);
    for (std::size_t i = 0; i < shards; ++i) {
        shards_.push_back(std::make_unique<Shard>(bucketsPerShard));
    }

    if (shards > 1) {
        shardMask_ = shards - 1;
        shardShift_ = 64 - log2OfPowerOfTwo(shards);
    }
}

std::size_t Database::shardIndexFor(const std::string& key) const noexcept {
    if (shardMask_ == 0) {
        return 0;  // single shard; also avoids a shift of 64, which is UB
    }

    // The HIGH bits. HashTable::bucketIndexFor masks the LOW bits, so drawing
    // both from the same end would put every key in a shard into one bucket.
    // hashKey already applies an avalanche step, so the top bits are as well
    // mixed as the bottom.
    return static_cast<std::size_t>(HashTable::hashKey(key) >> shardShift_) & shardMask_;
}

Database::Shard& Database::shardFor(const std::string& key) noexcept {
    return *shards_[shardIndexFor(key)];
}

const Database::Shard& Database::shardFor(const std::string& key) const noexcept {
    return *shards_[shardIndexFor(key)];
}

bool Database::fitsWithinLimit(const std::string& key, const std::string& value) const {
    const std::size_t limit = maxMemoryBytes_.load();
    if (limit == 0) {
        return true;
    }

    // Judged against the GLOBAL limit rather than the shard's slice. Comparing
    // against the slice looked reasonable and was wrong: with 64 shards and a
    // budget sized for 50 entries, each slice held less than one entry, so
    // every single write was rejected while the database sat empty.
    //
    // Checked BEFORE inserting -- inserting first and then discovering the
    // entry can never fit would mean evicting a shard to make room for
    // something that still does not fit.
    return HashTable::footprintEstimate(key, value) <= limit;
}

void Database::evictToFit(Shard& shard) {
    if (shard.maxMemoryBytes == 0) {
        return;
    }

    // Stops at one entry rather than zero: the bucket array counts toward usage
    // and never shrinks, so a slice smaller than the array alone would
    // otherwise spin evicting nothing.
    while (shard.table.memoryUsage() > shard.maxMemoryBytes && shard.table.size() > 1) {
        if (!shard.table.evictOldest().has_value()) {
            break;
        }
        ++shard.evicted;
    }
}

SetOutcome Database::set(const std::string& key, const std::string& value) {
    // No validation here on purpose. Empty keys and binary values are legal
    // storage; rejecting malformed *commands* is the parser's job.
    Shard& shard = shardFor(key);
    WriteLock lock(shard.mutex);

    if (!fitsWithinLimit(key, value)) {
        return SetOutcome::Rejected;
    }

    // put() clears any previous expiration, so this makes the key persistent.
    const bool inserted = shard.table.put(key, value);
    evictToFit(shard);
    return inserted ? SetOutcome::Inserted : SetOutcome::Updated;
}

SetOutcome Database::set(const std::string& key,
                         const std::string& value,
                         std::chrono::seconds ttl) {
    Shard& shard = shardFor(key);
    WriteLock lock(shard.mutex);

    if (!fitsWithinLimit(key, value)) {
        return SetOutcome::Rejected;
    }

    const bool inserted = shard.table.put(key, value);

    // put() just reset the expiration, so setting it afterwards is what makes
    // this an expiring key. find() cannot fail -- put() guarantees the entry.
    shard.table.find(key)->expiration = Clock::now() + ttl;

    evictToFit(shard);
    return inserted ? SetOutcome::Inserted : SetOutcome::Updated;
}

std::optional<std::string> Database::get(const std::string& key) {
    Shard& shard = shardFor(key);
    ReadLock lock(shard.mutex);

    const Entry* entry = shard.table.find(key);
    if (entry == nullptr || entry->isExpiredAt(Clock::now())) {
        // Expired entries are reported absent but left in place: this is a
        // shared lock, and the sweeper does the reclaiming. A miss is also not
        // a use, so nothing is promoted.
        return std::nullopt;
    }

    // The copy happens here, under the lock. Returning a pointer instead would
    // hand the caller something another thread may free a nanosecond later.
    std::string value = entry->value;

    // A read counts as a use. This mutates the recency list while holding only
    // a SHARED lock, which is safe because that list carries its own mutex --
    // see the note on HashTable. Skipped when no limit is set, so a database
    // without eviction pays nothing for it.
    if (shard.maxMemoryBytes != 0) {
        shard.table.touch(key);
    }

    return value;
}

std::optional<std::chrono::seconds> Database::ttl(const std::string& key) {
    const Shard& shard = shardFor(key);
    ReadLock lock(shard.mutex);

    const Entry* entry = shard.table.find(key);
    if (entry == nullptr || !entry->hasExpiration()) {
        return std::nullopt;
    }

    const Clock::duration remaining = *entry->expiration - Clock::now();
    if (remaining <= Clock::duration::zero()) {
        return std::nullopt;  // already expired
    }

    // Rounded up: a key set a moment ago with EX 60 should report 60, not 59.
    return std::chrono::ceil<std::chrono::seconds>(remaining);
}

bool Database::del(const std::string& key) {
    Shard& shard = shardFor(key);
    WriteLock lock(shard.mutex);

    const Entry* entry = shard.table.find(key);
    if (entry == nullptr) {
        return false;
    }

    // An expired key is reclaimed but reported as a miss, so DEL agrees with
    // what EXISTS would have said a moment earlier.
    const bool wasLive = !entry->isExpiredAt(Clock::now());
    shard.table.remove(key);
    return wasLive;
}

bool Database::exists(const std::string& key) {
    const Shard& shard = shardFor(key);
    ReadLock lock(shard.mutex);

    const Entry* entry = shard.table.find(key);
    return entry != nullptr && !entry->isExpiredAt(Clock::now());
}

std::vector<std::string> Database::keys() {
    std::vector<std::string> result;

    // One shard at a time. Holding every lock at once would give a true
    // point-in-time snapshot at the cost of stalling the whole database, which
    // is a bad trade for a debugging command.
    for (const auto& shard : shards_) {
        ReadLock lock(shard->mutex);

        // One `now` per shard, so a slow walk cannot call a key live at the
        // start and expired at the end of the same shard's scan.
        const Clock::time_point now = Clock::now();
        std::vector<std::string> fromShard =
            shard->table.keysWhere([now](const Entry& entry) { return !entry.isExpiredAt(now); });

        result.insert(result.end(), std::make_move_iterator(fromShard.begin()),
                      std::make_move_iterator(fromShard.end()));
    }
    return result;
}

std::size_t Database::size() const {
    std::size_t total = 0;
    for (const auto& shard : shards_) {
        ReadLock lock(shard->mutex);
        total += shard->table.size();
    }
    return total;
}

bool Database::empty() const {
    for (const auto& shard : shards_) {
        ReadLock lock(shard->mutex);
        if (!shard->table.empty()) {
            return false;  // early exit; no need to visit the rest
        }
    }
    return true;
}

std::size_t Database::bucketCount() const {
    std::size_t total = 0;
    for (const auto& shard : shards_) {
        ReadLock lock(shard->mutex);
        total += shard->table.bucketCount();
    }
    return total;
}

std::size_t Database::memoryUsage() const {
    std::size_t total = 0;
    for (const auto& shard : shards_) {
        ReadLock lock(shard->mutex);
        total += shard->table.memoryUsage();
    }
    return total;
}

std::size_t Database::evictedCount() const {
    std::size_t total = 0;
    for (const auto& shard : shards_) {
        ReadLock lock(shard->mutex);
        total += shard->evicted;
    }
    return total;
}

std::size_t Database::removeExpired(std::size_t maxBuckets) {
    if (maxBuckets == 0) {
        return 0;
    }

    // The budget is split so one call still scans a bounded slice overall, not
    // a bounded slice per shard multiplied by the shard count.
    const std::size_t perShard = std::max<std::size_t>(maxBuckets / shards_.size(), 1);
    std::size_t removed = 0;

    for (const auto& shard : shards_) {
        WriteLock lock(shard->mutex);

        const std::size_t buckets = shard->table.bucketCount();
        if (buckets == 0) {
            continue;
        }

        // The table may have grown since the last pass, leaving the cursor out
        // of range. Wrapping is enough: a rehash only moves entries between
        // buckets, so anything skipped is caught on a later pass.
        if (shard->sweepCursor >= buckets) {
            shard->sweepCursor = 0;
        }

        const Clock::time_point now = Clock::now();
        const std::size_t scanned = std::min(perShard, buckets);

        removed += shard->table.removeIf(shard->sweepCursor, scanned,
                                         [now](const Entry& entry) {
                                             return entry.isExpiredAt(now);
                                         });

        shard->sweepCursor = (shard->sweepCursor + scanned) % buckets;
    }

    return removed;
}

std::vector<Entry> Database::snapshot() const {
    std::vector<Entry> result;

    for (const auto& shard : shards_) {
        ReadLock lock(shard->mutex);

        const Clock::time_point now = Clock::now();
        std::vector<Entry> fromShard = shard->table.entriesWhere(
            [now](const Entry& entry) { return !entry.isExpiredAt(now); });

        result.insert(result.end(), std::make_move_iterator(fromShard.begin()),
                      std::make_move_iterator(fromShard.end()));
    }
    return result;
}

std::size_t Database::restore(const std::vector<Entry>& entries) {
    // Cleared first, in full, so a restore replaces rather than merges.
    for (const auto& shard : shards_) {
        WriteLock lock(shard->mutex);
        shard->table.clear();
        shard->sweepCursor = 0;
    }

    const Clock::time_point now = Clock::now();
    std::size_t stored = 0;

    for (const Entry& entry : entries) {
        // A snapshot can outlive the keys inside it: an entry whose expiry has
        // already passed by the time the file is read is simply not loaded.
        if (entry.isExpiredAt(now)) {
            continue;
        }

        Shard& shard = shardFor(entry.key);
        WriteLock lock(shard.mutex);

        shard.table.put(entry.key, entry.value);
        if (entry.hasExpiration()) {
            // put() cleared the expiration, so it is reapplied here.
            shard.table.find(entry.key)->expiration = entry.expiration;
        }
        ++stored;
    }

    return stored;
}

void Database::setMaxMemory(std::size_t bytes) {
    // Divided evenly. With a well-mixed hash the shards hold similar amounts,
    // so an even split is close to fair; the cost of being wrong is that a
    // crowded shard evicts a little earlier than a global budget would.
    const std::size_t perShard =
        bytes == 0 ? 0 : std::max<std::size_t>(bytes / shards_.size(), 1);

    for (const auto& shard : shards_) {
        WriteLock lock(shard->mutex);
        shard->maxMemoryBytes = perShard;
        evictToFit(*shard);  // a tightened limit applies immediately
    }

    maxMemoryBytes_.store(bytes);
}

void Database::clear() {
    for (const auto& shard : shards_) {
        WriteLock lock(shard->mutex);
        shard->table.clear();
        shard->sweepCursor = 0;
    }
}

}  // namespace vcache
