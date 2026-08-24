#pragma once

// P5-00 阶段 B /metrics 端点（docs/tasks/P5-00.md 设计决定 D11）：mymuduo
// TcpServer 承载、共享主 loop、独立端口、非阻塞。GET / 返回 Prometheus 文本。
// 冻结用法见 tests/unit/PrometheusEndpointTest.cpp（GREEN 后即契约）。

#include "EventLoop.h"
#include "InetAddress.h"
#include "TcpServer.h"

#include <cstdint>

class PrometheusTelemetrySink;

class MetricsEndpoint {
public:
    MetricsEndpoint(EventLoop* loop, const InetAddress& addr, PrometheusTelemetrySink& sink);
    ~MetricsEndpoint();

    void start();

    // 实际绑定端口（监听后有效，>0）。
    uint16_t port() const;

    EventLoop* loop() const { return loop_; }

private:
    EventLoop* loop_;
    TcpServer server_;
    PrometheusTelemetrySink& sink_;
};
