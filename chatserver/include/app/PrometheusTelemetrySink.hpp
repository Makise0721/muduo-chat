#pragma once

// P5-00 阶段 B Prometheus 导出面（docs/tasks/P5-00.md 设计决定 D10/D11）：
// Telemetry::TelemetrySink 生产实现——Prometheus 文本导出（# TYPE/# HELP +
// 系列行，label 编码转义）+ span 结构化日志 sink + 有界样本预算（D10）。
//
// 冻结用法见 tests/unit/PrometheusEndpointTest.cpp（GREEN 后即契约）。
// mymuduo-safe：不包含领域 class Clock（app/Clock.hpp），时间源经模板适配器
// 注入（测试传 FakeClock，生产 main.cpp 传 mymuduo-safe 稳态时钟适配器），
// 满足 D12「mymuduo TU 不引入领域 class Clock」。

#include "app/Telemetry.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

class PrometheusTelemetrySink : public Telemetry::TelemetrySink {
public:
    // D10 有界样本预算：histogram 样本容器上限，超限丢最旧（沿 P3-12 H2）。
    static const size_t kMaxPrometheusHistogramSamples = 4096;

    // P5-00 M-2：span 两容器有界（spanDurationsMs + completedSpans_），上限
    // 4096 超限丢最旧（沿阶段 A kMaxRecordedSpans 纪律）。
    static const size_t kMaxRecordedSpans = 4096;

    // D11 span 生产日志记录（LogEvent component=trace 同款字段）。
    struct SpanLogRecord {
        std::string name;
        uint64_t spanId;
        uint64_t parentSpanId;
        int64_t durationMs;
        bool errored;
    };

    // ClockT 须提供 int64_t nowMs()（FakeClock/UnixEpochClock/任意适配器）。
    template <typename ClockT>
    explicit PrometheusTelemetrySink(ClockT& clock)
        : nowMs_([&clock] { return clock.nowMs(); })
    {
    }

    PrometheusTelemetrySink(const PrometheusTelemetrySink&) = delete;
    PrometheusTelemetrySink& operator=(const PrometheusTelemetrySink&) = delete;

    void addCounter(const std::string& name, uint64_t delta,
                    const std::vector<Telemetry::Label>& labels =
                        std::vector<Telemetry::Label>()) override;
    void setGauge(const std::string& name, int64_t value,
                  const std::vector<Telemetry::Label>& labels =
                      std::vector<Telemetry::Label>()) override;
    void recordHistogramSample(const std::string& name, uint64_t valueMs,
                               const std::vector<Telemetry::Label>& labels =
                                   std::vector<Telemetry::Label>()) override;
    uint64_t beginSpan(const std::string& name, uint64_t parentSpanId = 0,
                       const std::vector<Telemetry::Label>& labels =
                           std::vector<Telemetry::Label>()) override;
    void endSpan(uint64_t spanId, bool errored) override;

    // Prometheus 文本（# TYPE/# HELP + 系列行 + rejected 计数）。
    std::string render() const;

    // rejectedLabels/rejectedSeries 可见计数（Telemetry::Snapshot 观察面）。
    Telemetry::Snapshot snapshot() const;

    // 完成 span 日志（D11；字段 = LogEvent trace 同款）。
    std::vector<SpanLogRecord> spanLogs() const;

    // label value 转义：`\`→`\\`、`"`→`\"`（Prometheus 文本转义）。
    static std::string renderLabelEscaped(const std::string& value);

private:
    struct SeriesState {
        uint64_t counterTotal = 0;
        int64_t gaugeLast = 0;
        bool hasCounter = false;
        bool hasGauge = false;
        bool hasHistogram = false;
        std::vector<uint64_t> histogramSamplesMs;  // 有界（D10）
        uint64_t spansStarted = 0;
        uint64_t spansErrored = 0;
        std::vector<uint64_t> spanDurationsMs;
        std::vector<Telemetry::Label> labels;
        std::string name;
    };

    struct ActiveSpan {
        uint64_t spanId;
        std::string name;
        uint64_t parentSpanId;
        int64_t beginMs;
        size_t seriesIdx;
    };

    static std::string seriesKey(const std::string& name,
                                 const std::vector<Telemetry::Label>& labels);
    void requireLabelsAllowedLocked(const std::vector<Telemetry::Label>& labels);
    size_t findOrCreateSeriesLocked(const std::string& name,
                                    const std::vector<Telemetry::Label>& labels);
    void recordSpanLog(const SpanLogRecord& record);

    std::vector<SeriesState> series_;
    std::map<std::string, size_t> seriesIndex_;
    uint64_t nextSpanId_ = 1;
    std::map<uint64_t, ActiveSpan> activeSpans_;
    std::vector<SpanLogRecord> completedSpans_;
    uint64_t rejectedLabels_ = 0;
    uint64_t rejectedSeries_ = 0;
    mutable std::mutex mutex_;
    std::function<int64_t()> nowMs_;
};