#include "Logger.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <future>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

std::string runWithLevel(int level, const std::function<void()> &body)
{
    Logger::instance().setLogLevel(level);
    std::ostringstream sink;
    Logger::instance().setOutputStream(&sink);
    body();
    Logger::instance().flush();
    Logger::instance().setOutputStream(nullptr);
    return sink.str();
}

} // namespace

TEST(LoggerTest, LevelFilterSkipsBelowThreshold)
{
    std::string out = runWithLevel(ERROR, []
                                   {
                                       LOG_INFO("should not appear");
                                       LOG_ERROR("error visible");
                                   });
    EXPECT_EQ(std::string::npos, out.find("should not appear"));
    EXPECT_NE(std::string::npos, out.find("error visible"));
}

TEST(LoggerTest, DebugLevelShowsInfoAndError)
{
    std::string out = runWithLevel(DEBUG, []
                                   {
                                       LOG_INFO("info visible at debug");
                                       LOG_ERROR("error visible at debug");
                                   });
    EXPECT_NE(std::string::npos, out.find("info visible at debug"));
    EXPECT_NE(std::string::npos, out.find("error visible at debug"));
}

TEST(LoggerTest, ConcurrentLogsAreCompleteLines)
{
    Logger::instance().setLogLevel(INFO);
    std::ostringstream sink;
    Logger::instance().setOutputStream(&sink);
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
    {
        threads.emplace_back([t]
                             {
                                 for (int i = 0; i < 200; ++i)
                                 {
                                     LOG_INFO("T%d-%d", t, i);
                                 }
                             });
    }
    for (auto &th : threads)
    {
        th.join();
    }
    Logger::instance().flush();
    Logger::instance().setOutputStream(nullptr);

    std::istringstream in(sink.str());
    std::string line;
    int total = 0;
    while (std::getline(in, line))
    {
        ++total;
        if (line.empty())
        {
            continue;
        }
        size_t marker = line.find("[INFO]");
        ASSERT_NE(std::string::npos, marker);
        std::string rest = line.substr(marker + 6);
        size_t colon = rest.find(" : ");
        ASSERT_NE(std::string::npos, colon);
        std::string payload = rest.substr(colon + 3);
        ASSERT_GE(payload.size(), 4u);
        ASSERT_EQ('T', payload[0]);
        size_t dash = payload.find('-');
        ASSERT_NE(std::string::npos, dash);
        ASSERT_GT(dash, 1u);
        for (size_t k = 1; k < dash; ++k)
        {
            ASSERT_TRUE(payload[k] >= '0' && payload[k] <= '9');
        }
        for (size_t k = dash + 1; k < payload.size(); ++k)
        {
            ASSERT_TRUE(payload[k] >= '0' && payload[k] <= '9');
        }
    }
    EXPECT_EQ(800, total);
}

TEST(LoggerTest, FullQueueDropsAndCounts)
{
    Logger::instance().setQueueCapacity(16);
    Logger::instance().setLogLevel(INFO);
    std::ostringstream sink;
    Logger::instance().setOutputStream(&sink);
    for (int i = 0; i < 1000; ++i)
    {
        LOG_INFO("bulk-%d", i);
    }
    Logger::instance().flush();
    EXPECT_GT(Logger::instance().droppedCount(), 0u);
    Logger::instance().setOutputStream(nullptr);
    Logger::instance().setQueueCapacity(4096);
}

TEST(LoggerTest, FatalNotDroppedWhenQueueFull)
{
    Logger::instance().setQueueCapacity(1);
    Logger::instance().setLogLevel(INFO);
    std::ostringstream sink;
    Logger::instance().setOutputStream(&sink);
    for (int i = 0; i < 1000; ++i)
    {
        LOG_INFO("bulk-%d", i);
    }
    Logger::instance().log(FATAL, "", "LOG_FATAL", "fatal must survive");
    Logger::instance().flush();
    std::string out = sink.str();
    Logger::instance().setOutputStream(nullptr);
    Logger::instance().setQueueCapacity(4096);
    Logger::instance().setLogLevel(INFO);
    EXPECT_NE(std::string::npos, out.find("[FATAL]"));
    EXPECT_NE(std::string::npos, out.find("fatal must survive"));
}

TEST(LoggerTest, FlushEmptiesQueue)
{
    Logger::instance().setLogLevel(INFO);
    std::ostringstream sink;
    Logger::instance().setOutputStream(&sink);
    for (int i = 0; i < 10; ++i)
    {
        LOG_INFO("x-%d", i);
    }
    Logger::instance().flush();
    std::string out = sink.str();
    Logger::instance().setOutputStream(nullptr);
    EXPECT_EQ(10, static_cast<int>(std::count(out.begin(), out.end(), '\n')));
}

TEST(LoggerTest, StructuredFieldsReachableByRecorder)
{
    Logger::instance().setLogLevel(INFO);
    std::mutex m;
    std::vector<LogEvent> captured;
    std::promise<void> got;
    Logger::instance().setSinkCallback([&](const std::vector<LogEvent> &batch)
                                       {
                                           std::lock_guard<std::mutex> lk(m);
                                           captured.insert(captured.end(), batch.begin(), batch.end());
                                           got.set_value();
                                       });
    Logger::instance().setOutputStream(nullptr);
    const auto callerTid = std::this_thread::get_id();
    Logger::instance().log(INFO, "chat", "USER_LOGIN", "hello world");
    EXPECT_EQ(std::future_status::ready,
              got.get_future().wait_for(std::chrono::seconds(5)));

    std::vector<LogEvent> local;
    {
        std::lock_guard<std::mutex> lk(m);
        local = captured;
    }
    ASSERT_FALSE(local.empty());
    const LogEvent &e = local.front();
    EXPECT_EQ(INFO, e.level);
    EXPECT_EQ("chat", e.component);
    EXPECT_EQ("USER_LOGIN", e.eventName);
    EXPECT_EQ("hello world", e.message);
    EXPECT_EQ(callerTid, e.threadId);
    EXPECT_GT(e.timestamp.microSecondsSinceEpoch(), 0);

    Logger::instance().setSinkCallback(nullptr);
}

TEST(LoggerTest, BatchPreservesOrder)
{
    Logger::instance().setLogLevel(INFO);
    std::mutex m;
    std::vector<int> order;
    std::promise<void> done;
    Logger::instance().setSinkCallback([&](const std::vector<LogEvent> &batch)
                                       {
                                           std::lock_guard<std::mutex> lk(m);
                                           for (const LogEvent &e : batch)
                                           {
                                               order.push_back(atoi(e.message.c_str()));
                                           }
                                           if (order.size() >= 200)
                                           {
                                               done.set_value();
                                           }
                                       });
    Logger::instance().setOutputStream(nullptr);
    for (int i = 0; i < 200; ++i)
    {
        Logger::instance().log(INFO, "t", "e", std::to_string(i));
    }
    EXPECT_EQ(std::future_status::ready,
              done.get_future().wait_for(std::chrono::seconds(5)));

    std::vector<int> local;
    {
        std::lock_guard<std::mutex> lk(m);
        local = order;
    }
    ASSERT_EQ(200u, local.size());
    for (int i = 0; i < 200; ++i)
    {
        EXPECT_EQ(i, local[i]);
    }

    Logger::instance().setSinkCallback(nullptr);
}

TEST(LoggerTest, ThreadIdDoesNotInterleave)
{
    Logger::instance().setLogLevel(INFO);
    std::mutex m;
    std::vector<std::pair<std::thread::id, std::string>> captured;
    std::promise<void> done;
    Logger::instance().setSinkCallback([&](const std::vector<LogEvent> &batch)
                                       {
                                           std::lock_guard<std::mutex> lk(m);
                                           for (const LogEvent &e : batch)
                                           {
                                               captured.emplace_back(e.threadId, e.component);
                                           }
                                           if (captured.size() >= 400)
                                           {
                                               done.set_value();
                                           }
                                       });
    Logger::instance().setOutputStream(nullptr);

    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
    {
        threads.emplace_back([t]
                             {
                                 for (int i = 0; i < 100; ++i)
                                 {
                                     Logger::instance().log(INFO, "thread" + std::to_string(t),
                                                            "e", "m");
                                 }
                             });
    }
    for (auto &th : threads)
    {
        th.join();
    }
    EXPECT_EQ(std::future_status::ready,
              done.get_future().wait_for(std::chrono::seconds(5)));

    std::vector<std::pair<std::thread::id, std::string>> local;
    {
        std::lock_guard<std::mutex> lk(m);
        local = captured;
    }
    ASSERT_EQ(400u, local.size());
    std::map<std::string, std::thread::id> tidByComponent;
    for (const auto &p : local)
    {
        auto it = tidByComponent.find(p.second);
        if (it == tidByComponent.end())
        {
            tidByComponent[p.second] = p.first;
        }
        else
        {
            EXPECT_EQ(it->second, p.first);
        }
    }

    Logger::instance().setSinkCallback(nullptr);
}
