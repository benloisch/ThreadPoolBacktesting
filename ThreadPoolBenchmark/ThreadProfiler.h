#pragma once

#include <atomic>
#include <chrono>
#include <deque>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <iostream>

#include <Windows.h>
//#define PRINT_LOW_LEVEL_TO_VISUAL_STUDIO
//#define LOG_LOW_LEVEL_TO_FILE

// Define ENABLE_THREAD_PROFILER for debug builds
//#define ENABLE_THREAD_PROFILER

/**
 * @brief ThreadProfiler captures thread pool metrics such as thread count and queue size,
 *        and logs them periodically to a CSV file for post-run analysis.
 *
 * It supports:
 *  - Periodic sampling of live metrics into a rolling buffer.
 *  - Optional debug logging to file or Visual Studio output.
 *  - Low-overhead tracking enabled only in debug builds.
 */
class ThreadProfiler {
private:

    std::string snapshotsFileName;                  ///< Output filename for snapshots CSV
    std::string metricsFileName;                  ///< Output filename for metrics CSV
    std::ofstream snapshotsLogFileStream;         ///< CSV stream for writing periodic snapshots
    std::ofstream metricsLogFileStream;         ///< CSV stream for writing metrics
    std::ofstream debugLogFile;           ///< Optional debug log file stream

    std::atomic<bool> running;            ///< Flag used to stop logger thread cleanly
    std::thread loggerThread;             ///< Background thread writing snapshots to disk

    mutable std::mutex debugMutex;        ///< Protects concurrent writes to debug log file



    // --- ************** Metrics / counters **************** ---



    /**
     * @brief Counts how many tasks were submitted to the thread pool.
     *
     * This is incremented whenever a task is attempted to be enqueued, regardless of
     * whether the attempt succeeds or fails. It provides a measure of total throughput demand.
     */
    std::atomic<size_t> totalTasksSubmitted{ 0 };
    /**
     * @brief Counts how many tasks were actually executed by the thread pool.
     *
     * Incremented after a task finishes execution inside a worker thread.
     */
    std::atomic<size_t> tasksExecuted{ 0 };






    // --- ************** Snapshot properties **************** ---



    /**
     * @brief Struct representing a single point-in-time snapshot of the thread pool.
     */
    struct Snapshot {
        std::chrono::steady_clock::time_point time; ///< When the snapshot was taken
        size_t threadCount;                         ///< Number of active threads at snapshot
        size_t queueSize;                           ///< Number of items in the queue at snapshot
        double avgThreadCount = 0.0;                ///< Rolling average of thread count at time of snapshot
        double avgQueueSize = 0.0;                  ///< Rolling average of queue size at time of snapshot
    };

    std::deque<Snapshot> snapshots;       ///< Ring buffer of recent snapshots for rolling stats
    mutable std::mutex snapshotMutex;     ///< Guards access to the snapshot buffer

    //however many snapshots (once every snapshot interval) per rolling window timeframe
    std::chrono::milliseconds snapshotIntervalMs{ 1000 };   ///< Sampling/log frequency
    std::chrono::milliseconds rollingWindowMs{ 1000 };     ///< Rolling window size for averaging
    std::chrono::steady_clock::time_point lastSnapshotLog = std::chrono::steady_clock::now(); ///< Last logged time





public:
    /**
     * @brief Constructor to initialize profiler with output file and debug logging.
     * @param snapshotsFile Name of CSV file to write snapshots to.
     * @param openDebugLog If true, also opens a side debug log file.
     */
    explicit ThreadProfiler(const std::string& snapshotsFile, const std::string& metricsFile)
    {
#ifdef ENABLE_THREAD_PROFILER

        //metrics setup
        metricsFileName = metricsFile;
        metricsLogFileStream.open(metricsFileName);
        metricsLogFileStream << "Metric,Value\n";



        //snapshots setup
        snapshotsFileName = snapshotsFile;
        running = true;
        snapshotsLogFileStream.open(snapshotsFileName);
        snapshotsLogFileStream << "Time,Threads,QueueSize,AvgThreads,AvgQueueSize\n";

#ifdef LOG_LOW_LEVEL_TO_FILE
        std::string debugFile = "debug_" + fileName + ".txt";
        debugLogFile.open(debugFile, std::ios::out | std::ios::trunc);
#endif
        loggerThread = std::thread(&ThreadProfiler::loggerThreadFunc, this);
#endif
    }

    /**
     * @brief Destructor ensures background thread is stopped and all files are flushed/closed.
     */
    ~ThreadProfiler()
    {
#ifdef ENABLE_THREAD_PROFILER
        running = false;
        if (loggerThread.joinable())
            loggerThread.join();

        if (snapshotsLogFileStream.is_open())
            snapshotsLogFileStream.close();

        if (metricsLogFileStream.is_open())
            metricsLogFileStream.close();

        if (debugLogFile.is_open())
            debugLogFile.close();
#endif
    }






    // --- ************** Utility methods **************** ---

    /**
     * @brief Generates a timestamp string in a consistent log-friendly format.
     */
    std::string currentTimestamp() const {
#ifdef ENABLE_THREAD_PROFILER
        auto now = std::chrono::system_clock::now();
        auto t_c = std::chrono::system_clock::to_time_t(now);
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()) % 1'000'000'000;

        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &t_c);
#else
        localtime_r(&t_c, &tm);
#endif
        std::ostringstream oss;
        oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S")
            << "." << std::setw(9) << std::setfill('0') << ns.count();

        return oss.str();
#endif
    }








    // --- ************** Core logging methods **************** ---

    /**
     * @brief Logs a message to the threadpool-named debug log (thread-safe).
     */
    void logMessage(const std::string& msg) {
#ifdef ENABLE_THREAD_PROFILER
        std::string output = "[" + currentTimestamp() + "] " + msg + "\n";
        {
            std::scoped_lock<std::mutex> lock(debugMutex);
            if (debugLogFile.is_open()) {
                debugLogFile << output;
            }
        }
#ifdef PRINT_LOW_LEVEL_TO_VISUAL_STUDIO
        OutputDebugStringA(output.c_str());
#endif
#endif
    }

    /**
     * @brief Background thread function that periodically flushes the latest snapshot to disk.
     */
    void loggerThreadFunc() {
#ifdef ENABLE_THREAD_PROFILER
        while (running) {
            std::this_thread::sleep_for(snapshotIntervalMs);

            writeSnapshot();
            writeMetrics();
        }
#endif
    }





    // --- ************** Metrics / counters methods **************** ---

    /**
     * @brief Overwrites the metrics CSV file with the most recent counter values.
     *
     * This ensures only the latest values are present, and is called periodically from loggerThread.
     */
    void writeMetrics() {
#ifdef ENABLE_THREAD_PROFILER
        // Overwrite the file with fresh contents
        std::ofstream ofs(metricsFileName, std::ios::out | std::ios::trunc);
        if (!ofs.is_open())
            return;

        ofs << "Metric,Value\n";
        ///todo make sure to also print out snapshotIntervalMs ect so that when we display results we can also show the interval etc.
        ofs << "totalTasksSubmitted," << totalTasksSubmitted.load(std::memory_order_relaxed) << std::endl;
        ofs << "tasksExecuted," << tasksExecuted.load(std::memory_order_relaxed) << std::endl;

        ofs.flush();  // Ensure written to disk
#endif
    }
    /**
     * @brief Increments the totalTasksSubmitted counter.
     *
     * Called by ThreadPool each time a task is submitted via enqueue or tryEnqueue.
     */
    void incrementTasksSubmitted() {
#ifdef ENABLE_THREAD_PROFILER
        totalTasksSubmitted.fetch_add(1, std::memory_order_relaxed);
#endif
    }
    /**
     * @brief Increments the tasksExecuted counter.
     *
     * Called by each worker thread after it completes executing a task.
     */
    void incrementTasksExecuted() {
#ifdef ENABLE_THREAD_PROFILER
        tasksExecuted.fetch_add(1, std::memory_order_relaxed);
#endif
    }







    // --- ************** Snapshot methods **************** ---

    /**
     * @brief Takes a snapshot with the given thread/queue state and adds it to the rolling buffer.
     *        If older snapshots fall outside the configured rolling window, they are discarded.
     */
    void takeSnapshot(size_t threadCount, size_t queueSize) {
#ifdef ENABLE_THREAD_PROFILER
        auto now = std::chrono::steady_clock::now();

        double avgThreads = 0.0;
        double avgQueue = 0.0;

        {
            std::scoped_lock<std::mutex> lock(snapshotMutex);

            // 🔁 Throttle snapshot insertions to avoid over-sampling
            if (now - lastSnapshotLog < snapshotIntervalMs)
                return;

            // Add new snapshot (preliminary) to list
            snapshots.push_back({ now, threadCount, queueSize });

            // Remove old snapshots
            while (!snapshots.empty() && now - snapshots.front().time > rollingWindowMs) {
                snapshots.pop_front();
            }

            // Compute average across current buffer
            size_t totalThreads = 0, totalQueue = 0;
            for (const auto& snap : snapshots) {
                totalThreads += snap.threadCount;
                totalQueue += snap.queueSize;
            }
            const size_t count = snapshots.size();
            if (count > 0) {
                avgThreads = static_cast<double>(totalThreads) / count;
                avgQueue = static_cast<double>(totalQueue) / count;
            }

            // Backfill the last snapshot with computed averages
            snapshots.back().avgThreadCount = avgThreads;
            snapshots.back().avgQueueSize = avgQueue;
        }
#endif
    }


    /**
     * @brief Writes the most recent thread/queue snapshot to the snapshots CSV file.
     *
     * Called periodically by the background logger thread.
     */
    void writeSnapshot() {
#ifdef ENABLE_THREAD_PROFILER
        Snapshot snap;
        {
            std::scoped_lock<std::mutex> lock(snapshotMutex);
            if (!snapshots.empty())
                snap = snapshots.back();
            else
                snap = { std::chrono::steady_clock::now(), 0, 0 };
        }

        snapshotsLogFileStream << "\"TS: " << currentTimestamp() << "\","
            << snap.threadCount << ","
            << snap.queueSize << ","
            << snap.avgThreadCount << ","
            << snap.avgQueueSize << "\n";

        snapshotsLogFileStream.flush();
#endif
    }

    /**
     * @brief Computes the rolling average thread count and queue size
     *        over all snapshots currently in the rolling window.
     *
     * Note: If fewer than expected snapshots exist (e.g. early runtime),
     *       the average is computed over available samples.
     *
     * @return A pair: { averageThreadCount, averageQueueSize }
     */
    std::pair<double, double> computeRollingAverages() const {
#ifdef ENABLE_THREAD_PROFILER
        std::scoped_lock<std::mutex> lock(snapshotMutex);
        if (snapshots.empty()) return { 0.0, 0.0 };

        size_t totalThreads = 0, totalQueue = 0;
        for (const auto& snap : snapshots) {
            totalThreads += snap.threadCount;
            totalQueue += snap.queueSize;
        }

        const size_t count = snapshots.size();
        double avgThreads = static_cast<double>(totalThreads) / count;
        double avgQueue = static_cast<double>(totalQueue) / count;
        return { avgThreads, avgQueue };
#else
        return { 0.0, 0.0 };
#endif
    }

};
