#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <thread>

#include "Database.h"

namespace vcache {

struct SweeperConfig {
    // How long to wait between passes. Shorter reclaims memory sooner; longer
    // takes the write lock less often. Expired keys are invisible to clients
    // either way, so this trades memory against lock contention, never
    // correctness.
    std::chrono::milliseconds interval{100};

    // Buckets examined per pass. Bounded so the exclusive lock is held for a
    // short, predictable time no matter how large the keyspace grows.
    std::size_t bucketsPerPass = 128;
};

// Runs Database::removeExpired() on a timer.
//
// Reads hide expired keys the moment they lapse, so this thread is about
// reclaiming memory, not about correctness. Without it, a key written with a
// TTL and never read again would occupy memory forever -- which for a cache
// under continuous write load is the difference between steady state and an
// unbounded leak.
//
// The wait uses a condition variable rather than sleep_for so that stop()
// returns immediately instead of after up to a full interval.
class ExpirationSweeper {
public:
    ExpirationSweeper(Database& database, SweeperConfig config = SweeperConfig{});
    ~ExpirationSweeper();

    ExpirationSweeper(const ExpirationSweeper&) = delete;
    ExpirationSweeper& operator=(const ExpirationSweeper&) = delete;

    // Starts the background thread. Calling twice does nothing.
    void start();

    // Stops it and joins. Idempotent; called by the destructor.
    void stop();

    bool running() const noexcept { return running_.load(); }

    // Total entries reclaimed since construction. Diagnostics, and how the
    // tests confirm the thread is doing anything at all.
    std::size_t totalReclaimed() const noexcept { return totalReclaimed_.load(); }

    // Passes completed since construction.
    std::size_t passes() const noexcept { return passes_.load(); }

private:
    void sweepLoop();

    Database& database_;
    SweeperConfig config_;

    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable wakeup_;

    std::atomic<bool> running_{false};
    std::atomic<std::size_t> totalReclaimed_{0};
    std::atomic<std::size_t> passes_{0};
    bool stopRequested_ = false;  // guarded by mutex_
};

}  // namespace vcache
