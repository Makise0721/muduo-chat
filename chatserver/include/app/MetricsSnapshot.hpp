#pragma once

// P5-00 阶段 B 统一快照同源生成（docs/tasks/P5-00.md 设计决定 D8/D11、H-1）：
// 四个只读数据源（ReliableMessageMetrics::Snapshot 副本 + pool/executor 只读
// 桥接 + Telemetry::Snapshot 副本 + GapStats 缺口字段桥接）→ 单一全标量
// Snapshot（standard layout，沿 ReliableMessageMetrics::Snapshot 惯例）。
// SIGUSR1 METRICS 行与 /metrics 同一函数产出（同源，杜绝两处字段漂移）。
//
// 冻结用法见 tests/unit/MetricsSnapshotTest.cpp（GREEN 后即契约）。
// 本头包含 app/ReliableMessageMetrics.hpp 与 app/Telemetry.hpp（均为领域 TU，
// 无 mymuduo class Clock 冲突）；mymuduo TU 不得包含本头。

#include "app/ReliableMessageMetrics.hpp"
#include "app/Telemetry.hpp"

#include <cstdint>

namespace MetricsSnapshot {

// pool 只读桥接（main.cpp ConnectionPool::getInstance().metrics() 同款结构）。
struct PoolStats {
    uint64_t total = 0;
    uint64_t idle = 0;
    uint64_t active = 0;
};

// executor 只读桥接（ChatService::executorQueueDepth/Dropped 同款入口）。
struct ExecutorStats {
    uint64_t queueDepth = 0;
    uint64_t dropped = 0;
};

// P5-00 H-1：缺口字段只读桥接（main.cpp 经领域 wiring getter 聚合：EventLoop
// loopLagProbeMs、ChatServer/TcpServer acceptReasonCounts + outstanding 聚合、
// ProtocolCodec wiring fencingConflicts/consumerLag/rebalanceCount）。统一快照
// 全标量覆盖——三源工厂之外第四源。
struct GapStats {
    int64_t loopLagMs = -1;          // -1 = 未采样哨兵（与 Snapshot 默认一致）
    uint64_t acceptCount = 0;
    uint64_t acceptRateReject = 0;
    uint64_t acceptMaxReject = 0;
    uint64_t acceptEmfileRecover = 0;
    uint64_t outstandingBytes = 0;
    uint64_t fencingConflicts = 0;
    uint64_t consumerLag = 0;
    uint64_t rebalanceCount = 0;
};

// 统一全标量快照：reliable_* 21 字段 + pool/executor + 缺口字段 + telemetry 系列。
struct Snapshot {
    // reliable 21 字段（ReliableMessageMetrics::Snapshot 逐字段映射）。
    uint64_t accepts = 0;
    uint64_t duplicates = 0;
    uint64_t conflicts = 0;
    uint64_t rejectedTooManyRecipients = 0;
    uint64_t createdDeliveries = 0;
    uint64_t pending = 0;
    uint64_t inflight = 0;
    uint64_t acked = 0;
    uint64_t expired = 0;
    uint64_t attempts = 0;
    uint64_t retries = 0;
    uint64_t legacyModeCount = 0;
    uint64_t outboxLag = 0;
    uint64_t outboxPoison = 0;
    uint64_t ackLatencySamples = 0;
    uint64_t ackLatencyP50Ms = 0;
    uint64_t ackLatencyP95Ms = 0;
    uint64_t ackLatencyP99Ms = 0;
    int64_t oldestPendingAgeMs = -1;  // -1 = 无 Pending
    uint64_t consumerSeenConversations = 0;

    // pool/executor 桥接。
    uint64_t poolTotal = 0;
    uint64_t poolIdle = 0;
    uint64_t poolActive = 0;
    uint64_t executorQueueDepth = 0;
    uint64_t executorDropped = 0;

    // 缺口字段（由生产接线填充；三源工厂下保持默认：loopLagMs=-1 哨兵，其余 0）。
    int64_t loopLagMs = -1;
    uint64_t acceptCount = 0;
    uint64_t acceptRateReject = 0;
    uint64_t acceptMaxReject = 0;
    uint64_t acceptEmfileRecover = 0;
    uint64_t outstandingBytes = 0;
    uint64_t fencingConflicts = 0;
    uint64_t consumerLag = 0;
    uint64_t rebalanceCount = 0;

    // telemetry 系列（阶段 A Telemetry::Snapshot 原样嵌入）。
    Telemetry::Snapshot telemetry;
};

// 同源生成函数：reliable + pool/executor + telemetry + gap 四只读数据源 →
// 统一快照。gap 缺省 = 默认（缺口字段由生产接线填充；三源工厂下保持默认：
// loopLagMs=-1 哨兵，其余 0）。
Snapshot snapshot(const ReliableMessageMetrics::Snapshot& reliable,
                  const PoolStats& pool,
                  const ExecutorStats& executor,
                  const Telemetry::Snapshot& telemetry,
                  const GapStats& gap = GapStats());

} // namespace MetricsSnapshot
