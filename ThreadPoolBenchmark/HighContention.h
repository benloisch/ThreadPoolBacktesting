#pragma once
#include "Helpers.h"

// -----------------------------------
// Test_atomic_increment
// -----------------------------------
// All tasks increment a shared atomic counter.
// Purpose: Test synchronization performance under extreme write contention.
// Use Case: Simulate multiple threads updating shared state.
void Test_atomic_increment(size_t numTasks, std::function<void(const std::vector<Task>&)> pool) {
    std::atomic<size_t> counter{ 0 };

    std::vector<Task> tasks;
    tasks.reserve(numTasks);
    for (size_t i = 0; i < numTasks; ++i) {
        tasks.emplace_back([&counter]() {
            counter.fetch_add(1, std::memory_order_relaxed);
        });
    }

    pool(tasks);
}

// -----------------------------------
// Test_mutex_locking
// -----------------------------------
// All tasks lock and unlock a shared mutex.
// Purpose: Simulate lock contention bottlenecks (e.g., logging, shared data).
// Use Case: Tests pool behavior under serialization pressure.
void Test_mutex_locking(size_t numTasks, std::function<void(const std::vector<Task>&)> pool) {
    std::mutex mtx;
    volatile int shared = 0;

    std::vector<Task> tasks;
    tasks.reserve(numTasks);
    for (size_t i = 0; i < numTasks; ++i) {
        tasks.emplace_back([&]() {
            std::scoped_lock<std::mutex> lock(mtx);
            shared++;
        });
    }

    pool(tasks);
}

// -----------------------------------
// Test_false_sharing
// -----------------------------------
// All tasks update adjacent cache lines in the same aligned struct.
// Purpose: Simulate false sharing penalty under tight loop workloads.
// Use Case: Measures cost of poorly aligned memory updates.
void Test_false_sharing(size_t numTasks, std::function<void(const std::vector<Task>&)> pool) {
    struct alignas(64) SharedData {
        std::atomic<size_t> slots[64]; // 64 * 8B = 512B total
    };
    SharedData data;

    for (auto& slot : data.slots) slot = 0;

    std::vector<Task> tasks;
    tasks.reserve(numTasks);
    for (size_t i = 0; i < numTasks; ++i) {
        size_t slot = i % 64; // Reuse cache lines aggressively
        tasks.emplace_back([slot, &data]() {
            data.slots[slot].fetch_add(1, std::memory_order_relaxed);
        });
    }

    pool(tasks);
}

// -----------------------------------
// Test_contention_yield
// -----------------------------------
// All tasks attempt to acquire a shared flag; if unavailable, they yield.
// Purpose: Simulates passive spinning (cooperative multitasking).
void Test_contention_yield(size_t numTasks, std::function<void(const std::vector<Task>&)> pool) {
    std::atomic<bool> flag{ false };

    std::vector<Task> tasks;
    tasks.reserve(numTasks);
    for (size_t i = 0; i < numTasks; ++i) {
        tasks.emplace_back([&flag]() {
            while (flag.exchange(true, std::memory_order_acquire)) {
                std::this_thread::yield(); // passive wait
            }

            // Simulated critical section
            volatile int x = 0;
            for (int j = 0; j < 1000; ++j) x += j;

            flag.store(false, std::memory_order_release);
        });
    }

    pool(tasks);
}

// -----------------------------------
// Test_contention_spin
// -----------------------------------
// Same as yield test, but busy-waits instead of yielding.
// Purpose: Simulates aggressive spinning and CPU burn under contention.
void Test_contention_spin(size_t numTasks, std::function<void(const std::vector<Task>&)> pool) {
    std::atomic<bool> flag{ false };

    std::vector<Task> tasks;
    tasks.reserve(numTasks);
    for (size_t i = 0; i < numTasks; ++i) {
        tasks.emplace_back([&flag]() {
            while (flag.exchange(true, std::memory_order_acquire)) {
                // spin (wastes CPU cycles)
            }

            volatile int x = 0;
            for (int j = 0; j < 1000; ++j) x += j;

            flag.store(false, std::memory_order_release);
        });
    }

    pool(tasks);
}

// -----------------------------------
// Test_contention_backoff
// -----------------------------------
// Tasks back off exponentially when contention is detected.
// Purpose: Models exponential backoff for fairness and reduced churn.
void Test_contention_backoff(size_t numTasks, std::function<void(const std::vector<Task>&)> pool) {
    std::atomic<bool> flag{ false };

    std::vector<Task> tasks;
    tasks.reserve(numTasks);
    for (size_t i = 0; i < numTasks; ++i) {
        tasks.emplace_back([&flag]() {
            int backoff = 1;
            while (flag.exchange(true, std::memory_order_acquire)) {
                for (int i = 0; i < backoff; ++i) {
                    _mm_pause(); // x86 pause instruction (intrinsic)
                }
                backoff = std::min(backoff * 2, 1024);
            }

            volatile int x = 0;
            for (int j = 0; j < 1000; ++j) x += j;

            flag.store(false, std::memory_order_release);
        });
    }

    pool(tasks);
}
