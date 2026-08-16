#pragma once

#include "app/DomainTypes.hpp"

#include <cstdint>
#include <string>
#include <vector>

// P4-03 OutboxPublisher port（docs/tasks/P4-03.md §Interface/§冻结参数）：relay 的
// MessageAccepted 事件发布出口。port 不写库（publish 不改变 Message/Delivery），
// 同一事件重复 publish 安全（P4-04 消费者幂等）；relay 只在逐事件结果 ok 时标
// processed，失败保持未 processed、lease 到期重领重试（P3-09 语义原样）。

// 单事件 publish 请求：relay 已 claim 的事件 + Kafka 分区键（ConversationId 字符串化，
// 冻结参数）+ 目标 topic（测试注入 muduo-test- 前缀隔离）+ Conversation 内序号
// （信封 sequence 字段，供 P4-04 断言 partition 内顺序不倒退；请求结构冻结于 P4-03
// 测试，sequence 由 relay 从 Message.conversationId/sequence 补全）。
struct OutboxPublishRequest {
    OutboxEvent event;              // relay 已 claim 的事件（含递增后 attemptCount）
    ConversationId conversationId;  // Kafka 分区键（字符串化，冻结参数）
    uint64_t sequence = 0;          // Conversation 内序号（信封 sequence 字段）
    std::string topic;              // 目标 topic（测试注入 muduo-test- 前缀隔离）
};

// 错误类别（可区分，测试逐类别断言）；None 仅 ok=true 时使用。
enum class PublishFailure {
    None,
    Timeout,
    BrokerUnavailable,
    PayloadTooLarge,
    Other,
};

struct OutboxPublishOutcome {
    bool ok = false;
    PublishFailure failure = PublishFailure::Other;
    std::string error;  // 失败原因摘要（日志/指标，不含敏感 payload）
};

class OutboxPublisher {
public:
    virtual ~OutboxPublisher() = default;

    // 批量 publish（batch 有界，调用方不超上限）；返回逐事件结果（顺序与入参一致）。
    // deadlineMs 为整体操作软期限；超时/断连/broker 错误 → 对应事件失败，不抛、不写库。
    virtual std::vector<OutboxPublishOutcome> publish(
        const std::vector<OutboxPublishRequest>& batch, int64_t deadlineMs) = 0;
};
