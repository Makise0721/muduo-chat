#pragma once

#include "app/OutboxPublisher.hpp"

#include <cstdint>
#include <string>
#include <vector>

// 真实 broker adapter（进 chatserver_core）：内置最小 Kafka wire 客户端（Metadata +
// Produce API，仅 PLAINTEXT 单 broker，key=ConversationId 字符串，value=JSON 信封）。
// 不新增第三方客户端依赖（沿 P4-02 RedisConn 先例）。实现细节（wire/故障分类）在
// KafkaPublisher.cpp。
class KafkaPublisher : public OutboxPublisher {
public:
    // topicPrefix：安全前缀守卫——request.topic 不以该前缀开头的请求返回
    // PublishFailure::Other（fail-safe，测试隔离由 muduo-test- 保证）；非空时强制。
    // deadlineMs 为默认操作期限（每次 publish 的显式 deadlineMs 覆盖之）。
    KafkaPublisher(const std::string& host, int port, const std::string& topicPrefix,
                   int64_t deadlineMs);

    std::vector<OutboxPublishOutcome> publish(
        const std::vector<OutboxPublishRequest>& batch, int64_t deadlineMs) override;

private:
    std::string host_;
    int port_ = 0;
    std::string topicPrefix_;
    int64_t deadlineMs_;
};
