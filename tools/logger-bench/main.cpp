#include "Logger.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <thread>
#include <vector>

namespace {

constexpr int kThreads = 4;
constexpr int kPerThread = 200000;
constexpr int kMsgSize = 64;

void syncMode()
{
    std::ofstream sink("/dev/null");
    const int total = kThreads * kPerThread;
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < total; ++i)
    {
        sink << "[INFO] 20260810 00:00:00.000000 : "
             << "0123456789012345678901234567890123456789012345678901234567890123" << '\n';
    }
    auto t1 = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(t1 - t0).count();
    printf("sync   : %.0f msg/s (%.3f s, %d msgs)\n",
           total / secs, secs, total);
}

void asyncMode()
{
    Logger::instance().setLogLevel(INFO);
    std::ofstream sink("/dev/null");
    Logger::instance().setOutputStream(&sink);

    auto t0 = std::chrono::steady_clock::now();
    std::vector<std::thread> threads;
    std::atomic<uint64_t> calls{0};
    std::vector<long long> latencies(static_cast<size_t>(kThreads) * kPerThread, 0);
    for (int t = 0; t < kThreads; ++t)
    {
        threads.emplace_back([t, &latencies, &calls]
                             {
                                 char msg[kMsgSize] = {0};
                                 snprintf(msg, sizeof msg, "async-%d-%08d", t, 0);
                                 for (int i = 0; i < kPerThread; ++i)
                                 {
                                     const auto t0 = std::chrono::steady_clock::now();
                                     Logger::instance().log(INFO, "bench", "ASYNC", msg);
                                     const auto t1 = std::chrono::steady_clock::now();
                                     latencies[static_cast<size_t>(t) * kPerThread + i] =
                                         std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
                                     calls.fetch_add(1);
                                 }
                             });
    }
    for (auto &th : threads)
    {
        th.join();
    }
    const uint64_t total = calls.load();
    auto t1 = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(t1 - t0).count();

    std::sort(latencies.begin(), latencies.end());
    auto pct = [&](double p)
    {
        return latencies[static_cast<size_t>(latencies.size() * p)];
    };

    Logger::instance().flush();
    const uint64_t dropped = Logger::instance().droppedCount();
    printf("async  : %.0f msg/s (%.3f s, %llu msgs, dropped=%llu)\n",
           total / secs, secs,
           static_cast<unsigned long long>(total),
           static_cast<unsigned long long>(dropped));
    printf("async  : p50=%.0fns p95=%.0fns p99=%.0fns\n",
           static_cast<double>(pct(0.50)),
           static_cast<double>(pct(0.95)),
           static_cast<double>(pct(0.99)));
}

} // namespace

int main()
{
    printf("logger-bench threads=%d per_thread=%d msg_size=%d\n",
           kThreads, kPerThread, kMsgSize);
    syncMode();
    asyncMode();
    return 0;
}
