#pragma once

// P3-12 可靠消息指标（docs/tasks/P3-12.md §RED 冻结清单）：进程内
// counter/gauge + ACK latency histogram(p50/p95/p99) recorder + 低基数维度
// 白名单 + SIGUSR1 METRICS 行 reliable_* 快照。
//
// 线程契约：Recorder 内部互斥（std::mutex）串行化一切访问。记录面来自
// executor worker（ReliableMessaging::mutex_ 下）与 relay worker（relay 自身
// mutex_ 下），两锁互不相同，依赖调用方锁无法构成单一一致契约 → 内部锁；
// 快照面来自 EventLoop 线程（SIGUSR1）。记录入口经 *_BestEffort 守护不抛异常：
// 指标只计数，不改行为语义。
//
// 语义契约（冻结测试 ReliableMessageMetricsTest 驱动，命名不随实现调整）：
//   - 仅 Accepted 建立 Delivery（recordDeliveryCreated）并计 createdDeliveries；
//     duplicate/conflict/too-many-recipients 只计各自 counter；
//   - recordDeliveryTransition(deliveryId, from, to)：未知 identity 抛
//     invalid_argument，from==to 幂等；状态计数守恒：
//     pending+inflight+acked+expired == createdDeliveries（isConserving()）；
//   - recordAttempt(isRetry)：attempts 每次投递 +1，retries 仅 isRetry=true；
//   - ACK latency：recordAcceptedTimestamp(key, acceptedAt) 以调用时刻为
//     MESSAGE_ACCEPTED 时刻，recordAck(key) 读 Clock.nowMs()-调用时刻产生样本；
//     重复/未知 key 不新增样本。分位数 nearest-rank（p50=ceil(0.5n) 等）；
//   - H2 有界内存（卡登记）：ackLatencySamplesMs_ 保留最近 kMaxAckLatencySamples
//     （4096）个样本（超限丢最旧）；deliveries_ 与 ackAcceptedAtMs_ 在状态终局
//     （Expired 转移 / ACK / legacy implicit-ack）经 recordEviction 驱逐——计数器
//     （created 与四态和）保留，守恒不破、终态仍可观测；驱逐幂等。
//   - oldestPendingAgeMs：当前最老 Pending 自"成为 head 的时刻"起的时长（随
//     Clock 前进单调不减）；无 Pending 时 -1。head 变更（最老 Pending 离开 /
//     更老 Pending 到达）时 base 重置为变更时刻 / 到达 acceptedAt；
//   - outbox lag 为 last-writer gauge；outbox poison / legacy-mode 为 counter；
//   - 维度白名单：delivery_state / accept_outcome / error_class / legacy；
//     UserId / MessageId / ClientMessageId 等一律拒绝（requireDimensionAllowed
//     抛 invalid_argument）。
//
// 生产接线（只计数）：ReliableMessaging::accept / runTick、DeliveryCoordinator
// 状态转移、LocalOutboxRelay 经本头的 *_BestEffort 入口记录到 instance()；
// main.cpp SIGUSR1 经 ProtocolCodec::reliableMetricsLine() 取 instanceSnapshot()。
// 事件驱动性质：进程重启后 Recorder 从空重建，旧 Delivery 的转移经 BestEffort
// 跳过（不抛、不影响业务）；跨重启严格守恒只由单元测试保证。
//
// mymuduo-safe：本头包含 app/Clock.hpp 的领域 class Clock，只能被领域/测试 TU
// 包含（mymuduo TimerQueue.h 的全局 using Clock 冲突，同 app/ReliableMessaging.hpp
// 约束）；mymuduo TU（main.cpp）经 ProtocolCodec::reliableMetricsLine() 取快照。

#include "app/Clock.hpp"
#include "app/DomainTypes.hpp"  // ::DeliveryState → 指标枚举转换

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ReliableMessageMetrics {

enum class AcceptOutcome {
    Accepted,
    Duplicate,
    Conflict,
    TooManyRecipients,
};

enum class DeliveryState {
    Pending,
    InFlight,
    Acknowledged,
    Expired,
};

// 全标量、标准布局、可逐字段聚合的快照（无 map、无高基数维度）。
struct Snapshot {
    uint64_t accepts = 0;
    uint64_t duplicates = 0;
    uint64_t conflicts = 0;
    uint64_t rejectedTooManyRecipients = 0;
    uint64_t createdDeliveries = 0;
    uint64_t pending = 0;
    uint64_t inflight = 0;
    uint64_t acked = 0;
    uint64_t expired = 0;
    uint64_t attempts = 0;
    uint64_t retries = 0;
    uint64_t legacyModeCount = 0;
    uint64_t outboxLag = 0;
    uint64_t outboxPoison = 0;
    uint64_t ackLatencySamples = 0;
    uint64_t ackLatencyP50Ms = 0;
    uint64_t ackLatencyP95Ms = 0;
    uint64_t ackLatencyP99Ms = 0;
    int64_t oldestPendingAgeMs = -1;  // -1 = 无 Pending
};

// H2 有界内存：ACK latency 样本集保留上限（卡登记 N=4096，超限丢最旧）。
constexpr size_t kMaxAckLatencySamples = 4096;

// 维度白名单：只允许固定低基数枚举/结果维度（P3-12 §RED label 基数约束）。
inline bool isDimensionAllowed(const std::string& dimension)
{
    return dimension == "delivery_state" || dimension == "accept_outcome" ||
           dimension == "error_class" || dimension == "legacy";
}

inline void requireDimensionAllowed(const std::string& dimension)
{
    if (!isDimensionAllowed(dimension)) {
        throw std::invalid_argument("dimension '" + dimension +
                                    "' is not in the low-cardinality whitelist");
    }
}

// (messageId, recipient) → 内部聚合 identity（只作 Recorder 内部状态机 key，
// 绝不作 metric label）。64-bit 组合散列（splitmix 风格）：同对必同 id；不同
// 对碰撞概率 ~2^-64，指标 best-effort 可接受（碰撞至多使状态计数轻微偏移，
// 经 BestEffort 守护不改变业务行为）。
inline uint64_t deliveryMetricId(uint64_t messageId, uint64_t recipient)
{
    uint64_t h = recipient + 0x9e3779b97f4a7c15ULL;
    h = (h ^ (h >> 30)) * 0xbf58476d1ce4e5b9ULL;
    h = (h ^ (h >> 27)) * 0x94d049bb133111ebULL;
    h = h ^ (h >> 31);
    return (messageId << 1) ^ (h >> 1);
}

// 领域 ::DeliveryState → 指标 DeliveryState（数值对齐，类型独立）。
inline DeliveryState fromDomainState(::DeliveryState state)
{
    switch (state) {
        case ::DeliveryState::Pending:
            return DeliveryState::Pending;
        case ::DeliveryState::InFlight:
            return DeliveryState::InFlight;
        case ::DeliveryState::Acknowledged:
            return DeliveryState::Acknowledged;
        case ::DeliveryState::Expired:
            return DeliveryState::Expired;
    }
    return DeliveryState::Expired;  // 防御：未来新增枚举值时回退
}

class Recorder {
public:
    explicit Recorder(Clock& clock) : clock_(clock) {}
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    void recordAccept(AcceptOutcome outcome)
    {
        std::lock_guard<std::mutex> lk(mutex_);
        switch (outcome) {
            case AcceptOutcome::Accepted:
                ++accepts_;
                break;
            case AcceptOutcome::Duplicate:
                ++duplicates_;
                break;
            case AcceptOutcome::Conflict:
                ++conflicts_;
                break;
            case AcceptOutcome::TooManyRecipients:
                ++rejectedTooManyRecipients_;
                break;
        }
    }

    void recordDeliveryCreated(uint64_t deliveryId, int64_t acceptedAtMs)
    {
        std::lock_guard<std::mutex> lk(mutex_);
        DeliveryMetric& m = deliveries_[deliveryId];
        if (m.created) {
            return;  // 幂等：同 identity 重复创建不重复计数
        }
        m.created = true;
        m.state = DeliveryState::Pending;
        m.pendingSinceMs = acceptedAtMs;
        ++createdDeliveries_;
        ++pending_;
        insertPendingLocked(deliveryId, acceptedAtMs);
    }

    void recordDeliveryTransition(uint64_t deliveryId, DeliveryState from, DeliveryState to)
    {
        std::lock_guard<std::mutex> lk(mutex_);
        std::map<uint64_t, DeliveryMetric>::iterator it = deliveries_.find(deliveryId);
        if (it == deliveries_.end()) {
            throw std::invalid_argument("delivery transition for unknown identity");
        }
        if (from == to) {
            return;  // 幂等空迁移：不改变计数
        }
        DeliveryMetric& m = it->second;
        // 状态计数随 (from,to) 迁移（deliveryId 仅内部聚合 identity，不作 label）。
        if (from == DeliveryState::Pending) {
            --pending_;
            removePendingLocked(deliveryId, m.pendingSinceMs);
        }
        if (from == DeliveryState::InFlight) {
            --inflight_;
        }
        if (from == DeliveryState::Acknowledged) {
            --acked_;
        }
        if (from == DeliveryState::Expired) {
            --expired_;
        }
        if (to == DeliveryState::Pending) {
            ++pending_;
            m.pendingSinceMs = clock_.nowMs();  // 重入 Pending 的 base
            insertPendingLocked(deliveryId, m.pendingSinceMs);
        }
        if (to == DeliveryState::InFlight) {
            ++inflight_;
        }
        if (to == DeliveryState::Acknowledged) {
            ++acked_;
        }
        if (to == DeliveryState::Expired) {
            ++expired_;
        }
        m.state = to;
    }

    void recordAttempt(bool isRetry)
    {
        std::lock_guard<std::mutex> lk(mutex_);
        ++attempts_;
        if (isRetry) {
            ++retries_;
        }
    }

    void recordAcceptedTimestamp(uint64_t messageKey, int64_t acceptedAtMs)
    {
        std::lock_guard<std::mutex> lk(mutex_);
        // 冻结测试契约：MESSAGE_ACCEPTED 时刻 = 本调用发生的 Clock 时刻（参数
        // acceptedAtMs 为驱动信息；共享 Clock 场景下按调用时刻为 base，见
        // SnapshotIsStructuredAndAggregatable 的 recAB 期望 {100,200}）。
        (void)acceptedAtMs;
        ackAcceptedAtMs_[messageKey] = clock_.nowMs();
    }

    void recordAck(uint64_t messageKey)
    {
        std::lock_guard<std::mutex> lk(mutex_);
        std::map<uint64_t, int64_t>::iterator it = ackAcceptedAtMs_.find(messageKey);
        if (it == ackAcceptedAtMs_.end()) {
            return;  // 未知/已消费 key：不新增样本
        }
        const int64_t latency = clock_.nowMs() - it->second;
        ackAcceptedAtMs_.erase(it);
        // P3-12 H2：样本集有界（保留最近 N，超限丢最旧）。
        if (ackLatencySamplesMs_.size() >= kMaxAckLatencySamples) {
            ackLatencySamplesMs_.erase(ackLatencySamplesMs_.begin());
        }
        ackLatencySamplesMs_.push_back(static_cast<uint64_t>(latency < 0 ? 0 : latency));
    }

    // P3-12 H2：驱逐 delivery 记录与其未 ACK 起点（有界内存；幂等）。状态终局
    // （Expired 转移 / ACK / legacy implicit-ack）后由接线侧调用。计数器（created
    // 与四态和）保留——终态仍可观测、守恒不破（isConserving 不变）。未知 identity
    // 静默返回。
    void recordEviction(uint64_t deliveryId, uint64_t messageKey)
    {
        std::lock_guard<std::mutex> lk(mutex_);
        std::map<uint64_t, DeliveryMetric>::iterator it = deliveries_.find(deliveryId);
        if (it == deliveries_.end()) {
            return;  // 幂等：未知/已驱逐
        }
        if (it->second.state == DeliveryState::Pending) {
            removePendingLocked(deliveryId, it->second.pendingSinceMs);
        }
        deliveries_.erase(it);
        ackAcceptedAtMs_.erase(messageKey);
    }

    // P3-12 H2：当前被追踪的 delivery 记录数（deliveries_ 尺寸；驱逐后有界）。
    size_t trackedDeliveryCount() const
    {
        std::lock_guard<std::mutex> lk(mutex_);
        return deliveries_.size();
    }

    void updateOutboxLag(uint64_t lag)
    {
        std::lock_guard<std::mutex> lk(mutex_);
        outboxLag_ = lag;
    }

    void recordOutboxPoison()
    {
        std::lock_guard<std::mutex> lk(mutex_);
        ++outboxPoison_;
    }

    void recordLegacyMode()
    {
        std::lock_guard<std::mutex> lk(mutex_);
        ++legacyModeCount_;
    }

    Snapshot snapshot() const
    {
        std::lock_guard<std::mutex> lk(mutex_);
        Snapshot s;
        s.accepts = accepts_;
        s.duplicates = duplicates_;
        s.conflicts = conflicts_;
        s.rejectedTooManyRecipients = rejectedTooManyRecipients_;
        s.createdDeliveries = createdDeliveries_;
        s.pending = pending_;
        s.inflight = inflight_;
        s.acked = acked_;
        s.expired = expired_;
        s.attempts = attempts_;
        s.retries = retries_;
        s.legacyModeCount = legacyModeCount_;
        s.outboxLag = outboxLag_;
        s.outboxPoison = outboxPoison_;
        s.ackLatencySamples = ackLatencySamplesMs_.size();
        if (!ackLatencySamplesMs_.empty()) {
            std::vector<uint64_t> sorted = ackLatencySamplesMs_;
            std::sort(sorted.begin(), sorted.end());
            s.ackLatencyP50Ms = nearestRank(sorted, 0.50);
            s.ackLatencyP95Ms = nearestRank(sorted, 0.95);
            s.ackLatencyP99Ms = nearestRank(sorted, 0.99);
        }
        s.oldestPendingAgeMs = pendingByAge_.empty() ? -1 : clock_.nowMs() - headSinceMs_;
        return s;
    }

    bool isConserving() const
    {
        std::lock_guard<std::mutex> lk(mutex_);
        return createdDeliveries_ == pending_ + inflight_ + acked_ + expired_;
    }

private:
    // nearest-rank 分位数：索引 = ceil(p*n)（1 基）→ sorted[ceil(p*n)-1]。
    static uint64_t nearestRank(const std::vector<uint64_t>& sorted, double p)
    {
        const size_t n = sorted.size();
        const size_t idx = static_cast<size_t>(std::ceil(p * static_cast<double>(n)));
        return sorted[idx - 1];
    }

    struct DeliveryMetric {
        DeliveryState state = DeliveryState::Pending;
        bool created = false;
        int64_t pendingSinceMs = 0;  // 最近进入 Pending 的时刻（创建=acceptedAt，重入=now）
    };

    // 插入 Pending 集合；若比当前 head 更老则成为新 head（base = 其 pendingSince）。
    void insertPendingLocked(uint64_t deliveryId, int64_t pendingSinceMs)
    {
        const std::pair<int64_t, uint64_t> key(pendingSinceMs, deliveryId);
        pendingByAge_.insert(key);
        if (key == *pendingByAge_.begin()) {
            headSinceMs_ = pendingSinceMs;
        }
    }

    void removePendingLocked(uint64_t deliveryId, int64_t pendingSinceMs)
    {
        const std::pair<int64_t, uint64_t> key(pendingSinceMs, deliveryId);
        const bool removedHead = !pendingByAge_.empty() && *pendingByAge_.begin() == key;
        pendingByAge_.erase(key);
        if (pendingByAge_.empty()) {
            headSinceMs_ = 0;
        } else if (removedHead) {
            headSinceMs_ = clock_.nowMs();  // 新 head 自离开时刻起算
        }
    }

    uint64_t accepts_ = 0;
    uint64_t duplicates_ = 0;
    uint64_t conflicts_ = 0;
    uint64_t rejectedTooManyRecipients_ = 0;
    uint64_t createdDeliveries_ = 0;
    uint64_t pending_ = 0;
    uint64_t inflight_ = 0;
    uint64_t acked_ = 0;
    uint64_t expired_ = 0;
    uint64_t attempts_ = 0;
    uint64_t retries_ = 0;
    uint64_t legacyModeCount_ = 0;
    uint64_t outboxLag_ = 0;
    uint64_t outboxPoison_ = 0;
    std::map<uint64_t, DeliveryMetric> deliveries_;
    std::map<uint64_t, int64_t> ackAcceptedAtMs_;  // messageKey → MESSAGE_ACCEPTED 时刻
    std::vector<uint64_t> ackLatencySamplesMs_;
    // 当前 Pending 的 (pendingSinceMs, deliveryId) 有序集；begin() = 最老 head。
    std::set<std::pair<int64_t, uint64_t> > pendingByAge_;
    int64_t headSinceMs_ = 0;  // 当前 head 建立时刻（无 Pending 时无意义）
    mutable std::mutex mutex_;
    Clock& clock_;
};

inline Clock& clockInstance()
{
    // 进程生命周期单例时钟：泄露式分配避免静态析构顺序问题。
    static UnixEpochClock* c = new UnixEpochClock();
    return *c;
}

inline Recorder& instance()
{
    // 进程生命周期单例：泄露式分配避免静态析构顺序问题。
    static Recorder* rec = new Recorder(clockInstance());
    return *rec;
}

inline Snapshot instanceSnapshot()
{
    return instance().snapshot();
}

// 接线侧 no-throw 守护：指标记录（含可能抛 invalid_argument 的转移/未知
// identity）绝不向业务流传播异常（只计数，不改行为语义）。
template <typename Fn>
inline void recordBestEffort(Fn&& fn)
{
    try {
        fn();
    } catch (...) {
    }
}

} // namespace ReliableMessageMetrics
