#pragma once

#include "noncopyable.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <string>
#include <thread>
#include <ostream>

#define LOG_INFO(LogmsgFormat, ...)                       \
    do                                                    \
    {                                                     \
        Logger &logger = Logger::instance();              \
        if (logger.logLevel() <= INFO)            \
        {                                                 \
            char buf[1024] = {0};                         \
            snprintf(buf, sizeof buf, LogmsgFormat, ##__VA_ARGS__); \
            logger.log(INFO, buf);                \
        }                                                 \
    } while (0)

#define LOG_ERROR(LogmsgFormat, ...)                      \
    do                                                    \
    {                                                     \
        Logger &logger = Logger::instance();              \
        if (logger.logLevel() <= ERROR)           \
        {                                                 \
            char buf[1024] = {0};                         \
            snprintf(buf, sizeof buf, LogmsgFormat, ##__VA_ARGS__); \
            logger.log(ERROR, buf);               \
        }                                                 \
    } while (0)

#define LOG_FATAL(LogmsgFormat, ...)                      \
    do                                                    \
    {                                                     \
        Logger &logger = Logger::instance();              \
        char buf[1024] = {0};                             \
        snprintf(buf, sizeof buf, LogmsgFormat, ##__VA_ARGS__); \
        logger.log(FATAL, buf);                   \
        logger.flush();                                   \
        exit(-1);                                         \
    } while (0)

#ifdef MUDEBUG
#define LOG_DEBUG(LogmsgFormat, ...)                      \
    do                                                    \
    {                                                     \
        Logger &logger = Logger::instance();              \
        if (logger.logLevel() <= DEBUG)           \
        {                                                 \
            char buf[1024] = {0};                         \
            snprintf(buf, sizeof buf, LogmsgFormat, ##__VA_ARGS__); \
            logger.log(DEBUG, buf);               \
        }                                                 \
    } while (0)
#else
#define LOG_DEBUG(LogmsgFormat, ...)
#endif

enum LogLevel
{
    INFO,
    ERROR,
    FATAL,
    DEBUG,
};

class Logger : noncopyable
{
public:
    static Logger &instance();

    void setLogLevel(int level)
    {
        logLevel_.store(level);
    }

    int logLevel() const
    {
        return logLevel_.load();
    }

    void setQueueCapacity(size_t capacity);

    void setOutputStream(std::ostream *stream);

    void log(int level, const char *message);

    void flush();

    uint64_t droppedCount() const
    {
        return dropped_.load();
    }

private:
    Logger();
    ~Logger();
    void run();

    std::atomic<int> logLevel_;
    std::atomic<uint64_t> dropped_;
    std::mutex mutex_;
    std::condition_variable cond_;
    std::deque<std::pair<int, std::string>> queue_;
    size_t capacity_;
    std::ostream *stream_;
    bool exiting_;
    bool writing_;
    std::thread thread_;
};
