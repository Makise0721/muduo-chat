#include "app/BlockingExecutor.hpp"
#include "EventLoop.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <stdexcept>
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

} // namespace

TEST(BlockingExecutorTest, AcceptsUntilQueueIsFullThenFailsFast)
{
    LoopThread lt;
    BlockingExecutor ex(lt.loop, 1, 2);

    std::promise<void> slowStarted;
    std::promise<void> slowDone;
    ASSERT_EQ(SubmitResult::Accepted,
              ex.submit([&slowStarted, &slowDone] {
                            slowStarted.set_value();
                            std::this_thread::sleep_for(std::chrono::milliseconds(100));
                            slowDone.set_value();
                        },
                        [] {}));
    // 等 worker 确认取走慢任务（此刻队列空、worker 忙）。
    EXPECT_EQ(std::future_status::ready,
              slowStarted.get_future().wait_for(std::chrono::seconds(5)));
    EXPECT_EQ(SubmitResult::Accepted, ex.submit([] {}, [] {}));
    EXPECT_EQ(SubmitResult::Accepted, ex.submit([] {}, [] {}));
    // 队列容量 2 已满：第 3 个队列任务必须 fail-fast 拒绝，不阻塞。
    EXPECT_EQ(SubmitResult::RejectedFull, ex.submit([] {}, [] {}));
    EXPECT_EQ(std::future_status::ready,
              slowDone.get_future().wait_for(std::chrono::seconds(5)));
}

TEST(BlockingExecutorTest, SubmitRejectedAfterShutdownAndShutdownIsIdempotent)
{
    LoopThread lt;
    BlockingExecutor ex(lt.loop, 1, 4);
    ex.shutdown();
    EXPECT_EQ(SubmitResult::RejectedShutdown, ex.submit([] {}, [] {}));
    ex.shutdown();  // 幂等
    EXPECT_EQ(SubmitResult::RejectedShutdown, ex.submit([] {}, [] {}));
}

TEST(BlockingExecutorTest, ShutdownDrainsPendingTasks)
{
    LoopThread lt;
    BlockingExecutor ex(lt.loop, 1, 4);
    std::atomic<int> ran{0};
    std::promise<void> completionDone;
    ASSERT_EQ(SubmitResult::Accepted,
              ex.submit([&ran] { ++ran; }, [&completionDone] { completionDone.set_value(); }));
    ex.shutdown();
    EXPECT_EQ(1, ran.load());
    EXPECT_EQ(std::future_status::ready,
              completionDone.get_future().wait_for(std::chrono::seconds(5)));
}

TEST(BlockingExecutorTest, ExpiredTaskIsSkippedWithoutCallback)
{
    LoopThread lt;
    BlockingExecutor ex(lt.loop, 1, 4);

    std::promise<void> blockerDone;
    ASSERT_EQ(SubmitResult::Accepted,
              ex.submit([&blockerDone] { std::this_thread::sleep_for(std::chrono::milliseconds(150)); blockerDone.set_value(); },
                        [] {}));
    std::atomic<int> expiredRan{0};
    std::atomic<int> expiredCompleted{0};
    // 过期任务（1ms deadline）被 150ms 慢任务堵在队列中，应被跳过。
    EXPECT_EQ(SubmitResult::Accepted,
              ex.submit([&expiredRan] { expiredRan.fetch_add(1); },
                        [&expiredCompleted] { expiredCompleted.fetch_add(1); },
                        1));
    EXPECT_EQ(std::future_status::ready,
              blockerDone.get_future().wait_for(std::chrono::seconds(5)));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(0, expiredRan.load());
    EXPECT_EQ(0, expiredCompleted.load());
}

TEST(BlockingExecutorTest, TaskExceptionStillRunsCompletionAndKeepsWorkerAlive)
{
    LoopThread lt;
    BlockingExecutor ex(lt.loop, 1, 4);

    // 异常任务先提交：workerLoop catch 后仍调度 completion。
    std::promise<void> throwingCompletionDone;
    ASSERT_EQ(SubmitResult::Accepted,
              ex.submit([] { throw std::runtime_error("boom"); },
                        [&throwingCompletionDone] { throwingCompletionDone.set_value(); }));
    // 随后的正常任务必须照常执行（worker 未被异常打死）。
    std::promise<void> normalDone;
    ASSERT_EQ(SubmitResult::Accepted,
              ex.submit([&normalDone] { normalDone.set_value(); }, [] {}));
    EXPECT_EQ(std::future_status::ready,
              normalDone.get_future().wait_for(std::chrono::seconds(5)));
    // 异常任务的 completion 仍被调度（completion 默认 errno 为非成功值，
    // 客户端得到失败响应而非悬挂）。
    EXPECT_EQ(std::future_status::ready,
              throwingCompletionDone.get_future().wait_for(std::chrono::seconds(5)));
    // 后续 submit 正常。
    std::promise<void> laterDone;
    ASSERT_EQ(SubmitResult::Accepted,
              ex.submit([&laterDone] { laterDone.set_value(); }, [] {}));
    EXPECT_EQ(std::future_status::ready,
              laterDone.get_future().wait_for(std::chrono::seconds(5)));
}

TEST(BlockingExecutorTest, CompletionIsDispatchedBackToLoopThread)
{
    LoopThread lt;
    BlockingExecutor ex(lt.loop, 2, 4);
    std::atomic<std::thread::id> completionTid;
    std::promise<void> done;
    ASSERT_EQ(SubmitResult::Accepted,
              ex.submit([&lt] { std::this_thread::sleep_for(std::chrono::milliseconds(20)); },
                        [&completionTid, &done] { completionTid = std::this_thread::get_id(); done.set_value(); }));
    EXPECT_EQ(std::future_status::ready,
              done.get_future().wait_for(std::chrono::seconds(5)));
    EXPECT_EQ(lt.loopTid, completionTid.load());
}

TEST(BlockingExecutorTest, InlineExecutorRunsSynchronously)
{
    InlineBlockingExecutor ex;
    std::string order;
    ASSERT_EQ(SubmitResult::Accepted,
              ex.submit([&order] { order += "task;"; }, [&order] { order += "completion;"; }));
    EXPECT_EQ("task;completion;", order);
    ex.shutdown();
    EXPECT_EQ(SubmitResult::Accepted, ex.submit([] {}, [] {}));
}

TEST(BlockingExecutorTest, MetricsCountQueueDepthAndDrops)
{
    LoopThread lt;
    BlockingExecutor ex(lt.loop, 1, 2);

    std::promise<void> slowStarted;
    std::promise<void> slowRelease;
    ASSERT_EQ(SubmitResult::Accepted,
              ex.submit([&slowStarted, &slowRelease] {
                            slowStarted.set_value();
                            slowRelease.get_future().wait();
                        },
                        [] {}));
    EXPECT_EQ(std::future_status::ready,
              slowStarted.get_future().wait_for(std::chrono::seconds(5)));

    EXPECT_EQ(SubmitResult::Accepted, ex.submit([] {}, [] {}));
    EXPECT_EQ(SubmitResult::Accepted, ex.submit([] {}, [] {}));
    EXPECT_EQ(2, ex.queueDepth());
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(SubmitResult::RejectedFull, ex.submit([] {}, [] {}));
    }
    EXPECT_EQ(3u, ex.droppedFull());
    EXPECT_EQ(0u, ex.droppedShutdown());

    slowRelease.set_value();
    ex.shutdown();
    EXPECT_EQ(SubmitResult::RejectedShutdown, ex.submit([] {}, [] {}));
    EXPECT_EQ(1u, ex.droppedShutdown());
    EXPECT_EQ(0, ex.queueDepth());
}

TEST(BlockingExecutorTest, InlineExecutorMetricsAreZero)
{
    InlineBlockingExecutor ex;
    EXPECT_EQ(0, ex.queueDepth());
    EXPECT_EQ(0u, ex.droppedFull());
    EXPECT_EQ(0u, ex.droppedShutdown());
}
