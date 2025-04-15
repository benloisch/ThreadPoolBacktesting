#pragma once
#include "ThreadSafeQueue.h"

/**
 * @brief A thread pool implementation that supports both static and dynamic modes.
 *
 * This thread pool supports:
 *  - Enqueuing tasks through overloaded functions (accepting both const-reference and move semantics).
 *  - Two shutdown modes:
 *      - shutdownImmediately(): Immediately cancels pending tasks by clearing the queue.
 *      - shutdownProcessRemainingTasks(): Processes all queued tasks before shutdown.
 *  - Exception-safe task execution where each task call is wrapped in a try-catch block so that if a task throws,
 *    the worker thread catches the exception and continues processing subsequent tasks. Therefore, there is no need
 *    to respawn a worker thread if an exception occurs.
 *
 * When configured in dynamic mode (via the (minNumThreads, maxNumThreads) constructor), the pool adjusts the number
 * of worker threads dynamically based on workload:
 *  - The pool is initially started with a minimum number of threads.
 *  - When tasks are enqueued and the number of pending tasks exceeds the current thread count, new threads are spawned,
 *    up to the maximum specified.
 *  - Each dynamic worker thread uses waitPopFor() with a timeout. If a worker times out while waiting for a task and the
 *    current thread count exceeds the minimum, the worker exits. This allows the pool to shrink during periods of low load.
 *
 * The implementation leverages a high-performance, thread-safe queue (ThreadSafeQueue) which provides blocking and
 * timeout-based operations to allow for effective management of threads in a dynamic environment.
 */
class ThreadPool {
public:

    // Thread-safe queue for storing tasks.
    ThreadSafeQueue<std::function<void()>> taskQueue;
    // Container for worker threads.
    std::vector<std::thread> workers;
    // Atomic flag to indicate if the pool is still accepting tasks.
    std::atomic<bool> active;

    // used to identify thread pool
    std::string threadPoolName;

    const uint32_t numLogicalCores{ 24 };

    /**
     * @brief Timeout duration for dynamic worker threads.
     *
     * If a dynamic thread does not receive a task within this period,
     * it may self-terminate (if the current thread count exceeds the minimum).
     * This helps the pool scale down during periods of low activity.
     */
    std::chrono::milliseconds dynamicWorkerTimeout{ 500 };

    /**
     * @brief Enables round-robin CPU core affinity for worker threads.
     *
     * When set to true, each thread will be pinned to a specific logical core
     * at creation time, reducing context switching and improving cache locality.
     * Disabled by default.
     */
    bool enableAffinity = false;

    /**
    * @brief Core ID counter for assigning thread affinity in round-robin fashion.
    *
    * Atomically incremented as threads are created to evenly distribute
    * them across available logical CPU cores. Used only if enableAffinity is true.
    */
    std::atomic<size_t> nextCoreId{ 0 };  // Used for round-robin core assignment

    std::atomic<size_t> currentThreads{ 0 };

    // Mutex protecting the workers container during dynamic thread adjustments.
    std::mutex workerMutex;

    // Mutex protecting the thread pool name
    std::mutex threadPoolNameMutex;

    /**
     * @brief Dynamic mode constructor.
     *
     * Initializes the pool with dynamic thread count management. In dynamic mode, the pool starts with
     * minNumThreads workers, and can grow up to maxNumThreads as workload increases. Idle workers may exit
     * (using a timeout mechanism) so that the pool never goes below minNumThreads.
     *
     * @param minNumThreads The minimum number of worker threads to create.
     * @param maxNumThreads The maximum number of worker threads allowed in the pool.
     * @param maxQueueSize The maximum number of tasks allowed in the queue (0 means unbounded).
     */
    explicit ThreadPool(std::string name, size_t numThreads, size_t maxQueueSize, bool enableAfinity)
        : threadPoolName(name), active(true), taskQueue(maxQueueSize),
        currentThreads(numThreads), enableAffinity(enableAffinity)
    {
        // Spawn the initial number of worker threads.
        for (size_t i = 0; i < currentThreads; ++i) {
            workers.emplace_back(&ThreadPool::workerThread, this);
        }
    }

    /**
     * @brief Destructor.
     *
     * Ensures that the thread pool is shutdown immediately, canceling any pending tasks and joining all
     * worker threads.
     */
    ~ThreadPool() {
        shutdownImmediately();
    }

    /**
     * @brief Submits a task to the thread pool with optional blocking on queue capacity.
     *
     * This is the unified interface for submitting tasks to the thread pool. It acquires the internal
     * queue lock (blocking briefly if necessary), and either enqueues the task or fails depending on
     * the queue capacity and `blockIfQueueFull` policy.
     *
     * If the queue is bounded and currently full:
     *  - If blockIfQueueFull is true, this call blocks until space becomes available.
     *  - If blockIfQueueFull is false, the task is rejected immediately and false is returned.
     *
     * This supports both:
     *  - Latency-sensitive producers that prefer to drop tasks over blocking.
     *  - Guaranteed-throughput systems that must ensure task acceptance.
     *
     * Note: This method still blocks on the internal mutex. It is not fully lock-free.
     * In dynamic mode, a successful enqueue may trigger a new worker spawn if needed.
     *
     * @param task The task to be submitted (const lvalue overload).
     * @param blockIfQueueFull Whether to block on a full queue or reject the task immediately.
     * @return true if the task was accepted into the queue; false if the queue is full or pool inactive.
     */
    template <typename F>
    bool tryEnqueueTask(F&& task, bool blockIfQueueFull) {
        if (!active.load()) {
            return false;
        }

        if (blockIfQueueFull) {
            taskQueue.push(std::forward<F>(task));
        }
        else {
            if (!taskQueue.tryPush(std::forward<F>(task))) {
                return false;
            }
        }

        return true;
    }

    /**
     * @brief Immediately shuts down the thread pool.
     *
     * Pending tasks are cleared and will not be processed. All worker threads are signaled to exit and
     * are joined. This method is called during destruction.
     */
    void shutdownImmediately() {
        if (!active.exchange(false)) {
            return;
        }
        taskQueue.clear();
        taskQueue.shutdown();
        // Join all worker threads.
        for (std::thread& worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    /**
     * @brief Shuts down the thread pool after processing all remaining tasks.
     *
     * No new tasks are accepted. The thread pool continues to process tasks that are already enqueued.
     * Once the task queue is empty, the pool is shut down and all worker threads are joined.
     */
    void shutdownProcessRemainingTasks() {
        if (!active.exchange(false)) {
            return;
        }

        // Wait until the task queue is empty.
        // Busy-waiting until the task queue is empty before shutdown.
        // This is acceptable here because shutdown is only called once,
        // during program exit, and the wait duration is typically very short.
        // If shutdown were invoked frequently during runtime, consider replacing
        // this loop with a yield/sleep hybrid or a condition-variable-based wait
        // to reduce CPU usage under prolonged draining conditions.
        while (!taskQueue.empty()) {
            std::this_thread::yield();
        }

        taskQueue.shutdown();
        for (std::thread& worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }

        {
            std::lock_guard<std::mutex> lock(tasksCompletedMutex);
            for (const auto& kv : tasksCompleted) {
                //std::cout << "Thread " << kv.first << " completed " << kv.second << " tasks." << std::endl;
            }
        }
    }

private:
    std::mutex tasksCompletedMutex;
    std::unordered_map<std::thread::id, size_t> tasksCompleted;

    /**
     * @brief Worker thread function.
     *
     * In static mode, the worker thread continuously calls waitPop() on the task queue and executes
     * tasks as they become available. In dynamic mode, the worker thread uses waitPopFor() with a timeout.
     * If waitPopFor() times out and the current number of threads exceeds the minimum threshold, the worker
     * thread exits. Otherwise, it continues waiting for tasks.
     */
    void workerThread() {
        if (enableAffinity) {
            size_t coreId = nextCoreId++ % numLogicalCores;
            pinCurrentThreadToCore(coreId);
        }

        std::function<void()> task;

        // Static mode: block indefinitely until a task is available or shutdown is signaled.
        while (taskQueue.waitPop(task)) {
            try {
                task();
            }
            catch (const std::exception& e) {
                // e.what());
            }
            catch (...) {
            }

            {
                std::lock_guard<std::mutex> lock(tasksCompletedMutex);
                tasksCompleted[std::this_thread::get_id()]++;
            }
        }
    }

    void inline pinCurrentThreadToCore(size_t coreId) {
#ifdef _WIN32
        DWORD_PTR mask = 1ULL << coreId;
        SetThreadAffinityMask(GetCurrentThread(), mask);
#else
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(coreId, &cpuset);
        pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
#endif
    }
};
