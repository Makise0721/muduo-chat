#pragma once

#include "app/Clock.hpp"
#include "app/DomainTypes.hpp"

#include <cstdint>
#include <map>
#include <mutex>

// P4-01 Presence 目录 port（docs/tasks/P4-01.md、ADR-0002、
// docs/architecture/cluster-context-map.md §1/§3）。当前唯一 adapter 是
// in-memory（本深模块即具体实现，注入 Clock 判 TTL）；Redis adapter 与
// 生产 wiring 分别属 P4-02/P4-05，不在此卡实现。
//
// 语义（卡 Interface/完成定义，PresenceContractTest 逐条断言）：
// - 状态转换原子：单次调用二值结果，无部分状态；全部公开方法单锁互斥，
//   并发 claim 同一 user 恰一赢（最后一次写入者持最大 epoch）。
// - claim 不带 epoch：总是生成新 SessionEpoch 并原子覆盖既有条目，无需旧
//   lease 已过期（cluster-context-map §3 关键不变式）；epoch 由全局单调计数器
//   生成，绝不回退。
// - renew/release 为 compare-and-delete：携带的 epoch 与当前条目一致才生效；
//   epoch 不匹配返回 NotEpoch 且条目不变；条目不存在 / TTL 到期返回 NotFound。
// - locate 返回当前路由 (gateway_id, connection_id, session_epoch)；TTL 到期
//   视为不存在（expired=true），与"从未 claim / release 后"（expired=false）
//   可区分。
// - injectFailure 是 per-op 故障注入 seam（模拟依赖不可用）：注入时不改变
//   任何状态，关闭后恢复。
//
// 线程安全：全部公开方法单 mutex 互斥；Clock::nowMs 由调用方保证线程安全
// （FakeClock 内部互斥，UnixEpochClock 无共享状态）。

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
    DependencyUnavailable,  // 依赖不可用（injectFailure 注入）
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
    PresenceDirectory(Clock& clock, int64_t ttlMs);

    ClaimResult claim(UserId user, GatewayId gateway, ConnectionId conn);
    RenewResult renew(UserId user, GatewayId gateway, ConnectionId conn, SessionEpoch epoch);
    ReleaseResult release(UserId user, GatewayId gateway, ConnectionId conn, SessionEpoch epoch);
    LocateResult locate(UserId user);

    // 依赖不可用注入 seam（模拟 adapter 故障；per-op，不销毁既有条目）。
    void injectFailure(bool fail);

private:
    struct Entry {
        Entry() = default;
        Entry(GatewayId gw, ConnectionId conn, SessionEpoch ep, int64_t exp)
            : gatewayId(gw), connectionId(conn), sessionEpoch(ep), expiresAtMs(exp)
        {
        }
        GatewayId gatewayId;
        ConnectionId connectionId;
        SessionEpoch sessionEpoch;
        int64_t expiresAtMs = 0;
    };

    Clock& clock_;
    int64_t ttlMs_;
    uint64_t nextEpoch_ = 1;  // 全局单调 epoch 计数器（claim 每次 +1，绝不回退）
    bool failure_ = false;
    std::mutex mutex_;
    std::map<UserId, Entry> entries_;
};
