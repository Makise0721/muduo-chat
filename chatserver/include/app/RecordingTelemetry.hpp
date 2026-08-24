#pragma once

// P5-00 阶段 A 纯内存 RecordingTelemetry（docs/tasks/P5-00.md 设计决定
// D4/D5/D6）：TelemetrySink 的测试 adapter。全部状态由内部 mutex_ 保护
// （D5，否决 atomics）；完成 span 有界日志 kMaxRecordedSpans（D6，超限丢最
// 旧）；系列基数预算 kMaxSeries（D4）。Clock& 注入（FakeClock 确定性时间，
// 时长 = endSpan 时 nowMs() - begin 时 nowMs()）。header-only，零 .cpp。
// 冻结用法见 tests/unit/TelemetryContractTest.cpp。

#include "app/Telemetry.hpp"
#include "app/Clock.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

class RecordingTelemetry : public Telemetry::TelemetrySink {
public:
    // 哨兵：seriesIndexOf 未命中返回值（不可能是合法系列索引）。以 size_t 为
    // 底层类型的枚举常量：C++14 下经 const& 绑定（gtest 断言）会 odr-use
    // static const 成员而要求 out-of-line 定义，枚举常量无此问题（值语义不变）。
    enum : size_t { kNoSeries = static_cast<size_t>(-1) };

    // D6 有界内存：完成 span 日志上限，超限丢最旧（本阶段测试 4000 < 4096）。
    static const size_t kMaxRecordedSpans = 4096;

    explicit RecordingTelemetry(Clock& clock) : clock_(clock) {}
    RecordingTelemetry(const RecordingTelemetry&) = delete;
    RecordingTelemetry& operator=(const RecordingTelemetry&) = delete;

    void addCounter(const std::string& name, uint64_t delta,
                    const std::vector<Telemetry::Label>& labels = std::vector<Telemetry::Label>()) override
    {
        std::lock_guard<std::mutex> lk(mutex_);
        requireLabelsAllowedLocked(labels);
        series_[findOrCreateSeriesLocked(name, labels)].counterTotal += delta;
    }

    void setGauge(const std::string& name, int64_t value,
                  const std::vector<Telemetry::Label>& labels = std::vector<Telemetry::Label>()) override
    {
        std::lock_guard<std::mutex> lk(mutex_);
        requireLabelsAllowedLocked(labels);
        series_[findOrCreateSeriesLocked(name, labels)].gaugeLast = value;
    }

    void recordHistogramSample(const std::string& name, uint64_t valueMs,
                               const std::vector<Telemetry::Label>& labels = std::vector<Telemetry::Label>()) override
    {
        std::lock_guard<std::mutex> lk(mutex_);
        requireLabelsAllowedLocked(labels);
        series_[findOrCreateSeriesLocked(name, labels)].histogramSamplesMs.push_back(valueMs);
    }

    uint64_t beginSpan(const std::string& name, uint64_t parentSpanId = 0,
                       const std::vector<Telemetry::Label>& labels = std::vector<Telemetry::Label>()) override
    {
        std::lock_guard<std::mutex> lk(mutex_);
        requireLabelsAllowedLocked(labels);
        const size_t idx = findOrCreateSeriesLocked(name, labels);
        series_[idx].spansStarted += 1;
        const uint64_t id = nextSpanId_++;
        ActiveSpan active;
        active.spanId = id;
        active.name = name;
        active.parentSpanId = parentSpanId;
        active.beginMs = clock_.nowMs();
        active.seriesIdx = idx;
        activeSpans_[id] = active;
        return id;
    }

    void endSpan(uint64_t spanId, bool errored) override
    {
        std::lock_guard<std::mutex> lk(mutex_);
        std::map<uint64_t, ActiveSpan>::iterator it = activeSpans_.find(spanId);
        if (it == activeSpans_.end()) {
            throw std::invalid_argument("endSpan for unknown or already-closed span id");
        }
        const ActiveSpan active = it->second;
        activeSpans_.erase(it);
        const int64_t durationMs = clock_.nowMs() - active.beginMs;
        SeriesState& series = series_[active.seriesIdx];
        if (errored) {
            series.spansErrored += 1;
        }
        series.spanDurationsMs.push_back(static_cast<uint64_t>(durationMs < 0 ? 0 : durationMs));

        SpanRecord record;
        record.spanId = active.spanId;
        record.name = active.name;
        record.parentSpanId = active.parentSpanId;
        record.errored = errored;
        record.durationMs = durationMs;
        if (completedSpans_.size() >= kMaxRecordedSpans) {
            completedSpans_.erase(completedSpans_.begin());
        }
        completedSpans_.push_back(record);
    }

    // 完成 span 记录（公开结构，D6 有界日志元素）。
    struct SpanRecord {
        uint64_t spanId;
        std::string name;
        uint64_t parentSpanId;
        bool errored;
        int64_t durationMs;
    };

    // —— 公开观察面（冻结测试仅用这些）——
    size_t seriesIndexOf(const char* name) const
    {
        return seriesIndexOf(name, std::vector<Telemetry::Label>());
    }

    size_t seriesIndexOf(const char* name, const std::vector<Telemetry::Label>& labels) const
    {
        std::lock_guard<std::mutex> lk(mutex_);
        std::map<std::string, size_t>::const_iterator it = seriesIndex_.find(seriesKey(name, labels));
        if (it == seriesIndex_.end()) {
            return kNoSeries;
        }
        return it->second;
    }

    Telemetry::Snapshot snapshot() const
    {
        std::lock_guard<std::mutex> lk(mutex_);
        Telemetry::Snapshot s;
        s.seriesCount = series_.size();
        for (size_t i = 0; i < series_.size(); ++i) {
            s.series[i] = toSeriesSnapshot(series_[i]);
        }
        s.rejectedLabels = rejectedLabels_;
        s.rejectedSeries = rejectedSeries_;
        return s;
    }

    std::vector<SpanRecord> recordedSpans() const
    {
        std::lock_guard<std::mutex> lk(mutex_);
        return completedSpans_;
    }

private:
    struct SeriesState {
        uint64_t counterTotal = 0;
        int64_t gaugeLast = 0;
        std::vector<uint64_t> histogramSamplesMs;
        uint64_t spansStarted = 0;
        uint64_t spansErrored = 0;
        std::vector<uint64_t> spanDurationsMs;
    };

    struct ActiveSpan {
        uint64_t spanId;
        std::string name;
        uint64_t parentSpanId;
        int64_t beginMs;
        size_t seriesIdx;
    };

    static Telemetry::SeriesSnapshot toSeriesSnapshot(const SeriesState& state)
    {
        Telemetry::SeriesSnapshot out;
        out.counterTotal = state.counterTotal;
        out.gaugeLast = state.gaugeLast;
        out.histogramSamples = state.histogramSamplesMs.size();
        out.spansStarted = state.spansStarted;
        out.spansErrored = state.spansErrored;
        const std::vector<uint64_t> histSorted = sortedCopy(state.histogramSamplesMs);
        out.histogramP50Ms = nearestRank(histSorted, 0.50);
        out.histogramP95Ms = nearestRank(histSorted, 0.95);
        out.histogramP99Ms = nearestRank(histSorted, 0.99);
        const std::vector<uint64_t> spanSorted = sortedCopy(state.spanDurationsMs);
        out.spanDurationP50Ms = nearestRank(spanSorted, 0.50);
        out.spanDurationP95Ms = nearestRank(spanSorted, 0.95);
        out.spanDurationP99Ms = nearestRank(spanSorted, 0.99);
        return out;
    }

    static std::vector<uint64_t> sortedCopy(const std::vector<uint64_t>& samples)
    {
        std::vector<uint64_t> sorted = samples;
        std::sort(sorted.begin(), sorted.end());
        return sorted;
    }

    // nearest-rank 分位数：索引 = ceil(p*n)（1 基），空集全 0。
    static uint64_t nearestRank(const std::vector<uint64_t>& sorted, double p)
    {
        const size_t n = sorted.size();
        if (n == 0) {
            return 0;
        }
        const size_t idx = static_cast<size_t>(std::ceil(p * static_cast<double>(n)));
        return sorted[idx - 1];
    }

    // 系列键 = (name, 规范化 labels)：labels 按 key 排序后拼接；不同 label
    // 组合是不同系列（D4）。
    static std::string seriesKey(const std::string& name, const std::vector<Telemetry::Label>& labels)
    {
        std::vector<Telemetry::Label> sorted = labels;
        std::sort(sorted.begin(), sorted.end(), labelLess);
        std::string key = name;
        for (size_t i = 0; i < sorted.size(); ++i) {
            key += '\x1f';
            key += sorted[i].key;
            key += '\x1e';
            key += sorted[i].value;
        }
        return key;
    }

    static bool labelLess(const Telemetry::Label& a, const Telemetry::Label& b)
    {
        const int c = std::strcmp(a.key, b.key);
        if (c != 0) {
            return c < 0;
        }
        return std::strcmp(a.value, b.value) < 0;
    }

    // D4 拒绝语义：label 违规（key 不在白名单或 value 越界）→ 先
    // ++rejectedLabels 再抛 invalid_argument（编程错误显式暴露）。
    void requireLabelsAllowedLocked(const std::vector<Telemetry::Label>& labels)
    {
        for (size_t i = 0; i < labels.size(); ++i) {
            if (!Telemetry::isLabelKeyAllowed(labels[i].key)) {
                ++rejectedLabels_;
                throw std::invalid_argument("label key '" + std::string(labels[i].key) +
                                            "' is not in the low-cardinality whitelist");
            }
            if (!Telemetry::isLabelValueAllowed(labels[i].key, labels[i].value)) {
                ++rejectedLabels_;
                throw std::invalid_argument("label value '" + std::string(labels[i].value) +
                                            "' not allowed for key '" +
                                            std::string(labels[i].key) + "'");
            }
        }
    }

    // D4 系列预算：已有系列直接命中；新系列超 kMaxSeries → ++rejectedSeries
    // 并抛；拒绝后不产生系列。
    size_t findOrCreateSeriesLocked(const std::string& name, const std::vector<Telemetry::Label>& labels)
    {
        const std::string key = seriesKey(name, labels);
        std::map<std::string, size_t>::iterator it = seriesIndex_.find(key);
        if (it != seriesIndex_.end()) {
            return it->second;
        }
        if (series_.size() >= Telemetry::kMaxSeries) {
            ++rejectedSeries_;
            throw std::invalid_argument("series budget exceeded");
        }
        const size_t idx = series_.size();
        series_.push_back(SeriesState());
        seriesIndex_[key] = idx;
        return idx;
    }

    std::vector<SeriesState> series_;
    std::map<std::string, size_t> seriesIndex_;
    uint64_t nextSpanId_ = 1;
    std::map<uint64_t, ActiveSpan> activeSpans_;
    std::vector<SpanRecord> completedSpans_;
    uint64_t rejectedLabels_ = 0;
    uint64_t rejectedSeries_ = 0;
    mutable std::mutex mutex_;
    Clock& clock_;
};