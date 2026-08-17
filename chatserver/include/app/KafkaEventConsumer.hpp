#pragma once

#include "app/MessageStore.hpp"
#include "app/OutboxEventConsumer.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

// P4-04 Kafka 消费 adapter（docs/tasks/P4-04.md D1/D4/冻结参数）：内置最小
// Kafka wire 客户端（共享 KafkaWire seam）——Metadata v1（manual assign topic
// 全部分区）+ ListOffsets v1（earliest/latest）+ Fetch v4（magic-2 record batch
// 解析）+ OffsetFetch/OffsetCommit v2（simple-consumer 偏移持久化：generation=-1、
// member_id 空串，2026-08-16 实测校准 NONE，见卡待定①）。不做 consumer group
// 协议（FindCoordinator/JoinGroup/SyncGroup/Heartbeat 非目标）；实现细节在
// KafkaEventConsumer.cpp。
class KafkaEventConsumer : public OutboxEventConsumer {
public:
    // 构造注入（卡 D1：host/port/topic/groupId/fetchBatchLimit/deadlineMs 同
    // KafkaPublisher 形态）+ dead-letter 落库 store + 处理面 handler。
    KafkaEventConsumer(const std::string& host, int port, const std::string& topic,
                       const std::string& groupId, MessageStore& deadLetterStore,
                       DeliveryProgressHandler& handler, uint32_t fetchBatchLimit,
                       int64_t deadlineMs);
    ~KafkaEventConsumer() override;

    // poll 不抛（异常面全部收敛为结果字段：brokerOk/processed/offsetCommitted）。
    OutboxConsumeResult poll(int64_t deadlineMs) override;

    // preCommitHook（适配器具体 seam，不进 port；P4-01 injectFailure 先例）：
    // OffsetCommit 发出前回调；回调抛出 = 模拟"处理后 commit 前 kill"，本批
    // 不提交（processed 仍可为 true：批已全部终态）。
    void setPreCommitHook(std::function<void()> hook);

    // pimpl（wire 客户端/游标/lastSeen 内部态；前向声明 public——KafkaEventConsumer.cpp
    // 内部 seam 函数按 Impl& 传参）。
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};
