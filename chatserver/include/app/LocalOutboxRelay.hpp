#pragma once

#include "app/Clock.hpp"
#include "app/Config.hpp"  // OutboxConfig（P3-09 冻结参数，生产默认=卡冻结值）
#include "app/MessageStore.hpp"
#include "app/OutboxPublisher.hpp"

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// P3-09 本地 outbox relay：单 worker 线程 + 周期扫描（scanIntervalMs），有界
// 批量 claim 未处理 OutboxEvent（claimBatchSize/claimLeaseMs），经 OutboxPublisher
// port 发布 MessageAccepted 事件（P4-03 起出口从"消费即本地 wakeup"替换为 port，
// 本地 wakeup 语义由 LocalWakeupPublisher adapter 承接，P3-09 语义原样），逐事件
// 结果 ok 才标 processed，失败/崩溃由 lease 到期重领。周期扫描保证 accept 提交后
// wakeup 丢失（进程重启 / best-effort 通知丢失）仍推进。
//
// 线程约束：与 ReliableMessaging 同构——内部 mutex_ 串行化接口调用与 worker tick
// （claim 的原子性由 store 保证：MySQL 单条 UPDATE…LIMIT；InMemory 锁内完成）。
// stop 有界 drain/cancel（单轮批次有界 → join 有界），幂等。

class LocalOutboxRelay {
public:
    // topic：publish 请求携带的目标 topic（生产默认 muduo-outbox，P4-03 冻结命名；
    // LocalWakeupPublisher 忽略之）。publishDeadlineMs：publisher.publish 的整体
    // 软期限（P4-03 冻结 broker timeout 5000ms）。
    LocalOutboxRelay(MessageStore& store, OutboxPublisher& publisher, Clock& clock,
                     const OutboxConfig& config, const std::string& topic = "muduo-outbox",
                     int64_t publishDeadlineMs = 5000);
    LocalOutboxRelay(const LocalOutboxRelay&) = delete;
    LocalOutboxRelay& operator=(const LocalOutboxRelay&) = delete;
    ~LocalOutboxRelay();

    // 启动单 worker 周期扫描（幂等；timer 驱动，注入 Clock 推进通知 + 扫描间隔，
    // 不依赖固定 sleep）。
    void start();

    // 有界退出：通知 worker 后 join（单轮批次有界 → join 有界）；幂等，重复调用
    // 直接返回；stop 后公开 seam runScan 仍可用。
    void stop(int64_t deadlineMs);

    // 单轮扫描：claim 一批未处理/到期事件并逐个发布；返回本轮 claim 的事件数
    // （= claim 数，含判 poison 的事件，batch 封顶）。线程安全（内部锁）。
    int runScan();

private:
    int scanLocked();  // 须在持 mutex_ 时调用；返回本轮 claim 数
    void workerLoop();

    MessageStore& store_;
    OutboxPublisher& publisher_;
    Clock& clock_;
    OutboxConfig config_;
    std::string topic_;
    int64_t publishDeadlineMs_;
    std::string leaseOwner_;  // 本实例唯一 lease owner（并发 relay 竞争标识）
    std::mutex mutex_;
    std::condition_variable cv_;
    std::thread workerThread_;
    bool running_ = false;
    bool stopRequested_ = false;
};
