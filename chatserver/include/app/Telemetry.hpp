#pragma once

// P5-00 统一 Telemetry interface 与全局低基数预算契约（docs/tasks/P5-00.md
// 设计决定 D2/D3/D4/D7 冻结清单）。header-only：零 .cpp，沿
// ReliableMessageMetrics.hpp 全 header-only 先例。冻结用法见
// tests/unit/TelemetryContractTest.cpp（GREEN 后该文件即契约）。

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace Telemetry {

// 低基数 label 键值对：key 必须命中白名单
// (delivery_state/accept_outcome/error_class/legacy)，value 为每 key 有界枚举。
struct Label {
    const char* key;
    const char* value;
};

// D4 全局系列基数预算：进程内最多 kMaxSeries 个不同 (name, labels) 系列。
constexpr size_t kMaxSeries = 64;

// D3 label key 白名单（P3-12 已冻结四低基数维度）。
inline bool isLabelKeyAllowed(const std::string& key)
{
    return key == "delivery_state" || key == "accept_outcome" ||
           key == "error_class" || key == "legacy";
}

inline void requireLabelKeyAllowed(const std::string& key)
{
    if (!isLabelKeyAllowed(key)) {
        throw std::invalid_argument("label key '" + key +
                                    "' is not in the low-cardinality whitelist");
    }
}

// D3 每 key 有界枚举 value 集；未知 key 一律拒绝。
inline bool isLabelValueAllowed(const std::string& key, const std::string& value)
{
    if (key == "delivery_state") {
        return value == "pending" || value == "inflight" ||
               value == "acknowledged" || value == "expired";
    }
    if (key == "accept_outcome") {
        return value == "accepted" || value == "duplicate" ||
               value == "conflict" || value == "too_many_recipients";
    }
    if (key == "error_class") {
        return value == "timeout" || value == "unavailable" || value == "rejected";
    }
    if (key == "legacy") {
        return value == "true" || value == "false";
    }
    return false;
}

inline void requireLabelValueAllowed(const std::string& key, const std::string& value)
{
    if (!isLabelValueAllowed(key, value)) {
        throw std::invalid_argument("label value '" + value +
                                    "' not allowed for key '" + key + "'");
    }
}

// D7 快照惯例：全标量 standard-layout（static_assert 见冻结测试
// SnapshotAggregatesAcrossRecorders）。
struct SeriesSnapshot {
    uint64_t counterTotal = 0;
    int64_t gaugeLast = 0;
    uint64_t histogramSamples = 0;
    uint64_t histogramP50Ms = 0;
    uint64_t histogramP95Ms = 0;
    uint64_t histogramP99Ms = 0;
    uint64_t spansStarted = 0;
    uint64_t spansErrored = 0;
    uint64_t spanDurationP50Ms = 0;
    uint64_t spanDurationP95Ms = 0;
    uint64_t spanDurationP99Ms = 0;
};

struct Snapshot {
    SeriesSnapshot series[kMaxSeries];
    size_t seriesCount = 0;
    uint64_t rejectedLabels = 0;
    uint64_t rejectedSeries = 0;
};

// D2 API 形状（OTel 语义子集）：counter 单调累加 / gauge last-writer /
// histogram 分位数 / span(parent, error, duration)。label 参数默认空向量。
class TelemetrySink {
public:
    virtual ~TelemetrySink() = default;

    virtual void addCounter(const std::string& name, uint64_t delta,
                            const std::vector<Label>& labels = std::vector<Label>()) = 0;
    virtual void setGauge(const std::string& name, int64_t value,
                          const std::vector<Label>& labels = std::vector<Label>()) = 0;
    virtual void recordHistogramSample(const std::string& name, uint64_t valueMs,
                                       const std::vector<Label>& labels = std::vector<Label>()) = 0;
    virtual uint64_t beginSpan(const std::string& name, uint64_t parentSpanId = 0,
                               const std::vector<Label>& labels = std::vector<Label>()) = 0;
    virtual void endSpan(uint64_t spanId, bool errored) = 0;
};

} // namespace Telemetry