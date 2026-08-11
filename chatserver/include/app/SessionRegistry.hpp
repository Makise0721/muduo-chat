#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

class TcpConnection;
using TcpConnectionPtr = std::shared_ptr<TcpConnection>;

// 一个连接上已认证的会话：登录成功时绑定，登出/断开时解绑。
// userId=0 表示无会话；generation 来自 ChatApplication::beginSessionAttempt。
struct BoundSession {
    BoundSession() = default;
    BoundSession(int64_t uid, int64_t gen) : userId(uid), generation(gen) {}
    int64_t userId = 0;
    int64_t generation = 0;
};

// P3-05 进程内 SessionRegistry：深化 ChatService 的 _userConnMap，维护
// connection→session 与 user→connection 双向一致性（P4 才演进为
// PresenceDirectory port，见 docs/architecture/evolution-plan.md）。
//
// 语义：
// - 一个连接同时至多绑定一个 Session（B-21 收紧：同连接二次登录被拒）；
// - 一个 User 同时至多一个活动连接（B-08 单会话约束）；
// - 释放（unbind/unbindUser）幂等：未绑定时返回 0，绑定恰好释放一次。
//
// 线程安全：多 Reactor 下 handler/completion 在主 loop 线程、close 回调在
// 连接 loop 线程，全部公开方法互斥保护。
class SessionRegistry {
public:
    enum class BindResult {
        Ok,
        ConnectionBusy,  // 连接已绑定其他会话（同 User 或另一 User）
        UserBusy,        // 同一 User 已在另一连接活动
    };

    // 登录成功后绑定。单锁内原子判定：恰好一个成功者，无 find+insert 两步竞态。
    BindResult bind(const TcpConnectionPtr& conn, int64_t userId, int64_t generation);

    // 按连接解绑（断开/登出）。返回被释放的 userId；未绑定（重复释放）返回 0。
    int64_t unbind(const TcpConnectionPtr& conn);

    // 按用户解绑（登出按 payload id 语义）。返回被释放的 userId；未绑定返回 0。
    int64_t unbindUser(int64_t userId);

    // 未登录返回 false；out 为 nullptr 时仅做存在性检查。
    bool lookupByConnection(const TcpConnectionPtr& conn, BoundSession* out) const;

    // 未登录返回 nullptr。
    TcpConnectionPtr lookupByUser(int64_t userId) const;

    // 单锁快照：userIds 中已登录的 user → 连接（排除 excludeUserId），
    // 保持 groupChat 现状的单锁语义（一致性快照，非逐条查找）。
    std::unordered_map<int64_t, TcpConnectionPtr> snapshotConnections(
        const std::vector<int64_t>& userIds, int64_t excludeUserId) const;

    size_t size() const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<TcpConnectionPtr, BoundSession> byConnection_;
    std::unordered_map<int64_t, TcpConnectionPtr> byUser_;
};
