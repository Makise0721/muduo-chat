#include "app/InMemoryMessageStore.hpp"

#include <algorithm>

std::shared_ptr<const Message> InMemoryMessageStore::findAccepted(const ClientMessageId& clientMessageId,
                                                                  UserId sender)
{
    std::map<std::pair<uint64_t, std::string>, std::shared_ptr<Message> >::iterator it =
        acceptedByKey_.find(std::make_pair(sender.value, clientMessageId.value()));
    if (it == acceptedByKey_.end()) {
        return std::shared_ptr<const Message>();
    }
    return it->second;
}

ConversationId InMemoryMessageStore::getOrCreateConversation(const SessionIdentity& sender,
                                                             const SendMessageCommand& cmd)
{
    if (cmd.kind == SendMessageCommand::Kind::Direct) {
        const uint64_t low = std::min(sender.userId.value, cmd.directRecipient.value);
        const uint64_t high = std::max(sender.userId.value, cmd.directRecipient.value);
        const std::pair<uint64_t, uint64_t> key(low, high);
        std::map<std::pair<uint64_t, uint64_t>, uint64_t>::iterator it = directConversations_.find(key);
        if (it != directConversations_.end()) {
            return ConversationId{it->second};
        }
        const uint64_t id = nextConversationId_++;
        directConversations_.insert(std::make_pair(key, id));
        return ConversationId{id};
    }
    std::map<uint64_t, uint64_t>::iterator it = groupConversations_.find(cmd.groupId.value);
    if (it != groupConversations_.end()) {
        return ConversationId{it->second};
    }
    const uint64_t id = nextConversationId_++;
    groupConversations_.insert(std::make_pair(cmd.groupId.value, id));
    return ConversationId{id};
}

Message InMemoryMessageStore::insertMessage(const Message& draft)
{
    Message m = draft;
    m.id = MessageId{nextMessageId_++};
    uint64_t& next = nextSequenceByConversation_[m.conversationId.value];
    ++next;
    m.sequence = ConversationSequence{next};
    std::shared_ptr<Message> stored(new Message(m));
    messagesById_.insert(std::make_pair(m.id.value, stored));
    acceptedByKey_.insert(std::make_pair(
        std::make_pair(m.senderId.value, m.command.clientMessageId.value()), stored));
    return m;
}

void InMemoryMessageStore::insertDelivery(const Delivery& delivery)
{
    DeliveryKey key;
    key.messageId = delivery.messageId.value;
    key.recipient = delivery.recipient.value;
    deliveries_.insert(std::make_pair(key, delivery));
}

void InMemoryMessageStore::updateDelivery(const Delivery& delivery)
{
    DeliveryKey key;
    key.messageId = delivery.messageId.value;
    key.recipient = delivery.recipient.value;
    std::map<DeliveryKey, Delivery>::iterator it = deliveries_.find(key);
    if (it != deliveries_.end()) {
        it->second = delivery;
    }
}

std::vector<Delivery> InMemoryMessageStore::deliveriesByRecipient(UserId recipient)
{
    std::vector<Delivery> out;
    for (std::map<DeliveryKey, Delivery>::const_iterator it = deliveries_.begin();
         it != deliveries_.end(); ++it) {
        if (it->second.recipient.value == recipient.value) {
            Delivery d = it->second;
            std::map<uint64_t, std::shared_ptr<Message> >::const_iterator m =
                messagesById_.find(d.messageId.value);
            if (m != messagesById_.end()) {
                d.sequence = m->second->sequence;
            }
            out.push_back(d);
        }
    }
    return out;
}

std::vector<Delivery> InMemoryMessageStore::deliveriesByMessage(MessageId messageId)
{
    std::vector<Delivery> out;
    for (std::map<DeliveryKey, Delivery>::const_iterator it = deliveries_.begin();
         it != deliveries_.end(); ++it) {
        if (it->second.messageId.value == messageId.value) {
            Delivery d = it->second;
            std::map<uint64_t, std::shared_ptr<Message> >::const_iterator m =
                messagesById_.find(d.messageId.value);
            if (m != messagesById_.end()) {
                d.sequence = m->second->sequence;
            }
            out.push_back(d);
        }
    }
    return out;
}

std::shared_ptr<const Message> InMemoryMessageStore::findMessage(MessageId messageId)
{
    std::map<uint64_t, std::shared_ptr<Message> >::iterator it = messagesById_.find(messageId.value);
    if (it == messagesById_.end()) {
        return std::shared_ptr<const Message>();
    }
    return it->second;
}

std::vector<Delivery> InMemoryMessageStore::deliveriesDueForRetry(int64_t nowMs, uint64_t limit)
{
    std::vector<Delivery> due;
    for (std::map<DeliveryKey, Delivery>::iterator it = deliveries_.begin();
         it != deliveries_.end() && due.size() < limit; ++it) {
        Delivery& d = it->second;
        if (d.state == DeliveryState::InFlight && d.nextAttemptAtMs > 0 &&
            d.nextAttemptAtMs <= nowMs) {
            due.push_back(d);
        }
    }
    return due;
}

uint32_t InMemoryMessageStore::expireDeliveries(int64_t nowMs, uint64_t limit)
{
    uint32_t moved = 0;
    for (std::map<DeliveryKey, Delivery>::iterator it = deliveries_.begin();
         it != deliveries_.end() && moved < limit; ++it) {
        Delivery& d = it->second;
        if (d.state != DeliveryState::Acknowledged && d.state != DeliveryState::Expired &&
            d.expiresAtMs > 0 && nowMs > d.expiresAtMs) {
            d.state = DeliveryState::Expired;
            ++moved;
        }
    }
    return moved;
}

uint32_t InMemoryMessageStore::cleanupDeliveries(int64_t ackedBeforeMs, int64_t expiredBeforeMs,
                                                 uint64_t limit)
{
    // 与 MySQL adapter 一致：每类最多 limit 行（acked/expired 各计各的上限，
    // 总处理 <= 2*limit，有界幂等）。
    uint32_t removedAcked = 0;
    uint32_t removedExpired = 0;
    std::map<DeliveryKey, Delivery>::iterator it = deliveries_.begin();
    while (it != deliveries_.end()) {
        const Delivery& d = it->second;
        const bool dueAcked = d.state == DeliveryState::Acknowledged && d.acknowledgedAtMs > 0 &&
                              d.acknowledgedAtMs <= ackedBeforeMs;
        const bool dueExpired = d.state == DeliveryState::Expired && d.expiresAtMs > 0 &&
                                d.expiresAtMs <= expiredBeforeMs;
        if (dueAcked && removedAcked < limit) {
            it = deliveries_.erase(it);
            ++removedAcked;
        } else if (dueExpired && removedExpired < limit) {
            it = deliveries_.erase(it);
            ++removedExpired;
        } else {
            ++it;
        }
    }
    return removedAcked + removedExpired;
}
