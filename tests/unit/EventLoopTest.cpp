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

TEST(EventLoopTest, RunInLoopSameThreadExecutesDirectly)
{
    EventLoop loop;
    int called = 0;
    loop.runInLoop([&called] { ++called; });
    EXPECT_EQ(1, called);
}

TEST(EventLoopTest, QueueInLoopFromOtherThreadExecutesOnLoopThreadOnce)
{
    LoopThread lt;
    std::atomic<int> count{0};
    std::atomic<std::thread::id> execTid;
    std::promise<void> firstDone;
    std::promise<void> sentinel;
    lt.loop->queueInLoop([&]
                         {
                             ++count;
                             execTid = std::this_thread::get_id();
                             firstDone.set_value();
                         });
    lt.loop->queueInLoop([&] { sentinel.set_value(); });
    EXPECT_EQ(std::future_status::ready,
              firstDone.get_future().wait_for(std::chrono::seconds(10)));
    EXPECT_EQ(1, count.load());
    EXPECT_EQ(lt.loopTid, execTid.load());
    EXPECT_EQ(std::future_status::ready,
              sentinel.get_future().wait_for(std::chrono::seconds(10)));
}

TEST(EventLoopTest, RunInLoopFromOtherThreadIsQueued)
{
    LoopThread lt;
    std::atomic<int> count{0};
    std::promise<void> done;
    lt.loop->runInLoop([&]
                       {
                           ++count;
                           done.set_value();
                       });
    EXPECT_EQ(std::future_status::ready,
              done.get_future().wait_for(std::chrono::seconds(10)));
    EXPECT_EQ(1, count.load());
}

TEST(EventLoopTest, QuitFromOtherThreadStopsLoop)
{
    LoopThread lt;
    lt.loop->quit();
    EXPECT_EQ(std::future_status::ready,
              lt.endedF.wait_for(std::chrono::seconds(30)));
}
