#pragma once

#include "app/DomainTypes.hpp"

// 投递出口 port（计划 §3）：真实 SessionDeliverySink（P3-07）与 Recording/Scripted
// 测试 adapter。
// 本头自包含且不声明 class Clock：被 mymuduo 侧 TU（SessionDeliverySink.cpp，
// TcpConnection.h 的全局 `using Clock`）引入，只能依赖 DomainTypes.hpp。

// 投递命令种类（与 SendMessageCommand::Kind 对应；sink 侧避免依赖完整命令类型）。
enum class AttemptKind {
    Direct,
    Group,
};

// 一次 DeliveryAttempt：TCP 写入只是尝试，本值类型无"成功送达"语义。
struct DeliveryAttempt {
    MessageId messageId;
    ConversationId conversationId;
    ConversationSequence sequence;
    UserId senderId;        // 投递 JSON 的发送者（旧格式 id 字段）
    UserId recipient;
    AttemptKind kind;       // 投递 JSON 复用原命令 msgid 6/10
    UserId directRecipient; // Kind::Direct 的目标用户（= 接收者）
    GroupId groupId;        // Kind::Group 的群
    std::string content;
    uint32_t attemptNumber = 0;
};

// 一次投递尝试的结果（spec §3 网络联动）：只有 Accepted 才记录 attempt。
// 冻结映射见 docs/tasks/P3-07.md：Accepted+Normal→Accepted；Accepted+PauseProducer
// 或 WouldBlock→Paused（不记、保留 Pending、暂停该 Session 等 low-water）；
// Closed/TooLarge→Closed（不记、保留 Pending）。
enum class DeliverDisposition {
    Accepted,  // socket 准入且无背压：协调器记 attempt、转 InFlight
    Paused,    // 准入但 PauseProducer，或 WouldBlock：不记 attempt，low-water 后重试
    Closed,    // 连接断开/不存在（或 TooLarge 防御）：不记 attempt，保留 Pending
};

class DeliverySink {
public:
    virtual ~DeliverySink() = default;

    // 一次 DeliveryAttempt；调用不代表接收端已收到（at-least-once）。
    virtual DeliverDisposition deliver(const DeliveryAttempt& attempt) = 0;
};

// Stable adapter retained by ReliableMessaging while the concrete sink can be
// registered after lazy wiring has materialized. Unbound delivery fails closed
// so the coordinator keeps it Pending instead of recording a false Accepted.
class DelegatingDeliverySink : public DeliverySink {
public:
    DeliverDisposition deliver(const DeliveryAttempt& attempt) override
    {
        return delegate_ == nullptr ? DeliverDisposition::Closed
                                     : delegate_->deliver(attempt);
    }

    // Bind exactly once: a first non-null delegate succeeds, rebinding the
    // same pointer is idempotent, and a different delegate is rejected while
    // retaining the original target.
    bool bind(DeliverySink* delegate)
    {
        if (delegate == nullptr) {
            return false;
        }
        if (delegate_ == nullptr) {
            delegate_ = delegate;
            return true;
        }
        return delegate_ == delegate;
    }

private:
    DeliverySink* delegate_ = nullptr;
};
