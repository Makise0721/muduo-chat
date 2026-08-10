#pragma once

#include "noncopyable.h"
#include "Timestamp.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <ostream>
#include <string>
#include <thread>
#include <vector>

#define LOG_INFO(LogmsgFormat, ...)                       \
    do                                                    \
    {                                                     \
        Logger &logger = Logger::instance();              \
        if (logger.logLevel() <= INFO)            \
        {                                                 \
            char buf[1024] = {0};                         \
            snprintf(buf, sizeof buf, LogmsgFormat, ##__VA_ARGS__); \
            logger.log(INFO, "", "LOG_INFO", buf);        \
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
            logger.log(ERROR, "", "LOG_ERROR", buf);      \
        }                                                 \
    } while (0)

#define LOG_FATAL(LogmsgFormat, ...)                      \
    do                                                    \
    {                                                     \
        Logger &logger = Logger::instance();              \
        char buf[1024] = {0};                             \
        snprintf(buf, sizeof buf, LogmsgFormat, ##__VA_ARGS__); \
        logger.log(FATAL, "", "LOG_FATAL", buf);          \
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
            logger.log(DEBUG, "", "LOG_DEBUG", buf);      \
        }                                                 \
    } while (0)
#else
#define LOG_DEBUG(LogmsgFormat, ...)
#endif

enum LogLevel
{
    DEBUG,
    INFO,
    ERROR,
    FATAL,
};

struct LogEvent
{
    Timestamp timestamp;
    int level;
    std::thread::id threadId;
    std::string component;
    std::string eventName;
    std::string message;
};

class Logger : noncopyable
{
public:
    using SinkCallback = std::function<void(const std::vector<LogEvent> &)>;

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

    void setSinkCallback(const SinkCallback &cb);

    void log(int level,
             const std::string &component,
             const std::string &eventName,
             const std::string &message);

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
    std::deque<LogEvent> queue_;
    size_t capacity_;
    std::ostream *stream_;
    SinkCallback sinkCallback_;
    bool exiting_;
    bool writing_;
    std::thread thread_;
};
