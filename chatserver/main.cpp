#include "ChatServer.hpp"
#include "ChatService.hpp"
#include "app/Config.hpp"
#include "app/MetricsEndpoint.hpp"
#include "app/MetricsSnapshot.hpp"
#include "app/PrometheusTelemetrySink.hpp"
#include "app/ProtocolCodec.hpp"  // P3-08 configureReliableMessaging（可靠消息参数注入）
#include "db/ConnectionPool.hpp"
#include <chrono>
#include <iostream>
#include <memory>
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

// P5-00 阶段 B（H-1）：统一快照同源取数——SIGUSR1 METRICS 行与 /metrics 生产
// 都消费同一 MetricsSnapshot::snapshot()（缺口字段取真实值，经领域 wiring 暴露
// 的 getter：fencingConflicts/consumerLag/rebalanceCount 与 ChatServer/TcpServer
// acceptReasonCounts、EventLoop loopLagProbeMs、连接 outstanding 聚合）。
MetricsSnapshot::Snapshot buildUnifiedSnapshot(EventLoop* loop, ChatServer* v1,
                                               ChatServer* v2,
                                               PrometheusTelemetrySink& sink)
{
    ConnectionPool::Metrics m = ConnectionPool::getInstance().metrics();
    MetricsSnapshot::PoolStats pool;
    pool.total = static_cast<uint64_t>(m.total);
    pool.idle = static_cast<uint64_t>(m.idle);
    pool.active = static_cast<uint64_t>(m.active);
    MetricsSnapshot::ExecutorStats exec;
    exec.queueDepth = static_cast<uint64_t>(ChatService::instance()->executorQueueDepth());
    exec.dropped = ChatService::instance()->executorDroppedFull()
                   + ChatService::instance()->executorDroppedShutdown();

    MetricsSnapshot::GapStats gap;
    gap.loopLagMs = loop->loopLagProbeMs();
    const TcpServer::AcceptReasonCounts r1 = v1->acceptReasonCounts();
    const TcpServer::AcceptReasonCounts r2 = v2->acceptReasonCounts();
    gap.acceptCount = r1.accept + r2.accept;
    gap.acceptRateReject = r1.rateReject + r2.rateReject;
    gap.acceptMaxReject = r1.maxReject + r2.maxReject;
    gap.acceptEmfileRecover = r1.emfileRecover + r2.emfileRecover;
    gap.outstandingBytes = v1->totalOutstandingBytes() + v2->totalOutstandingBytes();
    gap.fencingConflicts = presenceFencingConflicts();
    gap.consumerLag = consumerLag();
    gap.rebalanceCount = rebalanceCount();

    return MetricsSnapshot::snapshot(ReliableMessageMetrics::instanceSnapshot(),
                                     pool, exec, sink.snapshot(), gap);
}

// SIGUSR1 METRICS 行行尾缺口字段（统一快照 Snapshot 字段；既有 pool_*/executor_*/
// reliable_* 字段格式原样保留，仅行尾追加）。
void appendGapFieldsFromSnapshot(std::ostringstream& os,
                                 const MetricsSnapshot::Snapshot& snap)
{
    os << " loop_lag_ms=" << snap.loopLagMs
       << " accept_count=" << snap.acceptCount
       << " accept_rate_reject=" << snap.acceptRateReject
       << " accept_max_reject=" << snap.acceptMaxReject
       << " accept_emfile_recover=" << snap.acceptEmfileRecover
       << " outstanding_bytes=" << snap.outstandingBytes
       << " fencing_conflicts=" << snap.fencingConflicts
       << " consumer_lag=" << snap.consumerLag
       << " rebalance_count=" << snap.rebalanceCount;
}

// /metrics 生产面：统一快照全标量 → sink gauge（reliable_*/pool_*/executor_* +
// 缺口字段，不只 lag+rejected）。counter 类字段以瞬时快照值 last-writer 暴露。
void publishSnapshotToSink(PrometheusTelemetrySink& sink,
                           const MetricsSnapshot::Snapshot& snap)
{
    sink.setGauge("pool_total", static_cast<int64_t>(snap.poolTotal));
    sink.setGauge("pool_idle", static_cast<int64_t>(snap.poolIdle));
    sink.setGauge("pool_active", static_cast<int64_t>(snap.poolActive));
    sink.setGauge("executor_queue", static_cast<int64_t>(snap.executorQueueDepth));
    sink.setGauge("executor_dropped", static_cast<int64_t>(snap.executorDropped));
    sink.setGauge("reliable_accepts", static_cast<int64_t>(snap.accepts));
    sink.setGauge("reliable_duplicates", static_cast<int64_t>(snap.duplicates));
    sink.setGauge("reliable_conflicts", static_cast<int64_t>(snap.conflicts));
    sink.setGauge("reliable_rejected_too_many_recipients",
                  static_cast<int64_t>(snap.rejectedTooManyRecipients));
    sink.setGauge("reliable_created_deliveries",
                  static_cast<int64_t>(snap.createdDeliveries));
    sink.setGauge("reliable_pending", static_cast<int64_t>(snap.pending));
    sink.setGauge("reliable_inflight", static_cast<int64_t>(snap.inflight));
    sink.setGauge("reliable_acked", static_cast<int64_t>(snap.acked));
    sink.setGauge("reliable_expired", static_cast<int64_t>(snap.expired));
    sink.setGauge("reliable_attempts", static_cast<int64_t>(snap.attempts));
    sink.setGauge("reliable_retries", static_cast<int64_t>(snap.retries));
    sink.setGauge("reliable_legacy_mode", static_cast<int64_t>(snap.legacyModeCount));
    sink.setGauge("reliable_outbox_lag", static_cast<int64_t>(snap.outboxLag));
    sink.setGauge("reliable_outbox_poison", static_cast<int64_t>(snap.outboxPoison));
    sink.setGauge("reliable_ack_latency_samples",
                  static_cast<int64_t>(snap.ackLatencySamples));
    sink.setGauge("reliable_ack_latency_p50_ms",
                  static_cast<int64_t>(snap.ackLatencyP50Ms));
    sink.setGauge("reliable_ack_latency_p95_ms",
                  static_cast<int64_t>(snap.ackLatencyP95Ms));
    sink.setGauge("reliable_ack_latency_p99_ms",
                  static_cast<int64_t>(snap.ackLatencyP99Ms));
    sink.setGauge("reliable_oldest_pending_age_ms", snap.oldestPendingAgeMs);
    sink.setGauge("consumer_seen_conversations",
                  static_cast<int64_t>(snap.consumerSeenConversations));
    sink.setGauge("loop_lag_ms", snap.loopLagMs);
    sink.setGauge("accept_count", static_cast<int64_t>(snap.acceptCount));
    sink.setGauge("accept_rate_reject", static_cast<int64_t>(snap.acceptRateReject));
    sink.setGauge("accept_max_reject", static_cast<int64_t>(snap.acceptMaxReject));
    sink.setGauge("accept_emfile_recover",
                  static_cast<int64_t>(snap.acceptEmfileRecover));
    sink.setGauge("outstanding_bytes", static_cast<int64_t>(snap.outstandingBytes));
    sink.setGauge("fencing_conflicts", static_cast<int64_t>(snap.fencingConflicts));
    sink.setGauge("consumer_lag", static_cast<int64_t>(snap.consumerLag));
    sink.setGauge("rebalance_count", static_cast<int64_t>(snap.rebalanceCount));
}

// P5-00 阶段 B：/metrics 生产时钟适配器（mymuduo-safe，不引入领域 class Clock）。
struct MetricsClock {
    int64_t nowMs() const
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
};

// P5-00 阶段 B：metrics 生产接线（cfg.metrics 注入）。enabled=true 时创建第三个
// TcpServer（MetricsEndpoint，共享主 loop、setThreadNum(0)）并 start；同时注册
// 统一快照 runEvery 采样 → sink gauge（同源，与 SIGUSR1 行一致）。disabled 返回
// nullptr。
MetricsEndpoint* configureMetrics(EventLoop* loop, const MetricsConfig& cfg,
                                  PrometheusTelemetrySink& sink,
                                  ChatServer* v1, ChatServer* v2)
{
    if (!cfg.enabled) {
        return nullptr;
    }
    MetricsEndpoint* ep = new MetricsEndpoint(
        loop, InetAddress(cfg.port, "127.0.0.1"), sink);
    ep->start();
    loop->runEvery(1000, [loop, &sink, v1, v2] {
        publishSnapshotToSink(sink, buildUnifiedSnapshot(loop, v1, v2, sink));
    });
    return ep;
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

    // P5-00 阶段 B：metrics 生产 sink（SIGUSR1 同源取数与 /metrics 共用；构造
    // 先于 sigChannel 注册，lambda 捕获引用在 loop.loop() 执行期有效）。
    MetricsClock metricsClock;
    PrometheusTelemetrySink metricsSink(metricsClock);

    ShutdownFlow flow;
    Channel sigChannel(&loop, gSignalFds[0]);
    sigChannel.setReadCallback([&loop, &server, &v2Server, &metricsSink,
                                shutdownTimeoutMs, &flow](Timestamp) {
        char buf[16];
        ssize_t n;
        while ((n = ::read(gSignalFds[0], buf, sizeof buf)) > 0) {
            for (ssize_t i = 0; i < n; ++i) {
                if (buf[i] == 2) {
                    // P2-10：SIGUSR1 = 快照一次运行期指标（pool + executor）。
                    // 整行拼装后单次输出，避免与异步 Logger 的 stdout 写入交错。
                    // P5-00 阶段 B（H-1）：统一快照同源——行尾缺口字段取真实值
                    // （与 /metrics 生产同一 MetricsSnapshot::snapshot()），既有
                    // pool_*/executor_*/reliable_* 字段格式零回退。
                    const MetricsSnapshot::Snapshot snap =
                        buildUnifiedSnapshot(&loop, &server, &v2Server, metricsSink);
                    std::ostringstream os;
                    os << "METRICS pool_total=" << snap.poolTotal
                       << " pool_idle=" << snap.poolIdle
                       << " pool_active=" << snap.poolActive
                       << " executor_queue=" << snap.executorQueueDepth
                       << " executor_drop_full=" << ChatService::instance()->executorDroppedFull()
                       << " executor_drop_shutdown=" << ChatService::instance()->executorDroppedShutdown();
                    // P3-12：同 METRICS 行追加可靠消息字段（整行单次输出保持）。
                    os << " " << reliableMetricsLine();
                    // P5-00 阶段 B（H-1）：统一快照同源缺口字段（行尾追加，格式零回退）。
                    appendGapFieldsFromSnapshot(os, snap);
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
    // P5-00 阶段 B：/metrics 端点（第三个 TcpServer 共享主 loop、setThreadNum(0)，
    // metrics.enabled=true 时启动；disabled → nullptr no-op）。统一快照同源采样。
    std::unique_ptr<MetricsEndpoint> metricsServer(
        configureMetrics(&loop, cfg.metrics, metricsSink, &server, &v2Server));
    loop.loop();

    std::cout << "EXITED" << std::endl;
    return 0;
}
