#pragma once

#include "Helpers.h"
#include <atomic>
#include <thread>
#include <chrono>
#include <random>

// ------------------------------
// Internal Utility
// ------------------------------
inline void RunBackToBack(
    size_t batchSize,
    std::function<void(const std::vector<Task>&)> pool,
    std::function<void()> taskBody)
{
    const size_t total = batchSize * 2;
    std::atomic<size_t> completed{ 0 };

    auto makeTask = [&]() -> Task {
        return [&] {
            taskBody();
            completed.fetch_add(1, std::memory_order_relaxed);
        };
    };

    std::vector<Task> first(batchSize);
    std::vector<Task> second(batchSize);
    for (size_t i = 0; i < batchSize; ++i) first[i] = makeTask();
    for (size_t i = 0; i < batchSize; ++i) second[i] = makeTask();

    pool(first);

    // Wait until ~90% of the first wave is complete
    size_t threshold = static_cast<size_t>(batchSize * 0.9);
    while (completed.load(std::memory_order_relaxed) < threshold)
        std::this_thread::sleep_for(std::chrono::microseconds(100));

    pool(second);

    // Wait until all tasks are complete
    while (completed.load(std::memory_order_relaxed) < total)
        std::this_thread::sleep_for(std::chrono::microseconds(100));
}

// -----------------------------------
// Test_back_to_back_noop
// -----------------------------------
// Submits two waves of 10k no-op tasks.
// Second wave is submitted after 90% of the first wave completes.
//
// Purpose:
//     Measures scheduler responsiveness under high backpressure.
//     Detects wake-up latency when work reappears unexpectedly.
//
// Workload:
//     Task = []() {};  (empty no-op)
//     Barrier = 90% of first batch must complete
//
inline void Test_back_to_back_noop(size_t numTasks, std::function<void(const std::vector<Task>&)> pool) {
    RunBackToBack(numTasks / 2, pool, [] {});
}


// -----------------------------------
// Test_back_to_back_short_compute
// -----------------------------------
// Submits two waves of short-compute tasks (~50µs).
// Second wave follows when first wave is ~90% complete.
//
// Purpose:
//     Measures pool's ability to stay hot under rapid follow-up load.
//     Tests reaction speed to a second CPU-light burst.
//
// Workload:
//     Task = simple loop (~100 iters)
//     Barrier = 90% of first batch must complete
//
inline void Test_back_to_back_short_compute(size_t numTasks, std::function<void(const std::vector<Task>&)> pool) {
    RunBackToBack(numTasks / 2, pool, [] {
        volatile int x = 0;
        for (int i = 0; i < 100; ++i) x += i;
    });
}


// -----------------------------------
// Test_back_to_back_heavy_compute
// -----------------------------------
// Submits two waves of heavy compute tasks (~1ms each).
// Second wave triggers once 90% of first completes.
//
// Purpose:
//     Tests thread pool’s resilience to long-running CPU bursts.
//     Evaluates core reuse and queue responsiveness under saturation.
//
// Workload:
//     Task = double exponent loop (~100,000 iters)
//     Barrier = 90% of first batch must complete
//
inline void Test_back_to_back_heavy_compute(size_t numTasks, std::function<void(const std::vector<Task>&)> pool) {
    RunBackToBack(numTasks / 2, pool, [] {
        volatile double x = 1.0;
        for (int i = 0; i < 100000; ++i) x *= 1.0000001;
    });
}


// -----------------------------------
// Test_back_to_back_mixed
// -----------------------------------
// Submits two waves of randomly mixed short/heavy tasks.
// Second wave submitted after 90% of first completes.
//
// Purpose:
//     Simulates market-like behavior where bursts of varied work follow each other.
//     Measures thread pool stability under dynamic/unpredictable load.
//
// Workload:
//     Task = 50/50 short (~100 iters) or heavy (~100,000 iters)
//     Barrier = 90% of first batch must complete
//
inline void Test_back_to_back_mixed(size_t numTasks, std::function<void(const std::vector<Task>&)> pool) {
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> dist(0, 1);

    RunBackToBack(numTasks / 2, pool, [&] {
        if (dist(rng) == 0) {
            volatile int x = 0;
            for (int i = 0; i < 100; ++i) x += i;
        }
        else {
            volatile double x = 1.0;
            for (int i = 0; i < 100000; ++i) x *= 1.0000001;
        }
    });
}
