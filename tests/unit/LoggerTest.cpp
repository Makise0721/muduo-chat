#include "Logger.h"

#include <gtest/gtest.h>

#include <algorithm>
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
    Logger::instance().log(FATAL, "fatal must survive");
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
