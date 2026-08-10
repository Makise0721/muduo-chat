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
}

TEST_F(ConfigTest, FullFileOverridesAll)
{
    std::string path = writeTempFile(
        "{\"server\":{\"v1\":{\"ip\":\"10.0.0.5\",\"port\":6100,\"threads\":4},"
        "\"v2\":{\"port\":7100}},"
        "\"db\":{\"host\":\"dbhost\",\"port\":3307,\"user\":\"u1\","
        "\"password\":\"p1\",\"dbname\":\"d1\",\"pool_size\":9},"
        "\"executor\":{\"workers\":2,\"queue_capacity\":128}}");
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
    EXPECT_EQ(cfg.executor.workers, 2);
    EXPECT_EQ(cfg.executor.queueCapacity, 128);
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

    EXPECT_FALSE(applyCliOverrides(&cfg, "1.2.3.4", "notaport", nullptr, &err));
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
