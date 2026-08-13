#pragma once

#include "app/MessageStore.hpp"

#include <map>
#include <memory>
#include <string>
#include <vector>

// 内存 adapter（计划 §3）：契约测试与开发期使用，无数据库依赖。
class InMemoryMessageStore : public MessageStore {
public:
    std::shared_ptr<const Message> findAccepted(const ClientMessageId& clientMessageId,
                                                UserId sender) override;
    ConversationId getOrCreateConversation(const SessionIdentity& sender,
                                           const SendMessageCommand& cmd) override;
    Message insertMessage(const Message& draft) override;
    void insertDelivery(const Delivery& delivery) override;
    void updateDelivery(const Delivery& delivery) override;
    std::vector<Delivery> deliveriesByRecipient(UserId recipient) override;
    std::vector<Delivery> deliveriesByMessage(MessageId messageId) override;
    std::shared_ptr<const Message> findMessage(MessageId messageId) override;
    std::vector<Delivery> deliveriesDueForRetry(int64_t nowMs, uint64_t limit) override;
    uint32_t expireDeliveries(int64_t nowMs, uint64_t limit) override;
    uint32_t cleanupDeliveries(int64_t ackedBeforeMs, int64_t expiredBeforeMs,
                               uint64_t limit) override;

private:
    struct DeliveryKey {
        uint64_t messageId = 0;
        uint64_t recipient = 0;

        bool operator<(const DeliveryKey& other) const
        {
            if (messageId != other.messageId) {
                return messageId < other.messageId;
            }
            return recipient < other.recipient;
        }
    };

    std::map<std::pair<uint64_t, std::string>, std::shared_ptr<Message> > acceptedByKey_;
    std::map<uint64_t, std::shared_ptr<Message> > messagesById_;
    std::map<std::pair<uint64_t, uint64_t>, uint64_t> directConversations_;
    std::map<uint64_t, uint64_t> groupConversations_;
    std::map<uint64_t, uint64_t> nextSequenceByConversation_;
    std::map<DeliveryKey, Delivery> deliveries_;
    uint64_t nextConversationId_ = 1;
    uint64_t nextMessageId_ = 1;
};
