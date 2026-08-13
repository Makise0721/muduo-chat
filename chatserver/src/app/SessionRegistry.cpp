#include "app/SessionRegistry.hpp"

void SessionRegistry::addConnection(const TcpConnectionPtr& conn)
{
    std::lock_guard<std::mutex> lock(mutex_);
    activeConnections_.insert(conn);
}

void SessionRegistry::removeConnection(const TcpConnectionPtr& conn)
{
    std::lock_guard<std::mutex> lock(mutex_);
    activeConnections_.erase(conn);
    closingConnections_.erase(conn);
}

bool SessionRegistry::reserveCloseIfBound(const TcpConnectionPtr& conn,
                                          const BoundSession& expected)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (activeConnections_.find(conn) == activeConnections_.end()) {
        return false;
    }
    auto bound = byConnection_.find(conn);
    if (bound == byConnection_.end() ||
        bound->second.userId != expected.userId ||
        bound->second.generation != expected.generation) {
        return false;
    }
    if (closingConnections_.find(conn) != closingConnections_.end()) {
        return false;
    }
    closingConnections_.insert(conn);
    return true;
}

SessionRegistry::BindResult SessionRegistry::bind(const TcpConnectionPtr& conn,
                                                  int64_t userId,
                                                  int64_t generation)
{
    std::lock_guard<std::mutex> lock(mutex_);
    // 活跃集合锁内判定：close 回调（removeConnection）先于本 completion 时
    // 连接已不在集合 → 拒绝，杜绝绑定已死连接导致的会话泄漏。
    if (activeConnections_.find(conn) == activeConnections_.end()) {
        return BindResult::ConnectionInactive;
    }
    if (closingConnections_.find(conn) != closingConnections_.end()) {
        return BindResult::ConnectionInactive;
    }
    if (byConnection_.find(conn) != byConnection_.end()) {
        return BindResult::ConnectionBusy;
    }
    if (byUser_.find(userId) != byUser_.end()) {
        return BindResult::UserBusy;
    }
    byConnection_.insert({conn, BoundSession(userId, generation)});
    byUser_.insert({userId, conn});
    return BindResult::Ok;
}

int64_t SessionRegistry::unbind(const TcpConnectionPtr& conn)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = byConnection_.find(conn);
    if (it == byConnection_.end()) {
        return 0;
    }
    int64_t userId = it->second.userId;
    byUser_.erase(userId);
    byConnection_.erase(it);
    return userId;
}

int64_t SessionRegistry::unbindUser(int64_t userId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = byUser_.find(userId);
    if (it == byUser_.end()) {
        return 0;
    }
    TcpConnectionPtr conn = it->second;
    byUser_.erase(it);
    auto cit = byConnection_.find(conn);
    if (cit != byConnection_.end() && cit->second.userId == userId) {
        byConnection_.erase(cit);
    }
    return userId;
}

bool SessionRegistry::lookupByConnection(const TcpConnectionPtr& conn,
                                         BoundSession* out) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = byConnection_.find(conn);
    if (it == byConnection_.end()) {
        return false;
    }
    if (out) {
        *out = it->second;
    }
    return true;
}

TcpConnectionPtr SessionRegistry::lookupByUser(int64_t userId) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = byUser_.find(userId);
    if (it == byUser_.end()) {
        return TcpConnectionPtr();
    }
    return it->second;
}

std::unordered_map<int64_t, TcpConnectionPtr> SessionRegistry::snapshotConnections(
    const std::vector<int64_t>& userIds, int64_t excludeUserId) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::unordered_map<int64_t, TcpConnectionPtr> out;
    for (size_t i = 0; i < userIds.size(); ++i) {
        int64_t uid = userIds[i];
        if (uid == excludeUserId) {
            continue;
        }
        auto it = byUser_.find(uid);
        if (it != byUser_.end()) {
            out.insert({uid, it->second});
        }
    }
    return out;
}

size_t SessionRegistry::size() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return byConnection_.size();
}
