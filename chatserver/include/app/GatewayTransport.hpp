#pragma once

#include "app/DeliverySink.hpp"        // DeliveryAttempt（投递负载，mymuduo-safe）
#include "app/PresenceDirectory.hpp"   // DeliveryRoute / GatewayId / ConnectionId（路由投影）

#include <string>

// P4-05 跨 Gateway 投递 port（docs/tasks/P4-05.md §Interface、ADR-0002、
// docs/architecture/cluster-context-map.md §3）：把一条投递命令交给目标 Gateway。
// 本地路由（route.gatewayId == 本地 GatewayId）不应经本 port——本地走 sink 直投
// （GatewayAwareDeliverySink 内部短路）。transport 只按路由递送，不承载消息真相；
// epoch 校验在目标侧（目标收到跨节点投递后核验本地会话，GatewayTransportTarget）。
//
// 不抛：全部失败收敛为结果字段（日志/指标用 error，不含敏感 payload）。

struct GatewayDeliverResult {
    bool ok = false;
    bool staleEpoch = false;    // 目标侧核验发现 epoch 不匹配：已丢弃并触发目标侧重路由
    bool unreachable = false;   // 目标 Gateway 不可达（crash/分区/未注册）
    std::string error;          // 失败摘要（日志/指标，不含敏感 payload）
};

class GatewayTransport {
public:
    virtual ~GatewayTransport() = default;

    // route = Presence locate 返回的 DeliveryRoute；attempt = P3-07 冻结投递负载。
    virtual GatewayDeliverResult deliver(const DeliveryRoute& route,
                                         const DeliveryAttempt& attempt) = 0;
};

// 目标侧入口（InProcessGatewayTransport 注册/回调对象；GatewayAwareDeliverySink
// 实现此面）。目标侧核验 route.sessionEpoch vs 本地会话（影子表）并落目标侧 sink；
// 不匹配 → staleEpoch=true（丢弃，目标侧重路由）；匹配 → 本地 sink 投递。
class GatewayTransportTarget {
public:
    virtual ~GatewayTransportTarget() = default;

    virtual GatewayDeliverResult deliverCrossNode(const DeliveryRoute& route,
                                                  const DeliveryAttempt& attempt) = 0;
};
