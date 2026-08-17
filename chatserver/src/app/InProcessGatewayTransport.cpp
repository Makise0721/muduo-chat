#include "app/InProcessGatewayTransport.hpp"

#include <utility>

bool InProcessGatewayTransport::registerTarget(GatewayId gateway, GatewayTransportTarget* target)
{
    if (target == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lk(mutex_);
    targets_[gateway] = target;
    return true;
}

void InProcessGatewayTransport::unregisterTarget(GatewayId gateway)
{
    std::lock_guard<std::mutex> lk(mutex_);
    targets_.erase(gateway);
}

GatewayDeliverResult InProcessGatewayTransport::deliver(const DeliveryRoute& route,
                                                        const DeliveryAttempt& attempt)
{
    GatewayTransportTarget* target = nullptr;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        std::map<GatewayId, GatewayTransportTarget*>::iterator it = targets_.find(route.gatewayId);
        if (it == targets_.end()) {
            // 目标 Gateway 崩溃/分区/未注册：不记 attempt，调用方（wrapper）保留 Pending，
            // 由后续 claim/调度重路由（卡 §RED 场景 5）。
            GatewayDeliverResult r;
            r.unreachable = true;
            r.error = "target gateway not registered in-process";
            return r;
        }
        target = it->second;
    }
    // 锁外回调：目标侧核验 epoch 并落目标 sink（不持 transport 锁执行目标侧 I/O）。
    return target->deliverCrossNode(route, attempt);
}
