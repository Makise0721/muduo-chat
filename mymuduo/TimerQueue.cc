#include "TimerQueue.h"

#include "EventLoop.h"
#include "Logger.h"

#include <sys/timerfd.h>
#include <unistd.h>
#include <strings.h>

#include <algorithm>

Timer::Timer(TimerCallback cb, TimePoint deadline, int64_t intervalMs)
    : callback_(std::move(cb)),
      expiration_(deadline),
      intervalMs_(intervalMs),
      cancelled_(false)
{
}

void Timer::restart(TimePoint now)
{
    if (repeat())
    {
        expiration_ += std::chrono::milliseconds(intervalMs_);
        if (expiration_ <= now)
        {
            const auto delta = now - expiration_;
            const auto skips = delta / std::chrono::milliseconds(intervalMs_) + 1;
            expiration_ += skips * std::chrono::milliseconds(intervalMs_);
        }
    }
    else
    {
        expiration_ = TimePoint();
    }
}

int TimerQueue::createTimerfd()
{
    int timerfd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (timerfd < 0)
    {
        LOG_FATAL("TimerQueue::createTimerfd() failed");
    }
    return timerfd;
}

TimerQueue::TimerQueue(EventLoop *loop)
    : loop_(loop),
      timerfd_(createTimerfd()),
      timerfdChannel_(loop, timerfd_),
      nextSequence_(0)
{
    timerfdChannel_.setReadCallback(std::bind(&TimerQueue::handleRead, this));
    timerfdChannel_.enableReading();
}

TimerQueue::~TimerQueue()
{
    timerfdChannel_.disableAll();
    timerfdChannel_.remove();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (timerfd_ >= 0)
        {
            ::close(timerfd_);
            timerfd_ = -1;
        }
    }
}

TimerId TimerQueue::addTimer(TimerCallback cb, int64_t delayMs, int64_t intervalMs)
{
    TimePoint deadline = SteadyClock::now() + std::chrono::milliseconds(delayMs);
    std::shared_ptr<Timer> timer(new Timer(std::move(cb), deadline, intervalMs));
    bool inserted = false;
    TimerId id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        id = TimerId(timer, nextSequence_++);
        inserted = insert(Entry(deadline, id));
    }
    if (inserted)
    {
        resetTimerfd(SteadyClock::now());
    }
    return id;
}

void TimerQueue::cancel(TimerId id)
{
    if (!id.timer)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    id.timer->cancel();
    timers_.erase(Entry(id.timer->expiration(), id));
}

void TimerQueue::handleRead()
{
    TimePoint now = SteadyClock::now();
    uint64_t expirations = 0;
    ssize_t n = ::read(timerfd_, &expirations, sizeof expirations);
    if (n != sizeof expirations)
    {
        LOG_ERROR("TimerQueue::handleRead() reads %zd bytes", n);
    }

    std::vector<TimerId> expired = getExpired(now);
    for (const TimerId &id : expired)
    {
        if (!id.timer->cancelled())
        {
            id.timer->run();
        }
    }
    resetTimerfd(now);
}

std::vector<TimerId> TimerQueue::getExpired(TimePoint now)
{
    std::vector<TimerId> expired;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Entry sentry(now + std::chrono::nanoseconds(1), TimerId{nullptr, -1});
        auto end = timers_.lower_bound(sentry);
        std::vector<Entry> toErase(timers_.begin(), end);
        timers_.erase(timers_.begin(), end);
        for (const Entry &entry : toErase)
        {
            expired.push_back(entry.second);
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    for (const TimerId &id : expired)
    {
        if (id.timer->repeat() && !id.timer->cancelled())
        {
            id.timer->restart(SteadyClock::now());
            insert(Entry(id.timer->expiration(), id));
        }
    }
    return expired;
}

void TimerQueue::resetTimerfd(TimePoint now)
{
    std::lock_guard<std::mutex> lock(mutex_);
    itimerspec newValue;
    bzero(&newValue, sizeof newValue);
    if (!timers_.empty())
    {
        int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         timers_.begin()->first - now)
                         .count();
        if (ns < 1)
        {
            ns = 1;
        }
        timespec ts;
        ts.tv_sec = static_cast<time_t>(ns / 1000000000);
        ts.tv_nsec = static_cast<long>(ns % 1000000000);
        newValue.it_value = ts;
    }
    if (timerfd_ >= 0 && ::timerfd_settime(timerfd_, 0, &newValue, nullptr) < 0)
    {
        LOG_ERROR("TimerQueue::resetTimerfd() failed");
    }
}

bool TimerQueue::insert(const Entry &entry)
{
    return timers_.insert(entry).second;
}
