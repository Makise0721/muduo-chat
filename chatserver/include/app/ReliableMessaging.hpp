#pragma once

#include "app/Config.hpp"       // P3-08 冻结参数 RetryConfig（生产 config 注入，默认=卡冻结值）
#include "app/DomainTypes.hpp"

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// P3-02 可靠消息模块：领域值类型见 app/DomainTypes.hpp（本头保持 mymuduo-safe：
// 只前向声明 class Clock——mymuduo TimerQueue.h 的全局 `using Clock` 与领域
// class Clock 同名冲突，完整定义在 app/Clock.hpp，仅供领域侧 TU 引入）。
// raw JSON 不进领域类型；codec/session 绑定在应用层（P3-05/P3-06）完成。

// 模块内部确定性测试 seam；定义见 app/Clock.hpp。
class Clock;

class MessageStore;
class DeliverySink;
class DeliveryCoordinator;

// P3-08 补缺轮 M1：scheduler 下一次唤醒间隔。固定 ack_timeout(30s) 轮询使
// attempt<6 的 backoff（base*2^n，均 < 30s）因轮询粒度不可观测；改为取"本 tick
// 触碰行中最早的 nextAttemptAtMs"与 ackTimeoutMs 的 min 作为下次唤醒间隔——
// backoff 早于 30s 时被精确唤醒，晚于 backoff 是 at-least-once 允许的容差（方向
// 安全）。纯函数（可单测）：nowMs 之后最早需要再次扫描的毫秒数（>=1；无触碰行
// 时按 ackTimeoutMs 轮询）。
inline int64_t computeNextWakeMs(int64_t nowMs, int64_t ackTimeoutMs,
                                 const std::vector<int64_t>& nextAttemptAtMs)
{
    int64_t wakeMs = ackTimeoutMs;
    for (size_t i = 0; i < nextAttemptAtMs.size(); ++i) {
        int64_t remain = nextAttemptAtMs[i] - nowMs;
        if (remain < 1) {
            remain = 1;  // 已到期行按 1ms 最小唤醒（避免零等待自旋）
        }
        if (remain < wakeMs) {
            wakeMs = remain;
        }
    }
    if (wakeMs < 1) {
        wakeMs = 1;
    }
    return wakeMs;
}

// P3 深模块（计划 §3）：网络层只学习 accept/acknowledge/sessionAvailable/sessionClosed，
// 重试、租约、Conversation 分配、幂等与状态机全部在模块内部（P3-07 起
// claim/lease/ACK/背压状态机委托给 DeliveryCoordinator）。
// 同一实现将以 MySQL MessageStore 复用（P3-04），测试只穿过本 interface。
// 线程约束：P3-07 起由单一调用者串行驱动（executor 单 worker）；P3-08 引入
// 内部有界 batch scheduler（单后台线程），接口调用与 scheduler tick 经内部
// 互斥串行化（接口仍满足"同一时刻至多一个调用者在接口上"，scheduler 只做
// 幂等的到期重投/过期/清理扫描）。
class ReliableMessaging {
public:
    ReliableMessaging(MessageStore& store, DeliverySink& sink, Clock& clock, uint64_t leaseMs);
    // P3-08：注入重试/保留参数（测试用）；生产经 AppConfig 注入，默认 = 冻结值。
    ReliableMessaging(MessageStore& store, DeliverySink& sink, Clock& clock, uint64_t leaseMs,
                      const RetryConfig& config);
    ReliableMessaging(const ReliableMessaging&) = delete;
    ReliableMessaging& operator=(const ReliableMessaging&) = delete;
    ~ReliableMessaging();

    // 持久接受：同 (sender, ClientMessageId) 幂等返回原结果；
    // 不同 payload 复用 key → IdempotencyConflict。
    // 存储层故障（并发竞争、依赖忙、cap 超限等）时 accept/acknowledge 可能抛出
    // MessageStoreError（或 store 定义的异常类型）；调用方（P3-06 协议层）负责
    // 映射为 AcceptOutcome 错误结果。
    // P3-07：提交后对在线接收者立即 claim 投递（DeliveryCoordinator::onAccepted）。
    AcceptOutcome accept(const SessionIdentity& sender, const SendMessageCommand& cmd);

    // 接收端按 MessageId 显式确认；ACK 主体只来自 Session，他人 ACK 不越权。
    AckOutcome acknowledge(const SessionIdentity& acker, MessageId messageId);

    // Session 上线：claim 自己名下 Pending Delivery（含租约到期重领）并交给 DeliverySink。
    void sessionAvailable(const SessionIdentity& session);

    // Session 下线：名下 InFlight 立即回 Pending（lease 释放，message-reliability.md §3）。
    void sessionClosed(const SessionIdentity& session);

    // P3-07 背压恢复（内部 seam，非 P3-02 契约一部分）：low-water 回调后解除
    // 暂停并重新 claim（PauseProducer/WouldBlock 不自旋，spec §3 网络联动）。
    void resume(const SessionIdentity& session);

    // P3-09 outbox relay 出口：对 payload 派生接收者触发 coordinator 的幂等接受
    // 通知（与 accept 提交后的在线 claim 等价）。同一 MessageAccepted 事件重放只
    // 产生幂等 wakeup——DeliveryCoordinator claimFor 对已 InFlight/Acknowledged
    // 行 fencing、scheduler 幂等，不重复投递、不新建 Message/Delivery。
    // 存储异常传播给调用方（relay 逐事件 try/catch）：wakeup 失败时事件保持未
    // processed、lease 到期重试——"处理成功才标 processed"与 lost wakeup 恢复成立
    // （不经 accept 路径的 notifyAcceptedBestEffort；见 P3-09 M）。由 relay worker
    // 线程调用，与 executor/scheduler 经 mutex_ 串行化。
    void wakeupAccepted(const std::vector<UserId>& recipients);

    void start();

    // P3-08：有界退出——通知 scheduler 线程后 join（tick 批次有界，drain 有界）；
    // stop 幂等，重复调用直接返回。
    void stop(int64_t deadlineMs);

private:
    // P3-08 内部有界 batch scheduler：单后台线程，timer 驱动（注入 Clock 计算
    // 下一到期时刻），接口调用与 scheduler tick 经 mutex_ 串行化。
    // runTick：到期重投 + 过期转移 + retention cleanup（幂等、批次有界）。
    // 内部 seam（scheduler 与 sessionAvailable 触发），须在持 mutex_ 时调用。
    void schedulerLoop();

    MessageStore& store_;  // accept 直用；claim/ack 委托给 coordinator_（共享同一 store）
    Clock& clock_;
    std::unique_ptr<DeliveryCoordinator> coordinator_;
    RetryConfig config_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::thread schedulerThread_;
    bool running_ = false;        // start() 后 true，stop() 后 false（幂等 gate）
    bool stopRequested_ = false;  // scheduler 退出标志
    int64_t lastCleanupMs_ = 0;   // 上次 retention cleanup 时刻（周期扫描）

public:
    // 内部 seam（非 P3-02 契约）：单轮到期扫描（重投/过期/清理）。接口触发路径
    // （sessionAvailable 等）也驱动一次扫描（幂等）；生产另有 scheduler 定时驱动；
    // 调用方须与其它 RM 调用串行化（内部均持锁）。返回下次唤醒间隔毫秒数
    // （computeNextWakeMs 语义；schedulerLoop 使用，sessionAvailable 触发路径忽略）。
    // 测试经 sessionAvailable 驱动，不直接调用。
    int64_t runTick();
};
