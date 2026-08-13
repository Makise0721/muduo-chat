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
    // workers 只能为 1（多 worker 破坏同连接串行依赖，R1 校验），此处仅能
    // 验证"显式写 1 合法"；queue_capacity 仍可被覆盖。
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
        // 新校验（依赖 R1 实现，未落地前跑红属预期）：executor 单 worker 设计
        // 上限、v1/v2 端口必须不同、int 溢出（4294967296 超出 int 范围）。
        {"{\"executor\":{\"workers\":2}}", "executor.workers' must be 1"},
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
