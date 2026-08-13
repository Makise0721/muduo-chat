#pragma once

#include <chrono>
#include <cstdint>
#include <functional>

// 模块内部确定性测试 seam 与持久 lease 使用的生产时钟。
// 独立头：mymuduo TimerQueue.h 的全局 `using Clock = std::chrono::steady_clock`
// 与领域 class Clock 同名冲突——本头使领域类型可在 mymuduo TU（如
// SessionDeliverySink）安全引入（ReliableMessaging.hpp 仅前向声明）。
class Clock {
public:
    virtual ~Clock() = default;
    virtual int64_t nowMs() = 0;
    // P3-08：注册"时钟推进"通知回调。测试 FakeClock 在 set/advance 后调用以
    // 唤醒 scheduler 条件变量（确定性测试不依赖固定 sleep）；生产 UnixEpochClock
    // 无推进事件，默认空操作。
    virtual void registerAdvanceNotifier(std::function<void()> notifier)
    {
        (void)notifier;
    }
};

// Unix epoch milliseconds are persisted in MessageDelivery DATETIME columns;
// they must use the wall-clock epoch rather than a process-local monotonic
// clock whose epoch is unspecified and cannot survive a restart.
class UnixEpochClock : public Clock {
public:
    int64_t nowMs() override
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }
};
