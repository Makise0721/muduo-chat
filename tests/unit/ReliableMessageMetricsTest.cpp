// P3-12 RED：可靠消息指标（docs/tasks/P3-12.md 冻结清单）的测试 recorder。
//
// 本文件 RED 引用尚不存在的公开 metrics 接口 app/ReliableMessageMetrics.hpp：
// 当前 chatserver 无该头 → 编译失败（missing header）即合法 RED。
// GREEN 时按本文件用法精确实现该接口（命名/语义冻结，不随实现调整）。
//
// 冻结指标清单（P3-12 RED 节）：
//   counter：accepts/duplicates/conflicts/rejected-too-many-recipients、
//            attempts/retries、legacy-mode count、outbox poison；
//   gauge/状态计数：pending/inflight/acked/expired（守恒）、outbox lag、
//            oldest pending age；
//   histogram/p50/p95/p99：ACK latency（MESSAGE_ACCEPTED → DELIVERY_ACK）。
//
// 高基数约束（测试必须拒绝）：UserId/MessageId/ClientMessageId 禁止作为 metric
// label，只允许固定低基数维度（delivery_state/accept_outcome/error_class/legacy）。
// 状态计数守恒：pending+inflight+acked+expired == createdDeliveries。
//
// 预期接口（GREEN 目标，本文件按此用法驱动；Identity 仅内部聚合用，不作 label）：
//   namespace ReliableMessageMetrics {
//     enum class AcceptOutcome { Accepted, Duplicate, Conflict, TooManyRecipients };
//     enum class DeliveryState { Pending, InFlight, Acknowledged, Expired };
//     struct Snapshot {   // 全标量、标准布局、可逐字段聚合
//       uint64_t accepts, duplicates, conflicts, rejectedTooManyRecipients;
//       uint64_t createdDeliveries, pending, inflight, acked, expired;
//       uint64_t attempts, retries, legacyModeCount;
//       uint64_t outboxLag, outboxPoison;
//       uint64_t ackLatencySamples, ackLatencyP50Ms, ackLatencyP95Ms, ackLatencyP99Ms;
//       int64_t oldestPendingAgeMs;  // -1 = 无 Pending
//     };
//     bool isDimensionAllowed(const std::string& dimension);
//     void requireDimensionAllowed(const std::string& dimension);  // 非法抛 invalid_argument
//     class Recorder {
//       explicit Recorder(Clock& clock);
//       void recordAccept(AcceptOutcome outcome);
//       void recordDeliveryCreated(uint64_t deliveryId, int64_t acceptedAtMs);
//       void recordDeliveryTransition(uint64_t deliveryId, DeliveryState from, DeliveryState to);
//       void recordAttempt(bool isRetry);
//       void recordAcceptedTimestamp(uint64_t messageKey, int64_t acceptedAtMs);
//       void recordAck(uint64_t messageKey);        // 重复 ACK 不新增样本
//       void updateOutboxLag(uint64_t lag);
//       void recordOutboxPoison();
//       void recordLegacyMode();
//       Snapshot snapshot() const;
//       bool isConserving() const;                  // 四态和 == createdDeliveries
//     };
//     Snapshot instanceSnapshot();                  // SIGUSR1 快照行 reliable_* 字段
//   }
//
// 语义约束（本文件逐测试断言，GREEN 必须满足）：
//   - 仅 Accepted 建立 Delivery；duplicate/conflict/too-many-recipients 只计各自 counter；
//   - attempts 每投递 +1，retries 仅 attempt_count>1（isRetry=true）计入；
//   - ACK latency = 注入 Clock 在 recordAck 时刻读 nowMs 减 MESSAGE_ACCEPTED 时刻；
//   - oldest pending age 随 Clock 前进单调不减，无 Pending 时为 -1；
//   - 快照为标准布局标量结构，counter/gauge 字段可逐字段求和聚合。

#include "app/ReliableMessageMetrics.hpp"  // RED：尚不存在 → 编译失败即合法 RED
#include "FakeClock.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace {

const int64_t kT0 = 1000000;
const int64_t kLease = 1000;

void expectConserving(const ReliableMessageMetrics::Recorder& rec,
                      const char* where)
{
    const ReliableMessageMetrics::Snapshot s = rec.snapshot();
    SCOPED_TRACE(where);
    EXPECT_TRUE(rec.isConserving()) << "state sum must equal createdDeliveries";
    EXPECT_EQ(s.createdDeliveries, s.pending + s.inflight + s.acked + s.expired)
        << "pending+inflight+acked+expired must equal accepted deliveries";
}

} // namespace

TEST(ReliableMessageMetricsTest, AcceptCountsIncrementPerOutcome)
{
    FakeClock clock;
    clock.set(kT0);
    ReliableMessageMetrics::Recorder rec(clock);

    // 同 key 首提：accepts+1；重复：duplicates+1；冲突：conflicts+1；超 fan-out：+too-many。
    rec.recordAccept(ReliableMessageMetrics::AcceptOutcome::Accepted);
    rec.recordAccept(ReliableMessageMetrics::AcceptOutcome::Duplicate);
    rec.recordAccept(ReliableMessageMetrics::AcceptOutcome::Conflict);
    rec.recordAccept(ReliableMessageMetrics::AcceptOutcome::TooManyRecipients);

    const ReliableMessageMetrics::Snapshot s = rec.snapshot();
    EXPECT_EQ(1u, s.accepts);
    EXPECT_EQ(1u, s.duplicates);
    EXPECT_EQ(1u, s.conflicts);
    EXPECT_EQ(1u, s.rejectedTooManyRecipients);

    // 各结果 counter 独立，互不影响。
    rec.recordAccept(ReliableMessageMetrics::AcceptOutcome::Accepted);
    rec.recordAccept(ReliableMessageMetrics::AcceptOutcome::Duplicate);
    const ReliableMessageMetrics::Snapshot s2 = rec.snapshot();
    EXPECT_EQ(2u, s2.accepts);
    EXPECT_EQ(2u, s2.duplicates);
    EXPECT_EQ(1u, s2.conflicts);
    EXPECT_EQ(1u, s2.rejectedTooManyRecipients);

    // duplicate/conflict/too-many-recipients 不建立 Delivery：状态计数不被触碰。
    EXPECT_EQ(0u, s2.createdDeliveries);
    EXPECT_EQ(0u, s2.pending + s2.inflight + s2.acked + s2.expired);
    expectConserving(rec, "after accept outcomes");
}

TEST(ReliableMessageMetricsTest, DeliveryStateCountsConserve)
{
    FakeClock clock;
    clock.set(kT0);
    ReliableMessageMetrics::Recorder rec(clock);

    // 建立 3 条 Delivery（Pending 起点），随后逐状态迁移；每步守恒。
    rec.recordDeliveryCreated(1, kT0);
    rec.recordDeliveryCreated(2, kT0);
    rec.recordDeliveryCreated(3, kT0);
    expectConserving(rec, "created");

    rec.recordDeliveryTransition(1, ReliableMessageMetrics::DeliveryState::Pending,
                                 ReliableMessageMetrics::DeliveryState::InFlight);
    rec.recordDeliveryTransition(2, ReliableMessageMetrics::DeliveryState::Pending,
                                 ReliableMessageMetrics::DeliveryState::InFlight);
    expectConserving(rec, "claim");
    EXPECT_EQ(1u, rec.snapshot().pending);
    EXPECT_EQ(2u, rec.snapshot().inflight);

    rec.recordDeliveryTransition(1, ReliableMessageMetrics::DeliveryState::InFlight,
                                 ReliableMessageMetrics::DeliveryState::Acknowledged);
    rec.recordDeliveryTransition(2, ReliableMessageMetrics::DeliveryState::InFlight,
                                 ReliableMessageMetrics::DeliveryState::Pending);  // 断线回滚
    rec.recordDeliveryTransition(3, ReliableMessageMetrics::DeliveryState::Pending,
                                 ReliableMessageMetrics::DeliveryState::InFlight);
    rec.recordDeliveryTransition(2, ReliableMessageMetrics::DeliveryState::Pending,
                                 ReliableMessageMetrics::DeliveryState::InFlight);
    rec.recordDeliveryTransition(2, ReliableMessageMetrics::DeliveryState::InFlight,
                                 ReliableMessageMetrics::DeliveryState::Expired);  // retention
    expectConserving(rec, "mixed lifecycle");

    const ReliableMessageMetrics::Snapshot s = rec.snapshot();
    EXPECT_EQ(3u, s.createdDeliveries);
    EXPECT_EQ(0u, s.pending);
    EXPECT_EQ(1u, s.inflight);
    EXPECT_EQ(1u, s.acked);
    EXPECT_EQ(1u, s.expired);
    expectConserving(rec, "final");

    // 未知 identity 的迁移、以及 from==to 空迁移：前者拒绝（不产生游离计数），
    // 后者幂等（不改变计数）。
    EXPECT_THROW(rec.recordDeliveryTransition(99, ReliableMessageMetrics::DeliveryState::InFlight,
                                              ReliableMessageMetrics::DeliveryState::Acknowledged),
                 std::invalid_argument);
    rec.recordDeliveryTransition(3, ReliableMessageMetrics::DeliveryState::InFlight,
                                 ReliableMessageMetrics::DeliveryState::InFlight);
    EXPECT_EQ(1u, rec.snapshot().inflight);
    expectConserving(rec, "after guarded transitions");
}

TEST(ReliableMessageMetricsTest, AttemptsAndRetriesIncrement)
{
    FakeClock clock;
    clock.set(kT0);
    ReliableMessageMetrics::Recorder rec(clock);

    // attempts：每次投递 +1（首投与重投都是 attempt）；retries：仅 attempt_count>1 计入。
    rec.recordAttempt(false);  // 首投
    rec.recordAttempt(false);  // 另一条消息首投
    rec.recordAttempt(true);   // 第一条消息重投（attempt#2）
    rec.recordAttempt(true);   // 第二条消息重投（attempt#2）

    const ReliableMessageMetrics::Snapshot s = rec.snapshot();
    EXPECT_EQ(4u, s.attempts);
    EXPECT_EQ(2u, s.retries);
    EXPECT_LE(s.retries, s.attempts);  // retry 是 attempts 的子集
}

TEST(ReliableMessageMetricsTest, AckLatencyRecorded)
{
    FakeClock clock;
    clock.set(kT0);
    ReliableMessageMetrics::Recorder rec(clock);

    // 注入 Clock 确定性：recordAck 时读 nowMs，latency = now - accepted 时刻。
    rec.recordAcceptedTimestamp(11, clock.nowMs());
    clock.advance(100);
    rec.recordAck(11);

    rec.recordAcceptedTimestamp(12, clock.nowMs());
    clock.advance(200);
    rec.recordAck(12);

    rec.recordAcceptedTimestamp(13, clock.nowMs());
    clock.advance(300);
    rec.recordAck(13);

    // 样本 {100, 200, 300}：nearest-rank p50=ceil(0.5*3)=2→200，p95=p99=ceil(0.95/0.99*3)=3→300。
    ReliableMessageMetrics::Snapshot s = rec.snapshot();
    EXPECT_EQ(3u, s.ackLatencySamples);
    EXPECT_EQ(200u, s.ackLatencyP50Ms);
    EXPECT_EQ(300u, s.ackLatencyP95Ms);
    EXPECT_EQ(300u, s.ackLatencyP99Ms);

    // 重复 ACK 不新增样本。
    rec.recordAck(11);
    s = rec.snapshot();
    EXPECT_EQ(3u, s.ackLatencySamples);

    // 无 accept 时刻的 ACK（未知 key）不新增样本。
    rec.recordAck(999);
    s = rec.snapshot();
    EXPECT_EQ(3u, s.ackLatencySamples);
}

TEST(ReliableMessageMetricsTest, OldestPendingAgeGauge)
{
    FakeClock clock;
    clock.set(kT0);
    ReliableMessageMetrics::Recorder rec(clock);

    int64_t prev = -1;
    // 逐段推进 clock，断言陈旧度单调不减（只要还有 Pending）。
    rec.recordDeliveryCreated(1, clock.nowMs());
    clock.advance(1000);
    EXPECT_EQ(1000, rec.snapshot().oldestPendingAgeMs);
    prev = rec.snapshot().oldestPendingAgeMs;

    // 新 Pending 到来不改变 oldest（d1 仍最老）。
    rec.recordDeliveryCreated(2, clock.nowMs());
    clock.advance(2000);
    const int64_t now = rec.snapshot().oldestPendingAgeMs;
    EXPECT_GE(now, prev);
    EXPECT_EQ(3000, now);
    prev = now;

    // d1 离开 Pending：oldest 落到 d2，age 回到 d2 的等待时长。
    rec.recordDeliveryTransition(1, ReliableMessageMetrics::DeliveryState::Pending,
                                 ReliableMessageMetrics::DeliveryState::InFlight);
    clock.advance(1000);
    EXPECT_EQ(1000, rec.snapshot().oldestPendingAgeMs);

    // 全部离开 Pending：age 归零（-1 = 无 Pending），clock 继续前进保持 -1。
    rec.recordDeliveryTransition(2, ReliableMessageMetrics::DeliveryState::Pending,
                                 ReliableMessageMetrics::DeliveryState::InFlight);
    EXPECT_EQ(-1, rec.snapshot().oldestPendingAgeMs);
    clock.advance(5000);
    EXPECT_EQ(-1, rec.snapshot().oldestPendingAgeMs);
}

TEST(ReliableMessageMetricsTest, OutboxLagAndPoisonCounters)
{
    FakeClock clock;
    clock.set(kT0);
    ReliableMessageMetrics::Recorder rec(clock);

    // lag 是 gauge：accept 后未处理事件 → lag>0；relay 消费 → 下降；清空 → 0。
    rec.updateOutboxLag(5);
    EXPECT_EQ(5u, rec.snapshot().outboxLag);
    rec.updateOutboxLag(3);
    EXPECT_EQ(3u, rec.snapshot().outboxLag);
    rec.updateOutboxLag(0);
    EXPECT_EQ(0u, rec.snapshot().outboxLag);

    // poison 是 counter：不可处理事件逐条 +1。
    rec.recordOutboxPoison();
    rec.recordOutboxPoison();
    EXPECT_EQ(2u, rec.snapshot().outboxPoison);

    // outbox 计数独立于 accept/状态计数（不互相污染）。
    const ReliableMessageMetrics::Snapshot s = rec.snapshot();
    EXPECT_EQ(0u, s.accepts);
    EXPECT_EQ(0u, s.createdDeliveries);
    expectConserving(rec, "after outbox events");
}

TEST(ReliableMessageMetricsTest, LegacyModeCountIncrements)
{
    FakeClock clock;
    clock.set(kT0);
    ReliableMessageMetrics::Recorder rec(clock);

    // 缺 client_message_id 走 implicit-ack 的在线/离线 Delivery 单独计数（spec §5.1）。
    rec.recordLegacyMode();
    rec.recordLegacyMode();
    EXPECT_EQ(2u, rec.snapshot().legacyModeCount);

    // v2 消息（Accepted）不增加 legacy count。
    rec.recordAccept(ReliableMessageMetrics::AcceptOutcome::Accepted);
    const ReliableMessageMetrics::Snapshot s = rec.snapshot();
    EXPECT_EQ(2u, s.legacyModeCount);
    EXPECT_EQ(1u, s.accepts);
}

TEST(ReliableMessageMetricsTest, HighCardinalityLabelsRejected)
{
    // 冻结白名单：只允许固定低基数枚举/结果维度。
    EXPECT_TRUE(ReliableMessageMetrics::isDimensionAllowed("delivery_state"));
    EXPECT_TRUE(ReliableMessageMetrics::isDimensionAllowed("accept_outcome"));
    EXPECT_TRUE(ReliableMessageMetrics::isDimensionAllowed("error_class"));
    EXPECT_TRUE(ReliableMessageMetrics::isDimensionAllowed("legacy"));

    // 高基数字段（UserId/MessageId/ClientMessageId）禁止作为 label。
    EXPECT_FALSE(ReliableMessageMetrics::isDimensionAllowed("user_id"));
    EXPECT_FALSE(ReliableMessageMetrics::isDimensionAllowed("message_id"));
    EXPECT_FALSE(ReliableMessageMetrics::isDimensionAllowed("client_message_id"));
    EXPECT_FALSE(ReliableMessageMetrics::isDimensionAllowed("sender_id"));
    EXPECT_FALSE(ReliableMessageMetrics::isDimensionAllowed("peer_ip"));

    // 白名单维度不抛；高基数/未知维度必须抛 invalid_argument。
    ReliableMessageMetrics::requireDimensionAllowed("delivery_state");
    EXPECT_THROW(ReliableMessageMetrics::requireDimensionAllowed("user_id"),
                 std::invalid_argument);
    EXPECT_THROW(ReliableMessageMetrics::requireDimensionAllowed("message_id"),
                 std::invalid_argument);
    EXPECT_THROW(ReliableMessageMetrics::requireDimensionAllowed("client_message_id"),
                 std::invalid_argument);
    EXPECT_THROW(ReliableMessageMetrics::requireDimensionAllowed("anything_else"),
                 std::invalid_argument);
}

TEST(ReliableMessageMetricsTest, SnapshotIsStructuredAndAggregatable)
{
    // 结构化：标准布局标量结构，可逐字段序列化/聚合（无 map、无高基数维度）。
    static_assert(std::is_standard_layout<ReliableMessageMetrics::Snapshot>::value,
                  "snapshot must be a flat standard-layout struct");

    FakeClock clock;
    clock.set(kT0);

    // Phase A：1 accept + 1 delivery 全生命周期 + 1 ACK 样本(100ms) + 1 attempt + outbox。
    ReliableMessageMetrics::Recorder recA(clock);
    recA.recordAccept(ReliableMessageMetrics::AcceptOutcome::Accepted);
    recA.recordDeliveryCreated(1, kT0);
    recA.recordDeliveryTransition(1, ReliableMessageMetrics::DeliveryState::Pending,
                                  ReliableMessageMetrics::DeliveryState::InFlight);
    recA.recordDeliveryTransition(1, ReliableMessageMetrics::DeliveryState::InFlight,
                                  ReliableMessageMetrics::DeliveryState::Acknowledged);
    recA.recordAcceptedTimestamp(11, kT0);
    clock.advance(100);
    recA.recordAck(11);
    recA.recordAttempt(false);
    recA.updateOutboxLag(3);
    recA.recordOutboxPoison();

    // Phase B：1 duplicate + 1 delivery 全生命周期 + 1 ACK 样本(200ms) + 1 retry。
    ReliableMessageMetrics::Recorder recB(clock);
    recB.recordAccept(ReliableMessageMetrics::AcceptOutcome::Duplicate);
    recB.recordDeliveryCreated(2, clock.nowMs());
    recB.recordDeliveryTransition(2, ReliableMessageMetrics::DeliveryState::Pending,
                                  ReliableMessageMetrics::DeliveryState::InFlight);
    recB.recordDeliveryTransition(2, ReliableMessageMetrics::DeliveryState::InFlight,
                                  ReliableMessageMetrics::DeliveryState::Acknowledged);
    recB.recordAcceptedTimestamp(22, kT0 + 100);
    clock.advance(200);
    recB.recordAck(22);
    recB.recordAttempt(true);
    recB.updateOutboxLag(5);
    recB.recordLegacyMode();

    // Combined：A+B 全部事件依次灌入一个 recorder（共享同一 Clock）。
    ReliableMessageMetrics::Recorder recAB(clock);
    recAB.recordAccept(ReliableMessageMetrics::AcceptOutcome::Accepted);
    recAB.recordDeliveryCreated(1, kT0);
    recAB.recordDeliveryTransition(1, ReliableMessageMetrics::DeliveryState::Pending,
                                   ReliableMessageMetrics::DeliveryState::InFlight);
    recAB.recordDeliveryTransition(1, ReliableMessageMetrics::DeliveryState::InFlight,
                                   ReliableMessageMetrics::DeliveryState::Acknowledged);
    recAB.recordAcceptedTimestamp(11, kT0);
    clock.advance(100);
    recAB.recordAck(11);
    recAB.recordAttempt(false);
    recAB.updateOutboxLag(3);
    recAB.recordOutboxPoison();
    recAB.recordAccept(ReliableMessageMetrics::AcceptOutcome::Duplicate);
    recAB.recordDeliveryCreated(2, kT0 + 100);
    recAB.recordDeliveryTransition(2, ReliableMessageMetrics::DeliveryState::Pending,
                                   ReliableMessageMetrics::DeliveryState::InFlight);
    recAB.recordDeliveryTransition(2, ReliableMessageMetrics::DeliveryState::InFlight,
                                   ReliableMessageMetrics::DeliveryState::Acknowledged);
    recAB.recordAcceptedTimestamp(22, kT0 + 100);
    clock.advance(200);
    recAB.recordAck(22);
    recAB.recordAttempt(true);
    recAB.updateOutboxLag(5);
    recAB.recordLegacyMode();

    const ReliableMessageMetrics::Snapshot a = recA.snapshot();
    const ReliableMessageMetrics::Snapshot b = recB.snapshot();
    const ReliableMessageMetrics::Snapshot ab = recAB.snapshot();

    // counter 字段：逐字段求和聚合。
    EXPECT_EQ(a.accepts + b.accepts, ab.accepts);
    EXPECT_EQ(a.duplicates + b.duplicates, ab.duplicates);
    EXPECT_EQ(a.conflicts + b.conflicts, ab.conflicts);
    EXPECT_EQ(a.rejectedTooManyRecipients + b.rejectedTooManyRecipients,
              ab.rejectedTooManyRecipients);
    EXPECT_EQ(a.createdDeliveries + b.createdDeliveries, ab.createdDeliveries);
    EXPECT_EQ(a.pending + b.pending, ab.pending);
    EXPECT_EQ(a.inflight + b.inflight, ab.inflight);
    EXPECT_EQ(a.acked + b.acked, ab.acked);
    EXPECT_EQ(a.expired + b.expired, ab.expired);
    EXPECT_EQ(a.attempts + b.attempts, ab.attempts);
    EXPECT_EQ(a.retries + b.retries, ab.retries);
    EXPECT_EQ(a.legacyModeCount + b.legacyModeCount, ab.legacyModeCount);
    EXPECT_EQ(a.outboxPoison + b.outboxPoison, ab.outboxPoison);

    // gauge 字段：last-writer 语义（ab 的 lag 取最后写入的 B）。
    EXPECT_EQ(b.outboxLag, ab.outboxLag);

    // histogram：样本数求和；分位数按合并样本集重算（{100,200} → p50=100,p95=p99=200）。
    EXPECT_EQ(a.ackLatencySamples + b.ackLatencySamples, ab.ackLatencySamples);
    EXPECT_EQ(100u, ab.ackLatencyP50Ms);
    EXPECT_EQ(200u, ab.ackLatencyP95Ms);
    EXPECT_EQ(200u, ab.ackLatencyP99Ms);

    // 无 Pending 时 oldest pending age = -1（结构化，非缺失字段）。
    EXPECT_EQ(-1, ab.oldestPendingAgeMs);
    expectConserving(recAB, "aggregated snapshot");
}

// P3-12 H2 RED：ACK latency 样本集必须有界（卡登记 N=4096，超过丢最旧）。
// RED 依据：现实现 ackLatencySamplesMs_ 无限累积 → 断言样本数不超过上限失败。
TEST(ReliableMessageMetricsTest, AckLatencySampleSetBounded)
{
    FakeClock clock;
    clock.set(kT0);
    ReliableMessageMetrics::Recorder rec(clock);

    // >N 个 ACK 后样本数保持 ≤ N（保留最近 N，丢最旧）。
    for (size_t i = 0; i < ReliableMessageMetrics::kMaxAckLatencySamples + 100; ++i) {
        rec.recordAcceptedTimestamp(static_cast<uint64_t>(1000 + i), clock.nowMs());
        rec.recordAck(static_cast<uint64_t>(1000 + i));
    }
    const ReliableMessageMetrics::Snapshot s = rec.snapshot();
    EXPECT_LE(s.ackLatencySamples, ReliableMessageMetrics::kMaxAckLatencySamples);
    // 恰保持上限（丢最旧，保留最近 N；分位数仍由保留样本计算）。
    EXPECT_EQ(static_cast<uint64_t>(ReliableMessageMetrics::kMaxAckLatencySamples),
              s.ackLatencySamples);
}

// P3-12 H2 RED：deliveries_ 与 ackAcceptedAtMs_ 在状态终局（Expired/cleanup）时
// 驱逐——过期后 trackedDeliveryCount 回落，未 ACK 起点同步清除（不再产生样本）。
// RED 依据：现实现记录永不驱逐（进程生命周期累积）→ recordEviction /
// trackedDeliveryCount 尚不存在（编译失败即合法 RED）。
TEST(ReliableMessageMetricsTest, DeliveryRecordEvictedOnExpiry)
{
    FakeClock clock;
    clock.set(kT0);
    ReliableMessageMetrics::Recorder rec(clock);

    rec.recordDeliveryCreated(1, kT0);
    rec.recordDeliveryCreated(2, kT0);
    EXPECT_EQ(2u, rec.trackedDeliveryCount());

    // 未 ACK 起点先登记：过期驱逐时应同步清除（有界内存语义）。
    rec.recordAcceptedTimestamp(11, clock.nowMs());

    // 过期转移已记（Expired 状态可观测），随后 recordEviction（runTick 过期
    // 转移挂点补记）驱逐该 delivery 记录：尺寸回落、计数器保留、守恒不破。
    rec.recordDeliveryTransition(1, ReliableMessageMetrics::DeliveryState::Pending,
                                 ReliableMessageMetrics::DeliveryState::Expired);
    EXPECT_EQ(1u, rec.snapshot().expired);
    rec.recordEviction(1, 11);
    EXPECT_EQ(1u, rec.trackedDeliveryCount());
    EXPECT_EQ(1u, rec.snapshot().expired);  // 计数器保留：Expired 仍可观测
    EXPECT_EQ(2u, rec.snapshot().createdDeliveries);
    EXPECT_TRUE(rec.isConserving());

    // 已被驱逐的未 ACK 起点不再产生 latency 样本。
    clock.advance(100);
    rec.recordAck(11);
    EXPECT_EQ(0u, rec.snapshot().ackLatencySamples);

    // 幂等：重复驱逐不改变计数。
    rec.recordEviction(1, 11);
    EXPECT_EQ(1u, rec.trackedDeliveryCount());
    EXPECT_TRUE(rec.isConserving());
}
