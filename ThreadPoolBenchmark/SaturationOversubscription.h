#pragma once
#include "Helpers.h"

extern inline size_t threadCount;

// -----------------------------------
// Oversubscription Test Generator
// -----------------------------------
// Purpose: Stress-test thread pools with extreme oversubscription ratios
// Use Case: Evaluate efficiency under pressure and whether threads yield/block effectively

inline TestFunction make_oversubscription_test(size_t multiplier) {
    return [multiplier](size_t threadCount, std::function<void(const std::vector<Task>&)> pool) {
        const size_t numTasks = threadCount * multiplier;

        std::vector<Task> tasks;
        tasks.reserve(numTasks);
        for (size_t i = 0; i < numTasks; ++i) {
            tasks.emplace_back([]() {
                volatile int x = 0;
                for (int i = 0; i < 10'000; ++i)
                    x += i;
            });
        }

        pool(tasks);
    };
}

// -----------------------------------
// Exported Test Wrappers
// These wrap the parameterized generator in Helpers-compatible form
// -----------------------------------

inline void Test_oversub_10x(size_t numTasks, std::function<void(const std::vector<Task>&)> pool) {
    make_oversubscription_test(10)(threadCount, pool);
}

inline void Test_oversub_50x(size_t numTasks, std::function<void(const std::vector<Task>&)> pool) {
    make_oversubscription_test(50)(threadCount, pool);
}

inline void Test_oversub_100x(size_t numTasks, std::function<void(const std::vector<Task>&)> pool) {
    make_oversubscription_test(100)(threadCount, pool);
}
