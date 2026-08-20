#pragma once

#include "app/Clock.hpp"
#include "app/PresenceDirectory.hpp"
#include "app/RedisConn.hpp"

#include <cstdint>
#include <string>

// P4-02 Redis presence adapter（docs/tasks/P4-02.md、docs/adr/0002、
// docs/architecture/cluster-context-map.md §1/§3）。claim/renew/release 用 Lua
// 原子脚本（单脚本多键天然原子）；claim 经全局单调计数器键 `presence:v1:epoch`
// （INCR）生成新 epoch，release 后重新 claim 的 epoch 仍严格大于旧值（与
// in-memory adapter 的全局单调计数器语义一致）。条目键值只含路由与 epoch
// （{"g":gateway,"c":connection,"e":epoch,"x":expiresAtMs}，全字符串防精度丢失），
// 绝不含消息真相。locate 由注入 Clock 判定 `now >= expiresAtMs` 是否过期
// （context-map §3 TTL 落地约束）。P4-06 环境复原：claim/renew 写值同时
// PEXPIRE key <ttlMs>（与 value 的 expiresAtMs 同一 ttl，物理删除与逻辑判定
// 一致不冲突）——value 仍是逻辑权威，EXPIRE 仅防物理累积（SIGKILL 无 release
// 时旧键随 TTL 过期，避免无界累积）。
//
// 线程语义：实例持单一 RedisConn（同步请求/响应），不跨线程共享；并发 claim 经
// 每线程/每进程独立实例（独立连接）触发 Redis Lua 原子性——正是多 Gateway 现实
// 形态（并发冲突恰一赢，败者 epoch 被 fencing）。
//
// 故障语义：连接失败/命令超时/断连 → DependencyUnavailable；恢复后下一条命令
// 自动重连（错误时关闭连接，ensureConnected 惰性重建）。

class RedisPresenceDirectory : public PresenceDirectory {
public:
    RedisPresenceDirectory(Clock& clock, const std::string& host, int port, int db,
                           int64_t ttlMs, int64_t connectTimeoutMs, int64_t commandTimeoutMs);

    ClaimResult claim(UserId user, GatewayId gateway, ConnectionId conn) override;
    RenewResult renew(UserId user, GatewayId gateway, ConnectionId conn,
                      SessionEpoch epoch) override;
    ReleaseResult release(UserId user, GatewayId gateway, ConnectionId conn,
                          SessionEpoch epoch) override;
    LocateResult locate(UserId user) override;

private:
    bool ensureConnected();
    RedisConn::Reply eval(const char* script, const std::vector<std::string>& keys,
                          const std::vector<std::string>& argv);
    std::string keyFor(UserId user) const;

    Clock& clock_;
    std::string host_;
    int port_ = 0;
    int db_ = 0;
    int64_t ttlMs_ = 0;
    int64_t connectTimeoutMs_ = 0;
    int64_t commandTimeoutMs_ = 0;
    RedisConn conn_;
};
