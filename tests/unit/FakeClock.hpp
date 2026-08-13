#pragma once

#include "app/Clock.hpp"

#include <cstdint>
#include <functional>
#include <mutex>

// 确定性测试时钟：测试直接控制 nowMs（P3-08 引入生产系统时钟实现）。
// P3-08：内部互斥使 scheduler 线程与测试线程读写串行化（TSan 安全）；
// set/advance 后调用已注册的推进通知（ReliableMessaging::start 注册，唤醒
// scheduler 条件变量——测试不依赖固定 sleep）。
class FakeClock : public Clock {
public:
    void set(int64_t ms)
    {
        std::lock_guard<std::mutex> lk(mutex_);
        nowMs_ = ms;
        notify();
    }

    void advance(int64_t delta)
    {
        std::lock_guard<std::mutex> lk(mutex_);
        nowMs_ += delta;
        notify();
    }

    int64_t nowMs() override
    {
        std::lock_guard<std::mutex> lk(mutex_);
        return nowMs_;
    }

    void registerAdvanceNotifier(std::function<void()> notifier) override
    {
        std::lock_guard<std::mutex> lk(mutex_);
        notify_ = std::move(notifier);
    }

private:
    // 须在持 mutex_ 时调用；通知回调（cv_.notify_all）不阻塞、不取其它锁。
    void notify()
    {
        if (notify_) {
            notify_();
        }
    }

    std::mutex mutex_;
    int64_t nowMs_ = 0;
    std::function<void()> notify_;
};
