#include "app/MetricsEndpoint.hpp"
#include "app/PrometheusTelemetrySink.hpp"

#include "Buffer.h"

#include <sstream>
#include <string>

namespace {

void handleMetricsRequest(const TcpConnectionPtr& conn, Buffer* buf, PrometheusTelemetrySink& sink)
{
    const std::string data = buf->retrieveAllAsString();
    if (data.find("GET ") == std::string::npos) {
        return;
    }
    const std::string body = sink.render();
    std::ostringstream os;
    os << "HTTP/1.1 200 OK\r\n"
       << "Content-Type: text/plain\r\n"
       << "Content-Length: " << body.size() << "\r\n"
       << "Connection: close\r\n"
       << "\r\n"
       << body;
    conn->send(os.str());
    conn->shutdown();
}

} // namespace

MetricsEndpoint::MetricsEndpoint(EventLoop* loop, const InetAddress& addr,
                                 PrometheusTelemetrySink& sink)
    : loop_(loop),
      server_(loop, addr, "MetricsEndpoint"),
      sink_(sink)
{
    server_.setConnectionCallback([](const TcpConnectionPtr&) {});
    server_.setMessageCallback([this](const TcpConnectionPtr& conn, Buffer* buf, Timestamp) {
        handleMetricsRequest(conn, buf, sink_);
    });
}

MetricsEndpoint::~MetricsEndpoint()
{
}

void MetricsEndpoint::start()
{
    server_.setThreadNum(0);
    server_.start();
}

uint16_t MetricsEndpoint::port() const
{
    return static_cast<uint16_t>(server_.listenPort());
}
