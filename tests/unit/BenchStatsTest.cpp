#include "bench_stats.h"

#include <gtest/gtest.h>

namespace {

using chatbench::BenchReport;
using chatbench::Stats;

} // namespace

TEST(BenchStatsTest, PercentileGoldenValues)
{
    Stats s;
    for (int i = 1; i <= 10; ++i)
    {
        s.add(static_cast<double>(i));
    }
    EXPECT_EQ(10u, s.count());
    EXPECT_DOUBLE_EQ(5.0, s.percentile(0.50));
    EXPECT_DOUBLE_EQ(9.0, s.percentile(0.90));
    EXPECT_DOUBLE_EQ(10.0, s.percentile(0.99));
    EXPECT_DOUBLE_EQ(10.0, s.percentile(0.999));
    EXPECT_DOUBLE_EQ(10.0, s.max());
}

TEST(BenchStatsTest, PercentileEmpty)
{
    Stats s;
    EXPECT_EQ(0u, s.count());
    EXPECT_DOUBLE_EQ(0.0, s.percentile(0.50));
    EXPECT_DOUBLE_EQ(0.0, s.max());
}

TEST(BenchStatsTest, PercentileSingleSample)
{
    Stats s;
    s.add(42.0);
    EXPECT_DOUBLE_EQ(42.0, s.percentile(0.50));
    EXPECT_DOUBLE_EQ(42.0, s.percentile(0.999));
    EXPECT_DOUBLE_EQ(42.0, s.max());
}

TEST(BenchStatsTest, ReportSchema)
{
    BenchReport report;
    report.timestamp = "2026-08-09T12:00:00";
    report.host.cpu_model = "Test CPU";
    report.host.cpu_count = 8;
    report.host.kernel = "6.6-test";
    report.host.ulimit_nofile = 1024;
    report.build.commit = "abc1234";
    report.build.cxx_flags = "-g";
    report.build.build_type = "Debug";
    report.workload.host = "127.0.0.1";
    report.workload.port = 8000;
    report.workload.scenario = "echo";
    report.workload.connections = 2;
    report.workload.messages = 10;
    report.workload.payload_size = 64;
    report.workload.duration_ms = 2000;
    report.result.connections_ok = 2;
    report.result.messages_sent = 20;
    report.result.messages_received = 20;
    report.result.bytes_sent = 1280;
    report.result.bytes_received = 1280;
    report.result.early_closes = 1;
    report.result.latency_us.add(100.0);
    report.result.latency_us.add(200.0);

    nlohmann::json j = report.toJson();
    EXPECT_TRUE(j["tool"].is_string());
    EXPECT_EQ("chat-bench", j["tool"]);
    EXPECT_TRUE(j["timestamp"].is_string());
    EXPECT_TRUE(j["host"].is_object());
    EXPECT_TRUE(j["host"]["cpu_model"].is_string());
    EXPECT_TRUE(j["host"]["cpu_count"].is_number());
    EXPECT_TRUE(j["host"]["kernel"].is_string());
    EXPECT_TRUE(j["host"]["ulimit_nofile"].is_number());
    EXPECT_TRUE(j["build"].is_object());
    EXPECT_TRUE(j["build"]["commit"].is_string());
    EXPECT_TRUE(j["build"]["cxx_flags"].is_string());
    EXPECT_TRUE(j["build"]["build_type"].is_string());
    EXPECT_TRUE(j["workload"].is_object());
    EXPECT_TRUE(j["workload"]["host"].is_string());
    EXPECT_TRUE(j["workload"]["port"].is_number());
    EXPECT_TRUE(j["workload"]["scenario"].is_string());
    EXPECT_TRUE(j["workload"]["connections"].is_number());
    EXPECT_TRUE(j["workload"]["messages"].is_number());
    EXPECT_TRUE(j["workload"]["payload_size"].is_number());
    EXPECT_TRUE(j["workload"]["duration_ms"].is_number());
    EXPECT_TRUE(j["result"].is_object());
    EXPECT_TRUE(j["result"]["connections_ok"].is_number());
    EXPECT_TRUE(j["result"]["connections_failed"].is_number());
    EXPECT_TRUE(j["result"]["messages_sent"].is_number());
    EXPECT_TRUE(j["result"]["messages_received"].is_number());
    EXPECT_TRUE(j["result"]["bytes_sent"].is_number());
    EXPECT_TRUE(j["result"]["bytes_received"].is_number());
    EXPECT_TRUE(j["result"]["early_closes"].is_number());
    EXPECT_TRUE(j["result"]["latency_us"].is_object());
    EXPECT_TRUE(j["result"]["latency_us"]["p50"].is_number());
    EXPECT_TRUE(j["result"]["latency_us"]["p95"].is_number());
    EXPECT_TRUE(j["result"]["latency_us"]["p99"].is_number());
    EXPECT_TRUE(j["result"]["latency_us"]["p999"].is_number());
    EXPECT_TRUE(j["result"]["latency_us"]["max"].is_number());
    EXPECT_TRUE(j["result"]["msg_per_sec"].is_number());
    EXPECT_TRUE(j["result"]["throughput_mbps"].is_number());
}
