#pragma once

#include <tbb/task_arena.h>
#include <tbb/parallel_for.h>
#include <tbb/blocked_range.h>
#include <tbb/partitioner.h>

#include <vector>
#include <functional>
#include <atomic>
#include <string>
#include <cstddef>


enum class Strategy {
    BULK_PARALLEL_AUTO,
    BULK_PARALLEL_AFFINITY,
    BULK_PARALLEL_STATIC,
    TASK_GROUP,
    TASK_GROUP_BATCHED,
    TASK_GROUP_BATCHED_STREAMED,
    TASK_GROUP_BATCHED_STREAMED_AFFINITY
};

class oneTBB_Wrapper {

private:

    std::string name_;
    tbb::task_arena arena_;
    std::atomic<std::size_t> queueSize_;
    std::size_t maxQueueSize_;

    template <typename Partitioner>
    void bulk_parallel(const std::vector<std::function<void()>>& tasks,
        std::size_t taskCount,
        Partitioner& partitioner)
    {
        arena_.execute([this, &tasks, taskCount, &partitioner]() {
            tbb::parallel_for(
                tbb::blocked_range<std::size_t>(0, taskCount),
                [&](const tbb::blocked_range<std::size_t>& range) {
                for (std::size_t i = range.begin(); i < range.end(); ++i)
                    tasks[i]();
            },
                partitioner
            );
            queueSize_.fetch_sub(taskCount, std::memory_order_relaxed);
        });
    }

    void run_task_group(const std::vector<std::function<void()>>& tasks, std::size_t taskCount) {
        arena_.execute([this, &tasks, taskCount]() {
            tbb::task_group tg;
            for (std::size_t i = 0; i < taskCount; ++i) {
                tg.run([&tasks, i]() {
                    tasks[i]();
                });
            }
            tg.wait();
            queueSize_.fetch_sub(taskCount, std::memory_order_relaxed);
        });
    }

    void run_task_group_batched(const std::vector<std::function<void()>>& tasks, std::size_t taskCount) {
        arena_.execute([this, &tasks, taskCount]() {
            tbb::task_group tg;

            size_t batchSize = std::max<size_t>(64, taskCount / (arena_.max_concurrency() * 4));
            for (size_t i = 0; i < taskCount; i += batchSize) {
                tg.run([&, start = i] {
                    size_t end = std::min(start + batchSize, taskCount);
                    for (size_t j = start; j < end; ++j)
                        tasks[j]();
                });
            }

            tg.wait();
            queueSize_.fetch_sub(taskCount, std::memory_order_relaxed);
        });
    }

    void run_task_group_batched_streamed_affinity(const std::vector<std::function<void()>>& tasks,
        std::size_t taskCount,
        std::size_t flushBatchSize = 64)
    {
        static thread_local tbb::affinity_partitioner ap;  // must persist across calls

        // Large batch: submit immediately
        if (taskCount > flushBatchSize) {
            queueSize_.fetch_add(taskCount, std::memory_order_relaxed);

            arena_.execute([this, tasks = std::move(tasks), taskCount]() {
                tbb::parallel_for(
                    tbb::blocked_range<std::size_t>(0, taskCount),
                    [&](const tbb::blocked_range<std::size_t>& range) {
                    for (std::size_t i = range.begin(); i < range.end(); ++i)
                        tasks[i]();
                },
                    ap
                );
                queueSize_.fetch_sub(taskCount, std::memory_order_relaxed);
            });

            return;
        }

        // Small batch: accumulate and flush when enough
        static thread_local std::vector<std::function<void()>> buffer;

        buffer.insert(buffer.end(), tasks.begin(), tasks.end());

        if (buffer.size() >= flushBatchSize) {
            std::vector<std::function<void()>> batch;
            batch.swap(buffer);

            std::size_t batchCount = batch.size();
            queueSize_.fetch_add(batchCount, std::memory_order_relaxed);

            arena_.execute([this, batch = std::move(batch), batchCount]() {
                tbb::parallel_for(
                    tbb::blocked_range<std::size_t>(0, batchCount),
                    [&](const tbb::blocked_range<std::size_t>& range) {
                    for (std::size_t i = range.begin(); i < range.end(); ++i)
                        batch[i]();
                },
                    tbb::auto_partitioner{}
                );
                queueSize_.fetch_sub(batchCount, std::memory_order_relaxed);
            });
        }
    }

    void run_task_group_batched_streamed(const std::vector<std::function<void()>>& tasks,
        std::size_t taskCount,
        std::size_t flushBatchSize = 64)
    {
        // Large batch: submit immediately
        if (taskCount > flushBatchSize) {
            queueSize_.fetch_add(taskCount, std::memory_order_relaxed);

            arena_.execute([this, tasks = std::move(tasks), taskCount]() {
                tbb::parallel_for(
                    tbb::blocked_range<std::size_t>(0, taskCount),
                    [&](const tbb::blocked_range<std::size_t>& range) {
                    for (std::size_t i = range.begin(); i < range.end(); ++i)
                        tasks[i]();
                },
                    tbb::auto_partitioner{}
                );
                queueSize_.fetch_sub(taskCount, std::memory_order_relaxed);
            });

            return;
        }

        // Small batch: accumulate and flush when enough
        static thread_local std::vector<std::function<void()>> buffer;

        buffer.insert(buffer.end(), tasks.begin(), tasks.end());

        if (buffer.size() >= flushBatchSize) {
            std::vector<std::function<void()>> batch;
            batch.swap(buffer);

            std::size_t batchCount = batch.size();
            queueSize_.fetch_add(batchCount, std::memory_order_relaxed);

            arena_.execute([this, batch = std::move(batch), batchCount]() {
                tbb::parallel_for(
                    tbb::blocked_range<std::size_t>(0, batchCount),
                    [&](const tbb::blocked_range<std::size_t>& range) {
                    for (std::size_t i = range.begin(); i < range.end(); ++i)
                        batch[i]();
                },
                    tbb::auto_partitioner{}
                );
                queueSize_.fetch_sub(batchCount, std::memory_order_relaxed);
            });
        }
    }



    void flush_streamed_tasks() {
        static thread_local std::vector<std::function<void()>> buffer;

        if (buffer.empty()) return;

        std::vector<std::function<void()>> batch;
        batch.swap(buffer);

        std::size_t taskCount = batch.size();
        queueSize_.fetch_add(taskCount, std::memory_order_relaxed);

        arena_.execute([this, batch = std::move(batch), taskCount]() {
            tbb::parallel_for(
                tbb::blocked_range<std::size_t>(0, taskCount),
                [&](const tbb::blocked_range<std::size_t>& range) {
                for (std::size_t i = range.begin(); i < range.end(); ++i)
                    batch[i]();
            }
            );
            queueSize_.fetch_sub(taskCount, std::memory_order_relaxed);
        });
    }



public:
    oneTBB_Wrapper(const std::string& name, int threadCount, std::size_t maxQueueSize = 0)
        : name_(name)
        , arena_(threadCount)
        , queueSize_(0)
        , maxQueueSize_(maxQueueSize)
    {
    }

    std::size_t getQueueSize() const {
        return queueSize_.load(std::memory_order_relaxed);
    }

    bool tryEnqueue(const std::vector<std::function<void()>>& tasks, Strategy strategy) {
        std::size_t taskCount = tasks.size();
        if (taskCount == 0)
            return true;

        std::size_t current = queueSize_.load(std::memory_order_relaxed);
        if (maxQueueSize_ && current + taskCount > maxQueueSize_)
            return false;

        queueSize_.fetch_add(taskCount, std::memory_order_relaxed);

        switch (strategy) {
        case Strategy::BULK_PARALLEL_AUTO:
            {
                static tbb::auto_partitioner ap;
                bulk_parallel(tasks, taskCount, ap);
                break;
            }
            case Strategy::BULK_PARALLEL_AFFINITY: 
            {
                static tbb::affinity_partitioner ap;
                bulk_parallel(tasks, taskCount, ap);
                break;
            }
            case Strategy::BULK_PARALLEL_STATIC: 
            {
                static tbb::static_partitioner ap;
                bulk_parallel(tasks, taskCount, ap);
                break;
            }
            case Strategy::TASK_GROUP_BATCHED:
                run_task_group_batched(tasks, taskCount);
                break;
            case Strategy::TASK_GROUP:
                run_task_group(tasks, taskCount);
                break;
            case Strategy::TASK_GROUP_BATCHED_STREAMED:
                run_task_group_batched_streamed(tasks, taskCount);
                return true;
            case Strategy::TASK_GROUP_BATCHED_STREAMED_AFFINITY:
                run_task_group_batched_streamed_affinity(tasks, taskCount);
                return true;
            default:
                queueSize_.fetch_sub(taskCount, std::memory_order_relaxed);
                return false;
        }

        return true;
    }

    void shutdownProcessRemainingTasks() {

        // Flush any partial batches in this thread
        flush_streamed_tasks();


        // Wait until the queue is fully drained
        while (queueSize_.load(std::memory_order_relaxed) > 0) {
            std::this_thread::yield();  // Allow other threads to continue work
        }

        // Optional: forcibly terminate the arena (if you don’t reuse it)
        // arena_.terminate();  // not usually necessary unless doing manual arena mgmt
    }


};
