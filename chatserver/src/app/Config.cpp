#include "app/Config.hpp"

#include <climits>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>

#include "json.hpp"

namespace {

using json = nlohmann::json;

struct FieldError {
    std::string field;
    std::string what;
};

bool getUint16(const json& obj, const std::string& key, uint16_t* out,
               std::vector<FieldError>* errors, const std::string& path)
{
    if (!obj.contains(key)) {
        return true;
    }
    if (!obj[key].is_number_integer()) {
        errors->push_back({path + key, "must be an integer"});
        return false;
    }
    long long v = obj[key].get<long long>();
    if (v < 1 || v > 65535) {
        errors->push_back({path + key, "must be in [1,65535]"});
        return false;
    }
    *out = static_cast<uint16_t>(v);
    return true;
}

bool getIntMin1(const json& obj, const std::string& key, int* out,
                std::vector<FieldError>* errors, const std::string& path)
{
    if (!obj.contains(key)) {
        return true;
    }
    if (!obj[key].is_number_integer()) {
        errors->push_back({path + key, "must be an integer"});
        return false;
    }
    long long v = obj[key].get<long long>();
    if (v < 1 || v > INT_MAX) {
        errors->push_back({path + key, "must be in [1,2147483647]"});
        return false;
    }
    *out = static_cast<int>(v);
    return true;
}

// P3-08 正整数 int64（毫秒/期限等）。uint64 上限以 LLONG_MAX 收口，避免负数。
bool getInt64Positive(const json& obj, const std::string& key, int64_t* out,
                      std::vector<FieldError>* errors, const std::string& path)
{
    if (!obj.contains(key)) {
        return true;
    }
    if (!obj[key].is_number_integer()) {
        errors->push_back({path + key, "must be an integer"});
        return false;
    }
    long long v = obj[key].get<long long>();
    if (v < 1) {
        errors->push_back({path + key, "must be >= 1"});
        return false;
    }
    *out = static_cast<int64_t>(v);
    return true;
}

// P3-08 正整数 uint32（batch/limit）。
bool getUint32Positive(const json& obj, const std::string& key, uint32_t* out,
                       std::vector<FieldError>* errors, const std::string& path)
{
    if (!obj.contains(key)) {
        return true;
    }
    if (!obj[key].is_number_integer()) {
        errors->push_back({path + key, "must be an integer"});
        return false;
    }
    long long v = obj[key].get<long long>();
    if (v < 1 || v > 4294967295LL) {
        errors->push_back({path + key, "must be in [1,4294967295]"});
        return false;
    }
    *out = static_cast<uint32_t>(v);
    return true;
}

// P3-08 jitter 比例：[0,1]。
bool getJitterFraction(const json& obj, const std::string& key, double* out,
                       std::vector<FieldError>* errors, const std::string& path)
{
    if (!obj.contains(key)) {
        return true;
    }
    if (!obj[key].is_number()) {
        errors->push_back({path + key, "must be a number"});
        return false;
    }
    double v = obj[key].get<double>();
    if (v < 0.0 || v > 1.0) {
        errors->push_back({path + key, "must be in [0,1]"});
        return false;
    }
    *out = v;
    return true;
}

bool getNonEmptyString(const json& obj, const std::string& key, std::string* out,
                       std::vector<FieldError>* errors, const std::string& path)
{
    if (!obj.contains(key)) {
        return true;
    }
    if (!obj[key].is_string()) {
        errors->push_back({path + key, "must be a string"});
        return false;
    }
    std::string v = obj[key].get<std::string>();
    if (v.empty()) {
        errors->push_back({path + key, "must not be empty"});
        return false;
    }
    *out = v;
    return true;
}

bool checkKnownFields(const json& obj, const std::vector<std::string>& known,
                      std::vector<FieldError>* errors, const std::string& path)
{
    bool ok = true;
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        bool found = false;
        for (size_t i = 0; i < known.size(); ++i) {
            if (it.key() == known[i]) {
                found = true;
                break;
            }
        }
        if (!found) {
            errors->push_back({path + it.key(), "unknown field"});
            ok = false;
        }
    }
    return ok;
}

bool parseServerSection(const json& server, AppConfig* out,
                        std::vector<FieldError>* errors)
{
    bool ok = true;
    checkKnownFields(server, {"v1", "v2"}, errors, "server.");
    if (server.contains("v1")) {
        if (!server["v1"].is_object()) {
            errors->push_back({"server.v1", "must be an object"});
            ok = false;
        } else {
            const json& v1 = server["v1"];
            checkKnownFields(v1, {"ip", "port", "threads"}, errors, "server.v1.");
            getNonEmptyString(v1, "ip", &out->v1.ip, errors, "server.v1.");
            getUint16(v1, "port", &out->v1.port, errors, "server.v1.");
            getIntMin1(v1, "threads", &out->v1.threads, errors, "server.v1.");
        }
    }
    if (server.contains("v2")) {
        if (!server["v2"].is_object()) {
            errors->push_back({"server.v2", "must be an object"});
            ok = false;
        } else {
            const json& v2 = server["v2"];
            checkKnownFields(v2, {"port"}, errors, "server.v2.");
            getUint16(v2, "port", &out->v2.port, errors, "server.v2.");
        }
    }
    if (server.contains("v1") || server.contains("v2")) {
        if (out->v1.port == out->v2.port) {
            errors->push_back({"server.v1.port", "must differ from server.v2.port"});
            ok = false;
        }
    }
    return ok;
}

bool parseDbSection(const json& db, AppConfig* out, std::vector<FieldError>* errors)
{
    checkKnownFields(db, {"host", "port", "user", "password", "dbname", "pool_size"},
                     errors, "db.");
    bool ok = true;
    ok = getNonEmptyString(db, "host", &out->db.host, errors, "db.") && ok;
    ok = getUint16(db, "port", &out->db.port, errors, "db.") && ok;
    ok = getNonEmptyString(db, "user", &out->db.user, errors, "db.") && ok;
    if (db.contains("password")) {
        if (!db["password"].is_string()) {
            errors->push_back({"db.password", "must be a string"});
            ok = false;
        } else {
            out->db.password = db["password"].get<std::string>();
        }
    }
    ok = getNonEmptyString(db, "dbname", &out->db.dbname, errors, "db.") && ok;
    ok = getIntMin1(db, "pool_size", &out->db.poolSize, errors, "db.") && ok;
    return ok;
}

bool parseExecutorSection(const json& executor, AppConfig* out,
                          std::vector<FieldError>* errors)
{
    checkKnownFields(executor, {"workers", "queue_capacity"}, errors, "executor.");
    bool ok = true;
    ok = getIntMin1(executor, "workers", &out->executor.workers, errors, "executor.") && ok;
    ok = getIntMin1(executor, "queue_capacity", &out->executor.queueCapacity,
                    errors, "executor.") && ok;
    // P2-10：多 worker 破坏同连接串行依赖，P3 前只允许单 worker。
    if (ok && out->executor.workers != 1) {
        errors->push_back({"executor.workers", "must be 1"});
        ok = false;
    }
    return ok;
}

// P3-08 可靠消息参数（字段与 RetryConfig 一一对应；缺失保持卡冻结默认）。
bool parseReliableSection(const json& reliable, AppConfig* out,
                          std::vector<FieldError>* errors)
{
    checkKnownFields(reliable,
                     {"ack_timeout_ms", "backoff_base_ms", "backoff_cap_ms",
                      "backoff_multiplier", "jitter_fraction", "jitter_seed",
                      "message_retention_ms", "acked_retention_ms", "expired_retention_ms",
                      "cleanup_batch", "cleanup_cycle_ms", "retry_batch_limit"},
                     errors, "reliable.");
    bool ok = true;
    ok = getInt64Positive(reliable, "ack_timeout_ms", &out->reliable.ackTimeoutMs,
                          errors, "reliable.") && ok;
    ok = getInt64Positive(reliable, "backoff_base_ms", &out->reliable.backoffBaseMs,
                          errors, "reliable.") && ok;
    ok = getInt64Positive(reliable, "backoff_cap_ms", &out->reliable.backoffCapMs,
                          errors, "reliable.") && ok;
    ok = getInt64Positive(reliable, "backoff_multiplier", &out->reliable.backoffMultiplier,
                          errors, "reliable.") && ok;
    ok = getJitterFraction(reliable, "jitter_fraction", &out->reliable.jitterFraction,
                           errors, "reliable.") && ok;
    {
        uint32_t jitterSeed = 0;
        if (getUint32Positive(reliable, "jitter_seed", &jitterSeed, errors, "reliable.")) {
            out->reliable.jitterSeed = jitterSeed;
        } else {
            ok = false;
        }
    }
    ok = getInt64Positive(reliable, "message_retention_ms",
                          &out->reliable.messageRetentionMs, errors, "reliable.") && ok;
    ok = getInt64Positive(reliable, "acked_retention_ms", &out->reliable.ackedRetentionMs,
                          errors, "reliable.") && ok;
    ok = getInt64Positive(reliable, "expired_retention_ms", &out->reliable.expiredRetentionMs,
                          errors, "reliable.") && ok;
    ok = getUint32Positive(reliable, "cleanup_batch", &out->reliable.cleanupBatch,
                           errors, "reliable.") && ok;
    ok = getInt64Positive(reliable, "cleanup_cycle_ms", &out->reliable.cleanupCycleMs,
                          errors, "reliable.") && ok;
    ok = getUint32Positive(reliable, "retry_batch_limit", &out->reliable.retryBatchLimit,
                           errors, "reliable.") && ok;
    return ok;
}

} // namespace

namespace config {

bool loadConfigFile(const std::string& path, AppConfig* out, std::string* err)
{
    std::ifstream in(path.c_str());
    if (!in) {
        *err = "config error: cannot open config file '" + path + "'";
        return false;
    }
    std::stringstream ss;
    ss << in.rdbuf();

    json root;
    try {
        root = json::parse(ss.str());
    } catch (const std::exception& e) {
        *err = std::string("config error: invalid json in '") + path + "': " + e.what();
        return false;
    }
    if (!root.is_object()) {
        *err = std::string("config error: invalid json in '") + path + "': root must be an object";
        return false;
    }

    std::vector<FieldError> errors;
    checkKnownFields(root, {"server", "db", "executor", "reliable"}, &errors, "");

    if (root.contains("server")) {
        if (!root["server"].is_object()) {
            errors.push_back({"server", "must be an object"});
        } else {
            parseServerSection(root["server"], out, &errors);
        }
    }
    if (root.contains("db")) {
        if (!root["db"].is_object()) {
            errors.push_back({"db", "must be an object"});
        } else {
            parseDbSection(root["db"], out, &errors);
        }
    }
    if (root.contains("executor")) {
        if (!root["executor"].is_object()) {
            errors.push_back({"executor", "must be an object"});
        } else {
            parseExecutorSection(root["executor"], out, &errors);
        }
    }
    if (root.contains("reliable")) {
        if (!root["reliable"].is_object()) {
            errors.push_back({"reliable", "must be an object"});
        } else {
            parseReliableSection(root["reliable"], out, &errors);
        }
    }

    if (!errors.empty()) {
        std::stringstream msg;
        msg << "config error: '" << path << "'";
        for (size_t i = 0; i < errors.size(); ++i) {
            msg << " field '" << errors[i].field << "' " << errors[i].what << ";";
        }
        *err = msg.str();
        return false;
    }
    return true;
}

bool applyCliOverrides(AppConfig* cfg, const char* ip, const char* port,
                       const char* threads, std::string* err)
{
    if (ip == nullptr || ip[0] == '\0') {
        *err = "config error: ip must not be empty";
        return false;
    }
    long long portValue = 0;
    if (port == nullptr) {
        *err = "config error: port is required";
        return false;
    }
    char* end = nullptr;
    portValue = strtoll(port, &end, 10);
    if (end == nullptr || *end != '\0' || portValue < 1 || portValue > 65535) {
        *err = std::string("config error: port '") + port + "' must be in [1,65535]";
        return false;
    }
    if (threads != nullptr) {
        end = nullptr;
        long long t = strtoll(threads, &end, 10);
        if (end == nullptr || *end != '\0' || t < 1) {
            *err = std::string("config error: threads '") + threads + "' must be >= 1";
            return false;
        }
        if (t > INT_MAX) {
            *err = std::string("config error: threads '") + threads + "' must be <= 2147483647";
            return false;
        }
        cfg->v1.threads = static_cast<int>(t);
    }
    cfg->v1.ip = ip;
    cfg->v1.port = static_cast<uint16_t>(portValue);
    if (cfg->v2.port == cfg->v1.port) {
        *err = "config error: server.v1.port must differ from server.v2.port";
        return false;
    }
    cfg->v2.ip = cfg->v1.ip;
    return true;
}

void applyEnvOverrides(AppConfig* cfg)
{
    const char* pwd = getenv("DB_PASSWORD");
    if (pwd != nullptr) {
        cfg->db.password = pwd;
    }
}

} // namespace config
