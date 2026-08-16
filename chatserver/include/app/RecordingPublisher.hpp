#pragma once

#include "app/OutboxPublisher.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// 测试 adapter（进 chatserver_core）：记录每个 publish 请求与逐事件结果；默认全
// 成功，可脚本化注入失败（按 eventId 或 topic），失败不阻断同批（逐事件）。
class RecordingPublisher : public OutboxPublisher {
public:
    RecordingPublisher();

    std::vector<OutboxPublishOutcome> publish(
        const std::vector<OutboxPublishRequest>& batch, int64_t deadlineMs) override;

    void failEvent(uint64_t eventId, PublishFailure failure);   // 注入失败
    void failTopic(const std::string& topic, PublishFailure failure);
    void clearFailures();

    size_t publishCalls() const;                                        // publish() 调用次数
    const std::vector<std::vector<OutboxPublishRequest> >& batches() const;  // 每次入参
    const std::vector<OutboxPublishRequest>& requests() const;          // 全部事件扁平化（保序）
    const std::vector<OutboxPublishOutcome>& lastOutcomes() const;      // 最近一次逐事件结果
    int64_t lastDeadlineMs() const;                                     // 最近一次 publish 的 deadline

private:
    PublishFailure failureFor(const OutboxPublishRequest& req) const;

    std::vector<std::vector<OutboxPublishRequest> > batches_;
    std::vector<OutboxPublishRequest> requests_;
    std::vector<OutboxPublishOutcome> lastOutcomes_;
    int64_t lastDeadlineMs_ = 0;
    std::map<uint64_t, PublishFailure> failByEvent_;
    std::map<std::string, PublishFailure> failByTopic_;
};
