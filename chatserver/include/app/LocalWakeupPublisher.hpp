#pragma once

#include "app/MessageStore.hpp"
#include "app/OutboxPublisher.hpp"
#include "app/ReliableMessaging.hpp"

#include <memory>
#include <string>
#include <vector>

// P4-03 本地 wakeup adapter（docs/tasks/P4-03.md §Port 替换设计决定）：把
// OutboxPublisher port 的 MessageAccepted 事件转回 P3-09 的本地幂等 wakeup——
// 从 Message 命令派生接收者（direct → directRecipient；group → 成员快照，与
// P3-09 LocalOutboxRelay::recipientsFor 同构）并调 ReliableMessaging::wakeupAccepted。
// 异常（含 wakeup 传播的瞬时存储故障）= 该事件失败 → 返回 not-ok outcome（不抛，
// port 契约）；relay 因此不标 processed、lease 到期重领重试（语义与 P3-09 一致）。
// 从命令快照派生接收者（与 ReliableMessaging::accept 的 recipientsFor 同构）：
// direct → 目标用户；group → 成员快照（保序）。测试/其它 adapter 复用。
inline std::vector<UserId> outboxRecipientsFor(const SendMessageCommand& cmd)
{
    if (cmd.kind == SendMessageCommand::Kind::Direct) {
        return std::vector<UserId>(1, cmd.directRecipient);
    }
    return cmd.members;
}

class LocalWakeupPublisher : public OutboxPublisher {
public:
    LocalWakeupPublisher(MessageStore& store, ReliableMessaging& rm) : store_(store), rm_(rm) {}

    std::vector<OutboxPublishOutcome> publish(
        const std::vector<OutboxPublishRequest>& batch, int64_t deadlineMs) override
    {
        (void)deadlineMs;
        std::vector<OutboxPublishOutcome> outcomes;
        outcomes.reserve(batch.size());
        for (size_t i = 0; i < batch.size(); ++i) {
            const OutboxPublishRequest& req = batch[i];
            OutboxPublishOutcome oc;
            try {
                const std::shared_ptr<const Message> msg =
                    store_.findMessage(req.event.aggregateMessageId);
                if (!msg) {
                    oc.failure = PublishFailure::Other;
                    oc.error = "outbox event without message";
                } else {
                    rm_.wakeupAccepted(outboxRecipientsFor(msg->command));
                    oc.ok = true;
                    oc.failure = PublishFailure::None;
                }
            } catch (const std::exception& e) {
                oc.failure = PublishFailure::Other;
                oc.error = e.what();
            }
            outcomes.push_back(oc);
        }
        return outcomes;
    }

private:
    MessageStore& store_;
    ReliableMessaging& rm_;
};
