#pragma once

#include <functional>
#include <vector>
#include <atomic>
#include <memory>
#include <mutex>

#include "noncopyable.h"
#include "CurrentThread.h"
#include "Timestamp.h"
#include "TimerQueue.h"

class Channel;
class Poller;

class EventLoop : noncopyable
{
public:
    using Functor = std::function<void()>;

    EventLoop();
    ~EventLoop();

    void loop();
    void quit();

    Timestamp pollReturnTime() const { return pollReturnTime_; }

    // P5-00 D9 loop-affine 探针：最近一次 poll 的耗时差值（ms，自记录最近 poll
    // 返回时刻与耗时；从未测量 = 0），>=0。
    int64_t loopLagProbeMs() const { return loopLagMs_.load(); }

    // P5-00 M-1：lag 探针时钟源（steady_clock，mymuduo 内部 TimerQueue 同源）。
    // 亚秒分辨率 + 单调（墙钟回拨不产生负差值）；探针与测试共用同一入口。
    static int64_t steadyNowMs();

    void runInLoop(Functor cb);
    void queueInLoop(Functor cb);

    TimerId runAfter(int64_t delayMs, TimerCallback cb);
    TimerId runEvery(int64_t intervalMs, TimerCallback cb);
    void cancel(TimerId id);

    void wakeup();
    void updateChannel(Channel *channel);
    void removeChannel(Channel *channel);
    bool hasChannel(Channel *channel);

    bool isInLoopThread() const { return threadId_ == CurrentThread::tid(); }

private:
    void handleRead();
    void doPendingFunctors();

    using ChannelList = std::vector<Channel *>;

    std::atomic_bool loopling_;
    std::atomic_bool quit_;

    const pid_t threadId_;
    Timestamp pollReturnTime_;
    std::atomic<int64_t> loopLagMs_{0};  // P5-00 D9 loop-affine 探针（ms）
    std::unique_ptr<Poller> poller_;

    int wakeupFd_;
    std::mutex wakeupMutex_;
    std::unique_ptr<Channel> wakeupChannel_;

    ChannelList activeChannels_;
    Channel *currentActiveChannel_;

    std::atomic_bool callingPendingFunctors_;
    std::vector<Functor> pendingFunctors_;
    std::mutex mutex_;

    std::unique_ptr<TimerQueue> timerQueue_;
};