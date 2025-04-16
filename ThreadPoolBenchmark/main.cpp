#include "ThreadPool.h"   // Your primary thread pool implementation.
#include "hp_threadpool.h"
#include "dp_threadpool.h"
#include "BS_thread_pool.hpp"
#include "task_thread_pool.hpp"
#include "concurrentqueue.h"
#include <ppl.h>
#include <taskflow/taskflow.hpp>
#include <omp.h>
#define __TBB_NO_IMPLICIT_LINKAGE 1
#include <tbb/task_arena.h>
#include <tbb/parallel_for.h>


#include <Windows.h>
#undef min // Prevent macro interference
#include <chrono>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "Helpers.h"
#include "SequentialTaskSizeSweepTests.h"
#include "LatencyTests.h"
#include "HighContention.h"
#include "SaturationOversubscription.h"


// Global constant for all thread pool instances.
inline size_t threadCount = 24;

// Number of times to run each suite.
inline int runsPerSuite = 10;


// ===================================================================
//  ********** THREAD POOLS / TASK GROUPS TO BENCHMARK ***************
// ===================================================================

// Custom ThreadPool  (No Affinity)
//
// Uses your custom ThreadPool implementation with a fixed number of threads,
// no affinity binding, and all tasks submitted individually.
// Suitable for general-purpose task execution with centralized queuing.
//
// Pros: Simple and flexible.
// Cons: No NUMA/core locality awareness, possible contention on shared queue.
auto main_ThreadPool = [](const std::string& poolName) -> std::function<void(const std::vector<Task>&)> {
    return [poolName](const std::vector<Task>& tasks) {
        ThreadPool pool(poolName, threadCount, 0, false);
        for (const auto& task : tasks)
            pool.tryEnqueueTask(task, false);
        pool.shutdownProcessRemainingTasks();
    };
};

// Custom ThreadPool  (With Thread Affinity)
//
// Same as `main_ThreadPool` but enables affinity binding to assign threads to specific cores.
// Improves cache locality and reduces migration overhead on multi-core CPUs.
//
// Pros: Better performance on NUMA/many-core systems.
// Cons: Less dynamic load balancing if task times vary widely.
auto main_ThreadPool_Affinity = [](const std::string& poolName) -> std::function<void(const std::vector<Task>&)> {
    return [poolName](const std::vector<Task>& tasks) {
        ThreadPool pool(poolName, threadCount, 0, true);
        for (const auto& task : tasks)
            pool.tryEnqueueTask(task, false);
        pool.shutdownProcessRemainingTasks();
    };
};

// Custom ThreadPool  (Batched Submission, Affinity Enabled)
//
// Extends `main_ThreadPool_Affinity` by grouping task submission into batches of 16.
// Reduces enqueue overhead and improves throughput under high task count scenarios.
//
// Pros: Higher enqueue efficiency, good for microtask-heavy workloads.
// Cons: Increased latency for some tasks due to batching boundaries.
auto main_ThreadPool_Batched_16 = [](const std::string& poolName) -> std::function<void(const std::vector<Task>&)> {
    return [poolName](const std::vector<Task>& tasks) {
        ThreadPool pool(poolName, threadCount, 0, true);
        constexpr size_t batchSize = 16; // configurable external batch size
        for (size_t i = 0; i < tasks.size(); i += batchSize) {
            size_t end = std::min(i + batchSize, tasks.size());
            for (size_t j = i; j < end; ++j) {
                pool.tryEnqueueTask(tasks[j], false);
            }
        };
        pool.shutdownProcessRemainingTasks(); 
    };
};

// Custom ThreadPool + oneTBB Batching (Batch Size = specified)
//
// Uses your custom ThreadPool for task queuing and lifecycle control,
// but batches tasks in groups of 16 and executes each batch using
// `tbb::parallel_for` for efficient chunked parallelism.
//
// Suitable for high-throughput workloads with small, uniform tasks
// where enqueue overhead and thread contention are bottlenecks.
//
// Pros: Combines flexible queueing and logging with oneTBB's parallel efficiency.
//       Reduced overhead via batched submission and chunked execution.
// Cons: Slight latency added by batching; tasks lose direct core assignment control.
inline std::function<void(const std::vector<Task>&)> main_oneTBB_Batched_ThreadPool(
    const std::string& poolName,
    size_t batchSize)
{
    return [poolName, batchSize](const std::vector<Task>& tasks) {
        ThreadPool pool(poolName, threadCount, 0, false);
        std::vector<Task> batch;
        batch.reserve(batchSize);

        for (size_t i = 0; i < tasks.size(); ++i) {
            batch.emplace_back(tasks[i]);
            if (batch.size() == batchSize) {
                auto execBatch = batch;  // copy batch
                pool.tryEnqueueTask([execBatch]() mutable {
                    tbb::parallel_for(
                        tbb::blocked_range<size_t>(0, execBatch.size()),
                        [&](const tbb::blocked_range<size_t>& r) {
                        for (size_t i = r.begin(); i < r.end(); ++i)
                            execBatch[i]();
                    }
                    );
                }, false);
                batch.clear();
            }
        }

        // Handle leftovers
        if (!batch.empty()) {
            auto execBatch = batch;
            pool.tryEnqueueTask([execBatch]() mutable {
                tbb::parallel_for(
                    tbb::blocked_range<size_t>(0, execBatch.size()),
                    [&](const tbb::blocked_range<size_t>& r) {
                    for (size_t i = r.begin(); i < r.end(); ++i)
                        execBatch[i]();
                }
                );
            }, false);
        }

        pool.shutdownProcessRemainingTasks();
    };
}

// HPThreadPool 
//
// Lightweight, high-performance thread pool using spinlocks and a fixed-size worker pool.
// Tasks are posted with a lock-free ring buffer and processed until empty.
//
// Pros: Very fast for short, CPU-bound tasks.
// Cons: Manual task yield/spinloop management may underperform under imbalance.
//https://github.com/7starsea/hp-threadpool
auto HP_ThreadPool = [](const std::string& poolName) -> std::function<void(const std::vector<Task>&)> {
    return [poolName](const std::vector<Task>& tasks) {
        HPThreadPool::ThreadPool<std::function<void()>> pool(threadCount);
        pool.start();
        for (const auto& task : tasks)
            while (!pool.post(task)) std::this_thread::yield();
        while (!pool.is_task_done()) std::this_thread::yield();
        pool.stop();
    };
};

// DeveloperPaul ThreadPool 
//
// Mature thread pool implementation with internal task queue and worker thread management.
// Uses a blocking queue and condition variables to distribute work.
//
// Pros: Robust design, well-tested, simple API.
// Cons: May introduce slight overhead for signaling and queue locking.
//https://github.com/DeveloperPaul123/thread-pool
auto DP_ThreadPool = [](const std::string& poolName) -> std::function<void(const std::vector<Task>&)> {
    return [poolName](const std::vector<Task>& tasks) {
        dp::thread_pool pool(threadCount);
        for (const auto& task : tasks)
            pool.enqueue(task);
        pool.wait_for_tasks();
    };
};

// BS::thread_pool 
//
// Header-only, C++17 thread pool with support for task futures, priorities, and batching.
// Tasks are submitted via `submit_task`, and execution is automatic.
//
// Pros: Easy to use, feature-rich, consistent performance.
// Cons: Higher overhead for managing advanced features unless optimized.
//https://github.com/bshoshany/thread-pool
auto BS_ThreadPool = [](const std::string& poolName) -> std::function<void(const std::vector<Task>&)> {
    return [poolName](const std::vector<Task>& tasks) {
        BS::thread_pool pool(threadCount);
        for (const auto& task : tasks)
            pool.submit_task(task);
        pool.wait();
    };
};

// task_thread_pool 
//
// High-performance thread pool that supports `submit_detach` for fire-and-forget tasks.
// Internal synchronization uses atomics and optimized wait strategies.
//
// Pros: Extremely fast detachment pattern, low latency on submission.
// Cons: Less control over task lifecycle or task return values.
//https://github.com/alugowski/task-thread-pool
auto task_ThreadPool = [](const std::string& poolName) -> std::function<void(const std::vector<Task>&)> {
    return [poolName](const std::vector<Task>& tasks) {
        task_thread_pool::task_thread_pool pool(threadCount);
        for (const auto& task : tasks) 
            pool.submit_detach(task);
        pool.wait_for_tasks();
    };
};

// moodycamel::ConcurrentQueue + Manual Worker Threads
//
// Custom thread pool using lock-free MPMC queue and manual worker threads.
// Tasks are enqueued into the queue, and workers poll until all are done.
//
// Pros: Lock-free queue ensures very high throughput under pressure.
// Cons: Manual lifecycle management, no built-in thread abstraction.
//https://github.com/cameron314/concurrentqueue
auto moody_ConcurrentQueue_ThreadPool = [](const std::string& poolName) -> std::function<void(const std::vector<Task>&)> {
    return [poolName](const std::vector<Task>& tasks) {
        moodycamel::ConcurrentQueue<Task> queue;
        std::atomic<bool> done{ false };
        std::atomic<size_t> remainingTasks{ tasks.size() };
        std::vector<std::thread> workers;

        for (size_t i = 0; i < threadCount; ++i) {
            workers.emplace_back([&]() {
                Task task;
                while (!done || remainingTasks.load(std::memory_order_relaxed) > 0)
                {
                    while (queue.try_dequeue(task)) {
                        task();
                        --remainingTasks;
                    }
                    std::this_thread::yield(); // Backoff
                }
            });
        }

        // Enqueue all tasks
        for (const auto& task : tasks)
            queue.enqueue(task);

        // Wait until all tasks are completed
        while (remainingTasks.load(std::memory_order_relaxed) > 0)
            std::this_thread::yield();

        done = true;

        for (auto& thread : workers)
            thread.join();
    };
};

// Microsoft PPL Task Group Thread Pool 
//
// This pool  uses Concurrency::task_group from Microsoft's Parallel Patterns Library (PPL).
// It creates a group of tasks that run in parallel using the Windows ThreadPool backend,
// and waits for all of them to finish. This is suitable for moderately parallel workloads
// with dynamic task dispatch, and is backed by highly optimized Windows fibers.
//
// Pros: Minimal setup, good task granularity, OS-managed threads.
// Cons: Limited configurability (no affinity, no batching), not ideal for extreme HPC.
//
// Reference: https://learn.microsoft.com/en-us/cpp/parallel/concrt/task-groups
auto MS_PPL_TaskGroup = [](const std::string& poolName) -> std::function<void(const std::vector<Task>&)> {
    return [poolName](const std::vector<Task>& tasks) {
        Concurrency::task_group tg;
        for (const auto& task : tasks)
            tg.run(task);
        tg.wait(); // blocks until all tasks are finished
    };
};

// Microsoft PPL Parallel For Thread Pool 
//
// This pool  uses Concurrency::parallel_for to execute all tasks in the input vector
// by parallelizing a loop over task indices. It is ideal for uniform, loop-style workloads
// where each task performs a similar amount of work. Internally, PPL handles dynamic chunking,
// load balancing, and work stealing.
//
// Pros: Extremely efficient for bulk uniform workloads.
// Cons: Requires indexed task access, not flexible for heterogeneous tasks.
//
// Reference: https://learn.microsoft.com/en-us/cpp/parallel/concrt/parallel-algorithms
auto MS_PPL_TaskGroup_parallel_for = [](const std::string& poolName) -> std::function<void(const std::vector<Task>&)> {
    return [poolName](const std::vector<Task>& tasks) {
        Concurrency::parallel_for(0, static_cast<int>(tasks.size()), [&](int i) {
            tasks[i](); // Execute the task at index i
        });
    };
};

// OpenMP Parallel For Thread Pool
//
// Uses OpenMP to parallelize a loop over all tasks. Threads are created and managed by the compiler/runtime.
//
// Pros: Zero setup, good scaling for uniform tasks, no manual thread management.
// Cons: No persistent pool, can't enqueue irregular task graphs, less flexible.
//
// Requires: Compile with /openmp (MSVC) or -fopenmp (GCC/Clang)
auto OpenMP_parallel_for = [](const std::string& poolName) -> std::function<void(const std::vector<Task>&)> {
    return [poolName](const std::vector<Task>& tasks) {
#pragma omp parallel for num_threads(threadCount)
        for (int i = 0; i < static_cast<int>(tasks.size()); ++i) {
            tasks[i]();
        }
    };
};

// oneTBB_TaskGroup_parallel_for 
//
// Uses Intel oneAPI Threading Building Blocks (oneTBB) to execute all tasks via `tbb::parallel_for`.
// The thread count is limited by `tbb::task_arena` to ensure consistency across benchmarks.
// Tasks are accessed by index and executed in parallel via blocked ranges, allowing efficient chunking
// and load balancing across cores.
//
// Pros: Highly optimized parallel loop, NUMA-aware, auto-chunked.
// Cons: Requires indexed task access (not general queueing), no persistent pool.
//
// Requires: Linking against `tbb12.lib` (oneTBB runtime library)
// Reference: https://github.com/oneapi-src/oneTBB
auto oneTBB_TaskGroup_parallel_for = [](const std::string& poolName) -> std::function<void(const std::vector<Task>&)> {
    return [poolName](const std::vector<Task>& tasks) {
        tbb::task_arena arena(threadCount);  // Match thread count with others
        arena.execute([&] {
            tbb::parallel_for(
                tbb::blocked_range<size_t>(0, tasks.size()),
                [&](const tbb::blocked_range<size_t>& range) {
                for (size_t i = range.begin(); i < range.end(); ++i)
                    tasks[i]();
            }
            );
        });
    };
};

// oneTBB parallel_for (Grain Size = 16)
//
// Executes tasks using oneTBB's `parallel_for` with a manually specified grain size of 16,
// which controls how tasks are chunked into subranges. This reduces task overhead and
// improves cache usage for very small tasks.
//
// Pros: Better load balancing for microtasks, fewer scheduler decisions.
// Cons: Grain size must be tuned to workload; too small may increase overhead.
auto oneTBB_parallel_for_grain16 = [](const std::string& poolName) -> std::function<void(const std::vector<Task>&)> {
    return [poolName](const std::vector<Task>& tasks) {
        tbb::task_arena arena(threadCount);
        arena.execute([&] {
            tbb::parallel_for(
                tbb::blocked_range<size_t>(0, tasks.size(), 16),  // grain size = 16
                [&](const tbb::blocked_range<size_t>& r) {
                for (size_t i = r.begin(); i < r.end(); ++i)
                    tasks[i]();
            }
            );
        });
    };
};

// oneTBB parallel_for (Strict Arena)
//
// Executes tasks using a dedicated `tbb::task_arena` that is explicitly initialized and terminated.
// Ensures full control over thread pool isolation, parallelism degree, and lifecycle.
//
// Pros: Thread containment, full arena lifecycle control.
// Cons: Slightly more overhead due to arena init/teardown if used repeatedly.
auto oneTBB_parallel_for_strict_arena = [](const std::string& poolName) -> std::function<void(const std::vector<Task>&)> {
    return [poolName](const std::vector<Task>& tasks) {
        tbb::task_arena arena(threadCount);
        arena.initialize();  // Ensure explicit init

        arena.execute([&] {
            tbb::parallel_for(
                tbb::blocked_range<size_t>(0, tasks.size(), 32),
                [&](const tbb::blocked_range<size_t>& r) {
                for (size_t i = r.begin(); i < r.end(); ++i)
                    tasks[i]();
            }
            );
        });

        arena.terminate();  // Optional explicit shutdown
    };
};

// oneTBB task_group Fusion (Manual Chunking)
//
// Uses `tbb::task_group` to manually divide the task list into fixed-size chunks,
// each run as a separate subtask. This provides a balance between batching and flexibility.
//
// Pros: Manual control over batch sizes, avoids blocked_range overhead.
// Cons: Slightly more verbose, less automatic than parallel_for.

auto oneTBB_task_group_fusion = [](const std::string& poolName) -> std::function<void(const std::vector<Task>&)> {
    return [poolName](const std::vector<Task>& tasks) {
        tbb::task_arena arena(threadCount);
        arena.execute([&] {
            tbb::task_group tg;
            const size_t chunkSize = 32;

            for (size_t i = 0; i < tasks.size(); i += chunkSize) {
                tg.run([&, start = i] {
                    size_t end = std::min(start + chunkSize, tasks.size());
                    for (size_t j = start; j < end; ++j)
                        tasks[j]();
                });
            }

            tg.wait();  // Wait for all task chunks to finish
        });
    };
};

// oneTBB Static Unrolled For (Fixed Step)
//
// Executes tasks using `tbb::parallel_for` with a fixed step size (e.g. 64),
// manually controlling loop bounds and skipping blocked_range entirely.
// Suitable for uniform workloads with predictable execution time.
//
// Pros: Extremely fast, minimal scheduler overhead.
// Cons: Hardcoded chunk size, less adaptive to imbalance.

auto oneTBB_static_unrolled = [](const std::string& poolName) -> std::function<void(const std::vector<Task>&)> {
    return [poolName](const std::vector<Task>& tasks) {
        tbb::task_arena arena(threadCount);
        const size_t chunkSize = 64;

        arena.execute([&] {
            tbb::parallel_for(size_t(0), tasks.size(), chunkSize, [&](size_t i) {
                size_t end = std::min(i + chunkSize, tasks.size());
                for (size_t j = i; j < end; ++j)
                    tasks[j]();
            });
        });
    };
};



// Taskflow Executor Thread Pool
//
// Taskflow uses a graph-based task dependency model and a thread pool under the hood.
// Here we treat each task independently and submit them as a flat graph.
//
// Pros: Header-only, clean interface, internal thread pool management.
// Cons: No explicit thread pinning support, mostly suited for graph workloads.
//
// https://github.com/taskflow/taskflow
auto Taskflow_ThreadPool = [](const std::string& poolName) -> std::function<void(const std::vector<Task>&)> {
    return [poolName](const std::vector<Task>& tasks) {
        tf::Executor executor(threadCount);
        tf::Taskflow taskflow;
        for (const auto& task : tasks)
            taskflow.emplace(task);
        executor.run(taskflow).wait();
    };
};


int main() {

    // Adding a new runner is as simple as adding another entry here.
    std::vector<PoolSpec> pools = {
        /*
    {"main_ThreadPool", main_ThreadPool("main_ThreadPool")},
    {"main_AffinityOn_ThreadPool", main_ThreadPool_Affinity("main_AffinityOn_ThreadPool")},
    {"main_Batched_16_ThreadPool", main_ThreadPool_Batched_16("main_Batched_16_ThreadPool")},
    //{"main_oneTBB_Batched_16_ThreadPool", main_oneTBB_Batched_ThreadPool("main_oneTBB_Batched_16_ThreadPool", 16)},
    {"main_oneTBB_Batched_32_ThreadPool", main_oneTBB_Batched_ThreadPool("main_oneTBB_Batched_32_ThreadPool", 32)},
    //{"main_oneTBB_Batched_64_ThreadPool", main_oneTBB_Batched_ThreadPool("main_oneTBB_Batched_64_ThreadPool", 64)},
    //{"main_oneTBB_Batched_128_ThreadPool", main_oneTBB_Batched_ThreadPool("main_oneTBB_Batched_128_ThreadPool", 128)},
    {"HP_ThreadPool", HP_ThreadPool("HP_ThreadPool")},
    {"DP_ThreadPool", DP_ThreadPool("DP_ThreadPool")},
    {"BS_ThreadPool", BS_ThreadPool("BS_ThreadPool")},
    {"task_ThreadPool", task_ThreadPool("task_ThreadPool")},
    {"Taskflow_ThreadPool", Taskflow_ThreadPool("Taskflow_ThreadPool")},
    {"moody_ConcurrentQueue_ThreadPool", moody_ConcurrentQueue_ThreadPool("moody_ConcurrentQueue_ThreadPool")},
    {"MS_PPL_TaskGroup", MS_PPL_TaskGroup("MS_PPL_TaskGroup")},
    {"MS_PPL_parallel_for_TaskGroup", MS_PPL_TaskGroup_parallel_for("MS_PPL_parallel_for_TaskGroup")},
    {"oneTBB_parallel_for_TaskGroup", oneTBB_TaskGroup_parallel_for("oneTBB_parallel_for_TaskGroup")},
    {"OpenMP_parallel_for_TaskGroup", OpenMP_parallel_for("OpenMP_parallel_for_TaskGroup")},
    */

    // oneTBB variants for parallel_for tuning
    { "oneTBB_parallel_for_TaskGroup", oneTBB_TaskGroup_parallel_for("oneTBB_parallel_for_TaskGroup") },
    { "oneTBB_parallel_for_grain16",   oneTBB_parallel_for_grain16("oneTBB_parallel_for_grain16") },
    { "oneTBB_strict_arena",           oneTBB_parallel_for_strict_arena("oneTBB_strict_arena") },
    { "oneTBB_task_group_fusion",      oneTBB_task_group_fusion("oneTBB_task_group_fusion") },
    { "oneTBB_static_unrolled",        oneTBB_static_unrolled("oneTBB_static_unrolled") },


    };

#define LATENCY
#define SeqTaskSizeSweep
#define HighContention
#define SaturateOverSub

    // Define suites in a concise, declarative style.
    // Each suite contains tests, and each test has a name and a number of tasks to generate.

    std::vector<SuiteSpec> suites = {   
#ifdef SeqTaskSizeSweep
        {
            "SeqTaskSizeSweep",
            {
                { "noop", Test_noop, 100000 },
                { "short", Test_short, 100000 },
                { "medium", Test_medium, 5000 },
                { "heavy", Test_heavy, 100 },
                { "mixed_heavy_short_alternating", Test_mixed_heavy_short_alternating, 100 }
            }
        },
#endif
#ifdef HighContention
        {
            "HighContention",
            {
                { "atomic_increment", Test_atomic_increment, 100000 },
                { "mutex_locking", Test_mutex_locking, 100000 },
                { "false_sharing", Test_false_sharing, 100000 },
                { "contention_yield", Test_contention_yield, 100000 },
                { "contention_spin", Test_contention_spin, 100000 },
                { "contention_backoff", Test_contention_backoff, 100000 }
            }
        },
#endif
#ifdef SaturateOverSub
        {
            "SaturateOverSub",
            {
                { "oversub_10x", Test_oversub_10x, 0 },   // numTasks unused
                { "oversub_50x", Test_oversub_50x, 0 },
                { "oversub_100x", Test_oversub_100x, 0 }
            }
        },
#endif

    };


    Debug::debug_print("====RUN MAJOR SUITES EXCEPT LATENCY====");
    // Run all pools on all suites.
    std::vector<PoolResult> results = runAllPoolsOnSuites(suites, pools, runsPerSuite);
    // Print summary table.
    printSummaryTable(suites, results);

#ifdef LATENCY
    std::vector<LatencySuiteSpec> latencySuites = {
    {
        "LatencyFocus", {
            { "Test_latency_noop", Test_latency_noop, 50000 },
            { "Test_latency_short_compute", Test_latency_short_compute, 50000 },
            { "Test_latency_mixed_spike", Test_latency_mixed_spike, 2000 }
        }
    }
    };

    Debug::debug_print("====RUN LATENCY SUITE====");

    std::vector<PoolResult> latencyResults = runLatencySuites(latencySuites, pools, runsPerSuite);

    printLatencySummary(latencySuites, latencyResults);
#endif

    return 0;

}
