#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <utility>

namespace vcache {

// The clock used for TTL bookkeeping. steady_clock never jumps backwards when
// the system clock is adjusted, which is what we want for "expire 60s from now".
using Clock = std::chrono::steady_clock;

// A single record stored in the database.
//
// The key is kept inside the Entry (not only as the hash-table bucket key)
// because collision chains have to compare the full key to tell entries apart.
//
// An empty `expiration` means "this key never expires" -- the persistent-key
// representation described in Discovery Document section 8.
//
// steady_clock, not system_clock, is deliberate: a TTL of 60 seconds must mean
// 60 seconds of elapsed time even if an administrator or NTP moves the wall
// clock. The cost is that these time points are measured from an arbitrary
// boot-relative origin and are therefore meaningless across a restart, so
// Phase 7 has to persist the REMAINING DURATION rather than this timestamp.
struct Entry {
    std::string key;
    std::string value;
    std::optional<Clock::time_point> expiration;

    Entry() = default;

    Entry(std::string k, std::string v)
        : key(std::move(k)), value(std::move(v)), expiration(std::nullopt) {}

    bool hasExpiration() const noexcept { return expiration.has_value(); }

    // `now` is passed in rather than read here so that one sweep or one lookup
    // judges every entry against a single instant. Calling Clock::now() per
    // entry would let a long scan expire keys inconsistently with each other.
    bool isExpiredAt(Clock::time_point now) const noexcept {
        return expiration.has_value() && *expiration <= now;
    }
};

}  // namespace vcache
