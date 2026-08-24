#include "app/Config.hpp"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#include "gtest/gtest.h"

using namespace config;

namespace {

std::string writeTempFile(const char* content)
{
    static int counter = 0;
    char path[256];
    snprintf(path, sizeof(path), "/tmp/p209_config_%d_%d.json", getpid(), ++counter);
    std::ofstream out(path);
    out << content;
    out.close();
    return path;
}

class ConfigTest : public ::testing::Test {
protected:
    void TearDown() override
    {
        unsetenv("DB_PASSWORD");
    }
};

TEST_F(ConfigTest, Defaults)
{
    AppConfig cfg;
    EXPECT_EQ(cfg.v1.ip, "127.0.0.1");
    EXPECT_EQ(cfg.v1.port, 6000);
    EXPECT_EQ(cfg.v1.threads, 1);
    EXPECT_EQ(cfg.v2.port, 7000);
    EXPECT_EQ(cfg.db.host, "127.0.0.1");
    EXPECT_EQ(cfg.db.user, "root");
    EXPECT_EQ(cfg.db.password, "123456");
    EXPECT_EQ(cfg.db.dbname, "chat");
    EXPECT_EQ(cfg.db.port, 3306);
    EXPECT_EQ(cfg.db.poolSize, 5);
    EXPECT_EQ(cfg.executor.workers, 1);
    EXPECT_EQ(cfg.executor.queueCapacity, 64);
    // P3-08：reliable 生产默认 = 卡冻结值（测试小值绝不成为生产默认）。
    EXPECT_EQ(cfg.reliable.ackTimeoutMs, 30000);
    EXPECT_EQ(cfg.reliable.backoffBaseMs, 1000);
    EXPECT_EQ(cfg.reliable.backoffCapMs, 60000);
    EXPECT_EQ(cfg.reliable.backoffMultiplier, 2);
    EXPECT_DOUBLE_EQ(cfg.reliable.jitterFraction, 0.2);
    EXPECT_EQ(cfg.reliable.jitterSeed, 20260813u);
    EXPECT_EQ(cfg.reliable.messageRetentionMs, 7LL * 24 * 3600 * 1000);
    EXPECT_EQ(cfg.reliable.ackedRetentionMs, 24LL * 3600 * 1000);
    EXPECT_EQ(cfg.reliable.expiredRetentionMs, 24LL * 3600 * 1000);
    EXPECT_EQ(cfg.reliable.cleanupBatch, 100u);
    EXPECT_EQ(cfg.reliable.cleanupCycleMs, 60LL * 1000);
    EXPECT_EQ(cfg.reliable.retryBatchLimit, 500u);
    // P3-09：outbox 生产默认 = 卡冻结值（测试小值绝不成为生产默认）。
    EXPECT_EQ(cfg.outbox.claimBatchSize, 100u);
    EXPECT_EQ(cfg.outbox.scanIntervalMs, 5000);
    EXPECT_EQ(cfg.outbox.claimLeaseMs, 30000);
}

TEST_F(ConfigTest, FullFileOverridesAll)
{
    std::string path = writeTempFile(
        "{\"server\":{\"v1\":{\"ip\":\"10.0.0.5\",\"port\":6100,\"threads\":4},"
        "\"v2\":{\"port\":7100}},"
        "\"db\":{\"host\":\"dbhost\",\"port\":3307,\"user\":\"u1\","
        "\"password\":\"p1\",\"dbname\":\"d1\",\"pool_size\":9},"
        "\"executor\":{\"workers\":1,\"queue_capacity\":128}}");
    AppConfig cfg;
    std::string err;
    ASSERT_TRUE(loadConfigFile(path, &cfg, &err)) << err;
    EXPECT_EQ(cfg.v1.ip, "10.0.0.5");
    EXPECT_EQ(cfg.v1.port, 6100);
    EXPECT_EQ(cfg.v1.threads, 4);
    EXPECT_EQ(cfg.v2.port, 7100);
    EXPECT_EQ(cfg.db.host, "dbhost");
    EXPECT_EQ(cfg.db.port, 3307);
    EXPECT_EQ(cfg.db.user, "u1");
    EXPECT_EQ(cfg.db.password, "p1");
    EXPECT_EQ(cfg.db.dbname, "d1");
    EXPECT_EQ(cfg.db.poolSize, 9);
    // workers 合法范围 1..8（P3-11 放宽：同 Session 串行由 keyed lane 保证），
    // 此处验证"显式写 1 合法"；queue_capacity 仍可被覆盖。
    EXPECT_EQ(cfg.executor.workers, 1);
    EXPECT_EQ(cfg.executor.queueCapacity, 128);
    std::remove(path.c_str());
}

TEST_F(ConfigTest, ReliableSectionOverrides)
{
    std::string path = writeTempFile(
        "{\"reliable\":{\"ack_timeout_ms\":500,\"backoff_base_ms\":50,"
        "\"backoff_cap_ms\":2000,\"backoff_multiplier\":3,\"jitter_fraction\":0.1,"
        "\"jitter_seed\":7,\"message_retention_ms\":60000,\"acked_retention_ms\":5000,"
        "\"expired_retention_ms\":4000,\"cleanup_batch\":5,\"cleanup_cycle_ms\":1000,"
        "\"retry_batch_limit\":10}}");
    AppConfig cfg;
    std::string err;
    ASSERT_TRUE(loadConfigFile(path, &cfg, &err)) << err;
    EXPECT_EQ(cfg.reliable.ackTimeoutMs, 500);
    EXPECT_EQ(cfg.reliable.backoffBaseMs, 50);
    EXPECT_EQ(cfg.reliable.backoffCapMs, 2000);
    EXPECT_EQ(cfg.reliable.backoffMultiplier, 3);
    EXPECT_DOUBLE_EQ(cfg.reliable.jitterFraction, 0.1);
    EXPECT_EQ(cfg.reliable.jitterSeed, 7u);
    EXPECT_EQ(cfg.reliable.messageRetentionMs, 60000);
    EXPECT_EQ(cfg.reliable.ackedRetentionMs, 5000);
    EXPECT_EQ(cfg.reliable.expiredRetentionMs, 4000);
    EXPECT_EQ(cfg.reliable.cleanupBatch, 5u);
    EXPECT_EQ(cfg.reliable.cleanupCycleMs, 1000);
    EXPECT_EQ(cfg.reliable.retryBatchLimit, 10u);
    std::remove(path.c_str());
}

TEST_F(ConfigTest, ReliablePartialKeepsFrozenDefaults)
{
    std::string path = writeTempFile("{\"reliable\":{\"ack_timeout_ms\":100}}");
    AppConfig cfg;
    std::string err;
    ASSERT_TRUE(loadConfigFile(path, &cfg, &err)) << err;
    EXPECT_EQ(cfg.reliable.ackTimeoutMs, 100);
    EXPECT_EQ(cfg.reliable.backoffBaseMs, 1000);  // 缺失字段保持卡冻结默认
    EXPECT_EQ(cfg.reliable.messageRetentionMs, 7LL * 24 * 3600 * 1000);
    std::remove(path.c_str());
}

TEST_F(ConfigTest, OutboxSectionOverrides)
{
    std::string path = writeTempFile(
        "{\"outbox\":{\"claim_batch\":5,\"scan_interval_ms\":200,\"claim_lease_ms\":4000}}");
    AppConfig cfg;
    std::string err;
    ASSERT_TRUE(loadConfigFile(path, &cfg, &err)) << err;
    EXPECT_EQ(cfg.outbox.claimBatchSize, 5u);
    EXPECT_EQ(cfg.outbox.scanIntervalMs, 200);
    EXPECT_EQ(cfg.outbox.claimLeaseMs, 4000);
    // 未出现字段保持卡冻结默认。
    EXPECT_EQ(cfg.reliable.ackTimeoutMs, 30000);
    std::remove(path.c_str());
}

TEST_F(ConfigTest, OutboxPartialKeepsFrozenDefaults)
{
    std::string path = writeTempFile("{\"outbox\":{\"scan_interval_ms\":100}}");
    AppConfig cfg;
    std::string err;
    ASSERT_TRUE(loadConfigFile(path, &cfg, &err)) << err;
    EXPECT_EQ(cfg.outbox.scanIntervalMs, 100);
    EXPECT_EQ(cfg.outbox.claimBatchSize, 100u);  // 缺失字段保持卡冻结默认
    EXPECT_EQ(cfg.outbox.claimLeaseMs, 30000);
    std::remove(path.c_str());
}

// L2（2026-08-17）：P4-05 gateway 段最小解析用例——gateway.id 覆盖后缺失字段保持
// 卡冻结默认（GatewayId 生产默认 1、TTL 30s、topic/group/fetchBatchLimit 卡冻结值）。
TEST_F(ConfigTest, GatewaySectionOverridesKeepsFrozenDefaults)
{
    std::string path = writeTempFile(
        "{\"gateway\":{\"id\":3,"
        "\"presence\":{\"host\":\"redis.local\",\"port\":6380,\"db\":2,\"ttl_ms\":5000},"
        "\"kafka\":{\"host\":\"kafka.local\",\"port\":9093},"
        "\"consumer\":{\"topic\":\"my-topic\",\"group_id\":\"my-group\","
        "\"fetch_batch_limit\":50,\"poll_deadline_ms\":3000}}}");
    AppConfig cfg;
    std::string err;
    ASSERT_TRUE(loadConfigFile(path, &cfg, &err)) << err;
    EXPECT_EQ(cfg.gateway.id, 3u);
    EXPECT_EQ(cfg.gateway.presence.host, "redis.local");
    EXPECT_EQ(cfg.gateway.presence.port, 6380);
    EXPECT_EQ(cfg.gateway.presence.db, 2);
    EXPECT_EQ(cfg.gateway.presence.ttlMs, 5000);
    EXPECT_EQ(cfg.gateway.kafka.host, "kafka.local");
    EXPECT_EQ(cfg.gateway.kafka.port, 9093);
    EXPECT_EQ(cfg.gateway.consumer.topic, "my-topic");
    // 显式 group_id 优先（P4-07：显式值覆盖派生）。
    EXPECT_EQ(cfg.gateway.consumer.groupId, "my-group");
    EXPECT_EQ(cfg.gateway.effectiveConsumerGroupId(), "my-group");
    EXPECT_EQ(cfg.gateway.consumer.fetchBatchLimit, 50u);
    EXPECT_EQ(cfg.gateway.consumer.pollDeadlineMs, 3000);
    // 未出现字段保持卡冻结默认（缺省 = 冻结值）。
    EXPECT_EQ(cfg.gateway.presence.connectTimeoutMs, 1000);
    EXPECT_EQ(cfg.gateway.presence.commandTimeoutMs, 1000);
    std::remove(path.c_str());
}

// P4-07 H 修复：gateway.id 覆盖且未显式 group_id 时，生效消费组按 Gateway 派生。
TEST_F(ConfigTest, GatewayConsumerGroupIdDerivedFromId)
{
    std::string path = writeTempFile("{\"gateway\":{\"id\":7}}");
    AppConfig cfg;
    std::string err;
    ASSERT_TRUE(loadConfigFile(path, &cfg, &err)) << err;
    EXPECT_EQ(cfg.gateway.id, 7u);
    EXPECT_EQ(cfg.gateway.consumer.groupId, "");
    EXPECT_EQ(cfg.gateway.effectiveConsumerGroupId(), "muduo-outbox-consumer-7");
    std::remove(path.c_str());
}

// L2（2026-08-17）：gateway 段缺省（无 gateway 段）= 卡冻结默认（GatewayId=1、TTL
// 30s、presence/kafka 127.0.0.1、consumer 冻结命名）。
TEST_F(ConfigTest, GatewayDefaultsAreFrozenValues)
{
    AppConfig cfg;
    EXPECT_EQ(cfg.gateway.id, 1u);
    EXPECT_EQ(cfg.gateway.presence.host, "127.0.0.1");
    EXPECT_EQ(cfg.gateway.presence.port, 6379);
    EXPECT_EQ(cfg.gateway.presence.db, 0);
    EXPECT_EQ(cfg.gateway.presence.ttlMs, 30000);
    EXPECT_EQ(cfg.gateway.presence.connectTimeoutMs, 1000);
    EXPECT_EQ(cfg.gateway.presence.commandTimeoutMs, 1000);
    EXPECT_EQ(cfg.gateway.kafka.host, "127.0.0.1");
    EXPECT_EQ(cfg.gateway.kafka.port, 9092);
    EXPECT_EQ(cfg.gateway.consumer.topic, "muduo-outbox");
    // P4-07 H 修复：默认 group_id 空串（派生标记），生效消费组按 Gateway 派生。
    EXPECT_EQ(cfg.gateway.consumer.groupId, "");
    EXPECT_EQ(cfg.gateway.effectiveConsumerGroupId(), "muduo-outbox-consumer-1");
    EXPECT_EQ(cfg.gateway.consumer.fetchBatchLimit, 100u);
    EXPECT_EQ(cfg.gateway.consumer.pollDeadlineMs, 5000);
    // 空配置文件同样保持 gateway 冻结默认。
    std::string path = writeTempFile("{}");
    AppConfig empty;
    std::string err;
    ASSERT_TRUE(loadConfigFile(path, &empty, &err)) << err;
    EXPECT_EQ(empty.gateway.id, 1u);
    EXPECT_EQ(empty.gateway.presence.ttlMs, 30000);
    std::remove(path.c_str());
}

// P5-00 阶段 B L-2：metrics 段最小解析——enabled/port 正确解析；缺省保持默认关闭。
TEST_F(ConfigTest, MetricsSectionParsed)
{
    std::string path = writeTempFile("{\"metrics\":{\"enabled\":true,\"port\":9001}}");
    AppConfig cfg;
    std::string err;
    ASSERT_TRUE(loadConfigFile(path, &cfg, &err)) << err;
    EXPECT_TRUE(cfg.metrics.enabled);
    EXPECT_EQ(cfg.metrics.port, 9001);
    // 缺省（无 metrics 段）= 默认关闭（P5-00 D11）。
    std::string emptyPath = writeTempFile("{}");
    AppConfig empty;
    std::string emptyErr;
    ASSERT_TRUE(loadConfigFile(emptyPath, &empty, &emptyErr)) << emptyErr;
    EXPECT_FALSE(empty.metrics.enabled);
    EXPECT_EQ(empty.metrics.port, 7001);
    std::remove(path.c_str());
    std::remove(emptyPath.c_str());
}

// P5-00 阶段 B L-2：metrics 段未知字段 fail-fast（metrics. 前缀路径字段名，
// 与既有 unknown 字段用例同款）。
TEST_F(ConfigTest, MetricsUnknownFieldFailsFast)
{
    std::string path = writeTempFile("{\"metrics\":{\"unknown_field\":1}}");
    AppConfig cfg;
    std::string err;
    EXPECT_FALSE(loadConfigFile(path, &cfg, &err));
    EXPECT_NE(err.find("metrics.unknown_field"), std::string::npos) << err;
    std::remove(path.c_str());
}

// P3-11：executor 段部分覆盖（最小解析用例）——只出现 queue_capacity 时
// workers 保持卡冻结默认 1（缺失字段不改变默认；显式多 worker 见
// ExecutorWorkersMultiValueAllowed）。
TEST_F(ConfigTest, ExecutorPartialKeepsFrozenDefaults)
{
    std::string path = writeTempFile("{\"executor\":{\"queue_capacity\":32}}");
    AppConfig cfg;
    std::string err;
    ASSERT_TRUE(loadConfigFile(path, &cfg, &err)) << err;
    EXPECT_EQ(cfg.executor.queueCapacity, 32);
    EXPECT_EQ(cfg.executor.workers, 1);  // 缺失字段保持卡冻结默认
    std::remove(path.c_str());
}

// P3-11：correctness 全绿后放宽 `must be 1` 强制——显式 workers 2/8 合法并被
// 解析，默认仍 1；9（越 [1,8] 上限）与 0/-1（越下限）仍被拒（见 FailFastMatrix）。
TEST_F(ConfigTest, ExecutorWorkersMultiValueAllowed)
{
    for (int w : {2, 8}) {
        std::string json = "{\"executor\":{\"workers\":" + std::to_string(w) + "}}";
        std::string path = writeTempFile(json.c_str());
        AppConfig cfg;
        std::string err;
        ASSERT_TRUE(loadConfigFile(path, &cfg, &err))
            << "workers=" << w << " should be legal: " << err;
        EXPECT_EQ(cfg.executor.workers, w);
        EXPECT_EQ(cfg.executor.queueCapacity, 64);  // 缺失字段保持卡冻结默认
        std::remove(path.c_str());
    }
    // 缺省（无 executor 段）路径默认值不变。
    std::string path = writeTempFile("{}");
    AppConfig cfg;
    std::string err;
    ASSERT_TRUE(loadConfigFile(path, &cfg, &err)) << err;
    EXPECT_EQ(cfg.executor.workers, 1);
    std::remove(path.c_str());
}

TEST_F(ConfigTest, PartialFileKeepsDefaults)
{
    std::string path = writeTempFile("{\"server\":{\"v1\":{\"port\":6101}}}");
    AppConfig cfg;
    std::string err;
    ASSERT_TRUE(loadConfigFile(path, &cfg, &err)) << err;
    EXPECT_EQ(cfg.v1.port, 6101);
    EXPECT_EQ(cfg.v1.ip, "127.0.0.1");
    EXPECT_EQ(cfg.v1.threads, 1);
    EXPECT_EQ(cfg.v2.port, 7000);
    EXPECT_EQ(cfg.db.poolSize, 5);
    std::remove(path.c_str());
}

TEST_F(ConfigTest, EmptyFileIsValid)
{
    std::string path = writeTempFile("{}");
    AppConfig cfg;
    std::string err;
    ASSERT_TRUE(loadConfigFile(path, &cfg, &err)) << err;
    EXPECT_EQ(cfg.v1.port, 6000);
    std::remove(path.c_str());
}

struct FailCase {
    const char* json;
    const char* expectSubstr;
};

TEST_F(ConfigTest, FailFastMatrix)
{
    FailCase cases[] = {
        {"{\"server\":{\"v1\":{\"port\":0}}}", "server.v1.port"},
        {"{\"server\":{\"v1\":{\"port\":65536}}}", "server.v1.port"},
        {"{\"server\":{\"v1\":{\"port\":\"6000\"}}}", "server.v1.port"},
        {"{\"server\":{\"v1\":{\"ip\":\"\"}}}", "server.v1.ip"},
        {"{\"server\":{\"v1\":{\"threads\":0}}}", "server.v1.threads"},
        {"{\"server\":{\"v1\":{\"threads\":\"2\"}}}", "server.v1.threads"},
        {"{\"server\":{\"v2\":{\"port\":70000}}}", "server.v2.port"},
        {"{\"db\":{\"port\":0}}", "db.port"},
        {"{\"db\":{\"pool_size\":0}}", "db.pool_size"},
        {"{\"db\":{\"host\":\"\"}}", "db.host"},
        {"{\"db\":{\"user\":\"\"}}", "db.user"},
        {"{\"db\":{\"dbname\":\"\"}}", "db.dbname"},
        {"{\"db\":{\"pool_size\":\"5\"}}", "db.pool_size"},
        {"{\"executor\":{\"workers\":0}}", "executor.workers"},
        {"{\"executor\":{\"queue_capacity\":0}}", "executor.queue_capacity"},
        {"{\"executor\":{\"workers\":-1}}", "executor.workers"},
        {"{\"unknown_top\":1}", "unknown"},
        {"{\"server\":{\"unknown_field\":1}}", "unknown"},
        {"{\"db\":{\"unknown_field\":1}}", "unknown"},
        {"{\"executor\":{\"unknown_field\":1}}", "unknown"},
        // 顶层非对象 / 各 section 非对象（类型分支）。
        {"[1,2]", "root"},
        {"{\"server\":5}", "server"},
        {"{\"server\":{\"v1\":5}}", "server.v1"},
        {"{\"server\":{\"v2\":5}}", "server.v2"},
        {"{\"db\":5}", "db"},
        {"{\"executor\":5}", "executor"},
        // db 字符串字段类型分支。
        {"{\"db\":{\"password\":5}}", "db.password"},
        {"{\"db\":{\"host\":5}}", "db.host"},
        {"{\"db\":{\"user\":5}}", "db.user"},
        {"{\"db\":{\"dbname\":5}}", "db.dbname"},
        // 新校验（依赖 R1 实现，未落地前跑红属预期）：workers 上限 [1,8]、
        // v1/v2 端口必须不同、int 溢出（4294967296 超出 int 范围）。
        {"{\"executor\":{\"workers\":9}}", "executor.workers"},
        {"{\"server\":{\"v1\":{\"port\":6000},\"v2\":{\"port\":6000}}}", "port"},
        {"{\"executor\":{\"workers\":4294967296}}", "executor.workers"},
        {"{\"db\":{\"pool_size\":4294967296}}", "db.pool_size"},
        // P3-08 reliable 段校验（类型/范围/未知字段/非对象）。
        {"{\"reliable\":{\"ack_timeout_ms\":0}}", "reliable.ack_timeout_ms"},
        {"{\"reliable\":{\"ack_timeout_ms\":-5}}", "reliable.ack_timeout_ms"},
        {"{\"reliable\":{\"ack_timeout_ms\":\"100\"}}", "reliable.ack_timeout_ms"},
        {"{\"reliable\":{\"jitter_fraction\":1.5}}", "reliable.jitter_fraction"},
        {"{\"reliable\":{\"jitter_fraction\":-0.1}}", "reliable.jitter_fraction"},
        {"{\"reliable\":{\"cleanup_batch\":0}}", "reliable.cleanup_batch"},
        {"{\"reliable\":{\"retry_batch_limit\":4294967296}}", "reliable.retry_batch_limit"},
        {"{\"reliable\":{\"unknown_field\":1}}", "reliable.unknown_field"},
        {"{\"reliable\":5}", "reliable"},
        // P3-09 outbox 段校验（类型/范围/未知字段/非对象）。
        {"{\"outbox\":{\"claim_batch\":0}}", "outbox.claim_batch"},
        {"{\"outbox\":{\"scan_interval_ms\":0}}", "outbox.scan_interval_ms"},
        {"{\"outbox\":{\"scan_interval_ms\":-5}}", "outbox.scan_interval_ms"},
        {"{\"outbox\":{\"scan_interval_ms\":\"100\"}}", "outbox.scan_interval_ms"},
        {"{\"outbox\":{\"claim_lease_ms\":0}}", "outbox.claim_lease_ms"},
        {"{\"outbox\":{\"unknown_field\":1}}", "outbox.unknown_field"},
        {"{\"outbox\":5}", "outbox"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        std::string path = writeTempFile(cases[i].json);
        AppConfig cfg;
        std::string err;
        EXPECT_FALSE(loadConfigFile(path, &cfg, &err))
            << "case " << i << " should fail: " << cases[i].json;
        EXPECT_NE(err.find(cases[i].expectSubstr), std::string::npos)
            << "case " << i << " err should mention '" << cases[i].expectSubstr
            << "', got: " << err;
        std::remove(path.c_str());
    }
}

// L2（2026-08-17）：gateway 段 fail-fast——非法 id / presence db 越界 / 未知字段 /
// 非对象段全部被拒并带路径字段名。
TEST_F(ConfigTest, GatewayFailFastMatrix)
{
    FailCase cases[] = {
        {"{\"gateway\":{\"id\":0}}", "gateway.id"},           // id 越界（<1）
        {"{\"gateway\":{\"id\":-1}}", "gateway.id"},
        {"{\"gateway\":{\"id\":\"2\"}}", "gateway.id"},       // id 类型错
        {"{\"gateway\":{\"presence\":{\"db\":16}}}", "gateway.presence.db"},  // db 越界 [0,15]
        {"{\"gateway\":{\"presence\":{\"db\":-1}}}", "gateway.presence.db"},
        {"{\"gateway\":{\"presence\":{\"port\":0}}}", "gateway.presence.port"},
        {"{\"gateway\":{\"presence\":{\"ttl_ms\":0}}}", "gateway.presence.ttl_ms"},
        {"{\"gateway\":{\"unknown_field\":1}}", "gateway.unknown_field"},
        {"{\"gateway\":{\"presence\":{\"unknown_field\":1}}}", "gateway.presence.unknown_field"},
        {"{\"gateway\":{\"kafka\":{\"unknown_field\":1}}}", "gateway.kafka.unknown_field"},
        {"{\"gateway\":{\"consumer\":{\"unknown_field\":1}}}", "gateway.consumer.unknown_field"},
        {"{\"gateway\":5}", "gateway"},                       // 非对象段
        {"{\"gateway\":{\"presence\":5}}", "gateway.presence"},
        {"{\"gateway\":{\"kafka\":5}}", "gateway.kafka"},
        {"{\"gateway\":{\"consumer\":5}}", "gateway.consumer"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        std::string path = writeTempFile(cases[i].json);
        AppConfig cfg;
        std::string err;
        EXPECT_FALSE(loadConfigFile(path, &cfg, &err))
            << "case " << i << " should fail: " << cases[i].json;
        EXPECT_NE(err.find(cases[i].expectSubstr), std::string::npos)
            << "case " << i << " err should mention '" << cases[i].expectSubstr
            << "', got: " << err;
        std::remove(path.c_str());
    }
}

TEST_F(ConfigTest, MissingFileFails)
{
    AppConfig cfg;
    std::string err;
    EXPECT_FALSE(loadConfigFile("/tmp/does_not_exist_p209.json", &cfg, &err));
    EXPECT_NE(err.find("cannot open"), std::string::npos) << err;
}

TEST_F(ConfigTest, MalformedJsonFails)
{
    std::string path = writeTempFile("{not valid json!!!");
    AppConfig cfg;
    std::string err;
    EXPECT_FALSE(loadConfigFile(path, &cfg, &err));
    EXPECT_NE(err.find("invalid json"), std::string::npos) << err;
    std::remove(path.c_str());
}

TEST_F(ConfigTest, CliOverridesApply)
{
    AppConfig cfg;
    std::string err;
    ASSERT_TRUE(applyCliOverrides(&cfg, "192.168.1.9", "6200", "3", &err)) << err;
    EXPECT_EQ(cfg.v1.ip, "192.168.1.9");
    EXPECT_EQ(cfg.v1.port, 6200);
    EXPECT_EQ(cfg.v1.threads, 3);
    EXPECT_EQ(cfg.v2.ip, "192.168.1.9");
    EXPECT_EQ(cfg.v2.port, 7000);
}

TEST_F(ConfigTest, CliOverridesLeaveExecutorAndDb)
{
    AppConfig cfg;
    std::string err;
    ASSERT_TRUE(applyCliOverrides(&cfg, "1.2.3.4", "6000", nullptr, &err)) << err;
    EXPECT_EQ(cfg.db.poolSize, 5);
    EXPECT_EQ(cfg.executor.workers, 1);
}

TEST_F(ConfigTest, CliOverridesFailFast)
{
    AppConfig cfg;
    std::string err;
    EXPECT_FALSE(applyCliOverrides(&cfg, "", "6000", nullptr, &err));
    EXPECT_NE(err.find("ip"), std::string::npos) << err;

    EXPECT_FALSE(applyCliOverrides(&cfg, "1.2.3.4", "0", nullptr, &err));
    EXPECT_NE(err.find("port"), std::string::npos) << err;

    EXPECT_FALSE(applyCliOverrides(&cfg, "1.2.3.4", "70000", nullptr, &err));
    EXPECT_NE(err.find("port"), std::string::npos) << err;

    EXPECT_FALSE(applyCliOverrides(&cfg, "1.2.3.4", "6000", "0", &err));
    EXPECT_NE(err.find("threads"), std::string::npos) << err;

    EXPECT_FALSE(applyCliOverrides(&cfg, "1.2.3.4", "6000", "-2", &err));
    EXPECT_NE(err.find("threads"), std::string::npos) << err;

    // 超出 int 范围（2^31）：strtoll 成功但 >INT_MAX → 被拒。
    EXPECT_FALSE(applyCliOverrides(&cfg, "1.2.3.4", "6000", "2147483648", &err));
    EXPECT_NE(err.find("threads"), std::string::npos) << err;

    EXPECT_FALSE(applyCliOverrides(&cfg, "1.2.3.4", "notaport", nullptr, &err));
    EXPECT_NE(err.find("port"), std::string::npos) << err;

    // 十六进制串：strtoll(..., 10) 解析到 'x' 停止 → 被拒（不存在"0x10 被当作
    // 16"的怪癖，无需锁怪癖测试；此例仅锁定"十六进制输入非法"的现状行为）。
    EXPECT_FALSE(applyCliOverrides(&cfg, "1.2.3.4", "0x10", nullptr, &err));
    EXPECT_NE(err.find("port"), std::string::npos) << err;
}

TEST_F(ConfigTest, EnvOverridesPassword)
{
    AppConfig cfg;
    setenv("DB_PASSWORD", "from_env", 1);
    applyEnvOverrides(&cfg);
    EXPECT_EQ(cfg.db.password, "from_env");

    unsetenv("DB_PASSWORD");
    AppConfig cfg2;
    applyEnvOverrides(&cfg2);
    EXPECT_EQ(cfg2.db.password, "123456");
}

} // namespace
