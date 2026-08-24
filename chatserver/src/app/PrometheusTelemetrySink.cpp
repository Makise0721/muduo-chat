#include "app/PrometheusTelemetrySink.hpp"

#include "Logger.h"  // D11 span 生产 sink 落 LogEvent（component=trace）

#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>

namespace {

std::string escapeLabelValue(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '\\') {
            out += "\\\\";
        } else if (c == '"') {
            out += "\\\"";
        } else {
            out += c;
        }
    }
    return out;
}

// nearest-rank 分位数：索引 = ceil(p*n)（1 基），空集全 0。
uint64_t nearestRank(const std::vector<uint64_t>& sorted, double p)
{
    const size_t n = sorted.size();
    if (n == 0) {
        return 0;
    }
    const size_t idx = static_cast<size_t>(std::ceil(p * static_cast<double>(n)));
    return sorted[idx - 1];
}

} // namespace

std::string PrometheusTelemetrySink::renderLabelEscaped(const std::string& value)
{
    return escapeLabelValue(value);
}

void PrometheusTelemetrySink::addCounter(const std::string& name, uint64_t delta,
                                         const std::vector<Telemetry::Label>& labels)
{
    std::lock_guard<std::mutex> lk(mutex_);
    requireLabelsAllowedLocked(labels);
    SeriesState& s = series_[findOrCreateSeriesLocked(name, labels)];
    s.counterTotal += delta;
    s.hasCounter = true;
}

void PrometheusTelemetrySink::setGauge(const std::string& name, int64_t value,
                                       const std::vector<Telemetry::Label>& labels)
{
    std::lock_guard<std::mutex> lk(mutex_);
    requireLabelsAllowedLocked(labels);
    SeriesState& s = series_[findOrCreateSeriesLocked(name, labels)];
    s.gaugeLast = value;
    s.hasGauge = true;
}

void PrometheusTelemetrySink::recordHistogramSample(const std::string& name, uint64_t valueMs,
                                                    const std::vector<Telemetry::Label>& labels)
{
    std::lock_guard<std::mutex> lk(mutex_);
    requireLabelsAllowedLocked(labels);
    SeriesState& s = series_[findOrCreateSeriesLocked(name, labels)];
    // D10 有界样本预算：超限丢最旧。
    if (s.histogramSamplesMs.size() >= kMaxPrometheusHistogramSamples) {
        s.histogramSamplesMs.erase(s.histogramSamplesMs.begin());
    }
    s.histogramSamplesMs.push_back(valueMs);
    s.hasHistogram = true;
}

uint64_t PrometheusTelemetrySink::beginSpan(const std::string& name, uint64_t parentSpanId,
                                            const std::vector<Telemetry::Label>& labels)
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
    active.beginMs = nowMs_();
    active.seriesIdx = idx;
    activeSpans_[id] = active;
    return id;
}

void PrometheusTelemetrySink::endSpan(uint64_t spanId, bool errored)
{
    std::lock_guard<std::mutex> lk(mutex_);
    std::map<uint64_t, ActiveSpan>::iterator it = activeSpans_.find(spanId);
    if (it == activeSpans_.end()) {
        throw std::invalid_argument("endSpan for unknown or already-closed span id");
    }
    const ActiveSpan active = it->second;
    activeSpans_.erase(it);
    const int64_t durationMs = nowMs_() - active.beginMs;
    SeriesState& series = series_[active.seriesIdx];
    if (errored) {
        series.spansErrored += 1;
    }
    // P5-00 M-2：span 时长样本有界（kMaxRecordedSpans，超限丢最旧）。
    if (series.spanDurationsMs.size() >= kMaxRecordedSpans) {
        series.spanDurationsMs.erase(series.spanDurationsMs.begin());
    }
    series.spanDurationsMs.push_back(
        static_cast<uint64_t>(durationMs < 0 ? 0 : durationMs));

    SpanLogRecord record;
    record.name = active.name;
    record.spanId = active.spanId;
    record.parentSpanId = active.parentSpanId;
    record.durationMs = durationMs;
    record.errored = errored;
    // P5-00 M-2：完成 span 日志有界（kMaxRecordedSpans，超限丢最旧）。
    if (completedSpans_.size() >= kMaxRecordedSpans) {
        completedSpans_.erase(completedSpans_.begin());
    }
    completedSpans_.push_back(record);
    recordSpanLog(record);
}

Telemetry::Snapshot PrometheusTelemetrySink::snapshot() const
{
    std::lock_guard<std::mutex> lk(mutex_);
    Telemetry::Snapshot s;
    s.seriesCount = series_.size();
    for (size_t i = 0; i < series_.size(); ++i) {
        const SeriesState& st = series_[i];
        Telemetry::SeriesSnapshot& out = s.series[i];
        out.counterTotal = st.counterTotal;
        out.gaugeLast = st.gaugeLast;
        out.histogramSamples = st.histogramSamplesMs.size();
        out.spansStarted = st.spansStarted;
        out.spansErrored = st.spansErrored;
        std::vector<uint64_t> histSorted = st.histogramSamplesMs;
        std::sort(histSorted.begin(), histSorted.end());
        out.histogramP50Ms = nearestRank(histSorted, 0.50);
        out.histogramP95Ms = nearestRank(histSorted, 0.95);
        out.histogramP99Ms = nearestRank(histSorted, 0.99);
        std::vector<uint64_t> spanSorted = st.spanDurationsMs;
        std::sort(spanSorted.begin(), spanSorted.end());
        out.spanDurationP50Ms = nearestRank(spanSorted, 0.50);
        out.spanDurationP95Ms = nearestRank(spanSorted, 0.95);
        out.spanDurationP99Ms = nearestRank(spanSorted, 0.99);
    }
    s.rejectedLabels = rejectedLabels_;
    s.rejectedSeries = rejectedSeries_;
    return s;
}

std::vector<PrometheusTelemetrySink::SpanLogRecord> PrometheusTelemetrySink::spanLogs() const
{
    std::lock_guard<std::mutex> lk(mutex_);
    return completedSpans_;
}

std::string PrometheusTelemetrySink::render() const
{
    std::lock_guard<std::mutex> lk(mutex_);
    std::ostringstream os;
    for (size_t i = 0; i < series_.size(); ++i) {
        const SeriesState& st = series_[i];
        const std::string name = st.name;
        std::string suffix;
        if (!st.labels.empty()) {
            suffix += "{";
            for (size_t k = 0; k < st.labels.size(); ++k) {
                if (k > 0) {
                    suffix += ",";
                }
                suffix += st.labels[k].key;
                suffix += "=\"";
                suffix += escapeLabelValue(st.labels[k].value);
                suffix += "\"";
            }
            suffix += "}";
        }
        if (st.hasCounter) {
            os << "# TYPE " << name << " counter\n";
            os << "# HELP " << name << " \n";
            os << name << suffix << " " << st.counterTotal << "\n";
        }
        if (st.hasGauge) {
            os << "# TYPE " << name << " gauge\n";
            os << "# HELP " << name << " \n";
            os << name << suffix << " " << st.gaugeLast << "\n";
        }
        if (st.hasHistogram) {
            os << "# TYPE " << name << " histogram\n";
            os << "# HELP " << name << " \n";
            std::vector<uint64_t> sorted = st.histogramSamplesMs;
            std::sort(sorted.begin(), sorted.end());
            os << name << "_samples" << suffix << " " << sorted.size() << "\n";
            os << name << "_p50_ms" << suffix << " " << nearestRank(sorted, 0.50) << "\n";
            os << name << "_p95_ms" << suffix << " " << nearestRank(sorted, 0.95) << "\n";
            os << name << "_p99_ms" << suffix << " " << nearestRank(sorted, 0.99) << "\n";
        }
    }
    os << "rejected_labels " << rejectedLabels_ << "\n";
    os << "rejected_series " << rejectedSeries_ << "\n";
    return os.str();
}

void PrometheusTelemetrySink::recordSpanLog(const SpanLogRecord& record)
{
    // D11：span 生产 sink 落 LogEvent（component=trace，含
    // trace_id/span_id/parent/duration_ms/error）。
    std::ostringstream os;
    os << "trace_id=" << record.spanId
       << " span_id=" << record.spanId
       << " parent=" << record.parentSpanId
       << " duration_ms=" << record.durationMs
       << " error=" << (record.errored ? 1 : 0);
    Logger::instance().log(INFO, "trace", "span", os.str());
}

std::string PrometheusTelemetrySink::seriesKey(const std::string& name,
                                               const std::vector<Telemetry::Label>& labels)
{
    std::vector<Telemetry::Label> sorted = labels;
    std::sort(sorted.begin(), sorted.end(), [](const Telemetry::Label& a,
                                                const Telemetry::Label& b) {
        const int c = std::strcmp(a.key, b.key);
        if (c != 0) {
            return c < 0;
        }
        return std::strcmp(a.value, b.value) < 0;
    });
    std::string key = name;
    for (size_t i = 0; i < sorted.size(); ++i) {
        key += '\x1f';
        key += sorted[i].key;
        key += '\x1e';
        key += sorted[i].value;
    }
    return key;
}

void PrometheusTelemetrySink::requireLabelsAllowedLocked(
    const std::vector<Telemetry::Label>& labels)
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

size_t PrometheusTelemetrySink::findOrCreateSeriesLocked(
    const std::string& name, const std::vector<Telemetry::Label>& labels)
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
    SeriesState st;
    st.labels = labels;
    st.name = name;
    series_.push_back(st);
    seriesIndex_[key] = idx;
    return idx;
}