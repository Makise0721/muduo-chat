#pragma once

#include "app/Clock.hpp"
#include "app/DomainTypes.hpp"

#include <cstdint>

// P4-01/P4-02 Presence 目录 port（docs/tasks/P4-01.md、docs/tasks/P4-02.md、
// ADR-0002、docs/architecture/cluster-context-map.md §1/§3）。P4-01 为具体
// in-memory 深模块；P4-02 引入 Redis adapter 后抽象为 port，两个 adapter 共用
// 同一领域 interface（重构理由见 P4-02.md"Port 重构理由"节）。
//
// 语义（卡 Interface/完成定义，PresenceContractTest/RedisPresenceContractTest
// 逐条断言）：
// - 状态转换原子：单次调用二值结果，无部分状态。
// - claim 不带 epoch：总是生成新 SessionEpoch 并原子覆盖既有条目，无需旧 lease
//   已过期（cluster-context-map §3 关键不变式）。
// - renew/release 为 compare-and-delete：携带的 epoch 与当前条目一致才生效；
//   epoch 不匹配返回 NotEpoch 且条目不变；条目不存在 / TTL 到期返回 NotFound。
// - locate 返回当前路由 (gateway_id, connection_id, session_epoch)；TTL 到期
//   视为不存在（expired=true），与"从未 claim / release 后"（expired=false）
//   可区分。
// - dependency unavailable（Redis 不可用 / adapter 注入故障）返回
//   DependencyUnavailable，与各业务错误可区分。
//
// adapter 与线程语义：
// - InMemoryPresenceDirectory：单 mutex 互斥（P4-01 实现语义原样保留），
//   injectFailure 为其具体故障注入 seam（不进入 port）。
// - RedisPresenceDirectory：单 RedisConn 同步请求/响应，非线程共享；并发 claim
//   经每线程/每进程独立实例（独立连接）触发 Redis Lua 原子性（多 Gateway 现实）。

struct GatewayId {
    explicit GatewayId() = default;
    explicit GatewayId(uint64_t v) : value(v) {}
    uint64_t value = 0;
};
inline bool operator==(const GatewayId& a, const GatewayId& b) { return a.value == b.value; }
inline bool operator!=(const GatewayId& a, const GatewayId& b) { return !(a == b); }
inline bool operator<(const GatewayId& a, const GatewayId& b) { return a.value < b.value; }

struct ConnectionId {
    explicit ConnectionId() = default;
    explicit ConnectionId(uint64_t v) : value(v) {}
    uint64_t value = 0;
};
inline bool operator==(const ConnectionId& a, const ConnectionId& b) { return a.value == b.value; }
inline bool operator!=(const ConnectionId& a, const ConnectionId& b) { return !(a == b); }
inline bool operator<(const ConnectionId& a, const ConnectionId& b) { return a.value < b.value; }

struct SessionEpoch {
    explicit SessionEpoch() = default;
    explicit SessionEpoch(uint64_t v) : value(v) {}
    uint64_t value = 0;
};
inline bool operator==(const SessionEpoch& a, const SessionEpoch& b) { return a.value == b.value; }
inline bool operator!=(const SessionEpoch& a, const SessionEpoch& b) { return !(a == b); }
inline bool operator<(const SessionEpoch& a, const SessionEpoch& b) { return a.value < b.value; }

// 一条 Presence 条目的路由投影（不含消息真相）：接收者当前 Session 所在的
// Gateway/Connection 与 SessionEpoch（ADR-0002、context-map §1）。
struct DeliveryRoute {
    UserId user;
    GatewayId gatewayId;
    ConnectionId connectionId;
    SessionEpoch sessionEpoch;
};

enum class PresenceError {
    NotEpoch,               // renew/release 携带旧 epoch（compare-and-delete 被拒）
    NotFound,               // 条目不存在（TTL 到期 / 从未 claim / release 后）
    DependencyUnavailable,  // 依赖不可用（Redis 故障 / adapter 注入失败）
};

struct ClaimResult {
    bool ok = false;
    SessionEpoch epoch;
    PresenceError error = PresenceError::NotFound;
};

struct RenewResult {
    bool ok = false;
    int64_t expiresAtMs = 0;
    PresenceError error = PresenceError::NotFound;
};

struct ReleaseResult {
    bool ok = false;
    PresenceError error = PresenceError::NotFound;
};

struct LocateResult {
    bool ok = false;
    bool expired = false;  // ok=false 时：true = TTL 到期；false = 从未 claim/release 后
    DeliveryRoute route;
    PresenceError error = PresenceError::NotFound;
};

class PresenceDirectory {
public:
    virtual ~PresenceDirectory() = default;

    virtual ClaimResult claim(UserId user, GatewayId gateway, ConnectionId conn) = 0;
    virtual RenewResult renew(UserId user, GatewayId gateway, ConnectionId conn,
                              SessionEpoch epoch) = 0;
    virtual ReleaseResult release(UserId user, GatewayId gateway, ConnectionId conn,
                                  SessionEpoch epoch) = 0;
    virtual LocateResult locate(UserId user) = 0;
};
