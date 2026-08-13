#include "SnapshotScheduler.h"

namespace vcache {

SnapshotScheduler::SnapshotScheduler(SnapshotStore& store,
                                     Database& database,
                                     std::chrono::milliseconds interval)
    : store_(store), database_(database), interval_(interval) {}

SnapshotScheduler::~SnapshotScheduler() {
    stop();
}

void SnapshotScheduler::start() {
    if (interval_.count() <= 0) {
        return;  // periodic saving disabled
    }
    if (running_.exchange(true)) {
        return;  // already started
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopRequested_ = false;
    }
    thread_ = std::thread([this] { scheduleLoop(); });
}

void SnapshotScheduler::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopRequested_ = true;
    }
    wakeup_.notify_all();

    if (thread_.joinable()) {
        thread_.join();
    }
    running_.store(false);
}

std::string SnapshotScheduler::lastError() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastError_;
}

void SnapshotScheduler::scheduleLoop() {
    while (true) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wakeup_.wait_for(lock, interval_, [this] { return stopRequested_; });
            if (stopRequested_) {
                return;
            }
        }

        const SaveOutcome outcome = store_.save(database_);
        attempts_.fetch_add(1);

        if (!outcome.ok) {
            // A full disk or a bad path must not take the server down; the
            // failure is recorded and the next interval tries again.
            failures_.fetch_add(1);
            std::lock_guard<std::mutex> lock(mutex_);
            lastError_ = outcome.error;
        }
    }
}

}  // namespace vcache
