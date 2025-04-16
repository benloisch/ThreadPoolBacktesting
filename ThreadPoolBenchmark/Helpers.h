#pragma once

#include <Windows.h>
#undef min // Prevent macro interference

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

// Define a test function type.
// Each test function will generate its own tasks based on the provided count 
// and then call the provided pool .
using TestFunction = std::function<void(size_t, std::function<void(const std::vector<Task>&)>)>;

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

// ===================================
// Suite and Test Specifications
// ===================================


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
    constexpr int timeColWidth = 25;   // timing columns

    std::ostringstream oss;
    oss << "\n================== Summary Results ==================\n";

    // Header row
    oss << std::setw(nameColWidth) << std::left << " ";
    oss << std::setw(timeColWidth) << std::right << "total avg ";
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


// Latency-specific structures
using LatencyTestFunction = std::function<double(size_t, std::function<void(const std::vector<Task>&)>)>;

struct LatencyTestSpec {
    std::string testName;
    LatencyTestFunction testFunc;
    size_t numTasks;
};

struct LatencySuiteSpec {
    std::string suiteName;
    std::vector<LatencyTestSpec> tests;
};

struct LatencyResult {
    std::string poolName;
    std::vector<double> testLatenciesNs;    // per test (in same order as LatencySuiteSpec)
    std::vector<double> suiteAveragesNs;    // one average per suite
    double overallAverageNs;                // overall across all suites
};

struct LatencyStats {
    double minNs = std::numeric_limits<double>::max();
    double maxNs = 0;
    double sumNs = 0;
    double sumSqNs = 0;
    size_t count = 0;

    void record(double latencyNs) {
        minNs = std::min(minNs, latencyNs);
        maxNs = std::max(maxNs, latencyNs);
        sumNs += latencyNs;
        sumSqNs += latencyNs * latencyNs;
        ++count;
    }

    double getAvg() const {
        return (count == 0) ? 0.0 : sumNs / count;
    }

    double getStddev() const {
        if (count == 0) return 0.0;
        double avg = getAvg();
        double variance = (sumSqNs / count) - (avg * avg);
        return std::sqrt(std::max(0.0, variance));
    }

    void print(const std::string& label) const {
        return; //only print if curious about per-pool latency specific results
        if (count == 0) return;
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(3);
        oss << label << " Latency (ns): "
            << "avg: " << getAvg()
            << "  stddev: " << getStddev()
            << "  min: " << minNs
            << "  max: " << maxNs;
        Debug::debug_print(oss.str());
    }
};

inline std::vector<PoolResult> runLatencySuites(
    const std::vector<LatencySuiteSpec>& suites,
    const std::vector<PoolSpec>& pools,
    int runs)
{
    std::vector<PoolResult> results;

    for (const auto& poolSpec : pools) {
        std::ostringstream oss;
        oss << "=== Running tests on " << poolSpec.poolName << " ===";
        Debug::debug_print(oss.str());
        PoolResult pr;
        pr.poolName = poolSpec.poolName;
        double totalPoolAvgNs = 0.0;

        for (const auto& suite : suites) {
            double suiteTotalNs = 0.0;

            for (int i = 0; i < runs; ++i) {
                for (const auto& test : suite.tests) {
                    suiteTotalNs += test.testFunc(test.numTasks, poolSpec.pool);
                }
            }

            double suiteAvgNs = suiteTotalNs / (runs * suite.tests.size());
            pr.suiteAverages.push_back(suiteAvgNs / 1'000'000.0);  // Convert to ms
            totalPoolAvgNs += suiteAvgNs;
        }

        pr.overallAverage = (totalPoolAvgNs / suites.size()) / 1'000'000.0; // Convert to ms
        results.push_back(std::move(pr));
    }

    return results;
}


inline void printLatencySummary(
    const std::vector<LatencySuiteSpec>& suites,
    std::vector<PoolResult> poolResults)
{
    constexpr int nameColWidth = 40;
    constexpr int colWidth = 25;

    std::sort(poolResults.begin(), poolResults.end(),
        [](const PoolResult& a, const PoolResult& b) {
        return a.overallAverage < b.overallAverage;
    });

    std::ostringstream oss;
    oss << "\n================== Latency Summary ==================\n";
    oss << std::setw(nameColWidth) << std::left << " ";
    oss << std::setw(colWidth) << std::right << "total avg ";
    for (const auto& suite : suites)
        oss << std::setw(colWidth) << std::right << suite.suiteName;
    oss << "\n";

    for (const auto& pr : poolResults) {
        oss << std::setw(nameColWidth) << std::left << (pr.poolName + ":");

        std::ostringstream total;
        total << std::fixed << std::setprecision(3) << pr.overallAverage << " ms";
        oss << std::setw(colWidth) << std::right << total.str();

        for (double avg : pr.suiteAverages) {
            std::ostringstream val;
            val << std::fixed << std::setprecision(3) << avg << " ms";
            oss << std::setw(colWidth) << std::right << val.str();
        }

        oss << "\n";
    }

    oss << "=====================================================\n";
    Debug::debug_print(oss.str());
}

