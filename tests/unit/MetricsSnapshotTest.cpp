// P5-00 阶段 B RED：统一快照同源生成（docs/tasks/P5-00.md 设计决定 D8/D7、
// 「阶段 B」RED 冻结清单）。
//
// 本文件 RED 引用尚不存在的 app/MetricsSnapshot.hpp → 编译失败（missing header）
// 即合法 RED（沿阶段 A TelemetryContractTest / P3-12 先例）。GREEN 时按本文件
// 用法精确实现（命名/语义冻结）。
//
// 冻结契约（本文件逐测试断言，GREEN 必须满足）：
//   - MetricsSnapshot::snapshot(reliable, pool, executor, telemetry, gap) 为同源
//     生成函数：四个只读数据源（ReliableMessageMetrics::Snapshot 副本 + pool/
//     executor 只读桥接结构 + Telemetry::Snapshot 副本 + GapStats 缺口字段桥接）
//     → 单一全标量 Snapshot（standard layout，沿 ReliableMessageMetrics::Snapshot
//     惯例）。gap 缺省 = 默认（缺口字段由生产接线填充）。
//   - reliable_* 全字段逐字段对账（ReliableMessageMetrics::Snapshot 现为 20 字段；
//     docs/tasks/P5-00.md 记「21 字段」——文档计数偏差已登记，以冻结头实际字段集
//     为准，本测试对现有全部字段逐一断言）。
//   - pool/executor 只读桥接映射：pool total/idle/active + executor queue depth/
//     drop 计数进入统一快照。
//   - telemetry 系列（阶段 A Telemetry::Snapshot 全标量快照）原样嵌入统一快照，
//     逐字段对账（含 reject 计数）。
//   - 缺口字段（loop lag / accept reason / outstanding / fencing / consumer
//     lag/rebalance）经 GapStats 第四源进入统一快照（H-1：main.cpp 经领域 wiring
//     getter 聚合）；缺省保持默认（loopLagMs=-1 哨兵，其余 0）。
//   - 合并后逐字段正确：四源互不串扰、缺省字段保持默认。

#include "app/MetricsSnapshot.hpp"          // RED：尚不存在 → 编译失败即合法 RED
#include "app/ReliableMessageMetrics.hpp"   // 冻结头（只读数据源）
#include "app/Telemetry.hpp"                // 阶段 A 冻结头（只读数据源）
#include "app/RecordingTelemetry.hpp"       // 阶段 A 冻结 adapter（构造 telemetry 源）
#include "FakeClock.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

namespace {

const int64_t kT0 = 1000000;

void expectSeriesEqual(const Telemetry::SeriesSnapshot& a,
                       const Telemetry::SeriesSnapshot& b)
{
    EXPECT_EQ(a.counterTotal, b.counterTotal);
    EXPECT_EQ(a.gaugeLast, b.gaugeLast);
    EXPECT_EQ(a.histogramSamples, b.histogramSamples);
    EXPECT_EQ(a.histogramP50Ms, b.histogramP50Ms);
    EXPECT_EQ(a.histogramP95Ms, b.histogramP95Ms);
    EXPECT_EQ(a.histogramP99Ms, b.histogramP99Ms);
    EXPECT_EQ(a.spansStarted, b.spansStarted);
    EXPECT_EQ(a.spansErrored, b.spansErrored);
    EXPECT_EQ(a.spanDurationP50Ms, b.spanDurationP50Ms);
    EXPECT_EQ(a.spanDurationP95Ms, b.spanDurationP95Ms);
    EXPECT_EQ(a.spanDurationP99Ms, b.spanDurationP99Ms);
}

} // namespace

TEST(MetricsSnapshotTest, SnapshotStandardLayout)
{
    // 统一快照为全标量 standard-layout 结构（沿 ReliableMessageMetrics::Snapshot
    // 惯例；GREEN 实现不得引入 map/vector/虚函数）。
    static_assert(std::is_standard_layout<MetricsSnapshot::Snapshot>::value,
                  "unified snapshot must be a flat standard-layout struct");
    static_assert(std::is_standard_layout<MetricsSnapshot::PoolStats>::value,
                  "pool bridge must be a flat standard-layout struct");
    static_assert(std::is_standard_layout<MetricsSnapshot::ExecutorStats>::value,
                  "executor bridge must be a flat standard-layout struct");

    // 默认值结构化可观测：全 0（loopLagMs=-1 哨兵），telemetry 系列为空。
    MetricsSnapshot::Snapshot s;
    EXPECT_EQ(0u, s.accepts);
    EXPECT_EQ(0u, s.poolTotal);
    EXPECT_EQ(0u, s.executorQueueDepth);
    EXPECT_EQ(-1, s.loopLagMs);
    EXPECT_EQ(0u, s.acceptCount);
    EXPECT_EQ(0u, s.outstandingBytes);
    EXPECT_EQ(0u, s.fencingConflicts);
    EXPECT_EQ(0u, s.consumerLag);
    EXPECT_EQ(0u, s.rebalanceCount);
    EXPECT_EQ(0u, s.telemetry.seriesCount);
}

TEST(MetricsSnapshotTest, FromReliableBridgesFields)
{
    // reliable 源注入全 20 字段，逐字段对账进统一快照。
    ReliableMessageMetrics::Snapshot rm;
    rm.accepts = 1;
    rm.duplicates = 2;
    rm.conflicts = 3;
    rm.rejectedTooManyRecipients = 4;
    rm.createdDeliveries = 5;
    rm.pending = 6;
    rm.inflight = 7;
    rm.acked = 8;
    rm.expired = 9;
    rm.attempts = 10;
    rm.retries = 11;
    rm.legacyModeCount = 12;
    rm.outboxLag = 13;
    rm.outboxPoison = 14;
    rm.ackLatencySamples = 15;
    rm.ackLatencyP50Ms = 16;
    rm.ackLatencyP95Ms = 17;
    rm.ackLatencyP99Ms = 18;
    rm.oldestPendingAgeMs = 19;
    rm.consumerSeenConversations = 20;

    MetricsSnapshot::PoolStats pool;
    MetricsSnapshot::ExecutorStats exec;
    Telemetry::Snapshot ts;
    MetricsSnapshot::Snapshot s = MetricsSnapshot::snapshot(rm, pool, exec, ts);

    EXPECT_EQ(rm.accepts, s.accepts);
    EXPECT_EQ(rm.duplicates, s.duplicates);
    EXPECT_EQ(rm.conflicts, s.conflicts);
    EXPECT_EQ(rm.rejectedTooManyRecipients, s.rejectedTooManyRecipients);
    EXPECT_EQ(rm.createdDeliveries, s.createdDeliveries);
    EXPECT_EQ(rm.pending, s.pending);
    EXPECT_EQ(rm.inflight, s.inflight);
    EXPECT_EQ(rm.acked, s.acked);
    EXPECT_EQ(rm.expired, s.expired);
    EXPECT_EQ(rm.attempts, s.attempts);
    EXPECT_EQ(rm.retries, s.retries);
    EXPECT_EQ(rm.legacyModeCount, s.legacyModeCount);
    EXPECT_EQ(rm.outboxLag, s.outboxLag);
    EXPECT_EQ(rm.outboxPoison, s.outboxPoison);
    EXPECT_EQ(rm.ackLatencySamples, s.ackLatencySamples);
    EXPECT_EQ(rm.ackLatencyP50Ms, s.ackLatencyP50Ms);
    EXPECT_EQ(rm.ackLatencyP95Ms, s.ackLatencyP95Ms);
    EXPECT_EQ(rm.ackLatencyP99Ms, s.ackLatencyP99Ms);
    EXPECT_EQ(rm.oldestPendingAgeMs, s.oldestPendingAgeMs);
    EXPECT_EQ(rm.consumerSeenConversations, s.consumerSeenConversations);

    // 未提供的源保持默认（不串扰）。
    EXPECT_EQ(0u, s.poolTotal);
    EXPECT_EQ(0u, s.executorQueueDepth);
    EXPECT_EQ(0u, s.telemetry.seriesCount);
}

TEST(MetricsSnapshotTest, FromPoolAndExecutorBridges)
{
    // pool/executor 只读桥接：main.cpp 既有 ConnectionPool::getInstance().metrics()
    // 与 executor queue/drop 入口同款结构。
    MetricsSnapshot::PoolStats pool;
    pool.total = 5;
    pool.idle = 2;
    pool.active = 3;
    MetricsSnapshot::ExecutorStats exec;
    exec.queueDepth = 4;
    exec.dropped = 7;

    ReliableMessageMetrics::Snapshot rm;
    Telemetry::Snapshot ts;
    MetricsSnapshot::Snapshot s = MetricsSnapshot::snapshot(rm, pool, exec, ts);

    EXPECT_EQ(pool.total, s.poolTotal);
    EXPECT_EQ(pool.idle, s.poolIdle);
    EXPECT_EQ(pool.active, s.poolActive);
    EXPECT_EQ(exec.queueDepth, s.executorQueueDepth);
    EXPECT_EQ(exec.dropped, s.executorDropped);

    // 其余源保持默认（不串扰）。
    EXPECT_EQ(0u, s.accepts);
    EXPECT_EQ(0u, s.telemetry.seriesCount);
    EXPECT_EQ(-1, s.loopLagMs);
}

TEST(MetricsSnapshotTest, FromTelemetryBridgesSeries)
{
    // telemetry 源经阶段 A RecordingTelemetry 构造真实 Snapshot，逐字段进入统一
    // 快照（counter/gauge/histogram/span 系列 + reject 计数）。
    FakeClock clock;
    clock.set(kT0);
    RecordingTelemetry tm(clock);
    tm.addCounter("chat_accepts", 1);
    tm.setGauge("executor_queue_depth", 7);
    tm.recordHistogramSample("ack_latency_ms", 100);
    tm.recordHistogramSample("ack_latency_ms", 200);
    const uint64_t span = tm.beginSpan("deliver_message");
    clock.advance(30);
    tm.endSpan(span, false);
    const Telemetry::Snapshot ts = tm.snapshot();

    ReliableMessageMetrics::Snapshot rm;
    MetricsSnapshot::PoolStats pool;
    MetricsSnapshot::ExecutorStats exec;
    MetricsSnapshot::Snapshot s = MetricsSnapshot::snapshot(rm, pool, exec, ts);

    EXPECT_EQ(ts.seriesCount, s.telemetry.seriesCount);
    EXPECT_EQ(ts.rejectedLabels, s.telemetry.rejectedLabels);
    EXPECT_EQ(ts.rejectedSeries, s.telemetry.rejectedSeries);
    for (std::size_t i = 0; i < ts.seriesCount; ++i) {
        expectSeriesEqual(ts.series[i], s.telemetry.series[i]);
    }
    // counter 系列实际值抽查（进入统一快照的 series 有值，非零清理）。
    const std::size_t idx = tm.seriesIndexOf("chat_accepts");
    ASSERT_NE(RecordingTelemetry::kNoSeries, idx);
    EXPECT_EQ(1u, s.telemetry.series[idx].counterTotal);

    // 其余源保持默认（不串扰）。
    EXPECT_EQ(0u, s.accepts);
    EXPECT_EQ(0u, s.poolTotal);
}

TEST(MetricsSnapshotTest, FromGapBridgesFields)
{
    // H-1：缺口字段第四源 GapStats（main.cpp 经领域 wiring getter 聚合：EventLoop
    // loopLagProbeMs、ChatServer/TcpServer acceptReasonCounts + outstanding、
    // ProtocolCodec fencingConflicts/consumerLag/rebalanceCount）逐字段进入统一快照。
    MetricsSnapshot::GapStats gap;
    gap.loopLagMs = 42;
    gap.acceptCount = 3;
    gap.acceptRateReject = 1;
    gap.acceptMaxReject = 2;
    gap.acceptEmfileRecover = 4;
    gap.outstandingBytes = 999;
    gap.fencingConflicts = 5;
    gap.consumerLag = 6;
    gap.rebalanceCount = 7;

    ReliableMessageMetrics::Snapshot rm;
    MetricsSnapshot::PoolStats pool;
    MetricsSnapshot::ExecutorStats exec;
    Telemetry::Snapshot ts;
    MetricsSnapshot::Snapshot s = MetricsSnapshot::snapshot(rm, pool, exec, ts, gap);

    EXPECT_EQ(42, s.loopLagMs);
    EXPECT_EQ(3u, s.acceptCount);
    EXPECT_EQ(1u, s.acceptRateReject);
    EXPECT_EQ(2u, s.acceptMaxReject);
    EXPECT_EQ(4u, s.acceptEmfileRecover);
    EXPECT_EQ(999u, s.outstandingBytes);
    EXPECT_EQ(5u, s.fencingConflicts);
    EXPECT_EQ(6u, s.consumerLag);
    EXPECT_EQ(7u, s.rebalanceCount);

    // 其余源保持默认（不串扰）。
    EXPECT_EQ(0u, s.accepts);
    EXPECT_EQ(0u, s.poolTotal);
    EXPECT_EQ(0u, s.executorQueueDepth);
    EXPECT_EQ(0u, s.telemetry.seriesCount);
}

TEST(MetricsSnapshotTest, CombinedSnapshotAccurate)
{
    // 三源合并：reliable + pool/executor + telemetry 全部注入不同值，逐字段精确。
    ReliableMessageMetrics::Snapshot rm;
    rm.accepts = 11;
    rm.duplicates = 12;
    rm.conflicts = 13;
    rm.rejectedTooManyRecipients = 14;
    rm.createdDeliveries = 15;
    rm.pending = 16;
    rm.inflight = 17;
    rm.acked = 18;
    rm.expired = 19;
    rm.attempts = 20;
    rm.retries = 21;
    rm.legacyModeCount = 22;
    rm.outboxLag = 23;
    rm.outboxPoison = 24;
    rm.ackLatencySamples = 25;
    rm.ackLatencyP50Ms = 26;
    rm.ackLatencyP95Ms = 27;
    rm.ackLatencyP99Ms = 28;
    rm.oldestPendingAgeMs = 29;
    rm.consumerSeenConversations = 30;

    MetricsSnapshot::PoolStats pool;
    pool.total = 3;
    pool.idle = 1;
    pool.active = 2;
    MetricsSnapshot::ExecutorStats exec;
    exec.queueDepth = 8;
    exec.dropped = 9;

    FakeClock clock;
    clock.set(kT0);
    RecordingTelemetry tm(clock);
    tm.addCounter("chat_accepts", 1);
    const Telemetry::Snapshot ts = tm.snapshot();

    MetricsSnapshot::Snapshot s = MetricsSnapshot::snapshot(rm, pool, exec, ts);

    EXPECT_EQ(11u, s.accepts);
    EXPECT_EQ(12u, s.duplicates);
    EXPECT_EQ(13u, s.conflicts);
    EXPECT_EQ(14u, s.rejectedTooManyRecipients);
    EXPECT_EQ(15u, s.createdDeliveries);
    EXPECT_EQ(16u, s.pending);
    EXPECT_EQ(17u, s.inflight);
    EXPECT_EQ(18u, s.acked);
    EXPECT_EQ(19u, s.expired);
    EXPECT_EQ(20u, s.attempts);
    EXPECT_EQ(21u, s.retries);
    EXPECT_EQ(22u, s.legacyModeCount);
    EXPECT_EQ(23u, s.outboxLag);
    EXPECT_EQ(24u, s.outboxPoison);
    EXPECT_EQ(25u, s.ackLatencySamples);
    EXPECT_EQ(26u, s.ackLatencyP50Ms);
    EXPECT_EQ(27u, s.ackLatencyP95Ms);
    EXPECT_EQ(28u, s.ackLatencyP99Ms);
    EXPECT_EQ(29, s.oldestPendingAgeMs);
    EXPECT_EQ(30u, s.consumerSeenConversations);
    EXPECT_EQ(3u, s.poolTotal);
    EXPECT_EQ(1u, s.poolIdle);
    EXPECT_EQ(2u, s.poolActive);
    EXPECT_EQ(8u, s.executorQueueDepth);
    EXPECT_EQ(9u, s.executorDropped);
    EXPECT_EQ(1u, s.telemetry.seriesCount);
    EXPECT_EQ(1u, s.telemetry.series[tm.seriesIndexOf("chat_accepts")].counterTotal);
    EXPECT_EQ(ts.rejectedLabels, s.telemetry.rejectedLabels);

    // 缺口字段由生产接线填充，未提供 GapStats 时保持默认（H-1 第四源缺省）。
    EXPECT_EQ(-1, s.loopLagMs);
    EXPECT_EQ(0u, s.acceptCount);
    EXPECT_EQ(0u, s.acceptRateReject);
    EXPECT_EQ(0u, s.acceptMaxReject);
    EXPECT_EQ(0u, s.acceptEmfileRecover);
    EXPECT_EQ(0u, s.outstandingBytes);
    EXPECT_EQ(0u, s.fencingConflicts);
    EXPECT_EQ(0u, s.consumerLag);
    EXPECT_EQ(0u, s.rebalanceCount);
}
