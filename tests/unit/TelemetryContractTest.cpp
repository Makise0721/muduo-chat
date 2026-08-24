// P5-00 RED：统一 Telemetry interface 与全局低基数预算契约（计划 §9.2，
// docs/tasks/P5-00.md 设计决定 D2-D7 冻结清单）。
//
// 本文件 RED 引用尚不存在的公开头 app/Telemetry.hpp 与 app/RecordingTelemetry.hpp：
// 当前 chatserver 无这两个头 → 编译失败（missing header）即合法 RED（沿
// P3-12/P4-01..P4-05 先例）。GREEN 时按本文件用法精确实现（命名/语义冻结）。
//
// 冻结契约（本文件逐测试断言，GREEN 必须满足）：
//   - counter 单调累加，接口层无递减操作；
//   - gauge last-writer；
//   - histogram nearest-rank 分位数（p50/p95/p99 = ceil(p*n)，1 基索引；
//     空集全 0），与 ReliableMessageMetrics 既有语义一致；
//   - span 携带 parent 与 error 状态（begin 返回非 0 spanId，0 保留 = root；
//     时长由注入 Clock 差值决定）；未知/重复 endSpan 抛 invalid_argument；
//   - label key 白名单 + 每 key 有界枚举 value 集：
//     delivery_state ∈ {pending,inflight,acknowledged,expired}、
//     accept_outcome ∈ {accepted,duplicate,conflict,too_many_recipients}、
//     error_class ∈ {timeout,unavailable,rejected}、
//     legacy ∈ {true,false}；
//   - UserId/MessageId/ClientMessageId/ConnectionId 一律禁止作为 metric label：
//     注入即拒绝（抛 invalid_argument）且 rejectedLabels 计数可见；
//   - 快照为全标量 standard-layout 结构（固定 kMaxSeries 系列 POD 数组 +
//     reject 计数），跨 recorder 逐字段求和聚合正确；
//   - 跨线程并发记录后总量精确（TSan 干净设计：adapter 内部锁自证于
//     docs/tasks/P5-00.md 设计决定 D5）。
//
// 测试只穿过公开 interface（TelemetrySink）与 RecordingTelemetry 公开观察面
// （snapshot()/seriesIndexOf()/recordedSpans()），不读私有容器、不依赖 sleep。

#include "app/Telemetry.hpp"          // RED：尚不存在 → 编译失败即合法 RED
#include "app/RecordingTelemetry.hpp" // RED：尚不存在 → 编译失败即合法 RED
#include "FakeClock.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

const int64_t kT0 = 1000000;

Telemetry::Label makeLabel(const char* key, const char* value)
{
    Telemetry::Label label;
    label.key = key;
    label.value = value;
    return label;
}

} // namespace

TEST(TelemetryContractTest, CounterAccumulatesMonotonically)
{
    FakeClock clock;
    clock.set(kT0);
    RecordingTelemetry tm(clock);

    tm.addCounter("chat_accepts", 1);
    tm.addCounter("chat_accepts", 2);
    const Telemetry::Snapshot s = tm.snapshot();
    const std::size_t idx = tm.seriesIndexOf("chat_accepts");
    ASSERT_NE(RecordingTelemetry::kNoSeries, idx);
    EXPECT_EQ(1u, s.seriesCount);
    EXPECT_EQ(3u, s.series[idx].counterTotal);

    // 继续累加只增不减（接口层无递减操作）。
    tm.addCounter("chat_accepts", 1);
    EXPECT_EQ(4u, tm.snapshot().series[idx].counterTotal);

    // 同名不同 label 组合是不同系列。
    std::vector<Telemetry::Label> legacyOn;
    legacyOn.push_back(makeLabel("legacy", "true"));
    tm.addCounter("chat_accepts", 5, legacyOn);
    const Telemetry::Snapshot s2 = tm.snapshot();
    EXPECT_EQ(2u, s2.seriesCount);
    EXPECT_EQ(4u, s2.series[idx].counterTotal);
    const std::size_t idxLegacy = tm.seriesIndexOf("chat_accepts", legacyOn);
    ASSERT_NE(RecordingTelemetry::kNoSeries, idxLegacy);
    EXPECT_EQ(5u, s2.series[idxLegacy].counterTotal);
}

TEST(TelemetryContractTest, GaugeRecordsLastWriterValue)
{
    FakeClock clock;
    clock.set(kT0);
    RecordingTelemetry tm(clock);

    tm.setGauge("executor_queue_depth", 5);
    EXPECT_EQ(5, tm.snapshot().series[tm.seriesIndexOf("executor_queue_depth")].gaugeLast);

    // last-writer：后写覆盖前写。
    tm.setGauge("executor_queue_depth", 7);
    EXPECT_EQ(7, tm.snapshot().series[tm.seriesIndexOf("executor_queue_depth")].gaugeLast);

    // 不同系列互不影响。
    tm.setGauge("outbox_lag", 3);
    const Telemetry::Snapshot s = tm.snapshot();
    EXPECT_EQ(2u, s.seriesCount);
    EXPECT_EQ(7, s.series[tm.seriesIndexOf("executor_queue_depth")].gaugeLast);
    EXPECT_EQ(3, s.series[tm.seriesIndexOf("outbox_lag")].gaugeLast);
}

TEST(TelemetryContractTest, HistogramNearestRankQuantiles)
{
    FakeClock clock;
    clock.set(kT0);
    RecordingTelemetry tm(clock);

    // 样本 {100,200,300}：nearest-rank p50=ceil(0.5*3)=2→200，p95=p99=3→300。
    tm.recordHistogramSample("ack_latency_ms", 100);
    tm.recordHistogramSample("ack_latency_ms", 200);
    tm.recordHistogramSample("ack_latency_ms", 300);
    const std::size_t idx = tm.seriesIndexOf("ack_latency_ms");
    ASSERT_NE(RecordingTelemetry::kNoSeries, idx);
    Telemetry::SeriesSnapshot st = tm.snapshot().series[idx];
    EXPECT_EQ(3u, st.histogramSamples);
    EXPECT_EQ(200u, st.histogramP50Ms);
    EXPECT_EQ(300u, st.histogramP95Ms);
    EXPECT_EQ(300u, st.histogramP99Ms);

    // 单样本：所有分位数等于该样本（nearest-rank ceil(p*1)=1）。
    tm.recordHistogramSample("single_sample_ms", 42);
    st = tm.snapshot().series[tm.seriesIndexOf("single_sample_ms")];
    EXPECT_EQ(1u, st.histogramSamples);
    EXPECT_EQ(42u, st.histogramP50Ms);
    EXPECT_EQ(42u, st.histogramP95Ms);
    EXPECT_EQ(42u, st.histogramP99Ms);

    // 空系列：全 0（结构化标量，无缺失字段）。
    tm.addCounter("untouched_series", 1);
    st = tm.snapshot().series[tm.seriesIndexOf("untouched_series")];
    EXPECT_EQ(0u, st.histogramSamples);
    EXPECT_EQ(0u, st.histogramP50Ms);
    EXPECT_EQ(0u, st.histogramP95Ms);
    EXPECT_EQ(0u, st.histogramP99Ms);
}

TEST(TelemetryContractTest, SpanCarriesParentAndErrorState)
{
    FakeClock clock;
    clock.set(kT0);
    RecordingTelemetry tm(clock);

    // root span：成功，时长 30ms。
    const uint64_t root = tm.beginSpan("deliver_message");
    EXPECT_NE(0u, root);  // 0 保留 = root/no parent
    clock.advance(30);
    tm.endSpan(root, false);

    // child span：携带 parent，错误态，时长 50ms。
    const uint64_t child = tm.beginSpan("redis_locate", root);
    EXPECT_NE(0u, child);
    EXPECT_NE(root, child);
    clock.advance(50);
    tm.endSpan(child, true);

    // 公开观察面：parent/error/duration 完整。
    const std::vector<RecordingTelemetry::SpanRecord> spans = tm.recordedSpans();
    ASSERT_EQ(2u, spans.size());
    EXPECT_EQ(root, spans[0].spanId);
    EXPECT_EQ("deliver_message", spans[0].name);
    EXPECT_EQ(0u, spans[0].parentSpanId);
    EXPECT_FALSE(spans[0].errored);
    EXPECT_EQ(30, spans[0].durationMs);
    EXPECT_EQ(child, spans[1].spanId);
    EXPECT_EQ("redis_locate", spans[1].name);
    EXPECT_EQ(root, spans[1].parentSpanId);
    EXPECT_TRUE(spans[1].errored);
    EXPECT_EQ(50, spans[1].durationMs);

    // 系列聚合字段同步更新。
    const Telemetry::Snapshot s = tm.snapshot();
    const std::size_t idxRoot = tm.seriesIndexOf("deliver_message");
    const std::size_t idxChild = tm.seriesIndexOf("redis_locate");
    ASSERT_NE(RecordingTelemetry::kNoSeries, idxRoot);
    ASSERT_NE(RecordingTelemetry::kNoSeries, idxChild);
    EXPECT_EQ(1u, s.series[idxRoot].spansStarted);
    EXPECT_EQ(0u, s.series[idxRoot].spansErrored);
    EXPECT_EQ(30u, s.series[idxRoot].spanDurationP50Ms);
    EXPECT_EQ(30u, s.series[idxRoot].spanDurationP95Ms);
    EXPECT_EQ(1u, s.series[idxChild].spansStarted);
    EXPECT_EQ(1u, s.series[idxChild].spansErrored);
    EXPECT_EQ(50u, s.series[idxChild].spanDurationP50Ms);
    EXPECT_EQ(50u, s.series[idxChild].spanDurationP99Ms);

    // 编程错误显式暴露：未知 spanId 的 endSpan 抛 invalid_argument。
    EXPECT_THROW(tm.endSpan(99999, false), std::invalid_argument);
    // 重复 endSpan 同样抛（已消费的 id 不复用）。
    EXPECT_THROW(tm.endSpan(root, false), std::invalid_argument);
}

TEST(TelemetryContractTest, HighCardinalityLabelsRejectedAndCounted)
{
    FakeClock clock;
    clock.set(kT0);
    RecordingTelemetry tm(clock);

    // UserId/MessageId/ClientMessageId/ConnectionId 一律禁止作为 metric label：
    // 注入即拒绝（抛 invalid_argument）且 rejectedLabels 先行递增（计数可见）。
    const char* forbidden[] = {"user_id", "message_id", "client_message_id",
                               "connection_id"};
    for (size_t i = 0; i < sizeof(forbidden) / sizeof(forbidden[0]); ++i) {
        std::vector<Telemetry::Label> labels;
        labels.push_back(makeLabel(forbidden[i], "42"));
        const uint64_t before = tm.snapshot().rejectedLabels;
        EXPECT_THROW(tm.addCounter("reject_probe", 1, labels), std::invalid_argument)
            << "key=" << forbidden[i];
        EXPECT_EQ(before + 1, tm.snapshot().rejectedLabels) << "key=" << forbidden[i];
    }

    // require 校验版本对禁用 key 同样必须抛（编程错误显式暴露）。
    for (size_t i = 0; i < sizeof(forbidden) / sizeof(forbidden[0]); ++i) {
        EXPECT_THROW(Telemetry::requireLabelKeyAllowed(forbidden[i]),
                     std::invalid_argument);
    }

    // 注入被整体拒绝：不产生任何系列。
    EXPECT_EQ(RecordingTelemetry::kNoSeries, tm.seriesIndexOf("reject_probe"));

    // 白名单 key 但 value 越界同样拒绝并计数。
    std::vector<Telemetry::Label> badValue;
    badValue.push_back(makeLabel("delivery_state", "yesterday"));
    const uint64_t before = tm.snapshot().rejectedLabels;
    EXPECT_THROW(tm.addCounter("state_probe", 1, badValue), std::invalid_argument);
    EXPECT_EQ(before + 1, tm.snapshot().rejectedLabels);
    EXPECT_EQ(RecordingTelemetry::kNoSeries, tm.seriesIndexOf("state_probe"));

    // 拒绝语义覆盖全部记录入口（gauge/histogram/span 一致）。
    std::vector<Telemetry::Label> bad;
    bad.push_back(makeLabel("user_id", "7"));
    uint64_t prev = tm.snapshot().rejectedLabels;
    EXPECT_THROW(tm.setGauge("g_probe", 1, bad), std::invalid_argument);
    EXPECT_EQ(prev + 1, tm.snapshot().rejectedLabels);
    prev = tm.snapshot().rejectedLabels;
    EXPECT_THROW(tm.recordHistogramSample("h_probe", 1, bad), std::invalid_argument);
    EXPECT_EQ(prev + 1, tm.snapshot().rejectedLabels);
    prev = tm.snapshot().rejectedLabels;
    EXPECT_THROW(tm.beginSpan("s_probe", 0, bad), std::invalid_argument);
    EXPECT_EQ(prev + 1, tm.snapshot().rejectedLabels);

    // 全程零合法系列产生。
    EXPECT_EQ(0u, tm.snapshot().seriesCount);
}

TEST(TelemetryContractTest, LabelWhitelistBoundedEnumValues)
{
    // key 白名单（冻结四维度，沿 P3-12 已冻结集合）。
    EXPECT_TRUE(Telemetry::isLabelKeyAllowed("delivery_state"));
    EXPECT_TRUE(Telemetry::isLabelKeyAllowed("accept_outcome"));
    EXPECT_TRUE(Telemetry::isLabelKeyAllowed("error_class"));
    EXPECT_TRUE(Telemetry::isLabelKeyAllowed("legacy"));
    EXPECT_FALSE(Telemetry::isLabelKeyAllowed("user_id"));
    EXPECT_FALSE(Telemetry::isLabelKeyAllowed("message_id"));
    EXPECT_FALSE(Telemetry::isLabelKeyAllowed("client_message_id"));
    EXPECT_FALSE(Telemetry::isLabelKeyAllowed("connection_id"));
    EXPECT_FALSE(Telemetry::isLabelKeyAllowed(""));
    EXPECT_FALSE(Telemetry::isLabelKeyAllowed("anything_else"));

    // 有界枚举 value 集：合法值逐项放行。
    EXPECT_TRUE(Telemetry::isLabelValueAllowed("delivery_state", "pending"));
    EXPECT_TRUE(Telemetry::isLabelValueAllowed("delivery_state", "inflight"));
    EXPECT_TRUE(Telemetry::isLabelValueAllowed("delivery_state", "acknowledged"));
    EXPECT_TRUE(Telemetry::isLabelValueAllowed("delivery_state", "expired"));
    EXPECT_TRUE(Telemetry::isLabelValueAllowed("accept_outcome", "accepted"));
    EXPECT_TRUE(Telemetry::isLabelValueAllowed("accept_outcome", "duplicate"));
    EXPECT_TRUE(Telemetry::isLabelValueAllowed("accept_outcome", "conflict"));
    EXPECT_TRUE(Telemetry::isLabelValueAllowed("accept_outcome", "too_many_recipients"));
    EXPECT_TRUE(Telemetry::isLabelValueAllowed("error_class", "timeout"));
    EXPECT_TRUE(Telemetry::isLabelValueAllowed("error_class", "unavailable"));
    EXPECT_TRUE(Telemetry::isLabelValueAllowed("error_class", "rejected"));
    EXPECT_TRUE(Telemetry::isLabelValueAllowed("legacy", "true"));
    EXPECT_TRUE(Telemetry::isLabelValueAllowed("legacy", "false"));

    // 越界 value 一律拒绝。
    EXPECT_FALSE(Telemetry::isLabelValueAllowed("delivery_state", "weird"));
    EXPECT_FALSE(Telemetry::isLabelValueAllowed("legacy", "maybe"));
    EXPECT_FALSE(Telemetry::isLabelValueAllowed("accept_outcome", ""));
    EXPECT_FALSE(Telemetry::isLabelValueAllowed("unknown_key", "any"));

    // require 版本：合法不抛，越界抛 invalid_argument。
    EXPECT_NO_THROW(Telemetry::requireLabelValueAllowed("delivery_state", "pending"));
    EXPECT_THROW(Telemetry::requireLabelValueAllowed("delivery_state", "weird"),
                 std::invalid_argument);
    EXPECT_THROW(Telemetry::requireLabelValueAllowed("unknown_key", "any"),
                 std::invalid_argument);

    // 合法不同 value 组合构成不同系列（同 metric 名）。
    FakeClock clock;
    clock.set(kT0);
    RecordingTelemetry tm(clock);
    std::vector<Telemetry::Label> pending;
    pending.push_back(makeLabel("delivery_state", "pending"));
    std::vector<Telemetry::Label> inflight;
    inflight.push_back(makeLabel("delivery_state", "inflight"));
    tm.addCounter("by_state", 1, pending);
    tm.addCounter("by_state", 1, inflight);
    const std::size_t idxPending = tm.seriesIndexOf("by_state", pending);
    const std::size_t idxInflight = tm.seriesIndexOf("by_state", inflight);
    ASSERT_NE(RecordingTelemetry::kNoSeries, idxPending);
    ASSERT_NE(RecordingTelemetry::kNoSeries, idxInflight);
    EXPECT_NE(idxPending, idxInflight);
    EXPECT_EQ(2u, tm.snapshot().seriesCount);
}

TEST(TelemetryContractTest, SnapshotAggregatesAcrossRecorders)
{
    // 快照为全标量 standard-layout 结构（沿 ReliableMessageMetrics::Snapshot 惯例）。
    static_assert(
        (std::is_standard_layout<Telemetry::Snapshot>::value),
        "snapshot must be a flat standard-layout struct");
    static_assert(
        (std::is_standard_layout<Telemetry::SeriesSnapshot>::value),
        "series entry must be a flat standard-layout struct");

    FakeClock clock;
    clock.set(kT0);

    // Phase A：1 counter + 1 histogram 样本(100) + 1 成功 span(100ms)。
    RecordingTelemetry recA(clock);
    recA.addCounter("conv_accepts", 1);
    recA.recordHistogramSample("conv_latency_ms", 100);
    const uint64_t spanA = recA.beginSpan("conv_deliver");
    clock.advance(100);
    recA.endSpan(spanA, false);

    // Phase B：2 counter + 1 histogram 样本(200) + gauge + 1 错误 span(200ms)。
    RecordingTelemetry recB(clock);
    recB.addCounter("conv_accepts", 2);
    recB.recordHistogramSample("conv_latency_ms", 200);
    recB.setGauge("conv_gauge", 7);
    const uint64_t spanB = recB.beginSpan("conv_deliver");
    clock.advance(200);
    recB.endSpan(spanB, true);

    // Combined：全部事件按同一顺序灌入共享同一 Clock 的单个 recorder。
    RecordingTelemetry recAB(clock);
    recAB.addCounter("conv_accepts", 1);
    recAB.recordHistogramSample("conv_latency_ms", 100);
    const uint64_t spanAB1 = recAB.beginSpan("conv_deliver");
    clock.advance(100);
    recAB.endSpan(spanAB1, false);
    recAB.addCounter("conv_accepts", 2);
    recAB.recordHistogramSample("conv_latency_ms", 200);
    recAB.setGauge("conv_gauge", 7);
    const uint64_t spanAB2 = recAB.beginSpan("conv_deliver");
    clock.advance(200);
    recAB.endSpan(spanAB2, true);

    const Telemetry::Snapshot a = recA.snapshot();
    const Telemetry::Snapshot b = recB.snapshot();
    const Telemetry::Snapshot ab = recAB.snapshot();

    ASSERT_EQ(3u, a.seriesCount);
    ASSERT_EQ(4u, b.seriesCount);
    ASSERT_EQ(4u, ab.seriesCount);
    const std::size_t ia = recA.seriesIndexOf("conv_accepts");
    const std::size_t ib = recB.seriesIndexOf("conv_accepts");
    const std::size_t iab = recAB.seriesIndexOf("conv_accepts");

    // counter 字段：逐字段求和聚合。
    EXPECT_EQ(a.series[ia].counterTotal + b.series[ib].counterTotal,
              ab.series[iab].counterTotal);
    EXPECT_EQ(3u, ab.series[iab].counterTotal);

    // histogram：样本数求和；分位数按合并样本集重算（{100,200} → p50=100,p95=p99=200）。
    const std::size_t ihA = recA.seriesIndexOf("conv_latency_ms");
    const std::size_t ihB = recB.seriesIndexOf("conv_latency_ms");
    const std::size_t ihAB = recAB.seriesIndexOf("conv_latency_ms");
    EXPECT_EQ(a.series[ihA].histogramSamples + b.series[ihB].histogramSamples,
              ab.series[ihAB].histogramSamples);
    EXPECT_EQ(100u, ab.series[ihAB].histogramP50Ms);
    EXPECT_EQ(200u, ab.series[ihAB].histogramP95Ms);
    EXPECT_EQ(200u, ab.series[ihAB].histogramP99Ms);

    // span 聚合：started/errored 求和；时长分位按合并样本重算（{100,200}）。
    const std::size_t iaS = recA.seriesIndexOf("conv_deliver");
    const std::size_t ibS = recB.seriesIndexOf("conv_deliver");
    const std::size_t iabS = recAB.seriesIndexOf("conv_deliver");
    EXPECT_EQ(a.series[iaS].spansStarted + b.series[ibS].spansStarted,
              ab.series[iabS].spansStarted);
    EXPECT_EQ(2u, ab.series[iabS].spansStarted);
    EXPECT_EQ(a.series[iaS].spansErrored + b.series[ibS].spansErrored,
              ab.series[iabS].spansErrored);
    EXPECT_EQ(1u, ab.series[iabS].spansErrored);
    EXPECT_EQ(100u, ab.series[iabS].spanDurationP50Ms);
    EXPECT_EQ(200u, ab.series[iabS].spanDurationP95Ms);
    EXPECT_EQ(200u, ab.series[iabS].spanDurationP99Ms);

    // gauge：combined-stream last-writer（只有 B 写过 → ab 取 B 值）。
    EXPECT_EQ(b.series[recB.seriesIndexOf("conv_gauge")].gaugeLast,
              ab.series[recAB.seriesIndexOf("conv_gauge")].gaugeLast);
    EXPECT_EQ(7, ab.series[recAB.seriesIndexOf("conv_gauge")].gaugeLast);

    // reject 计数同样可逐字段求和（本测试全程零违规）。
    EXPECT_EQ(a.rejectedLabels + b.rejectedLabels, ab.rejectedLabels);
    EXPECT_EQ(0u, ab.rejectedLabels);
    EXPECT_EQ(a.rejectedSeries + b.rejectedSeries, ab.rejectedSeries);
    EXPECT_EQ(0u, ab.rejectedSeries);
}

TEST(TelemetryContractTest, SnapshotCrossThreadAggregationCorrect)
{
    FakeClock clock;
    clock.set(kT0);
    RecordingTelemetry tm(clock);

    const int kThreads = 8;
    const int kIters = 500;
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&tm, t]() {
            for (int i = 0; i < kIters; ++i) {
                tm.addCounter("mt_counter", 1);
                tm.recordHistogramSample("mt_hist", (i % 7) + 1);
                const uint64_t span = tm.beginSpan("mt_span");
                tm.endSpan(span, ((t + i) % 2) == 0);
            }
        });
    }
    for (size_t i = 0; i < threads.size(); ++i) {
        threads[i].join();
    }

    // join 提供 happens-before：总量必须精确（不靠偶然时序，不依赖 sleep）。
    const Telemetry::Snapshot s = tm.snapshot();
    EXPECT_EQ(static_cast<uint64_t>(kThreads * kIters),
              s.series[tm.seriesIndexOf("mt_counter")].counterTotal);
    EXPECT_EQ(static_cast<uint64_t>(kThreads * kIters),
              s.series[tm.seriesIndexOf("mt_hist")].histogramSamples);
    const std::size_t idxSpan = tm.seriesIndexOf("mt_span");
    EXPECT_EQ(static_cast<uint64_t>(kThreads * kIters), s.series[idxSpan].spansStarted);
    EXPECT_EQ(static_cast<uint64_t>(kThreads * kIters / 2),
              s.series[idxSpan].spansErrored);
    EXPECT_EQ(3u, s.seriesCount);
    EXPECT_EQ(0u, s.rejectedLabels);
    EXPECT_EQ(0u, s.rejectedSeries);
    // 全部 span 已终结并被有界日志记录（kThreads*kIters < kMaxRecordedSpans）。
    EXPECT_EQ(static_cast<size_t>(kThreads * kIters), tm.recordedSpans().size());
}

TEST(TelemetryContractTest, SeriesBudgetRejectsOverflowVisibly)
{
    FakeClock clock;
    clock.set(kT0);
    RecordingTelemetry tm(clock);

    // 填满低基数预算（kMaxSeries 个不同系列）。
    for (std::size_t i = 0; i < Telemetry::kMaxSeries; ++i) {
        tm.addCounter(("budget_m" + std::to_string(i)).c_str(), 1);
    }
    EXPECT_EQ(Telemetry::kMaxSeries, tm.snapshot().seriesCount);
    EXPECT_EQ(0u, tm.snapshot().rejectedSeries);

    // 超出预算的新系列：拒绝（抛）且 rejectedSeries 计数可见。
    EXPECT_THROW(tm.addCounter("budget_overflow", 1), std::invalid_argument);
    EXPECT_EQ(1u, tm.snapshot().rejectedSeries);
    EXPECT_EQ(Telemetry::kMaxSeries, tm.snapshot().seriesCount);
    EXPECT_EQ(RecordingTelemetry::kNoSeries, tm.seriesIndexOf("budget_overflow"));

    // 预算满时已有系列仍可写（预算约束的是系列数量，不是记录次数）。
    tm.addCounter("budget_m5", 2);
    EXPECT_EQ(3u, tm.snapshot()
                      .series[tm.seriesIndexOf("budget_m5")]
                      .counterTotal);
    EXPECT_EQ(1u, tm.snapshot().rejectedSeries);
}
