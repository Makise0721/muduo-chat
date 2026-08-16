#include "app/LocalOutboxRelay.hpp"

#include "app/ReliableMessageMetrics.hpp"  // P3-12 指标挂点（只计数，BestEffort 不抛）

#include <atomic>
#include <chrono>
#include <memory>
#include <unistd.h>
#include <vector>

namespace {

// 本进程内唯一 relay lease owner：进程标识 + 每实例计数。跨进程/多实例经 lease
// 竞争（VARCHAR(64) 足够容纳十进制 "relay:<boot>:<n>"）。
std::string nextLeaseOwner()
{
    static std::atomic<uint64_t> counter{0};
    const uint64_t c = counter.fetch_add(1) & 0xFFF;
    const uint64_t t = static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const uint64_t boot = (static_cast<uint64_t>(getpid()) << 52) ^ (t & 0xFFFFFFFFFFFFF) ^ (c << 60);
    return "relay:" + std::to_string(boot) + ":" + std::to_string(c);
}

} // namespace

LocalOutboxRelay::LocalOutboxRelay(MessageStore& store, OutboxPublisher& publisher, Clock& clock,
                                   const OutboxConfig& config, const std::string& topic,
                                   int64_t publishDeadlineMs)
    : store_(store),
      publisher_(publisher),
      clock_(clock),
      config_(config),
      topic_(topic),
      publishDeadlineMs_(publishDeadlineMs),
      leaseOwner_(nextLeaseOwner())
{
}

LocalOutboxRelay::~LocalOutboxRelay()
{
    stop(0);
}

void LocalOutboxRelay::start()
{
    std::lock_guard<std::mutex> lk(mutex_);
    if (running_) {
        return;
    }
    running_ = true;
    stopRequested_ = false;
    clock_.registerAdvanceNotifier([this] { cv_.notify_all(); });
    workerThread_ = std::thread([this] { workerLoop(); });
}

void LocalOutboxRelay::stop(int64_t deadlineMs)
{
    (void)deadlineMs;  // 有界 drain：单轮批次有界（claimBatchSize），join 即有界；
                       // deadline 为软提示，无需无界等待。
    std::unique_lock<std::mutex> lock(mutex_);
    if (!running_ && !workerThread_.joinable()) {
        return;  // 幂等：未 start 或已 stop 直接返回
    }
    stopRequested_ = true;
    cv_.notify_all();
    lock.unlock();
    if (workerThread_.joinable()) {
        workerThread_.join();
    }
    lock.lock();
    running_ = false;
    // 解除时钟推进通知：worker 已退出，不再需要唤醒（也避免悬挂的 `this` 回调
    // 在对象销毁后被测试时钟调用）。
    clock_.registerAdvanceNotifier(std::function<void()>());
}

int LocalOutboxRelay::runScan()
{
    std::lock_guard<std::mutex> lk(mutex_);
    return scanLocked();
}

int LocalOutboxRelay::scanLocked()
{
    const int64_t now = clock_.nowMs();
    std::vector<OutboxEvent> claimed = store_.claimOutboxEvents(
        now, leaseOwner_, now + config_.claimLeaseMs, config_.claimBatchSize);
    const int claimedCount = static_cast<int>(claimed.size());

    // P4-03：构建 publish 批——每事件 load Message（取 conversationId/sequence，
    // 缺 Message 或载荷损坏（findMessage 抛，如 outbox payload 被外部破坏）判
    // poison，不可 publish）；poison 事件不入批、保持未 processed，且不阻断同批
    // 后续事件（P3-09 逐事件 try/catch 语义原样）。
    std::vector<OutboxPublishRequest> requests;
    requests.reserve(claimed.size());
    for (size_t i = 0; i < claimed.size(); ++i) {
        const OutboxEvent& e = claimed[i];
        std::shared_ptr<const Message> msg;
        try {
            msg = store_.findMessage(e.aggregateMessageId);
        } catch (const std::exception&) {
            // 载荷损坏/瞬时存储故障：统一按 poison 处理（relay 不判 poison 类型，
            // 仅保持未 processed、lease 到期重试，见 P3-09 M）。
            msg.reset();
        }
        if (!msg) {
            // 存储一致性防御：缺 Message / 不可重放的事件 → 判 poison（可查询）。
            ReliableMessageMetrics::recordBestEffort([&] {
                ReliableMessageMetrics::instance().recordOutboxPoison();
            });
            continue;
        }
        OutboxPublishRequest req;
        req.event = e;
        req.conversationId = msg->conversationId;
        req.sequence = msg->sequence.value;
        req.topic = topic_;
        requests.push_back(req);
    }

    if (!requests.empty()) {
        // publisher 契约"不抛"；防御性 catch（如 adapter 违反契约）→ 整批保持未
        // processed，lease 到期重领重试（绝不崩溃、不静默丢弃）。
        std::vector<OutboxPublishOutcome> outcomes;
        try {
            outcomes = publisher_.publish(requests, publishDeadlineMs_);
        } catch (const std::exception& e) {
            ReliableMessageMetrics::recordBestEffort([&] {
                for (size_t i = 0; i < requests.size(); ++i) {
                    ReliableMessageMetrics::instance().recordOutboxPoison();
                }
            });
            outcomes.clear();
            (void)e;
        }
        if (outcomes.size() == requests.size()) {
            for (size_t i = 0; i < requests.size(); ++i) {
                if (outcomes[i].ok) {
                    // 处理成功（publish 成功、无异常）后才标 processed；标 processed
                    // 的瞬时存储故障逐事件 catch（P3-09 语义：不阻断同批后续事件，
                    // 事件保持未 processed、lease 到期重试）。
                    try {
                        store_.markOutboxProcessed(requests[i].event.id, clock_.nowMs());
                    } catch (const std::exception&) {
                        ReliableMessageMetrics::recordBestEffort([&] {
                            ReliableMessageMetrics::instance().recordOutboxPoison();
                        });
                    }
                } else {
                    // 发布失败（含 LocalWakeupPublisher 的 wakeup 存储故障）不标
                    // processed：事件保持未 processed、可查询，lease 保留由到期
                    // 驱动重领重试；不阻断同批后续事件。relay 不判 poison（无法
                    // 区分坏 payload 与瞬时故障；见 P3-09 M）——统一仅不标 processed。
                    // P3-12：失败事件逐条计 poison（best-effort 可观测）。
                    ReliableMessageMetrics::recordBestEffort([&] {
                        ReliableMessageMetrics::instance().recordOutboxPoison();
                    });
                }
            }
        }
    }

    // P3-12：outbox lag gauge = 未 processed 事件数（有界公开查询；查询失败不
    // 阻断 relay 消费——best-effort）。
    ReliableMessageMetrics::recordBestEffort([&] {
        const uint64_t lag = store_.countUnprocessedOutboxEvents();
        ReliableMessageMetrics::instance().updateOutboxLag(lag);
    });
    return claimedCount;
}

void LocalOutboxRelay::workerLoop()
{
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopRequested_) {
        try {
            scanLocked();
        } catch (...) {
            // store 故障（如 MySQL 不可用）best-effort：下一轮再试，不终止线程
            // （scheduler 模式同构；单轮批次有界 → 退出仍有界）。
        }
        int64_t waitMs = config_.scanIntervalMs;
        if (waitMs < 1) {
            waitMs = 1;
        }
        // 无谓词 wait_for：注入 Clock 推进通知 / stop() 都会提前唤醒重新扫描
        // （幂等），stopRequested_ 在循环顶部检查。
        cv_.wait_for(lock, std::chrono::milliseconds(waitMs));
    }
}
