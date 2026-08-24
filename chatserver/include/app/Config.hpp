#pragma once

#include <cstdint>
#include <string>

// P2-09 配置：默认值内建；--config JSON 文件可覆盖；argv 位置参数（ip port
// [threads]）与 DB_PASSWORD env 再覆盖。任何非法配置 fail-fast（返回错误字符串，
// 调用方 stderr + exit(1)，不起服务）。

// P3-08 冻结参数（docs/tasks/P3-08.md RED 前定案）：生产默认 = 卡冻结值；
// 测试注入小值只经构造参数，绝不作为生产默认。生产配置经 AppConfig.reliable
// （P2-09 JSON "reliable" 段）→ ProtocolCodec wiring 注入；测试不经 config 文件。
// 本头保持 mymuduo-safe（不引入 app/DomainTypes.hpp / ReliableMessaging.hpp 的
// class Clock 同名冲突），故 RetryConfig 定义于此、ReliableMessaging.hpp 引入。
struct RetryConfig {
    int64_t ackTimeoutMs = 30000;            // ACK 超时（spec §4：ACK 丢失靠重投收敛）
    int64_t backoffBaseMs = 1000;            // 指数退避起点
    int64_t backoffCapMs = 60000;            // 重投间隔上限
    int64_t backoffMultiplier = 2;           // 指数乘子（冻结=2）
    double jitterFraction = 0.2;             // ±20% 均匀 jitter（打散 herd 效应）
    uint64_t jitterSeed = 20260813;          // 确定性 jitter（测试注入固定种子）
    int64_t messageRetentionMs = 7LL * 24 * 3600 * 1000;  // Pending/InFlight → Expired 期限
    int64_t ackedRetentionMs = 24LL * 3600 * 1000;        // Acknowledged 行可查询期
    int64_t expiredRetentionMs = 24LL * 3600 * 1000;      // Expired 行 audited cleanup 前可查询期
    uint32_t cleanupBatch = 100;             // 每轮清理行数上限（有界幂等）
    int64_t cleanupCycleMs = 60LL * 1000;    // scheduler 周期清理间隔
    uint32_t retryBatchLimit = 500;          // 每轮重试/到期扫描批次上限
};

// P3-09 冻结参数（docs/tasks/P3-09.md §冻结参数 RED 前定案）：生产默认 = 卡冻结值；
// 测试注入小值只经构造参数，绝不作为生产默认。生产配置经 AppConfig.outbox
// （P2-09 JSON "outbox" 段）→ ProtocolCodec wiring 注入；测试不经 config 文件。
// 定义于 Config.hpp 而非 LocalOutboxRelay.hpp，与 RetryConfig 同构（mymuduo-safe，
// 避免 Config/LocalOutboxRelay/MessageStore 循环包含）；LocalOutboxRelay.hpp 引入。
struct OutboxConfig {
    uint32_t claimBatchSize = 100;    // 每轮最多 claim/处理的事件数（LIMIT 有界）
    int64_t scanIntervalMs = 5000;    // 周期扫描间隔（lost wakeup 恢复；timer 驱动）
    int64_t claimLeaseMs = 30000;     // claim lease 时长（到期可重领，崩溃无清理路径）
};

struct ServerEndpointConfig {
    std::string ip = "127.0.0.1";
    uint16_t port = 6000;
    int threads = 1;
};

struct DbConfig {
    std::string host = "127.0.0.1";
    uint16_t port = 3306;
    std::string user = "root";
    std::string password = "123456";
    std::string dbname = "chat";
    int poolSize = 5;
};

struct ExecutorConfig {
    int workers = 1;
    int queueCapacity = 64;
};

// P4-05 冻结参数（docs/tasks/P4-05.md §冻结参数 / 开卡待定 5）：生产默认 = 卡冻结值；
// 测试注入小值只经构造参数，绝不作为生产默认。生产配置经 AppConfig.gateway
// （P2-09 JSON "gateway" 段）→ ProtocolCodec wiring 注入；测试不经 config 文件。
// 定义于 Config.hpp（mymuduo-safe），ProtocolCodec wiring / main.cpp 共用。
struct PresenceConfig {
    std::string host = "127.0.0.1";
    uint16_t port = 6379;
    int db = 0;
    int64_t ttlMs = 30000;          // 生产默认 TTL 30s（P4-02 冻结）→ renew 间隔 TTL/2=15s
    int64_t connectTimeoutMs = 1000;
    int64_t commandTimeoutMs = 1000;
};

struct KafkaConfig {
    std::string host = "127.0.0.1";
    uint16_t port = 9092;
};

struct ConsumerConfig {
    std::string topic = "muduo-outbox";              // P4-03/P4-04 冻结生产命名
    // P4-07 H 修复：默认空串 = 按 Gateway 派生（effectiveConsumerGroupId），
    // 使每个 Gateway 独立消费组、各自消费全部 outbox 事件并只 wake 本地 active
    // 用户；config 显式 group_id 优先（显式值覆盖派生）。
    std::string groupId = "";                        // 显式 group_id（空 = 派生）
    uint32_t fetchBatchLimit = 100;                  // P4-04 冻结 fetch 批上限
    int64_t pollDeadlineMs = 5000;                   // 对 P4-04 commit/publish deadline 惯例
};

struct GatewayConfig {
    uint64_t id = 1;             // GatewayId 生产默认 1（冻结）
    PresenceConfig presence;
    KafkaConfig kafka;
    ConsumerConfig consumer;

    // P4-07 H 修复：生效消费组 = 显式 group_id（非空）优先，否则按 Gateway 派生
    // "muduo-outbox-consumer-<id>"——每 Gateway 独立消费组，避免共享组把 outbox
    // 事件只分给单一 Gateway 消费（跨节点 wakeup 空转）。
    std::string effectiveConsumerGroupId() const
    {
        if (!consumer.groupId.empty()) {
            return consumer.groupId;
        }
        return "muduo-outbox-consumer-" + std::to_string(id);
    }
};

// P5-00 阶段 B metrics 段（docs/tasks/P5-00.md 设计决定 D11）：默认关闭
//（enabled=false, port）；JSON "metrics" 段可覆盖。
struct MetricsConfig {
    bool enabled = false;
    uint16_t port = 7001;
};

struct AppConfig {
    AppConfig()
    {
        v2.port = 7000;
    }

    ServerEndpointConfig v1;
    ServerEndpointConfig v2;  // ip 继承 v1；仅 port 可配置
    DbConfig db;
    ExecutorConfig executor;
    RetryConfig reliable;   // P3-08 可靠消息参数（默认 = 卡冻结值）
    OutboxConfig outbox;    // P3-09 outbox relay 参数（默认 = 卡冻结值）
    GatewayConfig gateway;  // P4-05 gateway/presence/kafka/consumer 参数（默认 = 卡冻结值）
    MetricsConfig metrics;  // P5-00 阶段 B /metrics 端点参数（默认关闭）
};

namespace config {

// 解析 JSON 配置文件（nlohmann）。文件缺失/坏 JSON/未知字段/类型错/越界 →
// false + err（含路径与字段名）。不出现的字段保持默认。
bool loadConfigFile(const std::string& path, AppConfig* out, std::string* err);

// argv 覆盖：ip 非空；port 在 [1,65535]；threads 为 nullptr 时不改，否则 >=1。
// 成功后 cfg.v2.ip = cfg.v1.ip。
bool applyCliOverrides(AppConfig* cfg, const char* ip, const char* port,
                       const char* threads, std::string* err);

// DB_PASSWORD env 非空则覆盖 db.password（getenv 返回非 NULL 即覆盖）。
void applyEnvOverrides(AppConfig* cfg);

} // namespace config
