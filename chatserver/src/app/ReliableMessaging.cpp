#include "app/ReliableMessaging.hpp"

#include "app/DeliveryCoordinator.hpp"
#include "app/DeliverySink.hpp"
#include "app/MessageStore.hpp"

#include <memory>
#include <vector>

namespace {

std::vector<UserId> recipientsFor(const SendMessageCommand& cmd)
{
    if (cmd.kind == SendMessageCommand::Kind::Direct) {
        return std::vector<UserId>(1, cmd.directRecipient);
    }
    return cmd.members;
}

// Durable acceptance is already complete before this callback is entered.
// Delivery state persistence is therefore best-effort here: a later retry of
// the same idempotency key re-runs the claim for any still-Pending rows.
void notifyAcceptedBestEffort(DeliveryCoordinator& coordinator,
                              const std::vector<UserId>& recipients)
{
    try {
        coordinator.onAccepted(recipients);
    } catch (...) {
        // Do not turn a committed Message/Delivery set into DependencyBusy.
    }
}

} // namespace

ReliableMessaging::ReliableMessaging(MessageStore& store, DeliverySink& sink, Clock& clock,
                                     uint64_t leaseMs)
    : store_(store), coordinator_(new DeliveryCoordinator(store, sink, clock, leaseMs))
{
}

ReliableMessaging::~ReliableMessaging() = default;

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
            // A prior post-commit delivery update may have failed after the
            // durable accept.  Re-run the online claim on a same-key retry;
            // claimFor fences already InFlight/Acknowledged rows.
            notifyAcceptedBestEffort(*coordinator_, recipientsFor(existing->command));
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

    const std::vector<UserId> recipients = recipientsFor(cmd);
    for (size_t i = 0; i < recipients.size(); ++i) {
        Delivery delivery;
        delivery.messageId = accepted.id;
        delivery.conversationId = accepted.conversationId;
        delivery.recipient = recipients[i];
        store_.insertDelivery(delivery);
    }

    // P3-07：对在线接收者立即 claim（在线投递；离线保持 Pending 待登录 claim）。
    notifyAcceptedBestEffort(*coordinator_, recipients);

    outcome.ok = true;
    outcome.messageId = accepted.id;
    outcome.conversationId = accepted.conversationId;
    outcome.sequence = accepted.sequence;
    return outcome;
}

AckOutcome ReliableMessaging::acknowledge(const SessionIdentity& acker, MessageId messageId)
{
    return coordinator_->acknowledge(acker, messageId);
}

void ReliableMessaging::sessionAvailable(const SessionIdentity& session)
{
    coordinator_->sessionAvailable(session);
}

void ReliableMessaging::sessionClosed(const SessionIdentity& session)
{
    coordinator_->sessionClosed(session);
}

void ReliableMessaging::resume(const SessionIdentity& session)
{
    coordinator_->resume(session);
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
