#pragma once

#include "Helpers.h"

// -----------------------------------
// Test_noop
// -----------------------------------
// Generates a vector of completely empty tasks.
// Purpose: Measure raw scheduling overhead with no actual work.
// Use Case: Baseline comparison to isolate thread pool overhead.
void Test_noop(size_t numTasks, std::function<void(const std::vector<Task>&)> pool) {
    std::vector<Task> tasks(numTasks, []() {});
    pool(tasks);
}

// -----------------------------------
// Test_short
// -----------------------------------
// Generates tasks that perform a short integer loop.
// Purpose: Simulate many lightweight CPU-bound operations.
// Each task performs ~100 integer additions.
// Use Case: Microtask-heavy systems or small compute kernels.
void Test_short(size_t numTasks, std::function<void(const std::vector<Task>&)> pool) {
    std::vector<Task> tasks;
    tasks.reserve(numTasks);
    for (size_t i = 0; i < numTasks; ++i) {
        tasks.emplace_back([]() {
            volatile int val = 0;
            for (int j = 0; j < 100; ++j)
                val += j;
        });
    }
    pool(tasks);
}

// -----------------------------------
// Test_medium
// -----------------------------------
// Generates tasks that perform floating-point math in a loop.
// Purpose: Simulate medium-cost numeric workloads with L1/L2 cache fits.
// Each task performs ~10,000 iterations of sqrt(log(x)).
// Use Case: Financial indicators, scientific kernels, or moderate analytics.
void Test_medium(size_t numTasks, std::function<void(const std::vector<Task>&)> pool) {
    std::vector<Task> tasks;
    tasks.reserve(numTasks);
    for (size_t i = 0; i < numTasks; ++i) {
        tasks.emplace_back([]() {
            volatile double x = 0.0001;
            for (int j = 0; j < 10000; ++j)
                x += std::sqrt(std::log(x + 1.0));
        });
    }
    pool(tasks);
}

// -----------------------------------
// Test_heavy
// -----------------------------------
// Generates tasks that perform a large nested loop with integer math.
// Purpose: Simulate heavy compute-bound workloads with poor cache reuse.
// Each task performs 1 million (1000x1000) multiply-adds.
// Use Case: Stress test for compute bottlenecks and core saturation.
void Test_heavy(size_t numTasks, std::function<void(const std::vector<Task>&)> pool) {
    std::vector<Task> tasks;
    tasks.reserve(numTasks);
    for (size_t i = 0; i < numTasks; ++i) {
        tasks.emplace_back([]() {
            volatile int x = 0;
            for (int j = 0; j < 1000; ++j)
                for (int k = 0; k < 1000; ++k)
                    x += j * k;
        });
    }
    pool(tasks);
}

// -----------------------------------
// Test_mixed_heavy_short_alternating
// -----------------------------------
// Generates tasks alternating between a short task and a heavy task.
// Purpose: Simulate real-world task heterogeneity and test fairness/load balancing.
// Odd-indexed tasks are heavy compute loops; even-indexed are fast accumulation loops.
// Use Case: Evaluate scheduling fairness, thread starvation, and responsiveness.
void Test_mixed_heavy_short_alternating(size_t numTasks, std::function<void(const std::vector<Task>&)> pool) {
    std::vector<Task> tasks;
    tasks.reserve(numTasks);
    for (size_t i = 0; i < numTasks; ++i) {
        if (i % 2 == 0) {
            tasks.emplace_back([]() {
                volatile int val = 0;
                for (int j = 0; j < 100; ++j)
                    val += j;
            });
        }
        else {
            tasks.emplace_back([]() {
                volatile int x = 0;
                for (int j = 0; j < 1000; ++j)
                    for (int k = 0; k < 1000; ++k)
                        x += j * k;
            });
        }
    }
    pool(tasks);
}


