#include "ThreadPool.h"

#include <utility>

namespace vcache {

ThreadPool::ThreadPool(std::size_t threadCount, std::size_t maxQueuedTasks)
    : maxQueuedTasks_(maxQueuedTasks) {
    if (threadCount == 0) {
        threadCount = 1;  // a pool with no workers would silently never run anything
    }
    threadCount_ = threadCount;

    workers_.reserve(threadCount);
    for (std::size_t i = 0; i < threadCount; ++i) {
        workers_.emplace_back([this] { workerLoop(); });
    }
}

ThreadPool::~ThreadPool() {
    shutdown();
}

bool ThreadPool::trySubmit(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            return false;
        }
        if (maxQueuedTasks_ != kUnboundedQueue && tasks_.size() >= maxQueuedTasks_) {
            return false;
        }
        tasks_.push(std::move(task));
    }

    // Notify outside the lock: waking a worker while still holding the mutex it
    // needs makes it wake up only to block again.
    taskAvailable_.notify_one();
    return true;
}

void ThreadPool::shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    taskAvailable_.notify_all();

    for (std::thread& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();  // makes a second shutdown() a no-op
}

std::size_t ThreadPool::queuedTasks() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tasks_.size();
}

void ThreadPool::workerLoop() {
    while (true) {
        std::function<void()> task;

        {
            std::unique_lock<std::mutex> lock(mutex_);

            // The predicate form guards against spurious wakeups (a condition
            // variable is permitted to wake without being notified) and against
            // the lost-wakeup race where notify_one() fires between a worker
            // checking the queue and going to sleep.
            taskAvailable_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });

            // Shutting down AND drained: nothing left to do. Note the ordering --
            // queued tasks still run after shutdown() is called, so work already
            // accepted is not silently dropped.
            if (tasks_.empty()) {
                return;
            }

            task = std::move(tasks_.front());
            tasks_.pop();
        }

        // Run the task with the lock released, or the pool would execute one
        // task at a time no matter how many workers it has.
        try {
            task();
        } catch (...) {
            // An exception escaping a thread's entry function calls
            // std::terminate. One misbehaving task must not kill the server.
            taskExceptions_.fetch_add(1);
        }
    }
}

}  // namespace vcache
