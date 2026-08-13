#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace vcache {

// A fixed set of worker threads pulling tasks off a shared queue.
//
// The alternative -- spawning a thread per request -- is what Discovery Document
// section 11 warns against. Thread creation costs roughly 10-100 microseconds
// and about 8 MB of address space each, so under load a thread-per-request
// server spends more time making threads than answering clients, and a burst of
// connections can exhaust the process. A pool pays that cost once at startup.
//
// The queue is guarded by one mutex; workers sleep on a condition variable when
// it is empty, so idle threads consume no CPU at all.
//
// Thread-safe: trySubmit() may be called from any number of threads at once.
// shutdown() must not be called concurrently with itself.
class ThreadPool {
public:
    static constexpr std::size_t kUnboundedQueue = 0;

    // `maxQueuedTasks` bounds work waiting to start. Unbounded queues are a
    // memory leak with extra steps under sustained overload: the server would
    // keep accepting work it has no capacity to do.
    explicit ThreadPool(std::size_t threadCount,
                        std::size_t maxQueuedTasks = kUnboundedQueue);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // Queues a task. Returns false if the pool is shutting down or the queue is
    // full -- the caller decides what to do about it, which for the server means
    // telling the client it is busy rather than silently making it wait.
    bool trySubmit(std::function<void()> task);

    // Stops accepting new tasks, lets already-queued ones run, and joins every
    // worker. Idempotent. Called by the destructor.
    void shutdown();

    std::size_t threadCount() const noexcept { return threadCount_; }
    std::size_t queuedTasks() const;

    // Number of tasks that threw. A task exception cannot be allowed to escape
    // into the thread function -- that calls std::terminate and takes the whole
    // process down -- so they are caught and counted here instead.
    std::size_t taskExceptions() const noexcept { return taskExceptions_.load(); }

private:
    void workerLoop();

    mutable std::mutex mutex_;
    std::condition_variable taskAvailable_;
    std::queue<std::function<void()>> tasks_;
    std::vector<std::thread> workers_;

    std::size_t threadCount_ = 0;
    std::size_t maxQueuedTasks_ = kUnboundedQueue;
    bool stopping_ = false;

    std::atomic<std::size_t> taskExceptions_{0};
};

}  // namespace vcache
