#include "app/MetricsSnapshot.hpp"

namespace MetricsSnapshot {

Snapshot snapshot(const ReliableMessageMetrics::Snapshot& reliable,
                  const PoolStats& pool,
                  const ExecutorStats& executor,
                  const Telemetry::Snapshot& telemetry,
                  const GapStats& gap)
{
    Snapshot s;

    s.accepts = reliable.accepts;
    s.duplicates = reliable.duplicates;
    s.conflicts = reliable.conflicts;
    s.rejectedTooManyRecipients = reliable.rejectedTooManyRecipients;
    s.createdDeliveries = reliable.createdDeliveries;
    s.pending = reliable.pending;
    s.inflight = reliable.inflight;
    s.acked = reliable.acked;
    s.expired = reliable.expired;
    s.attempts = reliable.attempts;
    s.retries = reliable.retries;
    s.legacyModeCount = reliable.legacyModeCount;
    s.outboxLag = reliable.outboxLag;
    s.outboxPoison = reliable.outboxPoison;
    s.ackLatencySamples = reliable.ackLatencySamples;
    s.ackLatencyP50Ms = reliable.ackLatencyP50Ms;
    s.ackLatencyP95Ms = reliable.ackLatencyP95Ms;
    s.ackLatencyP99Ms = reliable.ackLatencyP99Ms;
    s.oldestPendingAgeMs = reliable.oldestPendingAgeMs;
    s.consumerSeenConversations = reliable.consumerSeenConversations;

    s.poolTotal = pool.total;
    s.poolIdle = pool.idle;
    s.poolActive = pool.active;
    s.executorQueueDepth = executor.queueDepth;
    s.executorDropped = executor.dropped;

    // H-1：缺口字段由生产接线（GapStats 第四源）填充；缺省保持默认。
    s.loopLagMs = gap.loopLagMs;
    s.acceptCount = gap.acceptCount;
    s.acceptRateReject = gap.acceptRateReject;
    s.acceptMaxReject = gap.acceptMaxReject;
    s.acceptEmfileRecover = gap.acceptEmfileRecover;
    s.outstandingBytes = gap.outstandingBytes;
    s.fencingConflicts = gap.fencingConflicts;
    s.consumerLag = gap.consumerLag;
    s.rebalanceCount = gap.rebalanceCount;

    s.telemetry = telemetry;

    return s;
}

} // namespace MetricsSnapshot
