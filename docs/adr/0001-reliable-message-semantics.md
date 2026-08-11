---
status: proposed
---

# Adopt durable acceptance and client-acknowledged at-least-once delivery

P3 will report `MessageAcceptance` only after one MySQL transaction commits the Message, its ConversationSequence, every recipient Delivery, and the OutboxEvent. Retries are idempotent by `(sender User, ClientMessageId)` and return the original MessageId and sequence. Delivery is at least once: a TCP write is only a DeliveryAttempt, the receiving client deduplicates by MessageId and sends a DeliveryAcknowledgement, and the server never claims end-to-end exactly-once delivery. Ordering is local to one Conversation; different Conversations may progress concurrently. For group Messages, membership is snapshotted when the Message is accepted, and the sender must be a member.

## Considered options

- Keep the current online-forward/offline-delete behavior: rejected because an online send can be lost before durable storage and login removes offline data before client confirmation.
- Treat a successful socket write as delivery: rejected because it proves neither peer receipt nor application processing.
- Promise exactly-once delivery: rejected because ACK loss and process/network failure necessarily produce either retries or loss; idempotent acceptance plus client deduplication is the honest contract.
- Guarantee global order: rejected because it couples unrelated Conversations and creates a system-wide serialization point.

## Consequences

- Sender identity comes from the authenticated Session, never from a client-supplied `id` field.
- P3 uses bounded fan-out-on-write for group Messages: the acceptance transaction snapshots recipients by creating their Delivery rows, and rejects groups above a configured recipient cap. Asynchronous large-group fan-out is a later, separately specified design.
- Reliable clients must provide a stable ClientMessageId and acknowledge received MessageIds. Bundled clients and test tools will be upgraded in P3; legacy clients remain a temporary compatibility mode and are outside the M3 reliability guarantee.
- The existing `OfflineMessage` table cannot be deleted until additive migration, backfill, cutover verification, and the rollback window have all completed.
