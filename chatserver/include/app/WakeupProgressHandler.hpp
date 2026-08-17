#pragma once

#include "app/LocalWakeupPublisher.hpp"
#include "app/MessageStore.hpp"
#include "app/OutboxEventConsumer.hpp"
#include "app/ReliableMessaging.hpp"

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// P4-04 in-process 处理 adapter（docs/tasks/P4-04.md D2）：DeliveryProgressHandler
// 的默认实现——把消费到的 MessageAccepted 事件转回 P3-09 的本地幂等 wakeup。
// 只推进不重建：绝不调 insertMessage/insertDelivery/accept（状态机只前进由
// DeliveryCoordinator::claimFor fencing 保证：有效 lease 的 InFlight 不重领、
// Acknowledged 短路）。
//   handle(record)：
//     store.findMessage(messageId) → 缺行 dead-letter（reason=message_missing，经
//     store.recordDeadLetter 幂等落库）→ DeadLettered；
//     outboxRecipientsFor(msg->command)（LocalWakeupPublisher 公开 inline 复用）
//     → rm.wakeupAccepted(recipients)（存储异常向上传播——consumer 批中止不提交，
//     下轮重放，D2 瞬时异常面）。
//   返回：wakeup 使任一 Delivery 行 state/attemptCount 前进（或新增行）→
//   Advanced；无变化（同 message_id 重放被 claimFor fencing）→ DuplicateNoOp。
class WakeupProgressHandler : public DeliveryProgressHandler {
public:
    WakeupProgressHandler(MessageStore& store, ReliableMessaging& rm) : store_(store), rm_(rm) {}

    ConsumeDisposition handle(const ConsumedOutboxRecord& record) override
    {
        const MessageId mid(record.messageId);
        const std::shared_ptr<const Message> msg = store_.findMessage(mid);
        if (!msg) {
            // 缺 Message 行：不可恢复不一致（accept 事务先于 publish 提交），
            // 立即 dead-letter（不无限重试，防 head-of-line 阻塞）。
            DeadLetterRecord dlr;
            dlr.topic = record.topic;
            dlr.partitionId = record.partition;
            dlr.kafkaOffset = record.offset;
            dlr.messageId = record.messageId;
            dlr.conversationId = record.conversationId;
            dlr.sequence = record.sequence;
            dlr.eventType = record.eventType;
            dlr.reason = "message_missing";
            dlr.rawValue = rebuildEnvelope(record);
            store_.recordDeadLetter(dlr);  // 异常传播（dead-letter 未落库 = 未终态）
            return ConsumeDisposition::DeadLettered;
        }
        // 前后快照对比：wakeup 是否实际推进了 Delivery（重复重放被 fencing → 无变化）。
        const std::vector<Delivery> before = store_.deliveriesByMessage(mid);
        rm_.wakeupAccepted(outboxRecipientsFor(msg->command));
        const std::vector<Delivery> after = store_.deliveriesByMessage(mid);
        return progressed(before, after) ? ConsumeDisposition::Advanced
                                         : ConsumeDisposition::DuplicateNoOp;
    }

private:
    // 任一行 state/attemptCount 变化或行数变化 = 推进；全不变 = 重复 no-op。
    static bool progressed(const std::vector<Delivery>& before, const std::vector<Delivery>& after)
    {
        if (before.size() != after.size()) {
            return true;
        }
        std::map<uint64_t, std::pair<int, uint32_t> > snapshot;  // recipient → (state, attempts)
        for (size_t i = 0; i < before.size(); ++i) {
            snapshot[before[i].recipient.value] = std::make_pair(
                static_cast<int>(before[i].state), before[i].attemptCount);
        }
        for (size_t i = 0; i < after.size(); ++i) {
            std::map<uint64_t, std::pair<int, uint32_t> >::const_iterator it =
                snapshot.find(after[i].recipient.value);
            if (it == snapshot.end()) {
                return true;  // 新行
            }
            if (it->second.first != static_cast<int>(after[i].state)
                || it->second.second != after[i].attemptCount) {
                return true;
            }
        }
        return false;
    }

    // 信封重建（dead-letter rawValue 证据面；handler 侧无原始 bytes，按 P4-03
    // 冻结信封字段重建——payload 为合法 JSON 对象的 dump 字符串）。
    static std::string rebuildEnvelope(const ConsumedOutboxRecord& record)
    {
        return "{\"message_id\":" + std::to_string(record.messageId)
            + ",\"conversation_id\":" + std::to_string(record.conversationId)
            + ",\"sequence\":" + std::to_string(record.sequence) + ",\"event_type\":\""
            + record.eventType + "\",\"payload\":" + record.payload + "}";
    }

    MessageStore& store_;
    ReliableMessaging& rm_;
};
