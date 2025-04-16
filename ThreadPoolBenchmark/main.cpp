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

// ===================================
// Global Configuration
// ===================================

// Global constant for all thread pool instances.
constexpr size_t threadCount = 24;

// Number of times to run each suite.
constexpr int runsPerSuite = 10;




// ===================================
// Debug Logging Utilities
// ===================================
namespace Debug {
    std::mutex debugMutex;

    inline std::string currentTimestampNs() {
        auto now = std::chrono::system_clock::now();
        auto t_c = std::chrono::system_clock::to_time_t(now);
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()) % 1'000'000'000;
        std::tm buf;
        localtime_s(&buf, &t_c);
        std::ostringstream oss;
        oss << std::put_time(&buf, "%Y-%m-%d %H:%M:%S")
            << "." << std::setw(9) << std::setfill('0') << ns.count();
        return oss.str();
    }

    inline void debug_print(const std::string& msg) {
        std::scoped_lock<std::mutex> lock(debugMutex);
        std::string output = "[" + currentTimestampNs() + "] " + msg + "\n";
        OutputDebugStringA(output.c_str());
    }
}


// Define a Task as a callable entity.
using Task = std::function<void()>;





// ===================================
// Pool Factory
// ===================================

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
            pool.shutdownProcessRemainingTasks();
        };
    };
};

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

// ===================================
// Test Functions
// ===================================

// Define a test function type.
// Each test function will generate its own tasks based on the provided count 
// and then call the provided pool .
using TestFunction = std::function<void(size_t, std::function<void(const std::vector<Task>&)>)>;

// Example test function: Test1.
void Test1(size_t numTasks, std::function<void(const std::vector<Task>&)> poolVector) {
    std::vector<Task> tasks;
    tasks.reserve(numTasks);
    for (size_t i = 0; i < numTasks; ++i) {
        tasks.push_back([]() {
            // Dummy work: simple accumulation loop.
            volatile int sum = 0;
            for (int j = 0; j < 1000; ++j)
                sum += j;
        });
    }
    poolVector(tasks);
}

// Example test function: Test2.
void Test2(size_t numTasks, std::function<void(const std::vector<Task>&)> pool) {
    std::vector<Task> tasks;
    tasks.reserve(numTasks);
    for (size_t i = 0; i < numTasks; ++i) {
        tasks.push_back([]() {
            // Dummy work: simple accumulation loop.
            volatile int sum = 0;
            for (int j = 0; j < 1000; ++j)
                sum += j;
        });
    }
    pool(tasks);
}








// ===================================
// Suite and Test Specifications
// ===================================

// Each TestSpec holds the test name, the test function, and the number of tasks.
struct TestSpec {
    std::string testName;
    TestFunction testFunc;
    size_t numTasks;
};

// A SuiteSpec consists of the suite name and a list of tests.
struct SuiteSpec {
    std::string suiteName;
    std::vector<TestSpec> tests;
};

// This function runs the tests in a suite one time using the specified pool .
// It prints out timing for each test.
void runSuiteOnPool(const SuiteSpec& suite, std::function<void(const std::vector<Task>&)> poolVector) {
    //std::cout << "Running Suite: " << suite.suiteName << std::endl;
    for (const auto& test : suite.tests) {
        //std::cout << "  Running " << test.testName
            //<< " (" << test.numTasks << " tasks)..." << std::endl;
        auto start = std::chrono::steady_clock::now();
        test.testFunc(test.numTasks, poolVector);
        auto end = std::chrono::steady_clock::now();
        double durationMs = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() / 1000.0;
        //std::cout << "    Completed in " << durationMs << " ms" << std::endl;
    }
}

// This function runs a suite multiple times and returns the average runtime (in milliseconds)
// for that suite.
double runSuiteMultipleTimes(const SuiteSpec& suite, std::function<void(const std::vector<Task>&)> poolVector, int runs) {
    double totalTime = 0.0;
    for (int i = 0; i < runs; i++) {
        //std::cout << "[" << suite.suiteName << "] Run " << (i + 1) << " of " << runs << std::endl;
        auto suiteStart = std::chrono::steady_clock::now();
        runSuiteOnPool(suite, poolVector);
        auto suiteEnd = std::chrono::steady_clock::now();
        double suiteRunTime = std::chrono::duration_cast<std::chrono::microseconds>(suiteEnd - suiteStart).count() / 1000.0;
        //std::cout << "  Suite Run Time: " << suiteRunTime << " ms" << std::endl;
        totalTime += suiteRunTime;
    }
    double avgTime = totalTime / runs;
    //std::cout << "\nAverage run time for " << suite.suiteName << ": " << avgTime << " ms\n";
    return avgTime;
}





// ===================================
// Pool Specification & Result Structures
// ===================================




// Encapsulates a pool runner with its name.
struct PoolSpec {
    std::string poolName;
    std::function<void(const std::vector<Task>&)> pool;
};

// To store the average results for a given pool.
struct PoolResult {
    std::string poolName;
    std::vector<double> suiteAverages; // one average per suite (in same order as suites vector)
    double overallAverage;
};





// ===================================
// Main for All Pools & Suites
// ===================================

// This function takes the defined suites and a list of pools,
// runs each suite runsPerSuite times on each pool, and returns a vector of PoolResult.
std::vector<PoolResult> runAllPoolsOnSuites(
    const std::vector<SuiteSpec>& suites,
    const std::vector<PoolSpec>& pools,
    int runs)
{
    std::vector<PoolResult> results;

    // Iterate each pool
    for (const auto& poolSpec : pools) {
        std::ostringstream oss;
        oss << "=== Running tests on " << poolSpec.poolName << " ===";
        Debug::debug_print(oss.str());
        PoolResult pr;
        pr.poolName = poolSpec.poolName;
        double totalPoolTime = 0.0;

        // Iterate through all suites for this pool.
        for (const auto& suite : suites) {
            double suiteAvg = runSuiteMultipleTimes(suite, poolSpec.pool, runs);
            pr.suiteAverages.push_back(suiteAvg);
            totalPoolTime += suiteAvg;
        }

        pr.overallAverage = totalPoolTime / suites.size();
        results.push_back(pr);
    }

    return results;
}

void printSummaryTable(const std::vector<SuiteSpec>& suites, std::vector<PoolResult> poolResults) {
    std::sort(poolResults.begin(), poolResults.end(),
        [](const PoolResult& a, const PoolResult& b) {
        return a.overallAverage < b.overallAverage;
    });

    constexpr int nameColWidth = 40;   // wider for pool names
    constexpr int timeColWidth = 15;   // timing columns

    std::ostringstream oss;
    oss << "\n================== Summary Results ==================\n";

    // Header row
    oss << std::setw(nameColWidth) << std::left << " ";
    oss << std::setw(timeColWidth) << std::right << "total avg";
    for (const auto& suite : suites) {
        oss << std::setw(timeColWidth) << std::right << suite.suiteName;
    }
    oss << "\n";

    // Result rows
    for (const auto& pr : poolResults) {
        oss << std::setw(nameColWidth) << std::left << (pr.poolName + ":");

        // Total average
        std::ostringstream total;
        total << std::fixed << std::setprecision(6) << pr.overallAverage << " ms";
        oss << std::setw(timeColWidth) << std::right << total.str();

        // Per-suite averages
        for (double avg : pr.suiteAverages) {
            std::ostringstream val;
            val << std::fixed << std::setprecision(6) << avg << " ms";
            oss << std::setw(timeColWidth) << std::right << val.str();
        }

        oss << "\n";
    }

    oss << "====================================================\n";
    Debug::debug_print(oss.str());
}







// ===================================
// Main: Define Suites, Pools, and Run Tests
// ===================================
int main() {

    // Define suites in a concise, declarative style.
    // Each suite contains tests, and each test has a name and a number of tasks to generate.
    std::vector<SuiteSpec> suites = {
        {"Suite1", { {"Test1", Test1, 1000}, {"Test2", Test2, 2000} }},
        {"Suite2", { {"Test1", Test1, 1000} }},
    };

    // Define pool runner specifications.
    // Adding a new runner is as simple as adding another entry here.
    std::vector<PoolSpec> pools = {
        {"main_ThreadPool", main_ThreadPool("main_ThreadPool")},
        {"main_ThreadPool_Affinity", main_ThreadPool_Affinity("main_ThreadPool_Affinity")},
        {"main_ThreadPool_Batched_16", main_ThreadPool_Batched_16("main_ThreadPool_Batched_16")},
        {"HP_ThreadPool", HP_ThreadPool("HP_ThreadPool")},
        {"DP_ThreadPool", DP_ThreadPool("DP_ThreadPool")},
        {"BS_ThreadPool", BS_ThreadPool("BS_ThreadPool")},
        {"task_ThreadPool", task_ThreadPool("task_ThreadPool")},
        {"moody_ConcurrentQueue_ThreadPool", moody_ConcurrentQueue_ThreadPool("moody_ConcurrentQueue_ThreadPool")},
        {"MS_PPL_TaskGroup", MS_PPL_TaskGroup("MS_PPL_TaskGroup")},
        {"MS_PPL_TaskGroup_parallel_for", MS_PPL_TaskGroup_parallel_for("MS_PPL_TaskGroup_parallel_for")},
        {"oneTBB_TaskGroup_parallel_for", oneTBB_TaskGroup_parallel_for("oneTBB_TaskGroup_parallel_for")},
        {"Taskflow_ThreadPool", Taskflow_ThreadPool("Taskflow_ThreadPool")},
        {"OpenMP_parallel_for", OpenMP_parallel_for("OpenMP_parallel_for")},

    };

    // Run all pools on all suites.
    std::vector<PoolResult> results = runAllPoolsOnSuites(suites, pools, runsPerSuite);

    // Print summary table.
    printSummaryTable(suites, results);

    return 0;
}
