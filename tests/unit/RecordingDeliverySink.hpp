#pragma once

#include "app/DeliverySink.hpp"

#include <vector>

// Recording 测试 adapter（计划 §3）：记录全部 DeliveryAttempt，供契约断言。
class RecordingDeliverySink : public DeliverySink {
public:
    void deliver(const DeliveryAttempt& attempt) override { attempts_.push_back(attempt); }

    const std::vector<DeliveryAttempt>& attempts() const { return attempts_; }

private:
    std::vector<DeliveryAttempt> attempts_;
};
