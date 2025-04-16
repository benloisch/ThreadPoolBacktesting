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
    TASK_GROUP
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


public:
    oneTBB_Wrapper(const std::string& name, std::size_t threadCount, std::size_t maxQueueSize = 0)
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
            case Strategy::TASK_GROUP:
                run_task_group(tasks, taskCount);
                break;
            default:
                queueSize_.fetch_sub(taskCount, std::memory_order_relaxed);
                return false;
        }

        return true;
    }

    void shutdownProcessRemainingTasks() {
        // Wait until the queue is fully drained
        while (queueSize_.load(std::memory_order_relaxed) > 0) {
            std::this_thread::yield();  // Allow other threads to continue work
        }

        // Optional: forcibly terminate the arena (if you don’t reuse it)
        // arena_.terminate();  // not usually necessary unless doing manual arena mgmt
    }

};
