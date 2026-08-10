#include "bench_stats.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/utsname.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace chatbench;

int connectTo(const std::string &host, int port)
{
    struct addrinfo hints;
    std::memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    std::string portStr = std::to_string(port);
    struct addrinfo *res = nullptr;
    if (getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res) != 0)
    {
        return -1;
    }
    int fd = -1;
    for (struct addrinfo *ai = res; ai != nullptr; ai = ai->ai_next)
    {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
        {
            continue;
        }
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
        {
            break;
        }
        ::close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

bool sendAll(int fd, const char *data, size_t len)
{
    size_t sent = 0;
    while (sent < len)
    {
        ssize_t n = ::send(fd, data + sent, len - sent, 0);
        if (n <= 0)
        {
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool recvAll(int fd, char *data, size_t len)
{
    size_t got = 0;
    while (got < len)
    {
        ssize_t n = ::recv(fd, data + got, len - got, 0);
        if (n <= 0)
        {
            return false;
        }
        got += static_cast<size_t>(n);
    }
    return true;
}

double nowMs()
{
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string timestampNow()
{
    char buf[32];
    std::time_t t = std::time(nullptr);
    std::tm tm;
    localtime_r(&t, &tm);
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S", &tm);
    return buf;
}

HostInfo collectHost()
{
    HostInfo info;
    FILE *f = std::fopen("/proc/cpuinfo", "r");
    if (f != nullptr)
    {
        char line[256];
        while (std::fgets(line, sizeof line, f) != nullptr)
        {
            if (std::strncmp(line, "model name", 10) == 0)
            {
                char *colon = std::strchr(line, ':');
                if (colon != nullptr)
                {
                    info.cpu_model = std::string(colon + 2);
                    while (!info.cpu_model.empty() && info.cpu_model.back() == '\n')
                    {
                        info.cpu_model.pop_back();
                    }
                    break;
                }
            }
        }
        std::fclose(f);
    }
    info.cpu_count = static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN));
    struct utsname uts;
    if (uname(&uts) == 0)
    {
        info.kernel = uts.release;
    }
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0)
    {
        info.ulimit_nofile = static_cast<uint64_t>(rl.rlim_cur);
    }
    return info;
}

BuildInfo collectBuild()
{
    BuildInfo info;
#ifdef GIT_COMMIT
    info.commit = GIT_COMMIT;
#endif
#ifdef CXX_FLAGS_STRING
    info.cxx_flags = CXX_FLAGS_STRING;
#endif
#ifdef BUILD_TYPE
    info.build_type = BUILD_TYPE;
#endif
    return info;
}

void runConnect(const Workload &wl, Result *result)
{
    Stats *latency = &result->latency_us;
    std::mutex m;
    std::vector<std::thread> threads;
    for (int i = 0; i < wl.connections; ++i)
    {
        threads.emplace_back([wl, result, latency, &m]
                             {
                                 double start = nowMs();
                                 int fd = connectTo(wl.host, wl.port);
                                 double elapsedUs = (nowMs() - start) * 1000.0;
                                 {
                                     std::lock_guard<std::mutex> lk(m);
                                     if (fd >= 0)
                                     {
                                         ++result->connections_ok;
                                         latency->add(elapsedUs);
                                     }
                                     else
                                     {
                                         ++result->connections_failed;
                                     }
                                 }
                                 if (fd >= 0)
                                 {
                                     ::close(fd);
                                 }
                             });
    }
    for (auto &t : threads)
    {
        t.join();
    }
}

void runEcho(const Workload &wl, Result *result)
{
    const std::string payload(static_cast<size_t>(wl.payload_size), 'e');
    std::mutex m;
    double start = nowMs();
    std::vector<std::thread> threads;
    for (int i = 0; i < wl.connections; ++i)
    {
        threads.emplace_back([wl, payload, result, &m]
                             {
                                 int fd = connectTo(wl.host, wl.port);
                                 if (fd < 0)
                                 {
                                     std::lock_guard<std::mutex> lk(m);
                                     ++result->connections_failed;
                                     return;
                                 }
                                 {
                                     std::lock_guard<std::mutex> lk(m);
                                     ++result->connections_ok;
                                 }
                                 std::vector<char> buf(payload.size());
                                 for (int j = 0; j < wl.messages; ++j)
                                 {
                                     double start = nowMs();
                                     if (!sendAll(fd, payload.data(), payload.size()) ||
                                         !recvAll(fd, buf.data(), buf.size()))
                                     {
                                         break;
                                     }
                                     double elapsedUs = (nowMs() - start) * 1000.0;
                                     {
                                         std::lock_guard<std::mutex> lk(m);
                                         result->latency_us.add(elapsedUs);
                                         ++result->messages_sent;
                                         ++result->messages_received;
                                     }
                                 }
                                 ::close(fd);
                             });
    }
    for (auto &t : threads)
    {
        t.join();
    }
    double elapsedMs = std::max(1.0, nowMs() - start);
    result->msg_per_sec =
        static_cast<double>(result->messages_sent) * 1000.0 / elapsedMs;
    result->throughput_mbps = static_cast<double>(result->messages_received) *
                              wl.payload_size * 8.0 / elapsedMs / 1000.0;
    result->bytes_sent =
        result->messages_sent * static_cast<uint64_t>(wl.payload_size);
    result->bytes_received =
        result->messages_received * static_cast<uint64_t>(wl.payload_size);
}

void runSlowConsumer(const Workload &wl, Result *result)
{
    const std::string payload(static_cast<size_t>(wl.payload_size), 's');
    const uint64_t burstBytes =
        static_cast<uint64_t>(wl.payload_size) * static_cast<uint64_t>(wl.messages);
    std::mutex m;
    std::vector<std::thread> threads;
    for (int i = 0; i < wl.connections; ++i)
    {
        threads.emplace_back([wl, payload, burstBytes, result, &m]
                             {
                                 int fd = connectTo(wl.host, wl.port);
                                 if (fd < 0)
                                 {
                                     std::lock_guard<std::mutex> lk(m);
                                     ++result->connections_failed;
                                     return;
                                 }
                                 {
                                     std::lock_guard<std::mutex> lk(m);
                                     ++result->connections_ok;
                                 }
                                  std::vector<char> chunk(4096);
                                  struct timeval tv;
                                  tv.tv_sec = 0;
                                  tv.tv_usec = 100000;
                                  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv,
                                             sizeof tv);
                                  uint64_t sent = 0;
                                  while (sent < burstBytes)
                                  {
                                      size_t len = std::min(burstBytes - sent, payload.size());
                                      if (!sendAll(fd, payload.data(), len))
                                      {
                                          break;
                                      }
                                      sent += len;
                                  }
                                  uint64_t received = 0;
                                  double deadline = nowMs() + wl.duration_ms;
                                  while (received < sent && nowMs() < deadline)
                                  {
                                      ssize_t n = ::recv(fd, chunk.data(), chunk.size(), 0);
                                      if (n < 0 &&
                                          (errno == EAGAIN || errno == EWOULDBLOCK))
                                      {
                                          continue;  // 接收超时：回到 deadline 检查
                                      }
                                      if (n <= 0)
                                      {
                                          break;
                                      }
                                      received += static_cast<uint64_t>(n);
                                      std::this_thread::sleep_for(
                                          std::chrono::milliseconds(5));
                                  }
                                 {
                                     std::lock_guard<std::mutex> lk(m);
                                     result->messages_sent += sent / wl.payload_size;
                                     result->messages_received += received / wl.payload_size;
                                     result->bytes_sent += sent;
                                     result->bytes_received += received;
                                     if (received < sent)
                                     {
                                         ++result->early_closes;
                                     }
                                 }
                                 ::close(fd);
                             });
    }
    for (auto &t : threads)
    {
        t.join();
    }
}

struct Options
{
    std::string host;
    int port = 0;
    std::string scenario;
    int connections = 1;
    int messages = 100;
    int payload_size = 64;
    int duration_ms = 2000;
};

bool parseArgs(int argc, char **argv, Options *opts)
{
    if (argc < 4)
    {
        return false;
    }
    opts->host = argv[1];
    opts->port = std::atoi(argv[2]);
    opts->scenario = argv[3];
    for (int i = 4; i + 1 < argc; i += 2)
    {
        std::string key = argv[i];
        int value = std::atoi(argv[i + 1]);
        if (key == "--connections")
        {
            opts->connections = value;
        }
        else if (key == "--messages")
        {
            opts->messages = value;
        }
        else if (key == "--payload-size")
        {
            opts->payload_size = value;
        }
        else if (key == "--duration-ms")
        {
            opts->duration_ms = value;
        }
        else
        {
            return false;
        }
    }
    return opts->scenario == "connect" || opts->scenario == "echo" ||
           opts->scenario == "slow-consumer";
}

} // namespace

int main(int argc, char **argv)
{
    Options opts;
    if (!parseArgs(argc, argv, &opts))
    {
        std::fprintf(stderr,
                     "usage: chat-bench <host> <port> <connect|echo|slow-consumer> "
                     "[--connections N] [--messages M] [--payload-size B] [--duration-ms T]\n");
        return 1;
    }

    BenchReport report;
    report.timestamp = timestampNow();
    report.host = collectHost();
    report.build = collectBuild();
    report.workload.host = opts.host;
    report.workload.port = opts.port;
    report.workload.scenario = opts.scenario;
    report.workload.connections = opts.connections;
    report.workload.messages = opts.messages;
    report.workload.payload_size = opts.payload_size;
    report.workload.duration_ms = opts.duration_ms;

    if (opts.scenario == "connect")
    {
        runConnect(report.workload, &report.result);
    }
    else if (opts.scenario == "echo")
    {
        runEcho(report.workload, &report.result);
    }
    else
    {
        runSlowConsumer(report.workload, &report.result);
    }

    std::cout << report.toJson().dump(2) << std::endl;
    return 0;
}
