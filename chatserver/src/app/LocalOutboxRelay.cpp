#include "app/LocalOutboxRelay.hpp"

#include "app/ReliableMessageMetrics.hpp"  // P3-12 指标挂点（只计数，BestEffort 不抛）

#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <unistd.h>

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

// 从命令快照派生接收者（与 ReliableMessaging::accept 的 recipientsFor 同构）：
// direct → 目标用户；group → 成员快照（保序）。
std::vector<UserId> recipientsFor(const SendMessageCommand& cmd)
{
    if (cmd.kind == SendMessageCommand::Kind::Direct) {
        return std::vector<UserId>(1, cmd.directRecipient);
    }
    return cmd.members;
}

} // namespace

LocalOutboxRelay::LocalOutboxRelay(MessageStore& store, MessageAcceptedConsumer& consumer,
                                   Clock& clock, const OutboxConfig& config)
    : store_(store), consumer_(consumer), clock_(clock), config_(config), leaseOwner_(nextLeaseOwner())
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
    for (size_t i = 0; i < claimed.size(); ++i) {
        const OutboxEvent& e = claimed[i];
        try {
            const std::shared_ptr<const Message> msg = store_.findMessage(e.aggregateMessageId);
            if (!msg) {
                // 存储一致性防御：缺 Message 的事件不可重放 → 判 poison（可查询）。
                throw std::runtime_error("outbox event without message");
            }
            consumer_.onAccepted(e, recipientsFor(msg->command));
            // 处理成功（wakeup 完成、无异常）后才标 processed。
            store_.markOutboxProcessed(e.id, clock_.nowMs());
        } catch (...) {
            // 处理失败（含 wakeup 传播的瞬时存储故障）不静默丢弃：事件保持未
            // processed、可查询（poison 谓词 = processed_at IS NULL），lease 保留
            // 由到期驱动重领重试；不阻断同批后续事件。relay 不判 poison（异常类型
            // 无法区分坏 payload 与瞬时存储故障；见 P3-09 M 的 L 登记）——统一仅不
            // 标 processed，瞬时故障靠 lease 到期自愈、真 poison 持续可查询。
            // P3-12：处理失败事件逐条计 poison（含瞬时故障——best-effort 可观测，
            // 精确区分属后续故障矩阵迭代）。
            ReliableMessageMetrics::recordBestEffort([&] {
                ReliableMessageMetrics::instance().recordOutboxPoison();
            });
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
