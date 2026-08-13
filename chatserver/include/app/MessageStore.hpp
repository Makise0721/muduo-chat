#pragma once

#include "app/ReliableMessaging.hpp"

#include <memory>
#include <vector>

// 内部 seam（计划 §3）：InMemory/MySQL 两个 adapter；本体只经此访问持久状态，
// 领域状态机逻辑（幂等判定、claim、lease、顺序）不放 adapter。
class MessageStore {
public:
    virtual ~MessageStore() = default;

    // (sender, clientMessageId) 已接受 → 原 Message；否则 nullptr。
    virtual std::shared_ptr<const Message> findAccepted(const ClientMessageId& clientMessageId,
                                                        UserId sender) = 0;

    // 命令参与者对应的 Conversation，不存在则创建（direct 按无序用户对，group 按 GroupId）。
    virtual ConversationId getOrCreateConversation(const SessionIdentity& sender,
                                                   const SendMessageCommand& cmd) = 0;

    // 写入 Message：adapter 分配 MessageId，并保证 Conversation 内 sequence 严格递增。
    virtual Message insertMessage(const Message& draft) = 0;

    virtual void insertDelivery(const Delivery& delivery) = 0;
    virtual void updateDelivery(const Delivery& delivery) = 0;

    virtual std::vector<Delivery> deliveriesByRecipient(UserId recipient) = 0;
    virtual std::vector<Delivery> deliveriesByMessage(MessageId messageId) = 0;
    virtual std::shared_ptr<const Message> findMessage(MessageId messageId) = 0;

    // ---- P3-08 到期扫描/过期/保留清理（InMemory/MySQL 双 adapter，LIMIT 有界）----

    // 返回 next_attempt_at <= nowMs 的 InFlight Delivery（最多 limit 行），供
    // ACK timeout 重投扫描使用。按存储侧一次有界查询返回，避免按活动会话逐人
    // 查询的 O(activeSessions) 扫描（无界）。默认空实现：既有测试替身不实现时
    // 无重投扫描。
    virtual std::vector<Delivery> deliveriesDueForRetry(int64_t nowMs, uint64_t limit)
    {
        (void)nowMs;
        (void)limit;
        return std::vector<Delivery>();
    }

    // 本 store 持久化时间粒度（毫秒）。runTick 把 runRetryScan 返回的
    // nextAttemptAtMs 向上对齐到该粒度再交 computeNextWakeMs（F1）：MySQL 以
    // DATETIME(0) 秒精度持久化 next_attempt_at（M2 ceil 写），due 判定
    // floor(now_sec)>=ceil 使行在 ceil 秒边界才到期；scheduler 若按 ms 级
    // nextAttemptAtMs 唤醒会提前 1..999ms 空转（行未到期 → runRetryScan 空 →
    // 回退 ackTimeoutMs 轮询，有效 ack_timeout 翻倍）。默认 1（InMemory 原样）；
    // MySQL 返回 1000。
    virtual uint32_t timeGranularityMs() { return 1; }

    // 未确认（Pending/InFlight）且 expires_at <= now 的 Delivery → Expired
    // （spec §3：Pending/InFlight --> Expired: retention deadline）。不删除、
    // 可查询。返回受影响行数；最多处理 limit 行。
    // 默认 no-op（返回 0）：既有测试替身/未来 adapter 未实现时不强制实现，
    // 生产两 adapter（InMemory/MySQL）均 override。
    virtual uint32_t expireDeliveries(int64_t nowMs, uint64_t limit)
    {
        (void)nowMs;
        (void)limit;
        return 0;
    }

    // acked/expired 独立 retention 清理（spec §3 Acknowledged/Expired --> [*]）：
    // Acknowledged 且 acknowledged_at <= ackedBeforeMs，或 Expired 且
    // expires_at <= expiredBeforeMs 的行删除（audited cleanup，绝不静默丢行）。
    // 返回删除行数；每类最多 limit 行（acked/expired 各计各的上限，总处理 <= 2*limit，
    // 有界幂等）。默认 no-op（返回 0），同 expireDeliveries。
    virtual uint32_t cleanupDeliveries(int64_t ackedBeforeMs, int64_t expiredBeforeMs,
                                       uint64_t limit)
    {
        (void)ackedBeforeMs;
        (void)expiredBeforeMs;
        (void)limit;
        return 0;
    }
};
