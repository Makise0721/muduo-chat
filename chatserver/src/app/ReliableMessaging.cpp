#include "app/ReliableMessaging.hpp"

#include "app/DeliveryCoordinator.hpp"
#include "app/DeliverySink.hpp"
#include "app/MessageStore.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <vector>
#include <unistd.h>

namespace {

// P3-08 进程实例标识（boot id）：pid ^ steady-clock 时刻 ^ 每进程构造计数。
// 同进程多次构造必不同（计数），跨进程/重启碰撞概率可忽略（pid/时刻）。持久
// lease owner 携带它，使跨进程重启后 owner 必不同（见 DomainTypes.hpp
// leaseBootId / DeliveryCoordinator.hpp sessionAvailable 的 fencing 判定）。
uint64_t nextBootId()
{
    static std::atomic<uint64_t> counter{0};
    const uint64_t c = counter.fetch_add(1) & 0xFFF;
    const uint64_t t = static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    return (static_cast<uint64_t>(getpid()) << 52) ^ (t & 0xFFFFFFFFFFFFF) ^ (c << 60);
}

std::vector<UserId> recipientsFor(const SendMessageCommand& cmd)
{
    if (cmd.kind == SendMessageCommand::Kind::Direct) {
        return std::vector<UserId>(1, cmd.directRecipient);
    }
    return cmd.members;
}

// Durable acceptance is already complete before this callback is entered.
// Delivery state persistence is therefore best-effort here: a later retry of
// the same idempotency key re-runs the claim for any still-Pending rows.
void notifyAcceptedBestEffort(DeliveryCoordinator& coordinator,
                              const std::vector<UserId>& recipients)
{
    try {
        coordinator.onAccepted(recipients);
    } catch (...) {
        // Do not turn a committed Message/Delivery set into DependencyBusy.
    }
}

// 到期重投/过期/清理均为幂等 housekeeping：单次存储故障不得把 accept/claim/
// scheduler 生命周期变成硬失败（scheduler 线程异常 = std::terminate）。
// 返回 runTick 计算的下次唤醒间隔；异常时返回 -1，由调用方按 ackTimeoutMs 兜底。
int64_t runTickBestEffort(ReliableMessaging& rm)
{
    try {
        return rm.runTick();
    } catch (...) {
        // best-effort：下一 tick（接口触发或 scheduler 定时）再试。
        return -1;
    }
}

} // namespace

ReliableMessaging::ReliableMessaging(MessageStore& store, DeliverySink& sink, Clock& clock,
                                     uint64_t leaseMs)
    : ReliableMessaging(store, sink, clock, leaseMs, RetryConfig())
{
}

ReliableMessaging::ReliableMessaging(MessageStore& store, DeliverySink& sink, Clock& clock,
                                     uint64_t leaseMs, const RetryConfig& config)
    : store_(store), clock_(clock),
      coordinator_(new DeliveryCoordinator(store, sink, clock, leaseMs, config, nextBootId())),
      config_(config)
{
}

ReliableMessaging::~ReliableMessaging()
{
    stop(0);
}

AcceptOutcome ReliableMessaging::accept(const SessionIdentity& sender, const SendMessageCommand& cmd)
{
    std::lock_guard<std::mutex> lk(mutex_);
    AcceptOutcome outcome;
    std::shared_ptr<const Message> existing = store_.findAccepted(cmd.clientMessageId, sender.userId);
    if (existing) {
        if (samePayload(existing->command, cmd)) {
            outcome.ok = true;
            outcome.duplicate = true;
            outcome.messageId = existing->id;
            outcome.conversationId = existing->conversationId;
            outcome.sequence = existing->sequence;
            // A prior post-commit delivery update may have failed after the
            // durable accept.  Re-run the online claim on a same-key retry;
            // claimFor fences already InFlight/Acknowledged rows.
            notifyAcceptedBestEffort(*coordinator_, recipientsFor(existing->command));
        } else {
            outcome.error = AcceptError::IdempotencyConflict;
        }
        return outcome;
    }

    Message draft;
    draft.senderId = sender.userId;
    draft.command = cmd;
    draft.conversationId = store_.getOrCreateConversation(sender, cmd);
    const Message accepted = store_.insertMessage(draft);

    const std::vector<UserId> recipients = recipientsFor(cmd);
    const int64_t expiresAtMs = clock_.nowMs() + config_.messageRetentionMs;
    for (size_t i = 0; i < recipients.size(); ++i) {
        Delivery delivery;
        delivery.messageId = accepted.id;
        delivery.conversationId = accepted.conversationId;
        delivery.recipient = recipients[i];
        delivery.expiresAtMs = expiresAtMs;  // P3-08：retention deadline（spec §4 过期行）
        store_.insertDelivery(delivery);
    }

    // P3-07：对在线接收者立即 claim（在线投递；离线保持 Pending 待登录 claim）。
    notifyAcceptedBestEffort(*coordinator_, recipients);

    outcome.ok = true;
    outcome.messageId = accepted.id;
    outcome.conversationId = accepted.conversationId;
    outcome.sequence = accepted.sequence;
    return outcome;
}

AckOutcome ReliableMessaging::acknowledge(const SessionIdentity& acker, MessageId messageId)
{
    std::lock_guard<std::mutex> lk(mutex_);
    return coordinator_->acknowledge(acker, messageId);
}

void ReliableMessaging::sessionAvailable(const SessionIdentity& session)
{
    std::lock_guard<std::mutex> lk(mutex_);
    coordinator_->sessionAvailable(session);
    // P3-08：sessionAvailable 也驱动一次到期扫描（确定性测试经此推进；
    // 生产由 scheduler 定时驱动，本调用幂等）。
    runTickBestEffort(*this);
}

void ReliableMessaging::sessionClosed(const SessionIdentity& session)
{
    std::lock_guard<std::mutex> lk(mutex_);
    coordinator_->sessionClosed(session);
}

void ReliableMessaging::resume(const SessionIdentity& session)
{
    std::lock_guard<std::mutex> lk(mutex_);
    coordinator_->resume(session);
}

int64_t ReliableMessaging::runTick()
{
    const int64_t now = clock_.nowMs();
    // 1) retention deadline：Pending/InFlight → Expired（可查询，不删除）。
    store_.expireDeliveries(now, config_.retryBatchLimit);
    // 2) acked/expired 独立 retention cleanup（周期执行、batch 有界、幂等）。
    if (now - lastCleanupMs_ >= config_.cleanupCycleMs) {
        lastCleanupMs_ = now;
        store_.cleanupDeliveries(now - config_.ackedRetentionMs,
                                 now - config_.expiredRetentionMs, config_.cleanupBatch);
    }
    // 3) 到期重投：只处理活动会话的到期 InFlight（offline 不消耗重试额度）。
    // 返回本 tick 重投行的 nextAttemptAtMs，用于计算下次唤醒间隔（补缺轮 M1：
    // 早于 ack_timeout 的 backoff 可被精确唤醒，ack_timeout 封顶）。
    // F1：MySQL 秒粒度持久化 next_attempt_at（M2 ceil 写 + due 判定 floor(now_sec)
    // >=ceil），runRetryScan 返回的 ms 级 nextAttemptAtMs 须向上对齐到
    // store.timeGranularityMs() 再交 computeNextWakeMs——否则 scheduler 按 ms 级
    // nextAttemptAtMs 唤醒时，行在 ceil 秒边界前 1..999ms 未到期，runRetryScan 空、
    // 回退 ackTimeoutMs 轮询（MySQL 上有效 ack_timeout 翻倍）。InMemory 粒度 1
    // 无操作（既有测试零影响）。
    const std::vector<int64_t> nextAttempts = coordinator_->runRetryScan(now);
    std::vector<int64_t> alignedNextAttempts;
    alignedNextAttempts.reserve(nextAttempts.size());
    const uint32_t granularityMs = store_.timeGranularityMs();
    for (size_t i = 0; i < nextAttempts.size(); ++i) {
        const int64_t g = static_cast<int64_t>(granularityMs);
        alignedNextAttempts.push_back(((nextAttempts[i] + g - 1) / g) * g);
    }
    int64_t wake = computeNextWakeMs(now, config_.ackTimeoutMs, alignedNextAttempts);
    // F1：computeNextWakeMs 的 ackTimeoutMs 封顶会把唤醒钉在 now+ack_timeout，对粗粒度
    // store（MySQL 秒）恰落在持久化 ceil 到期边界前 1..999ms——runRetryScan 空、回退整轮
    // ack_timeout（有效 ack_timeout 翻倍）。本 tick 触碰行的对齐到期边界若在
    // (ack_timeout, ack_timeout+granularity) 内（纯持久化 ceil 阴影，非真实超越 ack_timeout
    // 的 backoff），改按该边界唤醒；InMemory（粒度 1）与真实长 backoff 的封顶语义不变。
    if (granularityMs > 1 && !alignedNextAttempts.empty()) {
        int64_t earliest = alignedNextAttempts[0];
        for (size_t i = 1; i < alignedNextAttempts.size(); ++i) {
            if (alignedNextAttempts[i] < earliest) {
                earliest = alignedNextAttempts[i];
            }
        }
        const int64_t remain = earliest - now;
        const int64_t ceilCap = static_cast<int64_t>(config_.ackTimeoutMs) +
                                static_cast<int64_t>(granularityMs) - 1;
        if (remain >= 1 && remain <= ceilCap) {
            wake = remain;
        }
    }
    return wake;
}

void ReliableMessaging::start()
{
    std::lock_guard<std::mutex> lk(mutex_);
    if (running_) {
        return;
    }
    running_ = true;
    stopRequested_ = false;
    clock_.registerAdvanceNotifier([this] { cv_.notify_all(); });
    schedulerThread_ = std::thread([this] { schedulerLoop(); });
}

void ReliableMessaging::stop(int64_t deadlineMs)
{
    (void)deadlineMs;  // 有界 drain：tick 批次有界（retryBatchLimit/cleanupBatch），
                       // join 即有界；deadline 为软提示，无需无界等待。
    std::unique_lock<std::mutex> lock(mutex_);
    if (!running_ && !schedulerThread_.joinable()) {
        return;  // 幂等：未 start 或已 stop 直接返回
    }
    stopRequested_ = true;
    cv_.notify_all();
    lock.unlock();
    if (schedulerThread_.joinable()) {
        schedulerThread_.join();
    }
    lock.lock();
    running_ = false;
    // 解除时钟推进通知：scheduler 已退出，不再需要唤醒（也避免悬挂的
    // `this` 回调在对象销毁后被测试时钟调用）。
    clock_.registerAdvanceNotifier(std::function<void()>());
}

void ReliableMessaging::schedulerLoop()
{
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopRequested_) {
        // 用注入 Clock 计算下一到期时刻：本 tick 触碰行最早的 nextAttemptAtMs 与
        // ack_timeout 的 min（补缺轮 M1；无触碰行时按 ackTimeoutMs 轮询）。
        // runTickBestEffort 异常返回 -1 时按 ackTimeoutMs 兜底。
        int64_t waitMs = runTickBestEffort(*this);
        if (waitMs < 1) {
            waitMs = config_.ackTimeoutMs;
        }
        const int64_t now = clock_.nowMs();
        const int64_t untilCleanup = lastCleanupMs_ + config_.cleanupCycleMs - now;
        if (untilCleanup > 0 && untilCleanup < waitMs) {
            waitMs = untilCleanup;
        }
        if (waitMs < 1) {
            waitMs = 1;
        }
        // 无谓词 wait_for：FakeClock.advance()/接口通知/stop() 都会提前唤醒
        // 重新 tick（幂等），stopRequested_ 在循环顶部检查。
        cv_.wait_for(lock, std::chrono::milliseconds(waitMs));
    }
}
