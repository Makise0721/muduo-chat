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

struct AppConfig {
    AppConfig()
    {
        v2.port = 7000;
    }

    ServerEndpointConfig v1;
    ServerEndpointConfig v2;  // ip 继承 v1；仅 port 可配置
    DbConfig db;
    ExecutorConfig executor;
    RetryConfig reliable;  // P3-08 可靠消息参数（默认 = 卡冻结值）
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
