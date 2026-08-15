---
status: accepted
---

# Adopt cluster ownership and failure contract for multi-gateway routing

P4 routes messages across multiple Gateway processes while keeping MySQL the only Message truth source. Every connection belongs to exactly one Gateway, identified by a stable GatewayId. A Session (one logged-in User) is unique across the whole cluster: a User may have at most one active Session, regardless of which Gateway holds the connection. The Presence directory stores only routing plus a monotonic SessionEpoch (user -> {gateway_id, connection_id, session_epoch} with a TTL); it never stores message truth. Delivery of an accepted Message is routed to the Gateway that owns the recipient's current Session, and the routing is valid only while the PresenceLease epoch matches the Session epoch.

## Considered options

- Keep single-node semantics and add a second process later without a contract: rejected because SessionRegistry is process-local, the local outbox relay only wakes this instance's coordinator, and lease ownership encodes a process boot id with no Gateway identity — cross-node routing would be undefined behavior.
- Let Redis/Presence or the broker hold message truth: rejected because MessageAcceptance/DeliveryAcknowledgement state must stay in the durable ledger; a TTL store or a replayable log is the wrong authority for the message contract.
- Promise exactly-once delivery across nodes: rejected for the same reason as ADR-0001 — ACK loss, Gateway kill, and partition necessarily produce retries or loss; the contract is at-least-once with client-side MessageId deduplication, and P4 never claims cluster exactly-once.
- Route by ConnectionId globally: rejected because ConnectionId is only meaningful inside one Gateway; cross-node addressing must use GatewayId + SessionEpoch.
- A global in-memory session registry (current P3 design) as the cluster answer: rejected because it is a single point of ownership that cannot survive Gateway kill; Presence must be claimable/renewable/releasable per Gateway with epoch fencing.

## Consequences

- MySQL stays the Message truth source; durable MessageAcceptance keeps working during Presence/broker failures.
- A new login always creates a fresh monotonic SessionEpoch; any renew/release carrying an older epoch is fenced (compare-and-delete release).
- A Gateway kill leaves the killed instance's InFlight deliveries recoverable only through lease expiry and re-claim by a surviving Gateway; no sessionClosed cleanup exists for a hard kill.
- A Redis outage pauses new logins and cross-node direct delivery but not durable accept; a broker outage never alters Message/Delivery state because consumers deduplicate against the database.
- P4-01..P4-06 implement this contract through the PresenceDirectory interface, Redis fencing, the OutboxPublisher port, an idempotent consumer, Gateway-targeted delivery with epoch checks, and three-node chaos. Each dependency failure has exactly one expected accept/login/delivery degradation, fixed in docs/specs/cluster-failure-contract.md.
