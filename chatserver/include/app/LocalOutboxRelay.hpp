#pragma once

#include "app/Clock.hpp"
#include "app/Config.hpp"  // OutboxConfig（P3-09 冻结参数，生产默认=卡冻结值）
#include "app/MessageStore.hpp"

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// P3-09 本地 outbox relay：单 worker 线程 + 周期扫描（scanIntervalMs），有界
// 批量 claim 未处理 OutboxEvent（claimBatchSize/claimLeaseMs），对 payload 派生
// 接收者触发幂等 wakeup（生产接 ReliableMessaging::wakeupAccepted），处理成功才
// 标 processed，失败/崩溃由 lease 到期重领。周期扫描保证 accept 提交后 wakeup
// 丢失（进程重启 / best-effort 通知丢失）仍推进。
//
// 线程约束：与 ReliableMessaging 同构——内部 mutex_ 串行化接口调用与 worker tick
// （claim 的原子性由 store 保证：MySQL 单条 UPDATE…LIMIT；InMemory 锁内完成）。
// stop 有界 drain/cancel（单轮批次有界 → join 有界），幂等。

// MessageAccepted 事件的消费出口 port：relay 只负责重放；同一事件重放必须幂等
// （消费方对已 InFlight/Acknowledged Delivery fencing）。
class MessageAcceptedConsumer {
public:
    virtual ~MessageAcceptedConsumer() = default;

    // event 为已 claim 的事件；recipients 为该事件 payload 派生出的接收者
    // （direct → {directRecipient}，group → 成员快照）。throw = 该事件处理失败
    // （含 wakeup 传播的瞬时存储故障）：保持未 processed、可查询、lease 到期重试
    // （不静默丢弃），且不阻断同批后续事件（relay 不判 poison，见 P3-09 M）。
    virtual void onAccepted(const OutboxEvent& event, const std::vector<UserId>& recipients) = 0;
};

class LocalOutboxRelay {
public:
    LocalOutboxRelay(MessageStore& store, MessageAcceptedConsumer& consumer, Clock& clock,
                     const OutboxConfig& config);
    LocalOutboxRelay(const LocalOutboxRelay&) = delete;
    LocalOutboxRelay& operator=(const LocalOutboxRelay&) = delete;
    ~LocalOutboxRelay();

    // 启动单 worker 周期扫描（幂等；timer 驱动，注入 Clock 推进通知 + 扫描间隔，
    // 不依赖固定 sleep）。
    void start();

    // 有界退出：通知 worker 后 join（单轮批次有界 → join 有界）；幂等，重复调用
    // 直接返回；stop 后公开 seam runScan 仍可用。
    void stop(int64_t deadlineMs);

    // 单轮扫描：claim 一批未处理/到期事件并逐个消费；返回本轮 claim 的事件数
    // （= claim 数，含判 poison 的事件，batch 封顶）。线程安全（内部锁）。
    int runScan();

private:
    int scanLocked();  // 须在持 mutex_ 时调用；返回本轮 claim 数
    void workerLoop();

    MessageStore& store_;
    MessageAcceptedConsumer& consumer_;
    Clock& clock_;
    OutboxConfig config_;
    std::string leaseOwner_;  // 本实例唯一 lease owner（并发 relay 竞争标识）
    std::mutex mutex_;
    std::condition_variable cv_;
    std::thread workerThread_;
    bool running_ = false;
    bool stopRequested_ = false;
};
