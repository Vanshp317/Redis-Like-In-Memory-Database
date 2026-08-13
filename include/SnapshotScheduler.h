#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <thread>

#include "Database.h"
#include "Persistence.h"

namespace vcache {

// Writes a snapshot on a fixed interval.
//
// Same shape as ExpirationSweeper -- a thread waiting on a condition variable
// rather than sleeping, so stop() returns immediately instead of sitting out
// the rest of the interval. With a save interval measured in minutes, that
// difference is the whole of shutdown latency.
//
// Snapshots are written unconditionally rather than only when the keyspace has
// changed. Tracking dirtiness is what Redis's `save 900 1` style rules do, and
// it is a worthwhile refinement, but it needs a change counter that Phase 8's
// eviction would also have to maintain correctly.
class SnapshotScheduler {
public:
    SnapshotScheduler(SnapshotStore& store,
                      Database& database,
                      std::chrono::milliseconds interval);
    ~SnapshotScheduler();

    SnapshotScheduler(const SnapshotScheduler&) = delete;
    SnapshotScheduler& operator=(const SnapshotScheduler&) = delete;

    void start();
    void stop();

    bool running() const noexcept { return running_.load(); }

    // Snapshots attempted, and how many of those failed. A failing scheduled
    // save must not stop the server, so failures are counted rather than
    // thrown -- this is how anyone finds out about them.
    std::size_t attempts() const noexcept { return attempts_.load(); }
    std::size_t failures() const noexcept { return failures_.load(); }

    // Text of the most recent failure, empty if none.
    std::string lastError() const;

private:
    void scheduleLoop();

    SnapshotStore& store_;
    Database& database_;
    std::chrono::milliseconds interval_;

    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable wakeup_;

    std::atomic<bool> running_{false};
    std::atomic<std::size_t> attempts_{0};
    std::atomic<std::size_t> failures_{0};

    bool stopRequested_ = false;  // guarded by mutex_
    std::string lastError_;       // guarded by mutex_
};

}  // namespace vcache
