#pragma once

// Shared benchmark plumbing.
//
// Discovery Document section 16 is blunt about this: "Do not claim performance
// numbers until you measure them." The corollary is that a measurement is only
// worth quoting if the method behind it is visible, so the choices made here are
// spelled out rather than buried:
//
//   * Every measured loop runs a WARMUP pass first. The first pass through cold
//     caches, an unfaulted heap and a not-yet-branch-predicted loop measures
//     startup, not steady state.
//   * Every result is the MEDIAN of several repetitions. A mean lets one
//     scheduler hiccup dominate; a minimum flatters by reporting the luckiest
//     run on an otherwise busy machine.
//   * Results are consumed through doNotOptimize() so the optimiser cannot
//     delete the work being timed -- the classic way to measure a compiler's
//     dead-code elimination and call it a database.
//   * Keys are generated from a FIXED seed, so two runs compare like with like.
//
// What these numbers are NOT: a claim about any other machine, compiler,
// allocator or load. They are one machine, stated at the top of every report.

#include <sys/resource.h>
#ifdef __APPLE__
#include <mach/mach.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace bench {

using Clock = std::chrono::steady_clock;

// Stops the optimiser removing work whose result is otherwise unused.
template <typename T>
inline void doNotOptimize(const T& value) {
    asm volatile("" : : "r,m"(value) : "memory");
}

inline double secondsSince(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

// Latency distribution, in microseconds.
//
// Percentiles rather than an average alone, because an average hides exactly the
// behaviour that matters. A service where 99 requests take 1us and one takes
// 100ms averages 1ms and looks fine; the P99 says otherwise.
struct Latency {
    double mean = 0;
    double p50 = 0;
    double p95 = 0;
    double p99 = 0;
    double max = 0;
};

inline Latency summarise(std::vector<double>& samplesMicros) {
    Latency result;
    if (samplesMicros.empty()) {
        return result;
    }

    std::sort(samplesMicros.begin(), samplesMicros.end());

    double total = 0;
    for (const double sample : samplesMicros) {
        total += sample;
    }
    result.mean = total / static_cast<double>(samplesMicros.size());

    const auto at = [&samplesMicros](double quantile) {
        const auto lastIndex = static_cast<double>(samplesMicros.size() - 1);
        const auto index = static_cast<std::size_t>(quantile * lastIndex);
        return samplesMicros[index];
    };

    result.p50 = at(0.50);
    result.p95 = at(0.95);
    result.p99 = at(0.99);
    result.max = samplesMicros.back();
    return result;
}

// Runs `body` once as a warmup, then `repetitions` timed times, and returns the
// median elapsed seconds.
template <typename Body>
double timeMedian(int repetitions, const Body& body) {
    body();  // warmup, discarded

    std::vector<double> timings;
    timings.reserve(static_cast<std::size_t>(repetitions));
    for (int i = 0; i < repetitions; ++i) {
        const auto start = Clock::now();
        body();
        timings.push_back(secondsSince(start));
    }

    std::sort(timings.begin(), timings.end());
    return timings[timings.size() / 2];
}

// Deterministic key set, so every run and every implementation sees identical
// input.
inline std::vector<std::string> makeKeys(std::size_t count, const std::string& prefix = "key:") {
    std::mt19937_64 rng(0x5643414348450001ULL);  // fixed seed: reproducible runs
    std::vector<std::string> keys;
    keys.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        keys.push_back(prefix + std::to_string(rng()));
    }
    return keys;
}

// CURRENT resident set size in bytes.
//
// Deliberately not getrusage(ru_maxrss): that reports a PEAK, so once a process
// has allocated a lot once, every later before/after delta reads as zero. The
// first version of this file made exactly that mistake and produced a memory
// comparison that meant nothing.
inline std::size_t currentResidentBytes() {
#ifdef __APPLE__
    mach_task_basic_info info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS) {
        return 0;
    }
    return static_cast<std::size_t>(info.resident_size);
#else
    // /proc/self/statm reports current resident pages on Linux.
    std::ifstream statm("/proc/self/statm");
    std::size_t totalPages = 0;
    std::size_t residentPages = 0;
    if (statm >> totalPages >> residentPages) {
        return residentPages * static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    }
    return 0;
#endif
}

// ------------------------------------------------------------- reporting ----

inline void printHeading(const std::string& title) {
    std::cout << "\n" << title << "\n" << std::string(title.size(), '=') << "\n";
}

inline void printThroughputHeader() {
    std::cout << std::left << std::setw(34) << "  workload" << std::right << std::setw(14)
              << "ops/sec" << std::setw(12) << "ns/op" << "\n";
    std::cout << "  " << std::string(66, '-') << "\n";
}

inline void printThroughput(const std::string& label, double opsPerSecond) {
    std::cout << std::left << std::setw(34) << ("  " + label) << std::right << std::fixed
              << std::setprecision(0) << std::setw(14) << opsPerSecond << std::setw(12)
              << (1e9 / opsPerSecond) << "\n";
}

inline void printLatencyHeader() {
    std::cout << std::left << std::setw(22) << "  configuration" << std::right << std::setw(13)
              << "ops/sec" << std::setw(10) << "mean" << std::setw(10) << "p50" << std::setw(10)
              << "p95" << std::setw(10) << "p99" << std::setw(10) << "max" << "\n";
    std::cout << "  " << std::string(79, '-') << "\n";
}

inline void printLatency(const std::string& label, double opsPerSecond, const Latency& latency) {
    std::cout << std::left << std::setw(22) << ("  " + label) << std::right << std::fixed
              << std::setprecision(0) << std::setw(13) << opsPerSecond << std::setprecision(1)
              << std::setw(10) << latency.mean << std::setw(10) << latency.p50 << std::setw(10)
              << latency.p95 << std::setw(10) << latency.p99 << std::setw(10) << latency.max
              << "\n";
}

inline void printNote(const std::string& text) {
    std::cout << "  " << text << "\n";
}

}  // namespace bench
