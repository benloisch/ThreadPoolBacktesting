#pragma once

#include "Helpers.h"
#include <atomic>
#include <vector>
#include <thread>
#include <chrono>
#include <random>
#include <cstdint>

// Constants
constexpr size_t CACHE_LINE_SIZE = 64;

// -----------------------------------
// Test_false_sharing_shared_counter
// -----------------------------------
// All tasks increment a single shared counter.
// 
// Purpose:
//     Simulates heavy false sharing due to all threads hitting the same cache line.
//     Detects atomic contention and coherence overhead.
//
// Workload:
//     Task = ++shared_atomic
//     Shared = one std::atomic<size_t>
//
inline void Test_false_sharing_shared_counter(size_t numTasks, std::function<void(const std::vector<Task>&)> pool) {
    static std::atomic<size_t> sharedCounter{ 0 };

    std::vector<Task> tasks(numTasks);
    for (auto& t : tasks)
        t = [&] { sharedCounter.fetch_add(1, std::memory_order_relaxed); };

    pool(tasks);
}


// -----------------------------------
// Test_false_sharing_adjacent_slots
// -----------------------------------
// Each task writes to a different element in a tightly packed array.
// 
// Purpose:
//     Simulates multiple threads accessing adjacent memory,
//     revealing cache line ping-pong and write-through issues.
//
// Workload:
//     Task = slot[i] = i
//     Slots = 1 byte apart (false sharing risk)
//
inline void Test_false_sharing_adjacent_slots(size_t numTasks, std::function<void(const std::vector<Task>&)> pool) {
    static std::vector<std::uint8_t> slots(numTasks, 0);

    std::vector<Task> tasks(numTasks);
    for (size_t i = 0; i < numTasks; ++i)
        tasks[i] = [i] {
        slots[i] = static_cast<std::uint8_t>(i);
    };

    pool(tasks);
}


// -----------------------------------
// Test_false_sharing_padded_slots
// -----------------------------------
// Each task writes to a padded slot spaced 64 bytes apart.
//
// Purpose:
//     Eliminates false sharing by placing each slot on a separate cache line.
//     Acts as control group for the adjacent_slots test.
//
// Workload:
//     Task = padded[i] = i
//     Padding = 64 bytes per element
//
inline void Test_false_sharing_padded_slots(size_t numTasks, std::function<void(const std::vector<Task>&)> pool) {
    struct alignas(CACHE_LINE_SIZE) PaddedSlot {
        std::uint64_t value = 0;
    };

    static std::vector<PaddedSlot> padded(numTasks);

    std::vector<Task> tasks(numTasks);
    for (size_t i = 0; i < numTasks; ++i)
        tasks[i] = [i] {
        padded[i].value = static_cast<std::uint64_t>(i);
    };

    pool(tasks);
}


// -----------------------------------
// Test_false_sharing_stride_slots
// -----------------------------------
// Each task writes to a slot spaced with a stride > cache line.
// 
// Purpose:
//     Simulates controlled separation of memory accesses to reduce contention,
//     useful for exploring NUMA or memory controller interference.
//
// Workload:
//     Task = wide[i * stride] = i
//     Stride = 64 to 256 bytes
//
inline void Test_false_sharing_stride_slots(size_t numTasks, std::function<void(const std::vector<Task>&)> pool) {
    constexpr size_t STRIDE = 128;
    static std::vector<std::uint64_t> wide(numTasks * STRIDE, 0);

    std::vector<Task> tasks(numTasks);
    for (size_t i = 0; i < numTasks; ++i)
        tasks[i] = [i] {
        wide[i * STRIDE] = static_cast<std::uint64_t>(i);
    };

    pool(tasks);
}
