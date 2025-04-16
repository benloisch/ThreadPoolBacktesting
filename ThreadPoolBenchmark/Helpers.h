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

// ===================================
//  Monospace‑safe time formatter
//  (ASCII only, constant width)
// ===================================
inline std::string fmtTime(double ms)
{
    std::ostringstream os;
    os << std::fixed;
    if (ms >= 1.0) {
        os << std::setprecision(1) << ms << " ms";      // e.g. 17.5 ms
    }
    else {
        int us = static_cast<int>(std::round(ms * 1000.0));
        os << us << " us";                              // e.g. 412 us
    }
    return os.str();                                    // ASCII only
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

    // perTestAvgMs[suiteIndex][testIndex]  →  average ms for that test (across runs)
    std::vector<std::vector<double>> perTestAvgMs;

    // suiteAverages[suiteIndex]            →  average of that suite’s tests (ms)
    std::vector<double> suiteAverages;

    // overallAverage                       →  average of all suiteAverages (ms)
    double overallAverage = 0.0;
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
// Main for All Pools & Suites
// ===================================

// This function takes the defined suites and a list of pools,
// runs each suite runsPerSuite times on each pool, and returns a vector of PoolResult.
inline std::vector<PoolResult> runAllPoolsOnSuites(
    const std::vector<SuiteSpec>& suites,
    const std::vector<PoolSpec>& pools,
    int runsPerSuite)
{
    std::vector<PoolResult> results;

    for (const auto& poolSpec : pools) {
        std::ostringstream hdr;
        hdr << "=== Running suites on " << poolSpec.poolName << " ===";
        Debug::debug_print(hdr.str());

        PoolResult pr;
        pr.poolName = poolSpec.poolName;
        pr.perTestAvgMs.resize(suites.size());
        pr.suiteAverages.resize(suites.size());

        double totalAvgMs = 0.0;

        // ---------- every suite ----------
        for (size_t s = 0; s < suites.size(); ++s) {
            const auto& suite = suites[s];
            const size_t nTests = suite.tests.size();
            pr.perTestAvgMs[s].assign(nTests, 0.0);

            // ----- run the whole suite 'runsPerSuite' times -----
            for (int run = 0; run < runsPerSuite; ++run) {
                for (size_t t = 0; t < nTests; ++t) {
                    const auto& test = suite.tests[t];

                    auto start = std::chrono::steady_clock::now();
                    test.testFunc(test.numTasks, poolSpec.pool);
                    auto stop = std::chrono::steady_clock::now();

                    const double durMs =
                        std::chrono::duration_cast<std::chrono::microseconds>(stop - start).count()
                        / 1000.0;

                    pr.perTestAvgMs[s][t] += durMs;
                    /*
                    std::ostringstream msg;
                    msg << "  " << std::setw(18) << std::left << suite.suiteName << "::"
                        << std::setw(25) << std::left << test.testName
                        << "run " << (run + 1) << "/" << runsPerSuite << " : "
                        << std::fixed << std::setprecision(3) << durMs << " ms";
                    Debug::debug_print(msg.str());*/
                }
            }

            // ---- convert accumulated totals into averages ----
            double suiteSum = 0.0;
            for (size_t t = 0; t < nTests; ++t) {
                pr.perTestAvgMs[s][t] /= runsPerSuite;      // average for that test
                suiteSum += pr.perTestAvgMs[s][t];
            }
            pr.suiteAverages[s] = suiteSum / nTests;        // average for that suite
            totalAvgMs += pr.suiteAverages[s];
        }

        pr.overallAverage = totalAvgMs / suites.size();      // overall (raw) ms
        results.emplace_back(std::move(pr));
    }

    return results;
}


// ===================================
//  Summary – BEST‑PER‑TEST then average
//  (dynamically sized columns, ASCII‑only)
// ===================================
inline void printSummaryTableTaskNormalized(
    const std::vector<SuiteSpec>& suites,
    std::vector<PoolResult>       poolResults)
{
    using std::size_t;
    const size_t P = poolResults.size();
    const size_t S = suites.size();

    // ---------- 1. best time per individual test ----------
    std::vector<std::vector<double>> bestPerTest(S);
    for (size_t s = 0; s < S; ++s)
        bestPerTest[s].assign(suites[s].tests.size(),
            std::numeric_limits<double>::max());

    for (const auto& pr : poolResults)
        for (size_t s = 0; s < S; ++s)
            for (size_t t = 0; t < bestPerTest[s].size(); ++t)
                bestPerTest[s][t] =
                std::min(bestPerTest[s][t], pr.perTestAvgMs[s][t]);

    // ---------- 2. per‑suite & overall scores ----------
    std::vector<std::vector<double>> suiteScore(P, std::vector<double>(S));
    std::vector<double>              overallScore(P);

    for (size_t p = 0; p < P; ++p) {
        double sumSuites = 0.0;
        for (size_t s = 0; s < S; ++s) {
            double sumTests = 0.0;
            for (size_t t = 0; t < bestPerTest[s].size(); ++t)
                sumTests += bestPerTest[s][t] /
                poolResults[p].perTestAvgMs[s][t];

            suiteScore[p][s] = sumTests / bestPerTest[s].size();
            sumSuites += suiteScore[p][s];
        }
        overallScore[p] = sumSuites / S;
    }

    // ---------- 3. sort pools by overall score ----------
    std::vector<size_t> order(P);
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(),
        [&](size_t a, size_t b) { return overallScore[a] > overallScore[b]; });

    // ---------- 4. dynamic column widths ----------
    size_t nameW = 0;
    for (const auto& pr : poolResults) nameW = std::max(nameW, pr.poolName.size());
    nameW += 2;                                           // ": "

    auto fmtCell = [](double score, double ms) {
        std::ostringstream tmp;
        tmp << std::fixed << std::setprecision(2)
            << score << '(' << fmtTime(ms) << ')';
        return tmp.str();
    };

    std::vector<int> colW(S + 1, 0);                      // 0 = overall
    colW[0] = (int)std::string("overall").size();
    for (size_t s = 0; s < S; ++s)
        colW[s + 1] = (int)suites[s].suiteName.size();

    for (size_t p = 0; p < P; ++p) {
        colW[0] = std::max(colW[0],
            (int)fmtCell(overallScore[p], poolResults[p].overallAverage).size());
        for (size_t s = 0; s < S; ++s)
            colW[s + 1] = std::max(colW[s + 1],
                (int)fmtCell(suiteScore[p][s], poolResults[p].suiteAverages[s]).size());
    }
    for (int& w : colW) w += 1;                           // 1‑space padding

    // ---------- 5. header row (LEFT‑aligned) ----------
    std::ostringstream oss;
    oss << "\n=============== Task Normalised ===============\n";
    oss << std::setw((int)nameW) << "";                   // blank first column
    oss << std::setw(colW[0]) << std::left << "overall";
    for (size_t s = 0; s < S; ++s)
        oss << std::setw(colW[s + 1]) << std::left << suites[s].suiteName;
    oss << '\n';

    // ---------- 6. data rows ----------
    for (size_t idx : order) {
        const auto& pr = poolResults[idx];
        oss << std::setw((int)nameW) << std::left << (pr.poolName + ":");

        oss << std::setw(colW[0]) << std::right
            << fmtCell(overallScore[idx], pr.overallAverage);

        for (size_t s = 0; s < S; ++s)
            oss << std::setw(colW[s + 1]) << std::right
            << fmtCell(suiteScore[idx][s], pr.suiteAverages[s]);

        oss << '\n';
    }

    oss << "===============================================\n";
    Debug::debug_print(oss.str());
}

// ===================================
//  Summary – BEST SUITE‑AVERAGE
// ===================================
inline void printSummaryTableSuiteNormalized(
    const std::vector<SuiteSpec>& suites,
    std::vector<PoolResult>       poolResults)
{
    using std::size_t;
    const size_t P = poolResults.size();
    const size_t S = suites.size();

    // ---------- 1. best average time per suite ----------
    std::vector<double> bestSuiteMs(S, std::numeric_limits<double>::max());
    for (const auto& pr : poolResults)
        for (size_t s = 0; s < S; ++s)
            bestSuiteMs[s] = std::min(bestSuiteMs[s], pr.suiteAverages[s]);

    // ---------- 2. compute scores ----------
    std::vector<std::vector<double>> suiteScore(P, std::vector<double>(S));
    std::vector<double>              overallScore(P);

    for (size_t p = 0; p < P; ++p) {
        double sum = 0.0;
        for (size_t s = 0; s < S; ++s) {
            suiteScore[p][s] = bestSuiteMs[s] / poolResults[p].suiteAverages[s];
            sum += suiteScore[p][s];
        }
        overallScore[p] = sum / S;
    }

    // ---------- 3. sort by score ----------
    std::vector<size_t> order(P);
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(),
        [&](size_t a, size_t b) { return overallScore[a] > overallScore[b]; });

    // ---------- 4. column widths ----------
    size_t nameW = 0;
    for (const auto& pr : poolResults)
        nameW = std::max(nameW, pr.poolName.size());
    nameW += 2; // ": "

    auto fmtCell = [](double score, double ms) {
        std::ostringstream tmp;
        tmp << std::fixed << std::setprecision(2)
            << score << '(' << fmtTime(ms) << ')';
        return tmp.str();
    };

    std::vector<int> colW(S + 1, 0); // 0 = overall
    colW[0] = (int)std::string("overall").size();
    for (size_t s = 0; s < S; ++s)
        colW[s + 1] = (int)suites[s].suiteName.size();

    for (size_t p = 0; p < P; ++p) {
        colW[0] = std::max(colW[0],
            (int)fmtCell(overallScore[p], poolResults[p].overallAverage).size());
        for (size_t s = 0; s < S; ++s)
            colW[s + 1] = std::max(colW[s + 1],
                (int)fmtCell(suiteScore[p][s], poolResults[p].suiteAverages[s]).size());
    }

    for (int& w : colW) w += 1; // match your layout: 1-space buffer

    // ---------- 5. header row ----------
    std::ostringstream oss;
    oss << "\n=============== Suite Normalised ===============\n";
    oss << std::setw((int)nameW) << ""; // blank first column
    oss << std::setw(colW[0]) << std::left << "overall";
    for (size_t s = 0; s < S; ++s)
        oss << std::setw(colW[s + 1]) << std::left << suites[s].suiteName;
    oss << '\n';

    // ---------- 6. data rows ----------
    for (size_t idx : order) {
        const auto& pr = poolResults[idx];

        oss << std::setw((int)nameW) << std::left << (pr.poolName + ":");

        oss << std::setw(colW[0]) << std::right
            << fmtCell(overallScore[idx], pr.overallAverage);

        for (size_t s = 0; s < S; ++s)
            oss << std::setw(colW[s + 1]) << std::right
            << fmtCell(suiteScore[idx][s], pr.suiteAverages[s]);

        oss << '\n';
    }

    oss << "===============================================\n";
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

