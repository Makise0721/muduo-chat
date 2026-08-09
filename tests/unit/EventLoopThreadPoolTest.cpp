#include "EventLoop.h"
#include "EventLoopThreadPool.h"

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <set>
#include <vector>

TEST(EventLoopThreadPoolTest, ZeroThreadsReturnsBaseLoop)
{
    EventLoop base;
    EventLoopThreadPool pool(&base, "zero");
    pool.setThreadNum(0);
    pool.start();
    EXPECT_TRUE(pool.started());
    EXPECT_EQ(&base, pool.getNextLoop());
    EXPECT_EQ(&base, pool.getNextLoop());
    std::vector<EventLoop *> loops = pool.getAllLoops();
    ASSERT_EQ(1u, loops.size());
    EXPECT_EQ(&base, loops[0]);
}

TEST(EventLoopThreadPoolTest, RoundRobinAcrossWorkers)
{
    EventLoop base;
    EventLoopThreadPool pool(&base, "rr");
    pool.setThreadNum(3);
    pool.start();
    std::vector<EventLoop *> loops = pool.getAllLoops();
    ASSERT_EQ(3u, loops.size());
    for (EventLoop *loop : loops)
    {
        ASSERT_NE(nullptr, loop);
        EXPECT_NE(&base, loop);
    }
    std::vector<EventLoop *> order;
    for (int i = 0; i < 6; ++i)
    {
        order.push_back(pool.getNextLoop());
    }
    for (int i = 0; i < 6; ++i)
    {
        EXPECT_EQ(loops[i % 3], order[i]);
    }
    std::set<EventLoop *> seen(order.begin(), order.end());
    EXPECT_EQ(3u, seen.size());
}

TEST(EventLoopThreadPoolTest, WorkerLoopsAreLiveAndShutdownCleanly)
{
    std::vector<EventLoop *> workerLoops;
    {
        EventLoop base;
        EventLoopThreadPool pool(&base, "life");
        pool.setThreadNum(2);
        pool.start();
        workerLoops = pool.getAllLoops();
        ASSERT_EQ(2u, workerLoops.size());
        for (EventLoop *loop : workerLoops)
        {
            std::promise<void> done;
            loop->queueInLoop([&done] { done.set_value(); });
            EXPECT_EQ(std::future_status::ready,
                      done.get_future().wait_for(std::chrono::seconds(10)));
        }
    }
    workerLoops.clear();
}
