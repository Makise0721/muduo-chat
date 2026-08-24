#include "EventLoop.h"
#include "Logger.h"
#include "Poller.h"
#include "Channel.h"

#include <sys/eventfd.h>
#include <unistd.h>
#include <fcntl.h>
#include <memory>

__thread EventLoop *t_loopInThisThread = nullptr;

const int kPollTimeMs = 10000;

int createEventfd()
{
    int evtfd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (evtfd < 0)
    {
        LOG_FATAL("Failed in eventfd");
    }
    return evtfd;
}

EventLoop::EventLoop()
    : loopling_(false),
      quit_(false),
      threadId_(CurrentThread::tid()),
      pollReturnTime_(Timestamp::now()),
      poller_(Poller::newDefaultPoller(this)),
      wakeupFd_(createEventfd()),
      wakeupChannel_(new Channel(this, wakeupFd_)),
      callingPendingFunctors_(false),
      currentActiveChannel_(nullptr),
      timerQueue_(new TimerQueue(this))
{
    LOG_DEBUG("EventLoop created %p in thread %d", this, threadId_);
    if (t_loopInThisThread)
    {
        LOG_FATAL("Another EventLoop %p exists in this thread %d", t_loopInThisThread, threadId_);
    }
    else
    {
        t_loopInThisThread = this;
    }

    wakeupChannel_->setReadCallback(std::bind(&EventLoop::handleRead, this));
    wakeupChannel_->enableReading();
}

EventLoop::~EventLoop()
{
    LOG_DEBUG("EventLoop %p of thread %d destructed", this, threadId_);
    wakeupChannel_->disableAll();
    wakeupChannel_->remove();
    {
        std::lock_guard<std::mutex> lock(wakeupMutex_);
        if (wakeupFd_ >= 0)
        {
            ::close(wakeupFd_);
            wakeupFd_ = -1;
        }
    }
    t_loopInThisThread = nullptr;
}

int64_t EventLoop::steadyNowMs()
{
    // P5-00 M-1：同源稳态时钟（TimerQueue 内部 SteadyClock 同款）。
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               SteadyClock::now().time_since_epoch())
        .count();
}

TimerId EventLoop::runAfter(int64_t delayMs, TimerCallback cb)
{
    return timerQueue_->addTimer(std::move(cb), delayMs, 0);
}

TimerId EventLoop::runEvery(int64_t intervalMs, TimerCallback cb)
{
    return timerQueue_->addTimer(std::move(cb), intervalMs, intervalMs);
}

void EventLoop::cancel(TimerId id)
{
    timerQueue_->cancel(id);
}

void EventLoop::loop()
{
    loopling_ = true;

    LOG_INFO("EventLoop %p start looping", this);

    while (!quit_)
    {
        activeChannels_.clear();
        const int64_t pollStartMs = steadyNowMs();
        pollReturnTime_ = poller_->poll(kPollTimeMs, &activeChannels_);
        // P5-00 D9/M-1：loop-affine 探针 = 最近 poll 耗时差值（ms，>=0）。用
        // steady_clock（亚秒分辨率 + 单调）替代 Timestamp 墙钟，墙钟回拨不产生
        // 负值。
        const int64_t pollEndMs = steadyNowMs();
        int64_t lagMs = pollEndMs - pollStartMs;
        if (lagMs < 0) {
            lagMs = 0;
        }
        loopLagMs_.store(lagMs);

        for (Channel *channel : activeChannels_)
        {
            currentActiveChannel_ = channel;
            currentActiveChannel_->handleEvent(pollReturnTime_);
        }
        currentActiveChannel_ = nullptr;

        doPendingFunctors();
    }

    LOG_INFO("EventLoop %p stop looping", this);
    loopling_ = false;
}

void EventLoop::quit()
{
    quit_ = true;

    if (!isInLoopThread())
    {
        wakeup();
    }
}

void EventLoop::handleRead()
{
    uint64_t one = 1;
    ssize_t n = ::read(wakeupFd_, &one, sizeof one);
    if (n != sizeof one)
    {
        LOG_ERROR("EventLoop::handleRead() reads %zd bytes instead of 8", n);
    }
}

void EventLoop::wakeup()
{
    uint64_t one = 1;
    std::lock_guard<std::mutex> lock(wakeupMutex_);
    if (wakeupFd_ < 0)
    {
        return;
    }
    ssize_t n = ::write(wakeupFd_, &one, sizeof one);
    if (n != sizeof one)
    {
        LOG_ERROR("EventLoop::wakeup() writes %zd bytes instead of 8", n);
    }
}

void EventLoop::runInLoop(Functor cb)
{
    if (isInLoopThread())
    {
        cb();
    }
    else
    {
        queueInLoop(std::move(cb));
    }
}

void EventLoop::queueInLoop(Functor cb)
{
    {
        std::unique_lock<std::mutex> lock(mutex_);
        pendingFunctors_.emplace_back(std::move(cb));
    }

    if (!isInLoopThread() || callingPendingFunctors_)
    {
        wakeup();
    }
}

void EventLoop::updateChannel(Channel *channel)
{
    poller_->updateChannel(channel);
}

void EventLoop::removeChannel(Channel *channel)
{
    poller_->removeChannel(channel);
}

void EventLoop::doPendingFunctors()
{
    std::vector<Functor> functors;
    callingPendingFunctors_ = true;

    {
        std::unique_lock<std::mutex> lock(mutex_);
        functors.swap(pendingFunctors_);
    }

    for (const Functor &functor : functors)
    {
        functor();
    }
    callingPendingFunctors_ = false;
}
