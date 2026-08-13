// Phase 5a test suite -- the thread pool.
//
// These tests are only as good as the tool watching them: a race can pass a
// thousand runs and fail the next. The suite is built to run under
// ThreadSanitizer, which detects unsynchronised access even when the timing
// happens to work out.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

#include "ThreadPool.h"
#include "test_framework.h"

using vcache::ThreadPool;

VCACHE_TEST(PoolRunsEverySubmittedTask) {
    constexpr int kTasks = 10000;
    std::atomic<int> completed{0};

    {
        ThreadPool pool(8);
        for (int i = 0; i < kTasks; ++i) {
            CHECK(pool.trySubmit([&completed] { completed.fetch_add(1); }));
        }
        // The destructor drains the queue and joins, so by the time the scope
        // ends every task has run.
    }

    CHECK_EQ(completed.load(), kTasks);
}

VCACHE_TEST(WorkIsSpreadAcrossThreads) {
    // Proves the pool is actually parallel rather than one worker doing
    // everything. Each task sleeps briefly so the work cannot all be finished
    // by whichever thread wakes first.
    std::mutex mutex;
    std::set<std::thread::id> observed;

    {
        ThreadPool pool(4);
        for (int i = 0; i < 40; ++i) {
            CHECK(pool.trySubmit([&mutex, &observed] {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                std::lock_guard<std::mutex> lock(mutex);
                observed.insert(std::this_thread::get_id());
            }));
        }
    }

    CHECK(observed.size() > 1);
    CHECK(observed.size() <= std::size_t{4});
}

VCACHE_TEST(ThreadCountIsReportedAndNeverZero) {
    ThreadPool pool(6);
    CHECK_EQ(pool.threadCount(), std::size_t{6});

    // Zero workers would mean tasks are accepted and never run -- silently.
    ThreadPool degenerate(0);
    CHECK_EQ(degenerate.threadCount(), std::size_t{1});
}

VCACHE_TEST(SubmissionsFromManyThreadsAreSafe) {
    constexpr int kProducers = 8;
    constexpr int kPerProducer = 1000;
    std::atomic<int> completed{0};

    {
        ThreadPool pool(4);

        std::vector<std::thread> producers;
        producers.reserve(kProducers);
        for (int p = 0; p < kProducers; ++p) {
            producers.emplace_back([&pool, &completed] {
                for (int i = 0; i < kPerProducer; ++i) {
                    while (!pool.trySubmit([&completed] { completed.fetch_add(1); })) {
                        std::this_thread::yield();
                    }
                }
            });
        }
        for (std::thread& producer : producers) {
            producer.join();
        }
    }

    CHECK_EQ(completed.load(), kProducers * kPerProducer);
}

VCACHE_TEST(FullQueueRefusesWork) {
    // One worker held busy, a queue of two: the fourth submission must fail
    // rather than grow the queue without limit.
    std::atomic<bool> release{false};
    ThreadPool pool(1, /*maxQueuedTasks=*/2);

    CHECK(pool.trySubmit([&release] {
        while (!release.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }));

    // Let the worker pick up the blocking task before filling the queue.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    CHECK(pool.trySubmit([] {}));
    CHECK(pool.trySubmit([] {}));
    CHECK(!pool.trySubmit([] {}));  // queue full
    CHECK_EQ(pool.queuedTasks(), std::size_t{2});

    release.store(true);
}

VCACHE_TEST(UnboundedQueueAcceptsEverything) {
    std::atomic<bool> release{false};
    ThreadPool pool(1, ThreadPool::kUnboundedQueue);

    pool.trySubmit([&release] {
        while (!release.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    for (int i = 0; i < 5000; ++i) {
        CHECK(pool.trySubmit([] {}));
    }

    release.store(true);
}

VCACHE_TEST(QueuedTasksStillRunAfterShutdownIsRequested) {
    // Work already accepted is not silently dropped: shutdown() stops new
    // submissions but drains what is already queued.
    std::atomic<int> completed{0};

    ThreadPool pool(2);
    for (int i = 0; i < 500; ++i) {
        pool.trySubmit([&completed] { completed.fetch_add(1); });
    }

    pool.shutdown();
    CHECK_EQ(completed.load(), 500);
}

VCACHE_TEST(SubmissionAfterShutdownIsRefused) {
    ThreadPool pool(2);
    pool.shutdown();

    CHECK(!pool.trySubmit([] {}));
}

VCACHE_TEST(ShutdownIsIdempotent) {
    std::atomic<int> completed{0};
    ThreadPool pool(2);
    pool.trySubmit([&completed] { completed.fetch_add(1); });

    pool.shutdown();
    pool.shutdown();  // must not double-join or hang
    pool.shutdown();

    CHECK_EQ(completed.load(), 1);
}

VCACHE_TEST(AThrowingTaskDoesNotKillItsWorker) {
    // An exception escaping a thread's entry function calls std::terminate. If
    // this is handled wrongly, the process dies rather than the test failing.
    std::atomic<int> completed{0};

    {
        ThreadPool pool(2);
        for (int i = 0; i < 20; ++i) {
            pool.trySubmit([] { throw std::runtime_error("task failed"); });
        }
        for (int i = 0; i < 20; ++i) {
            pool.trySubmit([&completed] { completed.fetch_add(1); });
        }
    }

    CHECK_EQ(completed.load(), 20);  // the pool kept working
}

VCACHE_TEST(TaskExceptionsAreCounted) {
    ThreadPool pool(1);
    for (int i = 0; i < 5; ++i) {
        pool.trySubmit([] { throw std::runtime_error("boom"); });
    }
    pool.shutdown();

    CHECK_EQ(pool.taskExceptions(), std::size_t{5});
}

VCACHE_TEST(IdlePoolShutsDownImmediately) {
    // Workers wait on a condition variable, so an idle pool should cost nothing
    // and stop instantly rather than after some polling interval.
    const auto start = std::chrono::steady_clock::now();
    {
        ThreadPool pool(16);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;

    CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() < 500);
}

VCACHE_TEST(TasksSeeTheirCapturedState) {
    // Each task gets its own captured index; a pool that shared one slot would
    // show duplicates or gaps here.
    std::mutex mutex;
    std::vector<int> seen;

    {
        ThreadPool pool(4);
        for (int i = 0; i < 200; ++i) {
            pool.trySubmit([i, &mutex, &seen] {
                std::lock_guard<std::mutex> lock(mutex);
                seen.push_back(i);
            });
        }
    }

    CHECK_EQ(seen.size(), std::size_t{200});
    std::set<int> unique(seen.begin(), seen.end());
    CHECK_EQ(unique.size(), std::size_t{200});
}

int main() {
    return testing::runAll();
}
