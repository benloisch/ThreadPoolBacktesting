#pragma once

#include "Helpers.h"

// -----------------------------------
// Test_latency_noop
// -----------------------------------
// Measures pure scheduling latency using empty tasks.
// Each task captures its enqueue time, and records latency on first execution.
// Purpose: Isolate dispatch latency with zero computation cost.
// Use Case: Benchmark thread pool internal scheduling precision and responsiveness under idle or steady load.
double Test_latency_noop(size_t numTasks, std::function<void(const std::vector<Task>&)> pool) {
    struct alignas(64) Timing {
        std::chrono::steady_clock::time_point enqueue;
        std::atomic<double> latencyNs{ 0.0 };
    };
    std::vector<Timing> timings(numTasks);
    std::atomic<size_t> completed{ 0 };

    std::vector<Task> tasks;
    tasks.reserve(numTasks);
    for (size_t i = 0; i < numTasks; ++i) {
        timings[i].enqueue = std::chrono::steady_clock::now();
        tasks.emplace_back([i, &timings, &completed]() {
            auto exec = std::chrono::steady_clock::now();
            timings[i].latencyNs = std::chrono::duration<double, std::nano>(exec - timings[i].enqueue).count();
            completed.fetch_add(1, std::memory_order_relaxed);
        });
    }

    pool(tasks);
    while (completed.load(std::memory_order_relaxed) < numTasks)
        std::this_thread::yield();

    LatencyStats stats;
    for (const auto& t : timings)
        stats.record(t.latencyNs);
    stats.print("Test_latency_noop");
    return stats.getAvg();
}

// -----------------------------------
// Test_latency_short_compute
// -----------------------------------
// Measures latency of small compute tasks (light integer loop).
// Captures enqueue time, computes on thread, records scheduling delay.
// Purpose: Measure latency under lightweight CPU pressure.
// Use Case: Real-time low-latency systems with fast compute kernels.
double Test_latency_short_compute(size_t numTasks, std::function<void(const std::vector<Task>&)> pool) {
    struct alignas(64) Timing {
        std::chrono::steady_clock::time_point enqueue;
        std::atomic<double> latencyNs{ 0.0 };
    };
    std::vector<Timing> timings(numTasks);
    std::atomic<size_t> completed{ 0 };

    std::vector<Task> tasks;
    tasks.reserve(numTasks);
    for (size_t i = 0; i < numTasks; ++i) {
        timings[i].enqueue = std::chrono::steady_clock::now();
        tasks.emplace_back([i, &timings, &completed]() {
            auto exec = std::chrono::steady_clock::now();
            timings[i].latencyNs = std::chrono::duration<double, std::nano>(exec - timings[i].enqueue).count();

            volatile int x = 0;
            for (int j = 0; j < 100; ++j)
                x += j;

            completed.fetch_add(1, std::memory_order_relaxed);
        });
    }

    pool(tasks);
    while (completed.load(std::memory_order_relaxed) < numTasks)
        std::this_thread::yield();

    LatencyStats stats;
    for (const auto& t : timings)
        stats.record(t.latencyNs);
    stats.print("Test_latency_short_compute");
    return stats.getAvg();
}

// -----------------------------------
// Test_latency_mixed_spike
// -----------------------------------
// Alternates between tiny and large tasks to simulate spike scenarios.
// Half the tasks are heavy nested compute loops, half are trivial.
// Purpose: Test jitter and fairness under sharp task size disparity.
// Use Case: Real-time engines during load imbalance or priority conflict.
double Test_latency_mixed_spike(size_t numTasks, std::function<void(const std::vector<Task>&)> pool) {
    struct alignas(64) Timing {
        std::chrono::steady_clock::time_point enqueue;
        std::atomic<double> latencyNs{ 0.0 };
    };
    std::vector<Timing> timings(numTasks);
    std::atomic<size_t> completed{ 0 };

    std::vector<Task> tasks;
    tasks.reserve(numTasks);
    for (size_t i = 0; i < numTasks; ++i) {
        timings[i].enqueue = std::chrono::steady_clock::now();
        tasks.emplace_back([i, &timings, &completed]() {
            auto exec = std::chrono::steady_clock::now();
            timings[i].latencyNs = std::chrono::duration<double, std::nano>(exec - timings[i].enqueue).count();

            if (i % 2 == 0) {
                volatile int x = 0;
                for (int j = 0; j < 1000; ++j)
                    for (int k = 0; k < 1000; ++k)
                        x += j * k;
            }
            else {
                volatile int y = 0;
                for (int j = 0; j < 100; ++j)
                    y += j;
            }

            completed.fetch_add(1, std::memory_order_relaxed);
        });
    }

    pool(tasks);
    while (completed.load(std::memory_order_relaxed) < numTasks)
        std::this_thread::yield();

    LatencyStats stats;
    for (const auto& t : timings)
        stats.record(t.latencyNs);
    stats.print("Test_latency_mixed_spike");
    return stats.getAvg();
}