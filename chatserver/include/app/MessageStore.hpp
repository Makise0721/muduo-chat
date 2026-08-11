#pragma once

#include "app/ReliableMessaging.hpp"

#include <memory>
#include <vector>

// 内部 seam（计划 §3）：InMemory/MySQL 两个 adapter；本体只经此访问持久状态，
// 领域状态机逻辑（幂等判定、claim、lease、顺序）不放 adapter。
class MessageStore {
public:
    virtual ~MessageStore() = default;

    // (sender, clientMessageId) 已接受 → 原 Message；否则 nullptr。
    virtual std::shared_ptr<const Message> findAccepted(const ClientMessageId& clientMessageId,
                                                        UserId sender) = 0;

    // 命令参与者对应的 Conversation，不存在则创建（direct 按无序用户对，group 按 GroupId）。
    virtual ConversationId getOrCreateConversation(const SessionIdentity& sender,
                                                   const SendMessageCommand& cmd) = 0;

    // 写入 Message：adapter 分配 MessageId，并保证 Conversation 内 sequence 严格递增。
    virtual Message insertMessage(const Message& draft) = 0;

    virtual void insertDelivery(const Delivery& delivery) = 0;
    virtual void updateDelivery(const Delivery& delivery) = 0;

    virtual std::vector<Delivery> deliveriesByRecipient(UserId recipient) = 0;
    virtual std::vector<Delivery> deliveriesByMessage(MessageId messageId) = 0;
    virtual std::shared_ptr<const Message> findMessage(MessageId messageId) = 0;
};
