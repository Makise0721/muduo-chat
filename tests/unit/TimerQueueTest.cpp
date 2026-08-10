#include "EventLoop.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

namespace {

struct LoopThread
{
    std::mutex m;
    std::condition_variable cv;
    bool started = false;
    EventLoop *loop = nullptr;
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
            EventLoop *l = loop;
            l->queueInLoop([l] { l->quit(); });
        }
        t.join();
    }
};

} // namespace

TEST(TimerQueueTest, RunAfterFiresOnceOnLoopThread)
{
    LoopThread lt;
    std::atomic<int> count{0};
    std::atomic<std::thread::id> execTid;
    std::promise<void> done;
    lt.loop->runAfter(20, [&]
                      {
                          ++count;
                          execTid = std::this_thread::get_id();
                          done.set_value();
                      });
    EXPECT_EQ(std::future_status::ready,
              done.get_future().wait_for(std::chrono::seconds(5)));
    EXPECT_EQ(1, count.load());
    EXPECT_EQ(lt.loopTid, execTid.load());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(1, count.load());
}

TEST(TimerQueueTest, RunEveryFiresRepeatedlyWithoutDrift)
{
    LoopThread lt;
    std::atomic<int> count{0};
    std::mutex m;
    std::vector<std::chrono::steady_clock::time_point> stamps;
    std::promise<void> gotThree;
    lt.loop->runEvery(15, [&]
                      {
                          {
                              std::lock_guard<std::mutex> lk(m);
                              stamps.push_back(std::chrono::steady_clock::now());
                          }
                          if (++count >= 3)
                          {
                              gotThree.set_value();
                          }
                      });
    EXPECT_EQ(std::future_status::ready,
              gotThree.get_future().wait_for(std::chrono::seconds(5)));
    std::vector<std::chrono::steady_clock::time_point> local;
    {
        std::lock_guard<std::mutex> lk(m);
        local = stamps;
    }
    ASSERT_GE(local.size(), 3u);
    for (size_t i = 1; i < local.size(); ++i)
    {
        auto gap = std::chrono::duration_cast<std::chrono::milliseconds>(
                       local[i] - local[i - 1])
                       .count();
        EXPECT_GE(gap, 5);
        EXPECT_LE(gap, 200);
    }
}

TEST(TimerQueueTest, CancelledTimerNeverFires)
{
    LoopThread lt;
    std::atomic<int> count{0};
    TimerId id = lt.loop->runAfter(20, [&] { ++count; });
    lt.loop->cancel(id);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    EXPECT_EQ(0, count.load());
}

TEST(TimerQueueTest, CancelRepeatTimerStopsIt)
{
    LoopThread lt;
    std::atomic<int> count{0};
    std::promise<void> fired;
    TimerId id = lt.loop->runEvery(10, [&]
                                   {
                                       ++count;
                                       if (count >= 2)
                                       {
                                           fired.set_value();
                                       }
                                   });
    EXPECT_EQ(std::future_status::ready,
              fired.get_future().wait_for(std::chrono::seconds(5)));
    lt.loop->cancel(id);
    int after = count.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    EXPECT_EQ(after, count.load());
}

TEST(TimerQueueTest, SameDeadlineFiresInRegistrationOrder)
{
    LoopThread lt;
    std::mutex m;
    std::vector<int> order;
    std::promise<void> done;
    lt.loop->runAfter(20, [&]
                      {
                          std::lock_guard<std::mutex> lk(m);
                          order.push_back(1);
                      });
    lt.loop->runAfter(20, [&]
                      {
                          {
                              std::lock_guard<std::mutex> lk(m);
                              order.push_back(2);
                          }
                          done.set_value();
                      });
    EXPECT_EQ(std::future_status::ready,
              done.get_future().wait_for(std::chrono::seconds(5)));
    std::vector<int> local;
    {
        std::lock_guard<std::mutex> lk(m);
        local = order;
    }
    ASSERT_EQ(2u, local.size());
    EXPECT_EQ(1, local[0]);
    EXPECT_EQ(2, local[1]);
}

TEST(TimerQueueTest, RegisterFromOtherThread)
{
    LoopThread lt;
    std::promise<void> done;
    lt.loop->runAfter(20, [&done] { done.set_value(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    std::thread t([&]
                  {
                      lt.loop->runAfter(20, [&done] { done.set_value(); });
                  });
    t.join();
    EXPECT_EQ(std::future_status::ready,
              done.get_future().wait_for(std::chrono::seconds(5)));
}

TEST(TimerQueueTest, CancelFromOtherThread)
{
    LoopThread lt;
    std::atomic<int> count{0};
    std::promise<void> fired;
    TimerId id = lt.loop->runEvery(10, [&]
                                   {
                                       ++count;
                                       if (count >= 2)
                                       {
                                           fired.set_value();
                                       }
                                   });
    EXPECT_EQ(std::future_status::ready,
              fired.get_future().wait_for(std::chrono::seconds(5)));
    std::thread t([&] { lt.loop->cancel(id); });
    t.join();
    int after = count.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    EXPECT_EQ(after, count.load());
}

TEST(TimerQueueTest, CancelInsideCallback)
{
    LoopThread lt;
    std::atomic<int> count{0};
    std::promise<void> done;
    TimerId id = lt.loop->runEvery(10, [&]
                                   {
                                       ++count;
                                       if (count >= 2)
                                       {
                                           lt.loop->cancel(id);
                                           done.set_value();
                                       }
                                   });
    EXPECT_EQ(std::future_status::ready,
              done.get_future().wait_for(std::chrono::seconds(5)));
    int after = count.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    EXPECT_EQ(after, count.load());
    EXPECT_GE(after, 2);
}

TEST(TimerQueueTest, CancelDefaultTimerIdIsNoOp)
{
    LoopThread lt;
    TimerId id;
    EXPECT_NO_THROW(lt.loop->cancel(id));
    EXPECT_NO_THROW(lt.loop->cancel(id));
}

TEST(TimerQueueTest, CancelInsideCallbackTwiceIsIdempotent)
{
    LoopThread lt;
    std::atomic<int> count{0};
    std::promise<void> done;
    TimerId id = lt.loop->runEvery(10, [&]
                                   {
                                       if (++count >= 2)
                                       {
                                           lt.loop->cancel(id);
                                           lt.loop->cancel(id);
                                           done.set_value();
                                       }
                                   });
    EXPECT_EQ(std::future_status::ready,
              done.get_future().wait_for(std::chrono::seconds(5)));
    int after = count.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    EXPECT_EQ(after, count.load());
}

TEST(TimerQueueTest, CancelFiredOnceTimerIsNoOp)
{
    LoopThread lt;
    std::atomic<int> count{0};
    std::promise<void> fired;
    TimerId id = lt.loop->runAfter(20, [&]
                                   {
                                       ++count;
                                       fired.set_value();
                                   });
    EXPECT_EQ(std::future_status::ready,
              fired.get_future().wait_for(std::chrono::seconds(5)));
    EXPECT_NO_THROW(lt.loop->cancel(id));
}

TEST(TimerQueueTest, CancelFiredTimerRepeatedlyIsSafe)
{
    LoopThread lt;
    for (int round = 0; round < 500; ++round)
    {
        std::promise<void> fired;
        TimerId id = lt.loop->runAfter(1, [&fired] { fired.set_value(); });
        EXPECT_EQ(std::future_status::ready,
                  fired.get_future().wait_for(std::chrono::seconds(5)));
        lt.loop->cancel(id);
        lt.loop->cancel(id);
    }
}

TEST(TimerQueueTest, LongCallbackKeepsPlannedPhase)
{
    LoopThread lt;
    std::atomic<int> count{0};
    std::promise<void> done;
    lt.loop->runEvery(50, [&]
                      {
                          const int c = ++count;
                          std::this_thread::sleep_for(std::chrono::milliseconds(120));
                          if (c == 3)
                          {
                              done.set_value();
                          }
                      });
    EXPECT_EQ(std::future_status::ready,
              done.get_future().wait_for(std::chrono::seconds(5)));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    const int total = count.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    const int totalLater = count.load();
    EXPECT_LE(total, 4);
    EXPECT_LE(totalLater, 5);
}
