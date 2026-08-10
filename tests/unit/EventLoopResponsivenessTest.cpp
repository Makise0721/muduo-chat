#include "app/BlockingExecutor.hpp"
#include "EventLoop.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>

namespace {

struct LoopThread
{
    std::mutex m;
    std::condition_variable cv;
    bool started = false;
    EventLoop* loop = nullptr;
    std::thread::id loopTid;
    std::promise<void> ended;
    std::future<void> endedF;
    std::thread t;

    LoopThread()
        : endedF(ended.get_future()),
          t([this]
            {
                EventLoop l;
                {
                    std::lock_guard<std::mutex> lk(m);
                    loop = &l;
                    loopTid = std::this_thread::get_id();
                    started = true;
                }
                cv.notify_one();
                l.loop();
                ended.set_value();
            })
    {
        std::unique_lock<std::mutex> lk(m);
        cv.wait(lk, [this] { return started; });
    }

    ~LoopThread()
    {
        if (started && loop != nullptr &&
            endedF.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        {
            EventLoop* l = loop;
            l->queueInLoop([l] { l->quit(); });
        }
        t.join();
    }
};

int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace

// 延迟门槛（预先记录）：EventLoop 定时探针 50ms 间隔，慢 DB 任务（100ms × 10）
// 经 BlockingExecutor（2 worker）执行时，probe 最大间隔必须 < 200ms——
// 若慢任务直接跑在 loop 线程，probe 间隔会被拉长到 ≥100ms 且可累积到 200ms 以上。
TEST(EventLoopResponsivenessTest, SlowTasksDoNotBlockTheLoop)
{
    LoopThread lt;
    BlockingExecutor ex(lt.loop, 2, 16);

    std::atomic<int> probes{0};
    std::atomic<int64_t> maxGapMs{0};
    std::atomic<int64_t> lastProbeMs{0};
    lt.loop->runEvery(50, [&] {
        int64_t now = nowMs();
        int64_t last = lastProbeMs.load();
        if (last > 0) {
            int64_t gap = now - last;
            int64_t cur = maxGapMs.load();
            while (gap > cur && !maxGapMs.compare_exchange_weak(cur, gap)) {
            }
        }
        lastProbeMs = now;
        probes.fetch_add(1);
    });

    std::atomic<int> completed{0};
    for (int i = 0; i < 10; ++i) {
        ASSERT_EQ(SubmitResult::Accepted,
                  ex.submit([] { std::this_thread::sleep_for(std::chrono::milliseconds(100)); },
                            [&completed] { completed.fetch_add(1); }));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    EXPECT_GE(probes.load(), 6) << "loop probes must keep firing while slow tasks run";
    EXPECT_LT(maxGapMs.load(), 200) << "recorded latency gate: probe gap < 200ms";
    EXPECT_EQ(10, completed.load());
}
