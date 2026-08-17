#include "ChatServer.hpp"
#include "ChatService.hpp"
#include "app/Config.hpp"
#include "app/ProtocolCodec.hpp"  // P3-08 configureReliableMessaging（可靠消息参数注入）
#include "db/ConnectionPool.hpp"
#include <chrono>
#include <iostream>
#include <sstream>
#include <signal.h>
#include <unistd.h>
#include <sys/socket.h>
#include <fcntl.h>

namespace {

int gSignalFds[2];
static bool gShuttingDown = false;

void signalHandler(int sig) {
    char c = (sig == SIGUSR1) ? 2 : 1;
    ssize_t n = ::write(gSignalFds[1], &c, 1);
    (void)n;
}

int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// P2-09 关闭顺序：STOP_ACCEPT -> drain network（DRAINED/DRAIN_TIMEOUT）->
// EXECUTOR_SHUTDOWN（stop submissions + drain）-> POOL_SHUTDOWN -> QUIT_LOOPS -> EXITED。
// finishShutdown 在 loop 线程执行：executor shutdown 的 worker join 期间 completion
// 仅 queueInLoop 入队不阻塞；pool shutdown 在 worker join 后（租约已全归还）立即返回。
struct ShutdownFlow {
    bool forced = false;
    bool done = false;
    int64_t startMs = 0;
    int64_t forceAtMs = 0;
};

void finishShutdown(EventLoop* loop, ShutdownFlow* flow)
{
    if (flow->done) {
        return;
    }
    flow->done = true;
    std::cout << "EXECUTOR_SHUTDOWN" << std::endl;
    ChatService::instance()->shutdownApp();
    std::cout << "POOL_SHUTDOWN" << std::endl;
    ConnectionPool::getInstance().shutdown();
    std::cout << "QUIT_LOOPS" << std::endl;
    loop->quit();
}

void beginShutdown(EventLoop* loop, ChatServer* v1, ChatServer* v2,
                   int timeoutMs, ShutdownFlow* flow)
{
    if (gShuttingDown) {
        return;
    }
    gShuttingDown = true;
    std::cout << "STOP_ACCEPT" << std::endl;
    v1->stopAccept();
    v2->stopAccept();
    flow->startMs = nowMs();

    loop->runEvery(50, [loop, v1, v2, timeoutMs, flow]() {
        int pending = v1->connectionCount() + v2->connectionCount();
        if (pending == 0) {
            if (!flow->forced) {
                std::cout << "DRAINED pending=0" << std::endl;
            }
            finishShutdown(loop, flow);
            return;
        }
        if (flow->forced && nowMs() - flow->forceAtMs > timeoutMs) {
            // forceClose 后仍不清零：防御性强制（连接卡死）。
            finishShutdown(loop, flow);
        }
    });

    loop->runAfter(timeoutMs, [loop, v1, v2, flow]() {
        int pending = v1->connectionCount() + v2->connectionCount();
        std::cout << "DRAIN_TIMEOUT pending=" << pending << std::endl;
        v1->forceCloseAllConnections();
        v2->forceCloseAllConnections();
        flow->forced = true;
        flow->forceAtMs = nowMs();
    });
}

} // namespace

int main(int argc, char** argv) {
    // 位置参数：ip port [threadNum]；可选 --config <path>（任意位置）。
    const char* posIp = nullptr;
    const char* posPort = nullptr;
    const char* posThreads = nullptr;
    std::string configPath;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--config") {
            if (i + 1 >= argc) {
                std::cerr << "config error: missing value for --config" << std::endl;
                exit(1);
            }
            configPath = argv[++i];
        } else if (arg.size() > 0 && arg[0] == '-' && arg.size() > 1) {
            std::cerr << "config error: unknown option '" << arg << "'" << std::endl;
            exit(1);
        } else if (!posIp) {
            posIp = argv[i];
        } else if (!posPort) {
            posPort = argv[i];
        } else if (!posThreads) {
            posThreads = argv[i];
        } else {
            std::cerr << "config error: too many positional arguments" << std::endl;
            exit(1);
        }
    }
    if (!posIp || !posPort) {
        std::cerr << "command invalid! example: ./ChatServer 127.0.0.1 6000"
                     " [threadNum] [--config config.json]" << std::endl;
        exit(-1);
    }

    AppConfig cfg;
    std::string err;
    if (!configPath.empty() && !config::loadConfigFile(configPath, &cfg, &err)) {
        std::cerr << err << std::endl;
        exit(1);
    }
    if (!config::applyCliOverrides(&cfg, posIp, posPort, posThreads, &err)) {
        std::cerr << err << std::endl;
        exit(1);
    }
    config::applyEnvOverrides(&cfg);
    // P3-08：可靠消息生产参数注入（缺省 = 卡冻结值，见 AppConfig.reliable）。
    configureReliableMessaging(cfg.reliable);
    // P3-09：outbox relay 生产参数注入（缺省 = 卡冻结值，见 AppConfig.outbox）。
    configureOutboxRelay(cfg.outbox);
    // P4-05：Gateway/presence/kafka/consumer 生产参数注入（缺省 = 卡冻结值，
    // 见 AppConfig.gateway；GatewayId 默认 1，冻结）。
    configureGateway(cfg.gateway);

    auto& connPool = ConnectionPool::getInstance();
    connPool.init(cfg.db.host, cfg.db.user, cfg.db.password, cfg.db.dbname,
                  cfg.db.port, cfg.db.poolSize);

    // 池完全建不起来（DB 不可达）是启动错误：fail-fast，不起服务。
    ConnectionPool::Metrics poolMetrics = connPool.metrics();
    if (poolMetrics.idle + poolMetrics.active == 0) {
        std::cerr << "pool init failed: cannot connect to MySQL at "
                  << cfg.db.host << ":" << cfg.db.port << std::endl;
        exit(1);
    }

    int shutdownTimeoutMs = 5000;
    const char* timeoutEnv = getenv("CHAT_SHUTDOWN_TIMEOUT_MS");
    if (timeoutEnv) {
        int v = atoi(timeoutEnv);
        if (v > 0) {
            shutdownTimeoutMs = v;
        }
    }

    EventLoop loop;
    InetAddress addr(cfg.v1.port, cfg.v1.ip.c_str());
    std::cout << "Server starting on " << cfg.v1.ip << ":" << cfg.v1.port
              << " (v1 newline JSON, threads=" << cfg.v1.threads << ")" << std::endl;
    ChatServer server(&loop, addr, "ChatServer", ProtocolCodec::LegacyLine);
    server.setThreadNum(cfg.v1.threads);

    InetAddress v2Addr(cfg.v2.port, cfg.v2.ip.c_str());
    std::cout << "Server starting on " << cfg.v2.ip << ":" << cfg.v2.port
              << " (v2 binary, threads=" << cfg.v1.threads << ")" << std::endl;
    ChatServer v2Server(&loop, v2Addr, "ChatServerV2", ProtocolCodec::BinaryFrame);
    v2Server.setThreadNum(cfg.v1.threads);

    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, gSignalFds) < 0) {
        std::cerr << "socketpair failed" << std::endl;
        exit(-1);
    }
    for (int i = 0; i < 2; ++i) {
        int flags = fcntl(gSignalFds[i], F_GETFL, 0);
        fcntl(gSignalFds[i], F_SETFL, flags | O_NONBLOCK | FD_CLOEXEC);
    }
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    signal(SIGUSR1, signalHandler);
    signal(SIGPIPE, SIG_IGN);

    ShutdownFlow flow;
    Channel sigChannel(&loop, gSignalFds[0]);
    sigChannel.setReadCallback([&loop, &server, &v2Server, shutdownTimeoutMs, &flow](Timestamp) {
        char buf[16];
        ssize_t n;
        while ((n = ::read(gSignalFds[0], buf, sizeof buf)) > 0) {
            for (ssize_t i = 0; i < n; ++i) {
                if (buf[i] == 2) {
                    // P2-10：SIGUSR1 = 快照一次运行期指标（pool + executor）。
                    // 整行拼装后单次输出，避免与异步 Logger 的 stdout 写入交错。
                    ConnectionPool::Metrics m = ConnectionPool::getInstance().metrics();
                    std::ostringstream os;
                    os << "METRICS pool_total=" << m.total
                       << " pool_idle=" << m.idle
                       << " pool_active=" << m.active
                       << " executor_queue=" << ChatService::instance()->executorQueueDepth()
                       << " executor_drop_full=" << ChatService::instance()->executorDroppedFull()
                       << " executor_drop_shutdown=" << ChatService::instance()->executorDroppedShutdown();
                    // P3-12：同 METRICS 行追加可靠消息字段（整行单次输出保持）。
                    os << " " << reliableMetricsLine();
                    std::cout << os.str() << std::endl;
                } else {
                    beginShutdown(&loop, &server, &v2Server, shutdownTimeoutMs, &flow);
                }
            }
        }
    });
    sigChannel.enableReading();

    std::cout << "Server started, entering event loop" << std::endl;
    ChatService::instance()->bindLoop(&loop, cfg.executor.workers,
                                      cfg.executor.queueCapacity);
    // P4-05：renew 调度（D4：窗口 = TTL/2，生产 TTL 30s → 15s；main loop
    // runEvery 驱动，卡冻结载体；wiring 未构造 no-op）。
    loop.runEvery(cfg.gateway.presence.ttlMs / 2, renewAllPresence);
    server.start();
    v2Server.start();
    loop.loop();

    std::cout << "EXITED" << std::endl;
    return 0;
}
