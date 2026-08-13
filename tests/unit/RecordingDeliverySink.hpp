#pragma once

#include "app/DeliverySink.hpp"

#include <vector>

// Recording 测试 adapter（计划 §3）：记录全部 DeliveryAttempt（默认 Accepted），
// 供契约断言。
class RecordingDeliverySink : public DeliverySink {
public:
    DeliverDisposition deliver(const DeliveryAttempt& attempt) override
    {
        attempts_.push_back(attempt);
        return DeliverDisposition::Accepted;
    }

    const std::vector<DeliveryAttempt>& attempts() const { return attempts_; }

private:
    std::vector<DeliveryAttempt> attempts_;
};
