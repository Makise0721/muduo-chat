#include "app/InMemoryPresenceDirectory.hpp"

#include <utility>

InMemoryPresenceDirectory::InMemoryPresenceDirectory(Clock& clock, int64_t ttlMs)
    : clock_(clock), ttlMs_(ttlMs), nextEpoch_(1)
{
}

ClaimResult InMemoryPresenceDirectory::claim(UserId user, GatewayId gateway, ConnectionId conn)
{
    std::lock_guard<std::mutex> lk(mutex_);
    ClaimResult result;
    if (failure_) {
        result.error = PresenceError::DependencyUnavailable;
        return result;
    }
    // 无条件原子覆盖：无论旧 lease 是否过期，claim 都生成新 epoch 并整体覆盖
    // （cluster-context-map §3 关键不变式；epoch 由全局单调计数器生成，绝不回退）。
    const SessionEpoch epoch(nextEpoch_++);
    entries_[user] = Entry(gateway, conn, epoch, clock_.nowMs() + ttlMs_);
    result.ok = true;
    result.epoch = epoch;
    return result;
}

RenewResult InMemoryPresenceDirectory::renew(UserId user, GatewayId gateway, ConnectionId conn,
                                             SessionEpoch epoch)
{
    std::lock_guard<std::mutex> lk(mutex_);
    RenewResult result;
    if (failure_) {
        result.error = PresenceError::DependencyUnavailable;
        return result;
    }
    std::map<UserId, Entry>::iterator it = entries_.find(user);
    if (it == entries_.end()) {
        result.error = PresenceError::NotFound;
        return result;
    }
    if (clock_.nowMs() >= it->second.expiresAtMs) {
        result.error = PresenceError::NotFound;  // TTL 到期视为不存在
        return result;
    }
    if (!(it->second.sessionEpoch == epoch)) {
        result.error = PresenceError::NotEpoch;  // compare-and-delete：旧 epoch 被拒且条目不变
        return result;
    }
    it->second.expiresAtMs = clock_.nowMs() + ttlMs_;
    result.ok = true;
    result.expiresAtMs = it->second.expiresAtMs;
    return result;
}

ReleaseResult InMemoryPresenceDirectory::release(UserId user, GatewayId gateway,
                                                 ConnectionId conn, SessionEpoch epoch)
{
    std::lock_guard<std::mutex> lk(mutex_);
    ReleaseResult result;
    if (failure_) {
        result.error = PresenceError::DependencyUnavailable;
        return result;
    }
    std::map<UserId, Entry>::iterator it = entries_.find(user);
    if (it == entries_.end()) {
        result.error = PresenceError::NotFound;
        return result;
    }
    if (clock_.nowMs() >= it->second.expiresAtMs) {
        result.error = PresenceError::NotFound;  // TTL 到期视为不存在
        return result;
    }
    if (!(it->second.sessionEpoch == epoch)) {
        result.error = PresenceError::NotEpoch;  // compare-and-delete：旧 epoch 被拒且条目不变
        return result;
    }
    entries_.erase(it);
    result.ok = true;
    return result;
}

LocateResult InMemoryPresenceDirectory::locate(UserId user)
{
    std::lock_guard<std::mutex> lk(mutex_);
    LocateResult result;
    if (failure_) {
        result.error = PresenceError::DependencyUnavailable;
        return result;
    }
    std::map<UserId, Entry>::iterator it = entries_.find(user);
    if (it == entries_.end()) {
        result.error = PresenceError::NotFound;  // 从未 claim / release 后
        return result;
    }
    if (clock_.nowMs() >= it->second.expiresAtMs) {
        result.expired = true;  // TTL 到期视为不存在，与"从未 claim"可区分
        result.error = PresenceError::NotFound;
        return result;
    }
    result.ok = true;
    result.route.user = user;
    result.route.gatewayId = it->second.gatewayId;
    result.route.connectionId = it->second.connectionId;
    result.route.sessionEpoch = it->second.sessionEpoch;
    return result;
}

void InMemoryPresenceDirectory::injectFailure(bool fail)
{
    std::lock_guard<std::mutex> lk(mutex_);
    failure_ = fail;
}
