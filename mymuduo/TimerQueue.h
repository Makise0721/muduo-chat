#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <utility>
#include <vector>

#include "Channel.h"
#include "noncopyable.h"

class EventLoop;

using TimerCallback = std::function<void()>;
using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

class Timer : noncopyable
{
public:
    Timer(TimerCallback cb, TimePoint deadline, int64_t intervalMs);

    void run() const { callback_(); }
    void restart(TimePoint now);

    TimePoint expiration() const { return expiration_; }
    bool repeat() const { return intervalMs_ > 0; }
    bool cancelled() const { return cancelled_.load(); }
    void cancel() { cancelled_.store(true); }

private:
    TimerCallback callback_;
    TimePoint expiration_;
    const int64_t intervalMs_;
    std::atomic<bool> cancelled_;
};

struct TimerId
{
    std::shared_ptr<Timer> timer;
    int64_t sequence;

    TimerId() : sequence(0) {}

    TimerId(std::shared_ptr<Timer> t, int64_t seq)
        : timer(std::move(t)), sequence(seq)
    {
    }

    bool operator<(const TimerId &other) const
    {
        return sequence < other.sequence;
    }
};

class TimerQueue : noncopyable
{
public:
    using Entry = std::pair<TimePoint, TimerId>;

    explicit TimerQueue(EventLoop *loop);
    ~TimerQueue();

    TimerId addTimer(TimerCallback cb, int64_t delayMs, int64_t intervalMs);
    void cancel(TimerId id);

private:
    static int createTimerfd();
    void handleRead();
    std::vector<TimerId> getExpired(TimePoint now);
    void resetTimerfd(TimePoint now);
    bool insert(const Entry &entry);

    EventLoop *loop_;
    int timerfd_;
    Channel timerfdChannel_;
    std::mutex mutex_;
    std::set<Entry> timers_;
    int64_t nextSequence_;
};
