#include "app/InMemoryMessageStore.hpp"

#include <algorithm>

#include "json.hpp"

namespace {

// 与 MySQLMessageStore 的命令快照编码一致（kind/direct_recipient|group_id/
// members 保序），使双 adapter 的 OutboxEvent.payload 可观测对称。
std::string encodeOutboxPayload(const SendMessageCommand& cmd)
{
    nlohmann::json j;
    if (cmd.kind == SendMessageCommand::Kind::Direct) {
        j["kind"] = "DIRECT";
        j["direct_recipient"] = cmd.directRecipient.value;
    } else {
        j["kind"] = "GROUP";
        j["group_id"] = cmd.groupId.value;
        nlohmann::json members = nlohmann::json::array();
        for (size_t i = 0; i < cmd.members.size(); ++i) {
            members.push_back(cmd.members[i].value);
        }
        j["members"] = members;
    }
    return j.dump();
}

} // namespace

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
    // P3-09：与 MySQLMessageStore 事务内写入对称（accept 同点写 OutboxEvent）。
    // available_at 取 accept 时刻（Message.acceptedAtMs；MySQL 用 CURRENT_TIMESTAMP）。
    OutboxEvent ev;
    ev.id = nextOutboxEventId_++;
    ev.aggregateMessageId = m.id;
    ev.eventType = "MessageAccepted";
    ev.payload = encodeOutboxPayload(m.command);
    ev.availableAtMs = m.acceptedAtMs;
    outboxEvents_.insert(std::make_pair(ev.id, ev));
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

uint32_t InMemoryMessageStore::expireDeliveriesDetailed(int64_t nowMs, uint64_t limit,
                                                        std::vector<ExpiredDeliveryRecord>* out)
{
    // 与 expireDeliveries 同一谓词/有界顺序；额外逐行报告 (messageId, recipient,
    // fromState) 供 metrics 记录 Expired 转移（默认实现回退 expireDeliveries，
    // 双 adapter override 报明细）。
    uint32_t moved = 0;
    for (std::map<DeliveryKey, Delivery>::iterator it = deliveries_.begin();
         it != deliveries_.end() && moved < limit; ++it) {
        Delivery& d = it->second;
        if (d.state != DeliveryState::Acknowledged && d.state != DeliveryState::Expired &&
            d.expiresAtMs > 0 && nowMs > d.expiresAtMs) {
            if (out != nullptr) {
                ExpiredDeliveryRecord r;
                r.messageId = d.messageId.value;
                r.recipient = d.recipient.value;
                r.fromState = d.state;
                out->push_back(r);
            }
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

std::vector<OutboxEvent> InMemoryMessageStore::claimOutboxEvents(int64_t nowMs,
                                                                 const std::string& leaseOwner,
                                                                 int64_t leaseUntilMs,
                                                                 uint64_t limit)
{
    std::vector<OutboxEvent> out;
    for (std::map<uint64_t, OutboxEvent>::iterator it = outboxEvents_.begin();
         it != outboxEvents_.end() && out.size() < limit; ++it) {
        OutboxEvent& e = it->second;
        if (e.processedAtMs == 0 && e.availableAtMs <= nowMs &&
            (e.leaseOwner.empty() || e.leaseUntilMs <= nowMs)) {
            e.leaseOwner = leaseOwner;
            e.leaseUntilMs = leaseUntilMs;
            e.attemptCount += 1;
            out.push_back(e);
        }
    }
    return out;
}

void InMemoryMessageStore::markOutboxProcessed(uint64_t eventId, int64_t nowMs)
{
    std::map<uint64_t, OutboxEvent>::iterator it = outboxEvents_.find(eventId);
    if (it == outboxEvents_.end()) {
        return;
    }
    it->second.processedAtMs = nowMs;
    it->second.leaseOwner.clear();
    it->second.leaseUntilMs = 0;
}

void InMemoryMessageStore::markOutboxPoisoned(uint64_t eventId, int64_t nowMs)
{
    (void)nowMs;
    // poison 谓词 = 未处理（processed_at NULL）：事件已保持未 processed、lease
    // 保留由到期驱动重领重试；可查询、绝不静默丢弃。此处无额外状态要写。
    (void)eventId;
}

std::shared_ptr<const OutboxEvent> InMemoryMessageStore::findOutboxEvent(uint64_t eventId)
{
    std::map<uint64_t, OutboxEvent>::iterator it = outboxEvents_.find(eventId);
    if (it == outboxEvents_.end()) {
        return std::shared_ptr<const OutboxEvent>();
    }
    return std::shared_ptr<const OutboxEvent>(new OutboxEvent(it->second));
}

std::vector<OutboxEvent> InMemoryMessageStore::poisonedOutboxEvents(uint64_t limit)
{
    std::vector<OutboxEvent> out;
    for (std::map<uint64_t, OutboxEvent>::iterator it = outboxEvents_.begin();
         it != outboxEvents_.end() && out.size() < limit; ++it) {
        if (it->second.processedAtMs == 0) {
            out.push_back(it->second);
        }
    }
    return out;
}

uint64_t InMemoryMessageStore::countUnprocessedOutboxEvents()
{
    uint64_t n = 0;
    for (std::map<uint64_t, OutboxEvent>::const_iterator it = outboxEvents_.begin();
         it != outboxEvents_.end(); ++it) {
        if (it->second.processedAtMs == 0) {
            ++n;
        }
    }
    return n;
}

void InMemoryMessageStore::recordDeadLetter(const DeadLetterRecord& r)
{
    // UNIQUE(topic, partition_id, kafka_offset) 的内存等价：kill-前已落库的事件
    // 重放时不双插（幂等，与 MySQL INSERT IGNORE 对称）。
    for (size_t i = 0; i < deadLetters_.size(); ++i) {
        if (deadLetters_[i].topic == r.topic
            && deadLetters_[i].partitionId == r.partitionId
            && deadLetters_[i].kafkaOffset == r.kafkaOffset) {
            return;  // 已存在，成功返回
        }
    }
    deadLetters_.push_back(r);
}

std::vector<DeadLetterRecord> InMemoryMessageStore::deadLetters(uint64_t limit)
{
    std::vector<DeadLetterRecord> out;
    for (size_t i = 0; i < deadLetters_.size() && out.size() < limit; ++i) {
        out.push_back(deadLetters_[i]);
    }
    return out;
}
