#include "Logger.h"
#include "Timestamp.h"

#include <cstdio>
#include <cstdlib>
#include <iostream>

Logger &Logger::instance()
{
    static Logger logger;
    return logger;
}

Logger::Logger()
    : logLevel_(INFO),
      dropped_(0),
      capacity_(4096),
      stream_(&std::cout),
      thread_(&Logger::run, this),
      exiting_(false),
      writing_(false)
{
}

Logger::~Logger()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        exiting_ = true;
        cond_.notify_all();
    }
    thread_.join();
}

void Logger::setQueueCapacity(size_t capacity)
{
    std::lock_guard<std::mutex> lock(mutex_);
    capacity_ = capacity;
    while (queue_.size() > capacity_)
    {
        queue_.pop_front();
        dropped_.fetch_add(1);
    }
}

void Logger::setOutputStream(std::ostream *stream)
{
    std::lock_guard<std::mutex> lock(mutex_);
    stream_ = stream;
}

void Logger::log(int level, const char *message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.size() >= capacity_)
    {
        if (level == FATAL)
        {
            if (!queue_.empty())
            {
                queue_.pop_front();
                dropped_.fetch_add(1);
            }
        }
        else
        {
            dropped_.fetch_add(1);
            return;
        }
    }
    queue_.emplace_back(level, std::string(message));
    cond_.notify_one();
}

void Logger::flush()
{
    std::unique_lock<std::mutex> lock(mutex_);
    cond_.wait(lock, [this] { return queue_.empty() && !writing_; });
}

void Logger::run()
{
    for (;;)
    {
        std::string message;
        int level;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cond_.wait(lock, [this] { return exiting_ || !queue_.empty(); });
            if (queue_.empty())
            {
                if (exiting_)
                {
                    return;
                }
                continue;
            }
            level = queue_.front().first;
            message = std::move(queue_.front().second);
            queue_.pop_front();
            writing_ = true;
        }

        std::ostream *out = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            out = stream_;
        }
        if (out != nullptr)
        {
            const char *prefix = "[INFO] ";
            switch (level)
            {
            case INFO:
                prefix = "[INFO] ";
                break;
            case ERROR:
                prefix = "[ERROR] ";
                break;
            case FATAL:
                prefix = "[FATAL] ";
                break;
            case DEBUG:
                prefix = "[DEBUG] ";
                break;
            default:
                break;
            }
            *out << prefix << Timestamp::now().toString() << " : " << message << std::endl;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            writing_ = false;
            cond_.notify_all();
        }
    }
}
