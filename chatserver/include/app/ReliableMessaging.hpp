#pragma once

#include "app/DomainTypes.hpp"

#include <memory>

// P3-02 可靠消息模块：领域值类型见 app/DomainTypes.hpp（本头保持 mymuduo-safe：
// 只前向声明 class Clock——mymuduo TimerQueue.h 的全局 `using Clock` 与领域
// class Clock 同名冲突，完整定义在 app/Clock.hpp，仅供领域侧 TU 引入）。
// raw JSON 不进领域类型；codec/session 绑定在应用层（P3-05/P3-06）完成。

// 模块内部确定性测试 seam；定义见 app/Clock.hpp。
class Clock;

class MessageStore;
class DeliverySink;
class DeliveryCoordinator;

// P3 深模块（计划 §3）：网络层只学习 accept/acknowledge/sessionAvailable/sessionClosed，
// 重试、租约、Conversation 分配、幂等与状态机全部在模块内部（P3-07 起
// claim/lease/ACK/背压状态机委托给 DeliveryCoordinator）。
// 同一实现将以 MySQL MessageStore 复用（P3-04），测试只穿过本 interface。
// 线程约束：实例须由单一调用者串行驱动（P3-05 接入后为 SessionSerialExecutor 或等价串行执行器）；
// 接口内部非原子（读-改-写），并发调用未定义行为。
class ReliableMessaging {
public:
    ReliableMessaging(MessageStore& store, DeliverySink& sink, Clock& clock, uint64_t leaseMs);
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

    void start();

    // in-memory 无后台任务，stop 为空操作；P3-08 引入 timer 后有界退出。
    void stop(int64_t deadlineMs);

private:
    MessageStore& store_;  // accept 直用；claim/ack 委托给 coordinator_（共享同一 store）
    std::unique_ptr<DeliveryCoordinator> coordinator_;
};
