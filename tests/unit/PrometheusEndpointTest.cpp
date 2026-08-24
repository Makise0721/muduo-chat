// P5-00 阶段 B RED：Prometheus 导出面与 /metrics 端点（docs/tasks/P5-00.md 设计
// 决定 D8/D10/D11、「阶段 B」RED 冻结清单）。
//
// 本文件 RED 引用尚不存在的 app/PrometheusTelemetrySink.hpp 与
// app/MetricsEndpoint.hpp → 编译失败（missing header）即合法 RED（沿阶段 A
// TelemetryContractTest 先例）。GREEN 时按本文件用法精确实现（命名/语义冻结）。
//
// 冻结契约（本文件逐测试断言，GREEN 必须满足）：
//   - PrometheusTelemetrySink : Telemetry::TelemetrySink 生产实现：
//       - render()：Prometheus 文本（# TYPE/# HELP + counter/gauge/histogram
//         系列行，label 编码正确）；
//       - label value 转义：`\` → `\\`、`"` → `\"`（沿 Prometheus 文本转义）；
//       - 拒绝系列可见计数（high-cardinality label 注入被拒并计数，沿阶段 A D4
//         语义：先 ++rejectedLabels 再抛 invalid_argument）；
//       - 有界样本预算（D10）：histogram 样本容器有界
//         kMaxPrometheusHistogramSamples，超限丢最旧——样本数与分位数正确；
//       - span 生产 sink 产出结构化日志（spanLogs()，LogEvent component=trace
//         同款字段：trace_id/span_id/parent/duration_ms/error）。
//   - MetricsEndpoint（mymuduo TcpServer 承载、独立端口、非阻塞、共享主 loop）：
//     GET / 返回 render() 文本，200；start()/port() 暴露实际绑定端口。
//   - 端点进程级测试按可行性保留：本文件含一个真实 socket GET smoke
//     （随机端口 + 事件循环，沿 TcpServerTest ServerThread 形态）；若 GREEN 实现
//     结构上难做端点级单测，则该用例登记为「留进程测试」，其余 render() 纯函数
//     用例必须单测覆盖。

#include "app/PrometheusTelemetrySink.hpp"  // RED：尚不存在 → 编译失败即合法 RED
#include "app/MetricsEndpoint.hpp"          // RED：尚不存在 → 编译失败即合法 RED
#include "app/Telemetry.hpp"                // 阶段 A 冻结头
#include "EventLoop.h"                      // 端点共享主 loop（mymuduo）
#include "InetAddress.h"                    // 随机端口绑定
#include "FakeClock.hpp"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <future>
#include <stdexcept>
#include <string>
#include <thread>
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

bool findLine(const std::string& text, const std::string& needle)
{
    return text.find(needle) != std::string::npos;
}

} // namespace

TEST(PrometheusEndpointTest, RenderEmitsTypeAndHelpLines)
{
    FakeClock clock;
    clock.set(kT0);
    PrometheusTelemetrySink sink(clock);
    sink.addCounter("chat_accepts", 3);

    const std::string text = sink.render();
    EXPECT_TRUE(findLine(text, "# TYPE chat_accepts counter"));
    EXPECT_TRUE(findLine(text, "# HELP chat_accepts "));

    // histogram 系列也有 TYPE/HELP。
    sink.recordHistogramSample("ack_latency_ms", 100);
    const std::string text2 = sink.render();
    EXPECT_TRUE(findLine(text2, "# TYPE ack_latency_ms histogram"));
    EXPECT_TRUE(findLine(text2, "# HELP ack_latency_ms "));
}

TEST(PrometheusEndpointTest, RenderEmitsSeriesLines)
{
    FakeClock clock;
    clock.set(kT0);
    PrometheusTelemetrySink sink(clock);

    // counter（无 label）：值行。
    sink.addCounter("chat_accepts", 5);
    std::string text = sink.render();
    EXPECT_TRUE(findLine(text, "chat_accepts 5"));

    // gauge：last-writer 值行。
    sink.setGauge("executor_queue_depth", 7);
    text = sink.render();
    EXPECT_TRUE(findLine(text, "executor_queue_depth 7"));

    // counter 带 label：label 编码进系列名（白名单 key/value）。
    std::vector<Telemetry::Label> labels;
    labels.push_back(makeLabel("delivery_state", "pending"));
    sink.addCounter("deliveries", 2, labels);
    text = sink.render();
    EXPECT_TRUE(findLine(text, "deliveries{delivery_state=\"pending\"} 2"));

    // histogram 系列行：样本数 + 分位数。
    sink.recordHistogramSample("ack_latency_ms", 100);
    sink.recordHistogramSample("ack_latency_ms", 200);
    sink.recordHistogramSample("ack_latency_ms", 300);
    text = sink.render();
    EXPECT_TRUE(findLine(text, "ack_latency_ms_samples 3"));
    EXPECT_TRUE(findLine(text, "ack_latency_ms_p50_ms 200"));
    EXPECT_TRUE(findLine(text, "ack_latency_ms_p95_ms 300"));
    EXPECT_TRUE(findLine(text, "ack_latency_ms_p99_ms 300"));
}

TEST(PrometheusEndpointTest, RenderEscapesLabelValues)
{
    FakeClock clock;
    clock.set(kT0);
    PrometheusTelemetrySink sink(clock);

    // 白名单 value 内出现 `\` 与 `"` 时按 Prometheus 转义：`\`→`\\`、`"`→`\"`。
    // 注：白名单 value 集本身有界（delivery_state ∈ {...}），这里直接验证
    // render 的转义函数对 label value 字符的处理（GREEN 实现经统一编码 helper）。
    std::vector<Telemetry::Label> labels;
    labels.push_back(makeLabel("delivery_state", "pending"));
    const std::string rendered = sink.renderLabelEscaped("pending");
    EXPECT_EQ(rendered, "pending");

    // 含引号/反斜杠的值被正确转义（防注入成新 label 或破坏文本格式）。
    EXPECT_EQ("a\\\"b\\\\c", sink.renderLabelEscaped("a\"b\\c"));
    EXPECT_EQ("", sink.renderLabelEscaped(""));
}

TEST(PrometheusEndpointTest, RejectedLabelsVisiblyCounted)
{
    FakeClock clock;
    clock.set(kT0);
    PrometheusTelemetrySink sink(clock);

    // high-cardinality label 注入被拒（抛 invalid_argument）且 rejectedLabels
    // 可见（沿阶段 A D4：先 ++rejectedLabels 再抛）。
    std::vector<Telemetry::Label> bad;
    bad.push_back(makeLabel("user_id", "42"));
    const uint64_t before = sink.snapshot().rejectedLabels;
    EXPECT_THROW(sink.addCounter("reject_probe", 1, bad), std::invalid_argument);
    EXPECT_EQ(before + 1, sink.snapshot().rejectedLabels);

    // render() 包含拒绝计数（可观测）。
    EXPECT_TRUE(findLine(sink.render(), "rejected_labels 1"));
}

TEST(PrometheusEndpointTest, BoundedHistogramSamples)
{
    FakeClock clock;
    clock.set(kT0);
    PrometheusTelemetrySink sink(clock);

    // 超过 kMaxPrometheusHistogramSamples 丢最旧（D10 有界预算）。
    const std::size_t cap = PrometheusTelemetrySink::kMaxPrometheusHistogramSamples;
    for (std::size_t i = 0; i < cap + 10; ++i) {
        sink.recordHistogramSample("bounded_hist", static_cast<uint64_t>(i + 1));
    }
    const std::string text = sink.render();
    EXPECT_TRUE(findLine(text, std::string("bounded_hist_samples ") +
                                 std::to_string(cap)));
    // 最旧 10 个样本被丢弃：剩余样本 {cap-9 .. cap+10}，p50 落在中间段，
    // p95/p99 靠近上界——只断言有界（样本数 = cap）与分位数单调非零，不锁死值。
    EXPECT_TRUE(findLine(text, "bounded_hist_p50_ms 0") == false);
    EXPECT_TRUE(findLine(text, "bounded_hist_p95_ms 0") == false);
}

TEST(PrometheusEndpointTest, SpanSinkEmitsStructuredLog)
{
    FakeClock clock;
    clock.set(kT0);
    PrometheusTelemetrySink sink(clock);

    // span 生产 sink：结构化日志（LogEvent component=trace 同款字段）。
    const uint64_t root = sink.beginSpan("deliver_message");
    clock.advance(30);
    sink.endSpan(root, false);
    const uint64_t child = sink.beginSpan("redis_locate", root);
    clock.advance(50);
    sink.endSpan(child, true);

    const std::vector<PrometheusTelemetrySink::SpanLogRecord> logs = sink.spanLogs();
    ASSERT_EQ(2u, logs.size());
    EXPECT_EQ("deliver_message", logs[0].name);
    EXPECT_EQ(0u, logs[0].parentSpanId);
    EXPECT_EQ(30, logs[0].durationMs);
    EXPECT_FALSE(logs[0].errored);
    EXPECT_EQ("redis_locate", logs[1].name);
    EXPECT_EQ(root, logs[1].parentSpanId);
    EXPECT_EQ(50, logs[1].durationMs);
    EXPECT_TRUE(logs[1].errored);
}

// P5-00 M-2：span 两容器有界（completedSpans_ + spanDurationsMs，上限
// kMaxRecordedSpans=4096，超限丢最旧，沿阶段 A kMaxRecordedSpans 纪律）。
TEST(PrometheusEndpointTest, SpanLogBufferBounded)
{
    FakeClock clock;
    clock.set(kT0);
    PrometheusTelemetrySink sink(clock);

    const std::size_t cap = PrometheusTelemetrySink::kMaxRecordedSpans;
    // 每个 span 时长 = i+1（advance 变化量），便于区分最旧/最新与分位数。
    for (std::size_t i = 0; i < cap + 10; ++i) {
        const uint64_t span = sink.beginSpan("deliver_message");
        clock.advance(static_cast<int64_t>(i) + 1);
        sink.endSpan(span, false);
    }

    // completedSpans_ 有界：恰好 cap 条，最旧 10 条被丢弃（spanId=1 不在列）。
    const std::vector<PrometheusTelemetrySink::SpanLogRecord> logs = sink.spanLogs();
    ASSERT_EQ(cap, logs.size());
    EXPECT_NE(1u, logs[0].spanId);
    EXPECT_EQ(static_cast<uint64_t>(cap + 10), logs[cap - 1].spanId);

    // spanDurationsMs 有界：保留样本 = {11..cap+10}，p50 = ceil(0.5*cap) + 10。
    // 若未裁剪（cap+10 样本全保留），p50 = ceil(0.5*(cap+10)) + ... 不同——此断言
    // 锁死"最旧 10 个时长样本已被丢弃"。
    const Telemetry::Snapshot snap = sink.snapshot();
    ASSERT_GE(snap.seriesCount, 1u);
    const uint64_t expectedP50 = static_cast<uint64_t>((cap + 1) / 2) + 10;
    EXPECT_EQ(expectedP50, snap.series[0].spanDurationP50Ms);
}

// 端点进程级 smoke：随机端口 + 共享主 loop 的 MetricsEndpoint，GET / 返回
// render() 文本（沿 TcpServerTest ServerThread 形态）。若 GREEN 实现结构上难做
// 端点级单测，登记留进程测试（见本文件头注释），本用例保持。
struct EndpointThread {
    std::promise<void> readyP;
    std::future<void> readyF;
    std::promise<void> endedP;
    std::future<void> endedF;
    MetricsEndpoint* ep = nullptr;
    std::thread t;

    EndpointThread(PrometheusTelemetrySink& sink)
        : readyF(readyP.get_future()),
          endedF(endedP.get_future()),
          t([this, &sink] {
              EventLoop l;
              MetricsEndpoint ep(&l, InetAddress(0), sink);
              this->ep = &ep;
              ep.start();
              readyP.set_value();
              l.loop();
              endedP.set_value();
          })
    {
        // 超时等待独立成员函数（构造函数内不能展开 fatal assertion；半完成态下
        // fatal 也无意义，见 GREEN 适配记录）。
    }

    // P5-00 L-7：就绪等待移出构造体（成员函数可展开 ASSERT_*）——超时后 ep 为
    // nullptr，调用方必须以 ASSERT 拦截，防后续解引用崩溃。
    void waitReady()
    {
        ASSERT_EQ(std::future_status::ready,
                  readyF.wait_for(std::chrono::seconds(10)));
    }

    ~EndpointThread()
    {
        if (ep != nullptr &&
            endedF.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            MetricsEndpoint* e = ep;
            e->loop()->quit();
        }
        t.join();
    }
};

TEST(PrometheusEndpointTest, EndpointRespondsText)
{
    FakeClock clock;
    clock.set(kT0);
    PrometheusTelemetrySink sink(clock);
    sink.addCounter("chat_accepts", 1);

    EndpointThread et(sink);
    et.waitReady();
    // P5-00 L-7：超时后 ep 为 nullptr，先 ASSERT 拦截再解引用。
    ASSERT_NE(nullptr, et.ep);
    const int port = et.ep->port();
    ASSERT_GT(port, 0);

    // 原始 socket GET /。
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0);
    sockaddr_in addr;
    bzero(&addr, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    ASSERT_EQ(0, connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr));
    const char* req = "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    ssize_t n = write(fd, req, strlen(req));
    ASSERT_GT(n, 0);
    char buf[4096];
    n = read(fd, buf, sizeof buf - 1);
    ASSERT_GT(n, 0);
    buf[n] = '\0';
    close(fd);

    const std::string resp(buf);
    EXPECT_TRUE(findLine(resp, "200"));
    EXPECT_TRUE(findLine(resp, "text/plain"));
    EXPECT_TRUE(findLine(resp, "chat_accepts 1"));
}
