#pragma once

#include <Windows.h>

#include <condition_variable>
#include <chrono>
#include <atomic>
#include <memory>
#include <map>
#include <queue>
#include <mutex>
#include <string>
#include <thread>
#include <fstream>
#include <functional>
#include <vector>
#include <iostream>
#include <filesystem>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <future>
#include <stdexcept>
#include <unordered_map>
#include <deque>
#include <cstdlib>


/**
 * @brief High-performance, thread-safe, optionally bounded blocking queue with shutdown.
 *
 * This queue supports:
 *  - Multiple producer threads pushing items.
 *  - Multiple consumer threads popping items.
 *  - A maximum size (bounded behavior) or unbounded (if maxSize = 0).
 *  - Graceful shutdown, allowing blocked threads to wake and exit cleanly.
 *
 * Internal Mechanisms:
 *  - Two condition variables:
 *    - `_cvFull`: Signaled when the queue has space for producers to push
 *      (or if shutdown is triggered).
 *    - `_cv`: Signaled when the queue has new items for consumers to pop
 *      (or if shutdown is triggered).
 *  - A single mutex, `_mutex`, ensures exclusive access to the underlying deque.
 *  - An atomic `_size` holds the current number of items. This allows the `size()` and
 *    `empty()` queries to be lock-free (approximate if concurrency is high).
 */
template<typename T>
class ThreadSafeQueue {
private:
    // Protects access to _queue, _shutdown, and any push/pop operations.
    mutable std::mutex _mutex;

    // Condition variable for consumer threads (waitPop, waitPopFor).
    // Notified when new items are pushed or when shutdown is triggered.
    std::condition_variable _cv;

    // Condition variable for producer threads (push).
    // Notified when an item is popped (space is freed) or when shutdown is triggered.
    std::condition_variable _cvFull;

    // Underlying FIFO container.
    std::deque<T> _queue;

    // Max queue capacity (0 = no limit).
    const size_t _maxSize;

    // Current count of items in the queue (approximate if observed outside a lock).
    std::atomic<size_t> _size{ 0 };

    // Set to true to indicate shutdown, allowing all waiting threads to exit.
    std::atomic<bool> _shutdown = false;

public:
    /**
     * @brief Constructs the queue.
     *
     * @param maxSize The maximum number of items allowed in the queue.
     *                Use 0 for an unbounded queue (i.e., no limit).
     */
    explicit ThreadSafeQueue(size_t maxSize = 0)
        : _maxSize(maxSize) {
    }

    // Delete copy/move semantics to prevent accidental misuse with shared resources.
    ThreadSafeQueue(const ThreadSafeQueue&) = delete;
    ThreadSafeQueue& operator=(const ThreadSafeQueue&) = delete;
    ThreadSafeQueue(ThreadSafeQueue&&) = delete;
    ThreadSafeQueue& operator=(ThreadSafeQueue&&) = delete;

    /**
     * @brief Attempts to push a value into the queue without blocking on queue capacity.
     *
     * This function acquires the internal mutex (blocking if necessary) to ensure thread-safe access.
     * It checks if the queue is full (if bounded) or in shutdown state and returns false immediately
     * in those cases. Otherwise, the value is pushed into the queue.
     *
     * Note: This method is not fully non-blocking. It may block briefly on the mutex,
     * but never on queue capacity. It is intended for use cases where avoiding enqueue latency
     * due to full queues is more important than avoiding lock contention.
     *
     * @return true if the value is successfully enqueued; false if the queue is full or shutting down.
     */
    template <typename F>
    bool tryPush(F&& value) {
        if (_shutdown) return false;

        std::unique_lock<std::mutex> lock(_mutex);

        if (_maxSize && _queue.size() >= _maxSize) {
            return false;
        }

        _queue.emplace_back(std::forward<F>(value));
        ++_size;
        _cv.notify_one();
        return true;
    }

    /**
     * @brief Pushes an item into the queue (by copy).
     *
     * If the queue is bounded and already at its capacity, the calling thread
     * will block until space becomes available (i.e., after a consumer pops),
     * or until the queue is shutdown. If the queue is unbounded (maxSize == 0),
     * this never blocks unless a shutdown has been triggered.
     *
     * @param value The item to be copied into the queue.
     */
    template <typename F>
    void push(F&& value) {
        std::unique_lock<std::mutex> lock(_mutex);

        _cvFull.wait(lock, [&] {
            return _shutdown || !_maxSize || _queue.size() < _maxSize;
        });

        if (_shutdown) return;

        _queue.emplace_back(std::forward<F>(value));
        ++_size;

        // lock releases here (RAII)
        _cv.notify_one();
    }

    /**
     * @brief Attempts to pop an item from the queue without blocking.
     *
     * If the queue is empty, this immediately returns false and does not modify @p result.
     * Otherwise, the front item is popped and moved into @p result, and returns true.
     *
     * @param result Reference to a variable that receives the popped item.
     * @return true if an item was successfully popped, false if the queue was empty.
     */
    bool tryPop(T& result) {
        std::scoped_lock lock(_mutex);
        if (_queue.empty()) {
            return false;
        }
        result = std::move(_queue.front());
        _queue.pop_front();
        --_size;
        _cvFull.notify_one();
        return true;
    }

    /**
     * @brief Pops an item from the queue, blocking if the queue is empty.
     *
     * The calling thread will wait until there is at least one item in the queue or
     * until the queue is shutdown. If the queue is both empty and shutdown, returns false;
     * otherwise, pops the front item into @p result and returns true.
     *
     * @param result Reference to a variable that receives the popped item.
     * @return false if the queue is shutdown and empty, true otherwise.
     */
    bool waitPop(T& result) {
        std::unique_lock<std::mutex> lock(_mutex);
        _cv.wait(lock, [&] { return _shutdown || !_queue.empty(); });
        if (_shutdown && _queue.empty()) {
            return false;
        }
        result = std::move(_queue.front());
        _queue.pop_front();
        --_size;
        _cvFull.notify_one();
        return true;
    }

    /**
     * @brief Pops an item from the queue, blocking up to a specified duration.
     *
     * The calling thread will wait until there is at least one item in the queue,
     * until the specified timeout elapses, or until the queue is shutdown.
     *
     * @param result  Reference to a variable that receives the popped item.
     * @param timeout The maximum duration to wait for an item.
     * @return false if the timeout expires (and no item was popped) or if the queue
     *         is shutdown and empty. Otherwise, returns true once an item is popped.
     */
    bool waitPopFor(T& result, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(_mutex);

        bool gotTask = _cv.wait_for(lock, timeout, [&] { return _shutdown || !_queue.empty(); });

        if (!gotTask) {
            return false;
        }
        if (_shutdown && _queue.empty()) {
            return false;
        }
        result = std::move(_queue.front());
        _queue.pop_front();
        --_size;
        _cvFull.notify_one();
        return true;
    }

    /**
     * @brief Checks if the queue is empty.
     *
     * This is effectively a lock-free read of the atomic `_size`.
     * Note that due to concurrency, this is only an approximate snapshot.
     *
     * @return true if the queue is observed to have zero items, false otherwise.
     */
    bool empty() const {
        return _size.load(std::memory_order_relaxed) == 0;
    }

    /**
     * @brief Returns the current number of items in the queue.
     *
     * This is effectively a lock-free read of the atomic `_size`.
     * Because of concurrency, the returned value might already be outdated
     * by the time the caller uses it.
     *
     * @return The approximate number of items in the queue.
     */
    size_t size() const {
        return _size.load(std::memory_order_relaxed);
    }

    /**
     * @brief Initiates a graceful shutdown of the queue.
     *
     * Once shutdown is triggered:
     *  - All waiting producers (push calls) will unblock and return immediately without pushing.
     *  - All waiting consumers (pop calls) will unblock.
     *    - If the queue is empty after unblocking, pops return false.
     *    - If not empty, consumers can still pop remaining items until empty.
     *
     * This function notifies all waiting threads to allow them to exit cleanly.
     */
    void shutdown() {
        std::scoped_lock lock(_mutex);
        _shutdown = true;
        _cv.notify_all();
        _cvFull.notify_all();
    }

    /**
     * @brief Clears the queue.
     *
     * This is useful for an immediate shutdown where pending tasks should be discarded.
     */
    void clear() {
        std::scoped_lock lock(_mutex);
        _queue.clear();
        _size.store(0, std::memory_order_relaxed);
    }

    /**
     * @brief Checks whether the queue has been shut down.
     *
     * @return true if shutdown was requested, false otherwise.
     */
    bool isShutdown() const {
        return _shutdown;
    }
};
