#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "json.hpp"

namespace chatbench {

struct Stats
{
    std::vector<double> samples;

    void add(double value) { samples.push_back(value); }

    size_t count() const { return samples.size(); }

    double percentile(double p) const
    {
        if (samples.empty())
        {
            return 0.0;
        }
        std::vector<double> sorted = samples;
        std::sort(sorted.begin(), sorted.end());
        size_t idx = static_cast<size_t>(std::ceil(p * sorted.size())) - 1;
        if (idx >= sorted.size())
        {
            idx = sorted.size() - 1;
        }
        return sorted[idx];
    }

    double max() const
    {
        if (samples.empty())
        {
            return 0.0;
        }
        return *std::max_element(samples.begin(), samples.end());
    }
};

struct HostInfo
{
    std::string cpu_model;
    int cpu_count = 0;
    std::string kernel;
    uint64_t ulimit_nofile = 0;
};

struct BuildInfo
{
    std::string commit;
    std::string cxx_flags;
    std::string build_type;
};

struct Workload
{
    std::string host;
    int port = 0;
    std::string scenario;
    int connections = 0;
    int messages = 0;
    int payload_size = 0;
    int duration_ms = 0;
};

struct Result
{
    int connections_ok = 0;
    int connections_failed = 0;
    uint64_t messages_sent = 0;
    uint64_t messages_received = 0;
    Stats latency_us;
    double msg_per_sec = 0.0;
    double throughput_mbps = 0.0;
};

struct BenchReport
{
    std::string timestamp;
    HostInfo host;
    BuildInfo build;
    Workload workload;
    Result result;

    nlohmann::json toJson() const
    {
        nlohmann::json j;
        j["tool"] = "chat-bench";
        j["timestamp"] = timestamp;
        j["host"] = {
            {"cpu_model", host.cpu_model},
            {"cpu_count", host.cpu_count},
            {"kernel", host.kernel},
            {"ulimit_nofile", host.ulimit_nofile},
        };
        j["build"] = {
            {"commit", build.commit},
            {"cxx_flags", build.cxx_flags},
            {"build_type", build.build_type},
        };
        j["workload"] = {
            {"host", workload.host},
            {"port", workload.port},
            {"scenario", workload.scenario},
            {"connections", workload.connections},
            {"messages", workload.messages},
            {"payload_size", workload.payload_size},
            {"duration_ms", workload.duration_ms},
        };
        j["result"] = {
            {"connections_ok", result.connections_ok},
            {"connections_failed", result.connections_failed},
            {"messages_sent", result.messages_sent},
            {"messages_received", result.messages_received},
            {"latency_us",
             {
                 {"p50", result.latency_us.percentile(0.50)},
                 {"p95", result.latency_us.percentile(0.95)},
                 {"p99", result.latency_us.percentile(0.99)},
                 {"p999", result.latency_us.percentile(0.999)},
                 {"max", result.latency_us.max()},
             }},
            {"msg_per_sec", result.msg_per_sec},
            {"throughput_mbps", result.throughput_mbps},
        };
        return j;
    }
};

} // namespace chatbench
