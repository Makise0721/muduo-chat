#pragma once

#include "app/GatewayTransport.hpp"

#include <cstdint>
#include <string>
#include <vector>

// P4-05 recording/脚本化测试 adapter（docs/tasks/P4-05.md §最小实现）：记录每次
// deliver 调用与结果；可脚本化注入 staleEpoch/unreachable（一次性，触发即清除），
// 使 GatewayAwareDeliverySink 把跨节点投递降为 Closed（丢弃+重路由/保留 Pending）；
// clearScript 恢复 ok=true。recording transport 是断言替身，不接触真实目标 sink。
class RecordingGatewayTransport : public GatewayTransport {
public:
    struct Record {
        DeliveryRoute route;
        DeliveryAttempt attempt;
        GatewayDeliverResult result;
    };

    void scriptStaleEpoch();    // 下一条 deliver 返回 staleEpoch=true（丢弃）
    void scriptUnreachable();   // 下一条 deliver 返回 unreachable=true
    void clearScript();         // 恢复 ok=true

    GatewayDeliverResult deliver(const DeliveryRoute& route,
                                 const DeliveryAttempt& attempt) override;

    const std::vector<Record>& records() const { return records_; }

private:
    std::vector<Record> records_;
    bool scriptStale_ = false;
    bool scriptUnreachable_ = false;
};
