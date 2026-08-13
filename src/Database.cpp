#include "Database.h"

#include <mutex>
#include <shared_mutex>

namespace vcache {

namespace {

// Naming the two lock kinds makes each method's intent obvious at a glance:
// ReadLock means "others may read alongside me", WriteLock means "I am alone".
using ReadLock = std::shared_lock<std::shared_mutex>;
using WriteLock = std::unique_lock<std::shared_mutex>;

}  // namespace

Database::Database(std::size_t initialBucketCount) : table_(initialBucketCount) {}

SetOutcome Database::set(const std::string& key, const std::string& value) {
    // No validation here on purpose. Empty keys and binary values are legal
    // storage; rejecting malformed *commands* (wrong arity, unknown verb, bad
    // TTL) is the command parser's job. Splitting it that way keeps one place
    // to look when a client gets an error.
    WriteLock lock(mutex_);

    if (!fitsWithinLimitLocked(key, value)) {
        return SetOutcome::Rejected;
    }

    // put() clears any previous expiration, so this makes the key persistent.
    const bool inserted = table_.put(key, value);
    evictToFitLocked();
    return inserted ? SetOutcome::Inserted : SetOutcome::Updated;
}

SetOutcome Database::set(const std::string& key, const std::string& value,
                         std::chrono::seconds ttl) {
    WriteLock lock(mutex_);

    if (!fitsWithinLimitLocked(key, value)) {
        return SetOutcome::Rejected;
    }

    const bool inserted = table_.put(key, value);

    // put() just reset the expiration, so setting it afterwards is what turns
    // this into an expiring key. find() cannot fail here -- put() guarantees
    // the entry exists.
    Entry* entry = table_.find(key);
    entry->expiration = Clock::now() + ttl;

    evictToFitLocked();
    return inserted ? SetOutcome::Inserted : SetOutcome::Updated;
}

bool Database::fitsWithinLimitLocked(const std::string& key, const std::string& value) const {
    if (maxMemoryBytes_ == 0) {
        return true;
    }
    // Checked BEFORE inserting rather than after. Inserting first and then
    // discovering the entry can never fit would mean evicting the whole
    // keyspace to make room for something that still does not fit.
    return HashTable::footprintEstimate(key, value) <= maxMemoryBytes_;
}

void Database::evictToFitLocked() {
    if (maxMemoryBytes_ == 0) {
        return;
    }

    // Stops at one entry rather than zero: the bucket array is counted in
    // usage and never shrinks, so a limit smaller than the bucket array alone
    // would otherwise spin evicting nothing.
    while (table_.memoryUsage() > maxMemoryBytes_ && table_.size() > 1) {
        if (!table_.evictOldest().has_value()) {
            break;
        }
        ++evictedCount_;
    }
}

void Database::setMaxMemory(std::size_t bytes) {
    WriteLock lock(mutex_);
    maxMemoryBytes_ = bytes;
    evictToFitLocked();  // a tightened limit applies immediately
}

std::size_t Database::maxMemory() const {
    ReadLock lock(mutex_);
    return maxMemoryBytes_;
}

std::size_t Database::memoryUsage() const {
    ReadLock lock(mutex_);
    return table_.memoryUsage();
}

std::size_t Database::evictedCount() const {
    ReadLock lock(mutex_);
    return evictedCount_;
}

std::optional<std::string> Database::get(const std::string& key) {
    ReadLock lock(mutex_);

    const Entry* entry = table_.find(key);
    if (entry == nullptr || entry->isExpiredAt(Clock::now())) {
        // Expired entries are reported absent but deliberately left in place:
        // this is a shared lock, and the sweeper does the reclaiming. An
        // expired key is also not a "use", so it is not promoted.
        return std::nullopt;
    }

    // The copy happens here, under the lock. Returning a pointer instead would
    // hand the caller something another thread may free a nanosecond later.
    std::string value = entry->value;

    // A read counts as a use. This mutates the recency list while holding only
    // a SHARED lock, which is safe because that list carries its own mutex --
    // see the note on HashTable. Skipped entirely when no limit is set, so a
    // database without eviction pays nothing for it.
    if (maxMemoryBytes_ != 0) {
        table_.touch(key);
    }

    return value;
}

std::optional<std::chrono::seconds> Database::ttl(const std::string& key) {
    ReadLock lock(mutex_);

    const Entry* entry = table_.find(key);
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
    WriteLock lock(mutex_);

    const Entry* entry = table_.find(key);
    if (entry == nullptr) {
        return false;
    }

    // An expired key is reclaimed but reported as a miss, so DEL agrees with
    // what EXISTS would have said a moment earlier.
    const bool wasLive = !entry->isExpiredAt(Clock::now());
    table_.remove(key);
    return wasLive;
}

bool Database::exists(const std::string& key) {
    ReadLock lock(mutex_);

    const Entry* entry = table_.find(key);
    return entry != nullptr && !entry->isExpiredAt(Clock::now());
}

std::vector<std::string> Database::keys() {
    ReadLock lock(mutex_);

    // One `now` for the whole scan, so a slow walk cannot report a key as live
    // at the start and expired at the end of the same snapshot.
    const Clock::time_point now = Clock::now();

    // Builds the whole vector under the lock, so callers get a consistent
    // snapshot rather than a view that shifts while they iterate it.
    return table_.keysWhere([now](const Entry& entry) { return !entry.isExpiredAt(now); });
}

std::size_t Database::size() const {
    ReadLock lock(mutex_);
    return table_.size();
}

bool Database::empty() const {
    ReadLock lock(mutex_);
    return table_.empty();
}

std::size_t Database::bucketCount() const {
    ReadLock lock(mutex_);
    return table_.bucketCount();
}

std::size_t Database::removeExpired(std::size_t maxBuckets) {
    WriteLock lock(mutex_);

    const std::size_t buckets = table_.bucketCount();
    if (buckets == 0 || maxBuckets == 0) {
        return 0;
    }

    // The table may have grown or shrunk since the last pass, leaving the
    // cursor out of range. Wrapping is enough: a rehash only moves entries
    // between buckets, so anything skipped is caught on a later pass.
    if (sweepCursor_ >= buckets) {
        sweepCursor_ = 0;
    }

    const Clock::time_point now = Clock::now();
    const std::size_t scanned = std::min(maxBuckets, buckets);

    const std::size_t removed = table_.removeIf(
        sweepCursor_, scanned, [now](const Entry& entry) { return entry.isExpiredAt(now); });

    sweepCursor_ = (sweepCursor_ + scanned) % buckets;
    return removed;
}

std::vector<Entry> Database::snapshot() const {
    ReadLock lock(mutex_);

    const Clock::time_point now = Clock::now();
    return table_.entriesWhere([now](const Entry& entry) { return !entry.isExpiredAt(now); });
}

std::size_t Database::restore(const std::vector<Entry>& entries) {
    WriteLock lock(mutex_);

    table_.clear();
    sweepCursor_ = 0;

    const Clock::time_point now = Clock::now();
    std::size_t stored = 0;

    for (const Entry& entry : entries) {
        // A snapshot can outlive the keys inside it: an entry whose expiry has
        // already passed by the time the file is read is simply not loaded.
        if (entry.isExpiredAt(now)) {
            continue;
        }

        table_.put(entry.key, entry.value);
        if (entry.hasExpiration()) {
            // put() cleared the expiration, so it is reapplied here.
            table_.find(entry.key)->expiration = entry.expiration;
        }
        ++stored;
    }

    return stored;
}

void Database::clear() {
    WriteLock lock(mutex_);
    table_.clear();
    sweepCursor_ = 0;
}

}  // namespace vcache
