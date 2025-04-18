#pragma once

#include "Helpers.h"
#include <thread>
#include <chrono>
#include <random>

// 🧪 Delay between each submission (ms)
constexpr int dropoffDelayMs = 10;

// 🧪 Gradual submission loop
inline void GradualSubmit(size_t numTasks, std::function<void(const std::vector<Task>&)> pool, std::function<void()> taskGen) {
    std::vector<Task> taskVec(1);

    for (size_t i = 0; i < numTasks; ++i) {
        taskVec[0] = taskGen;
        pool(taskVec);
        std::this_thread::sleep_for(std::chrono::microseconds(dropoffDelayMs));
    }
}

// -----------------------------------
// Test_perf_dropoff_noop
// -----------------------------------
// Gradually submits 10,000 no-op tasks over 10 seconds.
// Simulates sparse load to detect scheduler responsiveness & wake-up cost.
// 
// Purpose:
//     Uncovers thread pools that degrade under low task arrival rate.
//
// Workload:
//     Task = []() {};     (empty no-op)
//     Delay = 1ms between each submission
//
inline void Test_perf_dropoff_noop(size_t numTasks, std::function<void(const std::vector<Task>&)> pool) {
    GradualSubmit(numTasks, pool, []() {});
}


// -----------------------------------
// Test_perf_dropoff_short_compute
// -----------------------------------
// Submits lightweight compute tasks (10–50 µs) every 1ms.
// 
// Purpose:
//     Tests wake-up cost under realistic low-latency workloads.
//
// Workload:
//     Task = simple int loop (~100 iters)
//     Delay = 1ms between each submission
//
inline void Test_perf_dropoff_short_compute(size_t numTasks, std::function<void(const std::vector<Task>&)> pool) {
    GradualSubmit(numTasks, pool, []() {
        volatile int x = 0;
        for (int i = 0; i < 100; ++i) x += i;
    });
}


// -----------------------------------
// Test_perf_dropoff_heavy_compute
// -----------------------------------
// Submits heavy CPU-bound tasks (~0.5–2ms) every 1ms.
// 
// Purpose:
//     Detects thread reuse inefficiency or idle thread starvation.
//
// Workload:
//     Task = double exponent loop (~100,000 iters)
//     Delay = 1ms between each submission
//
inline void Test_perf_dropoff_heavy_compute(size_t numTasks, std::function<void(const std::vector<Task>&)> pool) {
    GradualSubmit(numTasks, pool, []() {
        volatile double x = 1.0;
        for (int i = 0; i < 100000; ++i) x *= 1.0000001;
    });
}


// -----------------------------------
// Test_perf_dropoff_mixed
// -----------------------------------
// Submits a 50/50 mix of short and heavy tasks over time.
// 
// Purpose:
//     Simulates variable market conditions and thread contention.
//
// Workload:
//     Task = randomly chosen short or heavy
//     Delay = 1ms between each submission
//
inline void Test_perf_dropoff_mixed(size_t numTasks, std::function<void(const std::vector<Task>&)> pool) {
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> dist(0, 1);

    GradualSubmit(numTasks, pool, [&]() {
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


// -----------------------------------
// Test_perf_dropoff_jitter
// -----------------------------------
// Submits medium compute tasks with a random 1–5ms delay.
//
// Purpose:
//     Simulates noisy or jittery load (common in real systems).
//     Detects uneven idle recovery or jitter sensitivity.
//
// Workload:
//     Task = int loop (~500 iters)
//     Delay = 1–5ms randomized between tasks
//
inline void Test_perf_dropoff_jitter(size_t numTasks, std::function<void(const std::vector<Task>&)> pool) {
    std::mt19937 rng(1337);
    std::uniform_int_distribution<int> delayDist(50, 300);  // µs, not ms

    std::vector<Task> taskVec(1);
    taskVec[0] = []() {
        volatile int x = 0;
        for (int i = 0; i < 500; ++i) x += i;
    };

    for (size_t i = 0; i < numTasks; ++i) {
        pool(taskVec);
        std::this_thread::sleep_for(std::chrono::microseconds(delayDist(rng)));
    }
}
