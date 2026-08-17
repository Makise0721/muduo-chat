#pragma once

#include "app/Clock.hpp"               // Clock*：生产 renew 退避时钟注入（测试不经）
#include "app/DeliverySink.hpp"
#include "app/GatewayTransport.hpp"
#include "app/PresenceDirectory.hpp"

#include <cstdint>
#include <map>
#include <mutex>
#include <vector>

// P4-05 单 Gateway 投递 + 本地 Presence 生命周期接线对象（docs/tasks/P4-05.md
// 设计决定 D3/D4/D5，GatewayDeliveryContractTest / GatewayPresenceWiringContractTest
// 冻结接口）：持有本地影子表 (userId → (connId, SessionEpoch))（D5：claim 写入、
// release 清除；内部 seam，不进 Presence port）；deliver 前经 Presence locate +
// 影子 epoch 校验：
//   - 无条目 = 离线（Closed、保留 Pending，与单机离线语义一致）；
//   - 本地匹配 = 影子校验后本地 sink 直投（本地短路，绝不经过 transport）；
//   - 跨节点 = transport 路由（目标侧核验 epoch，GatewayTransportTarget）；
//   - epoch 不匹配 = 丢弃 + 重路由（Closed、保留 Pending；重路由 = 本地重 claim/
//     唤醒，不产生第二套状态机）。
// bindUser/unbindUser/renew 为生产接线公开面（login claim、close/loginout
// compare-and-delete release、renew 调度），全部穿过 PresenceDirectory port。
//
// 线程语义：单 mutex_ 串行化影子表与 presence 访问（RedisPresenceDirectory 持单
// RedisConn 不跨线程共享，本对象是生产唯一 accessor，锁内串行即安全）；
// deliver/deliverCrossNode 来自 RM 各线程（executor/scheduler/consumer poll），
// bindUser/unbindUser/renew 来自 EventLoop 线程（登录 completion / renew 定时器）。

class GatewayAwareDeliverySink : public DeliverySink, public GatewayTransportTarget {
public:
    GatewayAwareDeliverySink(PresenceDirectory& presence, GatewayId localGateway,
                             GatewayTransport& transport, DeliverySink& localSink);

    // login wiring（bind 成功后、sessionAvailableDelivery 提交前）：claim 生成新
    // epoch 原子覆盖 + 写影子表；Redis down → 失败（登录暂停：不建条目、影子
    // 不变）。调用方只在 ok 时提交 sessionAvailableDelivery。
    ClaimResult bindUser(UserId user, ConnectionId conn);
    // close/loginout wiring：对绑定 (user, gateway, conn, epoch) compare-and-delete
    // release；旧 epoch 被拒（NotEpoch）且条目不变；成功后清影子。
    ReleaseResult unbindUser(UserId user, ConnectionId conn, SessionEpoch epoch);
    // renew 调度（调用方每 TTL/2 驱动，D4）：携带当前影子 epoch；旧 epoch 由
    // adapter fencing（NotEpoch）——重登后旧调度 renew 被拒。
    RenewResult renew(UserId user);

    // 影子表投影（可观测内部 seam）：未绑定 = SessionEpoch(0)。
    SessionEpoch shadowEpoch(UserId user) const;
    bool locallyClaimed(UserId user) const;

    DeliverDisposition deliver(const DeliveryAttempt& attempt) override;
    GatewayDeliverResult deliverCrossNode(const DeliveryRoute& route,
                                          const DeliveryAttempt& attempt) override;

    // ---- P4-05 生产接线 seam（测试不经此；deps 文档见头注释）----
    // close/loginout 便捷入口：按本地影子 epoch compare-and-delete release
    //（调用方无需回传 epoch；幂等 NotFound）。
    ReleaseResult releaseUser(UserId user, ConnectionId conn);
    // renew 调度：遍历本地已 claim 用户（生产 main loop runEvery(TTL/2) 驱动）。
    void renewAllClaimed();
    // 生产 wiring 注入时钟（renew 失败指数退避）；测试缺省 nullptr → 不退避。
    void setClock(Clock* clock);

private:
    struct Shadow {
        ConnectionId connectionId;
        SessionEpoch epoch;
    };

    PresenceDirectory& presence_;
    GatewayId localGateway_;
    GatewayTransport& transport_;
    DeliverySink* localSink_;
    Clock* clock_ = nullptr;
    mutable std::mutex mutex_;
    std::map<UserId, Shadow> shadow_;           // D5 本地影子表
    std::map<UserId, int64_t> nextRenewAtMs_;   // renew 指数退避门（生产）
    std::map<UserId, int64_t> renewBackoffMs_;  // 当前退避窗口（下次 ×2，cap）
};
