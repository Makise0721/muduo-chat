#pragma once

#include "app/Clock.hpp"
#include "app/PresenceDirectory.hpp"

#include <cstdint>
#include <map>
#include <mutex>

// P4-01 in-memory adapter（docs/tasks/P4-01.md）。P4-02 抽象 port 后由
// PresenceDirectory.hpp 迁出：语义零改动，injectFailure 保留为具体 adapter 的
// per-op 故障注入 seam（不进入领域 interface，P4-01 头注释约定）。
//
// 线程安全：全部公开方法单 mutex 互斥；Clock::nowMs 由调用方保证线程安全
// （FakeClock 内部互斥，UnixEpochClock 无共享状态）。

class InMemoryPresenceDirectory : public PresenceDirectory {
public:
    InMemoryPresenceDirectory(Clock& clock, int64_t ttlMs);

    ClaimResult claim(UserId user, GatewayId gateway, ConnectionId conn) override;
    RenewResult renew(UserId user, GatewayId gateway, ConnectionId conn,
                      SessionEpoch epoch) override;
    ReleaseResult release(UserId user, GatewayId gateway, ConnectionId conn,
                          SessionEpoch epoch) override;
    LocateResult locate(UserId user) override;

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
