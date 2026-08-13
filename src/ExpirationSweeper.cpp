#include "ExpirationSweeper.h"

#include <utility>

namespace vcache {

ExpirationSweeper::ExpirationSweeper(Database& database, SweeperConfig config)
    : database_(database), config_(std::move(config)) {
    if (config_.bucketsPerPass == 0) {
        config_.bucketsPerPass = 1;  // zero would sweep nothing, silently
    }
}

ExpirationSweeper::~ExpirationSweeper() {
    stop();
}

void ExpirationSweeper::start() {
    if (running_.exchange(true)) {
        return;  // already started
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopRequested_ = false;
    }
    thread_ = std::thread([this] { sweepLoop(); });
}

void ExpirationSweeper::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopRequested_ = true;
    }
    // Wakes the wait immediately instead of leaving shutdown to sit through the
    // rest of the current interval.
    wakeup_.notify_all();

    if (thread_.joinable()) {
        thread_.join();
    }
    running_.store(false);
}

void ExpirationSweeper::sweepLoop() {
    while (true) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            // wait_for with a predicate: returns early when stop() fires, and
            // is immune to spurious wakeups.
            wakeup_.wait_for(lock, config_.interval, [this] { return stopRequested_; });
            if (stopRequested_) {
                return;
            }
        }

        // The database takes its own lock; this one is released first so a
        // sweep never holds two locks at once.
        const std::size_t reclaimed = database_.removeExpired(config_.bucketsPerPass);
        totalReclaimed_.fetch_add(reclaimed);
        passes_.fetch_add(1);
    }
}

}  // namespace vcache
