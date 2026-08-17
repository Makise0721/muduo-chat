#pragma once

#include "app/GatewayTransport.hpp"

#include <cstdint>
#include <map>
#include <mutex>

// P4-05 in-process 共享路由表 adapter（docs/tasks/P4-05.md §最小实现/§冻结参数）：
// 把投递交给注册在进程内的目标实例（目标侧校验 epoch 并落到目标侧 sink，
// GatewayTransportTarget）；目标未注册 = crash/分区/未注册 → unreachable=true。
// 生产单 Gateway 拓扑下仅本地 Gateway 注册，跨 GatewayId 路由即不可达（按卡冻结
// 语义保留 Pending）；测试以两个注册实例模拟多 Gateway（RED 场景 3/4/5）。
// 线程安全：registerTarget/unregisterTarget 单互斥；deliver 在锁外调用目标回调
//（避免持锁执行目标侧 I/O，且目标回调可再入 transport 接口）。
// **生命周期契约（M2）**：注册的 GatewayTransportTarget 指针的生命周期必须长于
// 任何并发的 deliver 调用——deliver 在锁外读取并调用该指针，调用方（wrapper
// 源侧）可能在 unregisterTarget 之后仍持有已解析的 route 并异步进入 deliver；若
// 目标对象先于在途 deliver 析构，则是 UAF。生产单 Gateway 拓扑下目标即本进程的
// wrapper（与 transport 同生命周期），测试双实例目标亦须在 harness 析构前保持
// 存活。unregisterTarget 只从路由表中移除后续路由，不等待在途回调。
class InProcessGatewayTransport : public GatewayTransport {
public:
    // 覆盖式注册（重注册同 GatewayId = 替换目标；测试场景 5 恢复即重注册）。
    bool registerTarget(GatewayId gateway, GatewayTransportTarget* target);
    void unregisterTarget(GatewayId gateway);

    GatewayDeliverResult deliver(const DeliveryRoute& route,
                                 const DeliveryAttempt& attempt) override;

private:
    std::mutex mutex_;
    std::map<GatewayId, GatewayTransportTarget*> targets_;
};
