#pragma once

#include <cstdint>
#include <string>

// P2-09 配置：默认值内建；--config JSON 文件可覆盖；argv 位置参数（ip port
// [threads]）与 DB_PASSWORD env 再覆盖。任何非法配置 fail-fast（返回错误字符串，
// 调用方 stderr + exit(1)，不起服务）。
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
