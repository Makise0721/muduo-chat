#include "app/RedisPresenceDirectory.hpp"

#include "json.hpp"

#include <cstdlib>
#include <utility>

namespace {

// claim：INCR 全局单调计数器键生成新 epoch（与 in-memory adapter 的"全局单调
// epoch 计数器"语义一致：release 后重新 claim 的 epoch 仍严格大于旧值，P4-01
// 契约断言），原子覆盖条目；value 内嵌 expiresAtMs（全字符串防 uint64 精度丢失）。
const char kClaimScript[] =
    "local e = redis.call('INCR', KEYS[2])\n"
    "local now = tonumber(ARGV[1])\n"
    "local ttl = tonumber(ARGV[2])\n"
    "local v = cjson.encode({ g = ARGV[3], c = ARGV[4], e = tostring(e), "
    "x = tostring(now + ttl) })\n"
    "redis.call('SET', KEYS[1], v)\n"
    "return e\n";

// renew：compare-and-update。状态码 {1,新expiresAtMs}=ok；{0}=NotFound（缺失或
// 已过期）；{2}=NotEpoch（旧 epoch，条目不变）。
const char kRenewScript[] =
    "local old = redis.call('GET', KEYS[1])\n"
    "if not old then return { 0 } end\n"
    "local o = cjson.decode(old)\n"
    "local now = tonumber(ARGV[1])\n"
    "if now >= tonumber(o.x) then return { 0 } end\n"
    "if tonumber(o.e) ~= tonumber(ARGV[2]) then return { 2 } end\n"
    "o.x = tostring(now + tonumber(ARGV[3]))\n"
    "redis.call('SET', KEYS[1], cjson.encode(o))\n"
    "return { 1, now + tonumber(ARGV[3]) }\n";

// release：compare-and-delete。{1}=已删；{0}=NotFound（缺失或已过期）；{2}=NotEpoch。
const char kReleaseScript[] =
    "local old = redis.call('GET', KEYS[1])\n"
    "if not old then return { 0 } end\n"
    "local o = cjson.decode(old)\n"
    "local now = tonumber(ARGV[1])\n"
    "if now >= tonumber(o.x) then return { 0 } end\n"
    "if tonumber(o.e) ~= tonumber(ARGV[2]) then return { 2 } end\n"
    "redis.call('DEL', KEYS[1])\n"
    "return { 1 }\n";

uint64_t toU64(const std::string& s)
{
    return static_cast<uint64_t>(std::strtoull(s.c_str(), nullptr, 10));
}

int64_t toI64(const std::string& s)
{
    return std::strtoll(s.c_str(), nullptr, 10);
}

}  // namespace

RedisPresenceDirectory::RedisPresenceDirectory(Clock& clock, const std::string& host, int port,
                                               int db, int64_t ttlMs, int64_t connectTimeoutMs,
                                               int64_t commandTimeoutMs)
    : clock_(clock),
      host_(host),
      port_(port),
      db_(db),
      ttlMs_(ttlMs),
      connectTimeoutMs_(connectTimeoutMs),
      commandTimeoutMs_(commandTimeoutMs)
{
}

std::string RedisPresenceDirectory::keyFor(UserId user) const
{
    return "presence:v1:" + std::to_string(user.value);
}

bool RedisPresenceDirectory::ensureConnected()
{
    if (conn_.connected()) {
        return true;
    }
    return conn_.connect(host_, port_, db_, connectTimeoutMs_);
}

RedisConn::Reply RedisPresenceDirectory::eval(const char* script,
                                              const std::vector<std::string>& keys,
                                              const std::vector<std::string>& argv)
{
    std::vector<std::string> cmd;
    cmd.reserve(3 + keys.size() + argv.size());
    cmd.push_back("EVAL");
    cmd.push_back(script);
    cmd.push_back(std::to_string(keys.size()));
    for (size_t i = 0; i < keys.size(); ++i) {
        cmd.push_back(keys[i]);
    }
    for (size_t i = 0; i < argv.size(); ++i) {
        cmd.push_back(argv[i]);
    }
    return conn_.command(cmd, commandTimeoutMs_);
}

ClaimResult RedisPresenceDirectory::claim(UserId user, GatewayId gateway, ConnectionId conn)
{
    ClaimResult result;
    if (!ensureConnected()) {
        result.error = PresenceError::DependencyUnavailable;
        return result;
    }
    std::vector<std::string> keys;
    keys.push_back(keyFor(user));
    keys.push_back("presence:v1:epoch");  // 全局单调 epoch 计数器键（永不被 release 删除）
    std::vector<std::string> argv;
    argv.push_back(std::to_string(clock_.nowMs()));
    argv.push_back(std::to_string(ttlMs_));
    argv.push_back(std::to_string(gateway.value));
    argv.push_back(std::to_string(conn.value));
    RedisConn::Reply r = eval(kClaimScript, keys, argv);
    if (r.type == RedisConn::Reply::Type::Integer) {
        result.ok = true;
        result.epoch = SessionEpoch(static_cast<uint64_t>(r.integer));
        return result;
    }
    result.error = PresenceError::DependencyUnavailable;
    return result;
}

RenewResult RedisPresenceDirectory::renew(UserId user, GatewayId gateway, ConnectionId conn,
                                          SessionEpoch epoch)
{
    RenewResult result;
    (void)gateway;
    (void)conn;
    if (!ensureConnected()) {
        result.error = PresenceError::DependencyUnavailable;
        return result;
    }
    std::vector<std::string> argv;
    argv.push_back(std::to_string(clock_.nowMs()));
    argv.push_back(std::to_string(epoch.value));
    argv.push_back(std::to_string(ttlMs_));
    RedisConn::Reply r = eval(kRenewScript, {keyFor(user)}, argv);
    if (r.type == RedisConn::Reply::Type::Array && !r.array.empty() &&
        r.array[0].type == RedisConn::Reply::Type::Integer) {
        const int64_t status = r.array[0].integer;
        if (status == 1) {
            result.ok = true;
            if (r.array.size() >= 2 && r.array[1].type == RedisConn::Reply::Type::Integer) {
                result.expiresAtMs = r.array[1].integer;
            }
            return result;
        }
        result.error = (status == 2) ? PresenceError::NotEpoch : PresenceError::NotFound;
        return result;
    }
    result.error = PresenceError::DependencyUnavailable;
    return result;
}

ReleaseResult RedisPresenceDirectory::release(UserId user, GatewayId gateway, ConnectionId conn,
                                              SessionEpoch epoch)
{
    ReleaseResult result;
    (void)gateway;
    (void)conn;
    if (!ensureConnected()) {
        result.error = PresenceError::DependencyUnavailable;
        return result;
    }
    std::vector<std::string> argv;
    argv.push_back(std::to_string(clock_.nowMs()));
    argv.push_back(std::to_string(epoch.value));
    RedisConn::Reply r = eval(kReleaseScript, {keyFor(user)}, argv);
    if (r.type == RedisConn::Reply::Type::Array && !r.array.empty() &&
        r.array[0].type == RedisConn::Reply::Type::Integer) {
        const int64_t status = r.array[0].integer;
        if (status == 1) {
            result.ok = true;
            return result;
        }
        result.error = (status == 2) ? PresenceError::NotEpoch : PresenceError::NotFound;
        return result;
    }
    result.error = PresenceError::DependencyUnavailable;
    return result;
}

LocateResult RedisPresenceDirectory::locate(UserId user)
{
    LocateResult result;
    if (!ensureConnected()) {
        result.error = PresenceError::DependencyUnavailable;
        return result;
    }
    RedisConn::Reply r = conn_.command({"GET", keyFor(user)}, commandTimeoutMs_);
    if (r.isError()) {
        result.error = PresenceError::DependencyUnavailable;
        return result;
    }
    if (r.type == RedisConn::Reply::Type::Nil) {
        result.error = PresenceError::NotFound;  // 从未 claim / release 后
        return result;
    }
    if (r.type != RedisConn::Reply::Type::Bulk) {
        result.error = PresenceError::DependencyUnavailable;
        return result;
    }
    try {
        nlohmann::json v = nlohmann::json::parse(r.str);
        const GatewayId g(toU64(v["g"].get<std::string>()));
        const ConnectionId c(toU64(v["c"].get<std::string>()));
        const SessionEpoch e(toU64(v["e"].get<std::string>()));
        const int64_t expiresAtMs = toI64(v["x"].get<std::string>());
        if (clock_.nowMs() >= expiresAtMs) {
            result.expired = true;  // TTL 到期视为不存在，与"从未 claim"可区分
            result.error = PresenceError::NotFound;
            return result;
        }
        result.ok = true;
        result.route.user = user;
        result.route.gatewayId = g;
        result.route.connectionId = c;
        result.route.sessionEpoch = e;
        return result;
    } catch (...) {
        result.error = PresenceError::DependencyUnavailable;
        return result;
    }
}
