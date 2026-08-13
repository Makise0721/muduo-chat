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
