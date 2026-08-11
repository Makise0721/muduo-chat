#pragma once

#include "app/ReliableMessaging.hpp"

// 投递出口 port（计划 §3）：真实 SessionRegistry adapter（P3-07）与 Recording 测试 adapter。
class DeliverySink {
public:
    virtual ~DeliverySink() = default;

    // 一次 DeliveryAttempt；调用不代表接收端已收到（at-least-once）。
    virtual void deliver(const DeliveryAttempt& attempt) = 0;
};
