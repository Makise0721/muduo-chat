#include "app/ReliableMessaging.hpp"

#include "app/DeliverySink.hpp"
#include "app/MessageStore.hpp"

#include <algorithm>
#include <map>
#include <memory>
#include <vector>

ReliableMessaging::ReliableMessaging(MessageStore& store, DeliverySink& sink, Clock& clock,
                                     uint64_t leaseMs)
    : store_(store), sink_(sink), clock_(clock), leaseMs_(leaseMs)
{
}

AcceptOutcome ReliableMessaging::accept(const SessionIdentity& sender, const SendMessageCommand& cmd)
{
    AcceptOutcome outcome;
    std::shared_ptr<const Message> existing = store_.findAccepted(cmd.clientMessageId, sender.userId);
    if (existing) {
        if (samePayload(existing->command, cmd)) {
            outcome.ok = true;
            outcome.duplicate = true;
            outcome.messageId = existing->id;
            outcome.conversationId = existing->conversationId;
            outcome.sequence = existing->sequence;
        } else {
            outcome.error = AcceptError::IdempotencyConflict;
        }
        return outcome;
    }

    Message draft;
    draft.senderId = sender.userId;
    draft.command = cmd;
    draft.conversationId = store_.getOrCreateConversation(sender, cmd);
    const Message accepted = store_.insertMessage(draft);

    std::vector<UserId> recipients;
    if (cmd.kind == SendMessageCommand::Kind::Direct) {
        recipients.push_back(cmd.directRecipient);
    } else {
        recipients = cmd.members;
    }
    for (size_t i = 0; i < recipients.size(); ++i) {
        Delivery delivery;
        delivery.messageId = accepted.id;
        delivery.conversationId = accepted.conversationId;
        delivery.recipient = recipients[i];
        store_.insertDelivery(delivery);
    }

    outcome.ok = true;
    outcome.messageId = accepted.id;
    outcome.conversationId = accepted.conversationId;
    outcome.sequence = accepted.sequence;
    return outcome;
}

AckOutcome ReliableMessaging::acknowledge(const SessionIdentity& acker, MessageId messageId)
{
    AckOutcome outcome;
    std::vector<Delivery> deliveries = store_.deliveriesByMessage(messageId);
    if (deliveries.empty()) {
        return outcome;  // NotFound
    }
    Delivery* mine = nullptr;
    for (size_t i = 0; i < deliveries.size(); ++i) {
        if (deliveries[i].recipient == acker.userId) {
            mine = &deliveries[i];
            break;
        }
    }
    if (mine == nullptr) {
        outcome.result = AckResult::NotRecipient;
        return outcome;
    }
    if (mine->state == DeliveryState::Acknowledged) {
        outcome.result = AckResult::Idempotent;
        return outcome;
    }
    mine->state = DeliveryState::Acknowledged;
    mine->acknowledgedAtMs = clock_.nowMs();
    store_.updateDelivery(*mine);
    claimFor(acker);  // ACK 放行同 Conversation 的下一 sequence
    outcome.result = AckResult::Acknowledged;
    return outcome;
}

void ReliableMessaging::sessionAvailable(const SessionIdentity& session)
{
    claimFor(session);
}

void ReliableMessaging::sessionClosed(const SessionIdentity& session)
{
    std::vector<Delivery> deliveries = store_.deliveriesByRecipient(session.userId);
    for (size_t i = 0; i < deliveries.size(); ++i) {
        Delivery& d = deliveries[i];
        if (d.state == DeliveryState::InFlight && d.leaseOwner == session) {
            d.state = DeliveryState::Pending;
            d.leaseOwner = SessionIdentity();
            d.leaseUntilMs = 0;
            store_.updateDelivery(d);
        }
    }
}

void ReliableMessaging::claimFor(const SessionIdentity& session)
{
    std::vector<Delivery> deliveries = store_.deliveriesByRecipient(session.userId);
    std::map<uint64_t, std::vector<Delivery*> > byConversation;
    for (size_t i = 0; i < deliveries.size(); ++i) {
        Delivery& d = deliveries[i];
        if (d.state == DeliveryState::Acknowledged || d.state == DeliveryState::Expired) {
            continue;
        }
        byConversation[d.conversationId.value].push_back(&d);
    }

    const int64_t now = clock_.nowMs();
    for (std::map<uint64_t, std::vector<Delivery*> >::iterator it = byConversation.begin();
         it != byConversation.end(); ++it) {
        // head-of-line：只考虑同 (recipient, conversation) 内 sequence 最小的未确认 Delivery。
        Delivery* head = nullptr;
        ConversationSequence headSequence;
        std::shared_ptr<const Message> headMessage;
        for (size_t i = 0; i < it->second.size(); ++i) {
            Delivery* d = it->second[i];
            std::shared_ptr<const Message> m = store_.findMessage(d->messageId);
            if (!m) {
                continue;  // 存储一致性防御：缺 Message 的 Delivery 不投递
            }
            if (head == nullptr || m->sequence.value < headSequence.value) {
                head = d;
                headSequence = m->sequence;
                headMessage = m;
            }
        }
        if (head == nullptr) {
            continue;
        }
        // 单在途：有效 lease 的 InFlight 不被重领；lease 到期可重领。
        const bool claimable = head->state == DeliveryState::Pending ||
                               (head->state == DeliveryState::InFlight && head->leaseUntilMs <= now);
        if (!claimable) {
            continue;
        }
        head->state = DeliveryState::InFlight;
        head->leaseOwner = session;
        head->leaseUntilMs = now + static_cast<int64_t>(leaseMs_);
        head->attemptCount += 1;
        store_.updateDelivery(*head);

        DeliveryAttempt attempt;
        attempt.messageId = head->messageId;
        attempt.conversationId = head->conversationId;
        attempt.sequence = headSequence;
        attempt.recipient = head->recipient;
        attempt.content = headMessage->command.content;
        attempt.attemptNumber = head->attemptCount;
        sink_.deliver(attempt);
    }
}

void ReliableMessaging::start()
{
    // in-memory 无后台任务；P3-08 引入 timer 驱动的到期重试。
}

void ReliableMessaging::stop(int64_t deadlineMs)
{
    (void)deadlineMs;
    // in-memory 无后台任务，直接返回；P3-08 引入 timer 后有界取消。
}
