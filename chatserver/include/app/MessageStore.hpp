#pragma once

#include "app/ReliableMessaging.hpp"

#include <memory>
#include <vector>

// 内部 seam（计划 §3）：InMemory/MySQL 两个 adapter；本体只经此访问持久状态，
// 领域状态机逻辑（幂等判定、claim、lease、顺序）不放 adapter。

// P3-12（metrics 最小扩展）：expire 明细行——逐行 (messageId, recipient,
// fromState) 供 ReliableMessageMetrics 记录 Pending/InFlight → Expired 转移。
struct ExpiredDeliveryRecord {
    uint64_t messageId = 0;
    uint64_t recipient = 0;
    DeliveryState fromState = DeliveryState::Pending;
};

// P4-04 消费侧 dead-letter 行（卡 D3：键 = Kafka record 定位 topic/partition/
// offset + 原始 bytes；发布侧 poison 按 outbox 事件 id 键控，语义不同不复用）。
struct DeadLetterRecord {
    std::string topic;
    int32_t partitionId = 0;
    int64_t kafkaOffset = 0;
    uint64_t messageId = 0;        // 信封 message_id（不可解析时 0）
    uint64_t conversationId = 0;
    uint64_t sequence = 0;
    std::string eventType;
    std::string reason;   // poison_payload|unknown_event_type|sequence_regression|
                          // sequence_conflict|message_missing
    std::string rawValue;  // 原始 record bytes（信封原文）；message_missing 场景为
                           // handler 侧按 P4-03 冻结信封字段重建的文本（handler 无
                           // 原始 bytes，见 WakeupProgressHandler）
};

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

    // P3-12（metrics 最小扩展，docs/tasks/P3-12.md 登记）：expire 转移的按行
    // 明细。与 expireDeliveries 等价地把到期 Pending/InFlight → Expired，并把每行
    // (messageId, recipient, fromState) 追加到 out（有界 limit 行），供
    // ReliableMessageMetrics 逐行记录状态转移。默认实现回退到 expireDeliveries
    // （out 不填）：既有测试替身只 override expireDeliveries 时行为不变；
    // InMemory/MySQL override 同时报告明细。
    virtual uint32_t expireDeliveriesDetailed(int64_t nowMs, uint64_t limit,
                                              std::vector<ExpiredDeliveryRecord>* out)
    {
        (void)out;
        return expireDeliveries(nowMs, limit);
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

    // ---- P3-09 outbox port（InMemory/MySQL 双 adapter；默认空实现沿用
    // deliveriesDueForRetry 模式，既有测试替身如 ReliableMessagingContractTest::
    // ThrowOnceOnUpdateStore 不实现时无 outbox 消费）----

    // 原子 claim 未处理（processed_at NULL）且 available_at<=nowMs 且（lease 为空
    // 或已到期）的事件，最多 limit 行；写 lease_owner+lease_until、attempt_count+1。
    // MySQL 用单条 UPDATE … WHERE … LIMIT（行锁串行化两 relay 竞争）；InMemory
    // 锁内完成。返回已 claim 事件（含递增后的 attempt_count）。
    virtual std::vector<OutboxEvent> claimOutboxEvents(int64_t nowMs,
                                                       const std::string& leaseOwner,
                                                       int64_t leaseUntilMs, uint64_t limit)
    {
        (void)nowMs;
        (void)leaseOwner;
        (void)leaseUntilMs;
        (void)limit;
        return std::vector<OutboxEvent>();
    }

    // 处理成功后才标 processed_at（nowMs）；同时释放 lease。失败/崩溃保持未
    // processed，lease 到期可重领。
    virtual void markOutboxProcessed(uint64_t eventId, int64_t nowMs)
    {
        (void)eventId;
        (void)nowMs;
    }

    // 处理失败/不可解析标记：事件保持未 processed（poison 谓词可查询、绝不
    // 静默丢弃），lease 保留由到期驱动重试（P3-12 再接管精确指标）。
    virtual void markOutboxPoisoned(uint64_t eventId, int64_t nowMs)
    {
        (void)eventId;
        (void)nowMs;
    }

    // 单事件查询（测试断言 / 管理查询）。
    virtual std::shared_ptr<const OutboxEvent> findOutboxEvent(uint64_t eventId)
    {
        (void)eventId;
        return std::shared_ptr<const OutboxEvent>();
    }

    // poison/未处理谓词（SQL：processed_at IS NULL），最多 limit 行；不静默丢弃。
    virtual std::vector<OutboxEvent> poisonedOutboxEvents(uint64_t limit)
    {
        (void)limit;
        return std::vector<OutboxEvent>();
    }

    // P3-12（metrics 最小扩展，docs/tasks/P3-12.md 登记）：outbox lag gauge 的
    // 有界公开查询——未 processed（processed_at IS NULL）事件总数。默认 0（同
    // outbox port 默认空实现模式）；InMemory/MySQL override。
    virtual uint64_t countUnprocessedOutboxEvents()
    {
        return 0;
    }

    // ---- P4-04 消费侧 dead-letter port（InMemory/MySQL 双 adapter，契约双跑；
    //      默认 no-op 沿 P3-09 模式，既有测试替身零伴改；行落地 MySQL
    //      KafkaDeadLetter 表（0003 migration），UNIQUE(topic,partition_id,
    //      kafka_offset) 幂等）----

    // 幂等落库：UNIQUE(topic,partition_id,kafka_offset) 冲突 = 已存在，成功返回。
    virtual void recordDeadLetter(const DeadLetterRecord& r) { (void)r; }
    // 可查询谓词（消费侧 poison 绝不只日志丢弃的证据面），最多 limit 行。
    virtual std::vector<DeadLetterRecord> deadLetters(uint64_t limit)
    {
        (void)limit;
        return std::vector<DeadLetterRecord>();
    }
};
