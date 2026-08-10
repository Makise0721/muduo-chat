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

void Logger::setSinkCallback(const SinkCallback &cb)
{
    std::lock_guard<std::mutex> lock(mutex_);
    sinkCallback_ = cb;
}

void Logger::log(int level,
                 const std::string &component,
                 const std::string &eventName,
                 const std::string &message)
{
    LogEvent event;
    event.timestamp = Timestamp::now();
    event.level = level;
    event.threadId = std::this_thread::get_id();
    event.component = component;
    event.eventName = eventName;
    event.message = message;

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
    queue_.emplace_back(std::move(event));
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
        std::vector<LogEvent> batch;
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
            while (batch.size() < 64 && !queue_.empty())
            {
                batch.push_back(std::move(queue_.front()));
                queue_.pop_front();
            }
            writing_ = true;
        }

        SinkCallback sink;
        std::ostream *out = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            sink = sinkCallback_;
            out = stream_;
        }
        if (sink)
        {
            sink(batch);
        }
        if (out != nullptr)
        {
            for (const LogEvent &event : batch)
            {
                const char *prefix = "[INFO] ";
                switch (event.level)
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
                *out << prefix << event.timestamp.toString() << " : "
                     << event.message << '\n';
            }
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            writing_ = false;
            cond_.notify_all();
        }
    }
}
