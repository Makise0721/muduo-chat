#include "app/GatewayAwareDeliverySink.hpp"

#include <utility>

namespace {

// renew 失败（Redis down）指数退避窗口（D4：不阻塞 accept/投递；生产默认）。
const int64_t kRenewBackoffBaseMs = 2000;
const int64_t kRenewBackoffCapMs = 60000;

}  // namespace

GatewayAwareDeliverySink::GatewayAwareDeliverySink(PresenceDirectory& presence,
                                                   GatewayId localGateway,
                                                   GatewayTransport& transport,
                                                   DeliverySink& localSink)
    : presence_(presence),
      localGateway_(localGateway),
      transport_(transport),
      localSink_(&localSink)
{
}

ClaimResult GatewayAwareDeliverySink::bindUser(UserId user, ConnectionId conn)
{
    std::lock_guard<std::mutex> lk(mutex_);
    // claim 不带 epoch：总是生成新 SessionEpoch 并原子覆盖既有条目
    //（cluster-context-map §3 关键不变式，无需旧 lease 已过期）。
    ClaimResult c = presence_.claim(user, localGateway_, conn);
    if (!c.ok) {
        // Redis down：登录暂停（不建 Presence 条目、影子不变；调用方回滚绑定）。
        return c;
    }
    Shadow s;
    s.connectionId = conn;
    s.epoch = c.epoch;
    shadow_[user] = s;
    nextRenewAtMs_.erase(user);
    renewBackoffMs_.erase(user);
    return c;
}

ReleaseResult GatewayAwareDeliverySink::unbindUser(UserId user, ConnectionId conn,
                                                   SessionEpoch epoch)
{
    std::lock_guard<std::mutex> lk(mutex_);
    // compare-and-delete：epoch 与当前条目一致才生效；NotEpoch 时条目不变。
    ReleaseResult r = presence_.release(user, localGateway_, conn, epoch);
    if (r.ok) {
        std::map<UserId, Shadow>::iterator it = shadow_.find(user);
        if (it != shadow_.end() && it->second.epoch == epoch) {
            shadow_.erase(it);
        }
        nextRenewAtMs_.erase(user);
        renewBackoffMs_.erase(user);
    }
    return r;
}

RenewResult GatewayAwareDeliverySink::renew(UserId user)
{
    // M1：阻塞式 Redis RTT 移出临界区（锁内快照 → 释放锁 renew → 回锁更新影子）。
    // 锁外 renew 期间（毫秒级 Redis RTT）bindUser/unbindUser 可在影子表上推进，
    // 故回锁时须按"当前影子 epoch == 快照 epoch"复核后才写回——否则放弃（他人已
    // 重 claim / release，本 renew 结果作废，返回 NotEpoch 让调用方按已有影子推进）。
    const int64_t now = clock_ != nullptr ? clock_->nowMs() : 0;

    SessionEpoch snapEpoch;
    ConnectionId snapConn;
    bool has = false;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        std::map<UserId, int64_t>::iterator nxt = nextRenewAtMs_.find(user);
        if (clock_ != nullptr && nxt != nextRenewAtMs_.end() && now < nxt->second) {
            // 退避窗口内：跳过本调度周期（Redis down 指数退避，不阻塞 accept/投递；
            // 测试缺省 clock_==nullptr → 不退避，now=0 不 gate）。
            RenewResult r;
            r.error = PresenceError::DependencyUnavailable;
            return r;
        }
        std::map<UserId, Shadow>::iterator it = shadow_.find(user);
        if (it == shadow_.end()) {
            RenewResult r;
            r.error = PresenceError::NotFound;
            return r;
        }
        snapEpoch = it->second.epoch;
        snapConn = it->second.connectionId;
        has = true;
    }

    RenewResult r = presence_.renew(user, localGateway_, snapConn, snapEpoch);
    if (r.ok) {
        std::lock_guard<std::mutex> lk(mutex_);
        // 复核：锁外 renew 期间他人可能已重 claim / release（epoch 变了则本结果
        // 作废）。
        std::map<UserId, Shadow>::iterator it = shadow_.find(user);
        if (it == shadow_.end() || it->second.epoch != snapEpoch) {
            RenewResult stale;
            stale.error = PresenceError::NotEpoch;
            return stale;
        }
        nextRenewAtMs_.erase(user);
        renewBackoffMs_.erase(user);
        return r;
    }
    if (r.error == PresenceError::NotFound && has) {
        // H1：条目丢失（Redis 恢复后原 lease 逻辑过期 / 被清理 / 从未续上）而本地
        // 影子仍 claimed → 原子重 claim（复用 claim 语义：新 epoch + 覆盖），无需
        // 重登即自愈（RedisRecoveryReclaimsPresenceAutomatically）。
        // 注意与"恰好被远端接管"的 NotEpoch 区分：被接管时条目存在且 epoch 不同，
        // renew 返回 NotEpoch（走下方返回，不重 claim、影子保留由新 claim 语义
        // 处理）；NotFound（缺失/过期）才重 claim。claim 仍走 presence（锁外 RTT）。
        ClaimResult c = presence_.claim(user, localGateway_, snapConn);
        if (c.ok) {
            std::lock_guard<std::mutex> lk(mutex_);
            std::map<UserId, Shadow>::iterator it = shadow_.find(user);
            if (it != shadow_.end() && it->second.epoch == snapEpoch) {
                // 重 claim 成功：更新影子为新 epoch（他人未在此期间重 claim）。
                it->second.epoch = c.epoch;
                nextRenewAtMs_.erase(user);
                renewBackoffMs_.erase(user);
            }
            RenewResult ok;
            ok.ok = true;
            return ok;
        }
        // 重 claim 也失败（Redis 又 down）：返回其 DependencyUnavailable（沿 renew
        // 的返回类型收敛）。
        RenewResult cr;
        cr.ok = false;
        cr.error = c.error;
        r = cr;
    }
    if (r.error == PresenceError::DependencyUnavailable && clock_ != nullptr) {
        // 指数退避：base 2s ×2 至 cap 60s（下次调度跳过；旧 epoch renew 被 fencing
        // 走 NotEpoch，不触发退避）。仅生产（clock 注入）启用；测试缺省 nullptr 不
        // 退避（避免 now=0 恒在窗口内 gate 掉后续 renew）。
        std::lock_guard<std::mutex> lk(mutex_);
        int64_t backoff = kRenewBackoffBaseMs;
        std::map<UserId, int64_t>::iterator bw = renewBackoffMs_.find(user);
        if (bw != renewBackoffMs_.end() && bw->second > 0) {
            backoff = bw->second;
        }
        nextRenewAtMs_[user] = now + backoff;
        const int64_t doubled = backoff * 2;
        renewBackoffMs_[user] = doubled > kRenewBackoffCapMs ? kRenewBackoffCapMs : doubled;
    }
    return r;
}

ReleaseResult GatewayAwareDeliverySink::releaseUser(UserId user, ConnectionId conn)
{
    std::lock_guard<std::mutex> lk(mutex_);
    std::map<UserId, Shadow>::iterator it = shadow_.find(user);
    if (it == shadow_.end()) {
        ReleaseResult r;
        r.error = PresenceError::NotFound;
        return r;
    }
    if (it->second.connectionId != conn) {
        // 防御：本连接不是当前 claim 的持有者（不应删除他人租约）。
        ReleaseResult r;
        r.error = PresenceError::NotEpoch;
        return r;
    }
    ReleaseResult r = presence_.release(user, localGateway_, conn, it->second.epoch);
    if (r.ok) {
        shadow_.erase(it);
        nextRenewAtMs_.erase(user);
        renewBackoffMs_.erase(user);
    }
    return r;
}

void GatewayAwareDeliverySink::renewAllClaimed()
{
    std::vector<UserId> users;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        users.reserve(shadow_.size());
        for (std::map<UserId, Shadow>::iterator it = shadow_.begin(); it != shadow_.end(); ++it) {
            users.push_back(it->first);
        }
    }
    for (size_t i = 0; i < users.size(); ++i) {
        (void)renew(users[i]);
    }
}

void GatewayAwareDeliverySink::setClock(Clock* clock)
{
    std::lock_guard<std::mutex> lk(mutex_);
    clock_ = clock;
}

SessionEpoch GatewayAwareDeliverySink::shadowEpoch(UserId user) const
{
    std::lock_guard<std::mutex> lk(mutex_);
    std::map<UserId, Shadow>::const_iterator it = shadow_.find(user);
    return it == shadow_.end() ? SessionEpoch(0) : it->second.epoch;
}

bool GatewayAwareDeliverySink::locallyClaimed(UserId user) const
{
    std::lock_guard<std::mutex> lk(mutex_);
    return shadow_.find(user) != shadow_.end();
}

DeliverDisposition GatewayAwareDeliverySink::deliver(const DeliveryAttempt& attempt)
{
    // P4-05 H1 本地短路：影子表是本 Gateway 的本地权威（D5）。已 claim 且 epoch
    // 匹配的本地用户**先查影子表短路本地直投，不进 Redis locate**（M1：阻塞式
    // Redis RTT 移出本地热路径）。安全论证（卡内记录）：影子表 entry 只在本地
    // claim 写入、release 清除，不含跨节点信息；Redis down 时登录 claim 暂停（P4-00/
    // ADR-0002），故不存在"本地影子命中但真实 Presence 已被另一 Gateway 接管"的
    // 迁移窗口——同 Gateway 用户间本地投递在 Redis down 下继续是冻结降级语义
    //（cluster-failure-contract §2 Redis down 行）。此短路只对本地影子命中生效；
    // 非 claim 用户仍走 Redis locate（离线/跨节点判定，见下）。
    {
        std::lock_guard<std::mutex> lk(mutex_);
        std::map<UserId, Shadow>::iterator it = shadow_.find(attempt.recipient);
        if (it != shadow_.end()) {
            // 本地 claim 命中：epoch/conn 由影子表权威（Redis down 时无迁移风险，
            // 见上安全论证），直接本地短路直投，绝不经过 transport（P4-05 可观察
            // 行为 ①）。
            return localSink_ != nullptr ? localSink_->deliver(attempt)
                                         : DeliverDisposition::Closed;
        }
    }

    // 非影子用户：deliver 前经 Presence locate（卡 Interface 可观察行为 ①）。
    DeliveryRoute route;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        const LocateResult l = presence_.locate(attempt.recipient);
        if (!l.ok) {
            // 无 Presence 条目 = 离线（含 Redis down：locate 失败按离线处理）——
            // Closed、保留 Pending，与单机离线语义一致（不误投）。
            return DeliverDisposition::Closed;
        }
        route = l.route;
    }

    if (route.gatewayId != localGateway_) {
        // 跨节点 → transport 路由（M2：locate 解析路由后已释放 mutex_ 再调
        // transport_.deliver，避免源侧持锁执行目标侧 I/O / AB-BA 环；结果映射回
        // 调用方即可，无需回锁写回状态——transport 是 stateless 路由）。
        const GatewayDeliverResult r = transport_.deliver(route, attempt);
        return r.ok ? DeliverDisposition::Accepted : DeliverDisposition::Closed;
    }
    // 本地匹配：epoch 校验 vs 本地影子表（D5）。
    std::lock_guard<std::mutex> lk(mutex_);
    std::map<UserId, Shadow>::iterator it = shadow_.find(attempt.recipient);
    if (it == shadow_.end() || it->second.epoch != route.sessionEpoch ||
        it->second.connectionId != route.connectionId) {
        // 旧 epoch（或本地未 claim）：丢弃 + 重路由（Closed 保留 Pending；重路由
        // = 本地重 claim/唤醒，绝不按旧 epoch 继续投递）。
        return DeliverDisposition::Closed;
    }
    // 本地短路直投（本地匹配绝不经过 transport）。
    return localSink_ != nullptr ? localSink_->deliver(attempt)
                                 : DeliverDisposition::Closed;
}

GatewayDeliverResult GatewayAwareDeliverySink::deliverCrossNode(const DeliveryRoute& route,
                                                                const DeliveryAttempt& attempt)
{
    std::lock_guard<std::mutex> lk(mutex_);
    if (route.gatewayId != localGateway_) {
        // 防御：路由目标不是本 Gateway（transport 路由表错配）。
        GatewayDeliverResult r;
        r.unreachable = true;
        r.error = "cross-node route targets a different gateway";
        return r;
    }
    std::map<UserId, Shadow>::iterator it = shadow_.find(route.user);
    if (it == shadow_.end() || it->second.epoch != route.sessionEpoch ||
        it->second.connectionId != route.connectionId) {
        // 目标侧核验发现 epoch 不匹配：丢弃（零写入），触发目标侧重路由。
        GatewayDeliverResult r;
        r.staleEpoch = true;
        r.error = "stale session epoch (dropped)";
        return r;
    }
    const DeliverDisposition d = localSink_ != nullptr ? localSink_->deliver(attempt)
                                                       : DeliverDisposition::Closed;
    GatewayDeliverResult r;
    r.ok = (d == DeliverDisposition::Accepted);
    if (!r.ok) {
        r.error = "local sink disposition is not accepted";
    }
    return r;
}
