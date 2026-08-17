#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// P4-04 幂等 Delivery consumer 公开接口（docs/tasks/P4-04.md §Interface 定案，
// tests/unit/OutboxConsumerContractTest.cpp 文件头契约块逐字一致——签名/参数序/
// 字段名冻结，GREEN 实现不得偏离）。
//
// 双 port（卡 D1）：
//   - 驱动面 OutboxEventConsumer：同步 poll(deadlineMs)，单调用方线程完成
//     fetch → 逐事件 handle → dead-letter 落库 → 全部终态后才 commit offset；
//     poll 本身不抛（store 异常/broker 故障 → 本批不提交、结果字段失败）。
//   - 处理面 DeliveryProgressHandler：只推进既有 Delivery（幂等 wakeup），绝不
//     重建 Message/Delivery（D2"只推进不重建"）。
// 无内部后台线程（生产接线由 P4-05 决定）。

// 一条从 broker 拉到的 MessageAccepted 事件（信封字段 P4-03 冻结，不改一字段）。
struct ConsumedOutboxRecord {
    std::string topic;
    int32_t partition = 0;
    int64_t offset = 0;
    uint64_t messageId = 0;
    uint64_t conversationId = 0;
    uint64_t sequence = 0;
    std::string eventType;   // 只接受 "MessageAccepted"
    std::string payload;     // 命令快照 JSON（P3-04 冻结编码）
};

enum class ConsumeDisposition {
    Advanced,       // 推进了既有 Delivery（幂等 wakeup 生效，Pending→InFlight 等）
    DuplicateNoOp,  // 同 message_id 重复投递：无操作成功（行/attemptCount 不变）
    DeadLettered,   // poison/乱序/缺行：已落 KafkaDeadLetter（可查询），未推进状态
};

// 处理面 port：只推进既有 Delivery，绝不重建（D2）。
class DeliveryProgressHandler {
public:
    virtual ~DeliveryProgressHandler() = default;
    virtual ConsumeDisposition handle(const ConsumedOutboxRecord& record) = 0;
};

// 驱动面 port：同步 poll（fetch → 逐事件 handle → poison/乱序 dead-letter 落库
// → 全部终态后才 commit offset）。store 异常/broker 故障 → 本批不提交、返回
// 失败（下轮重放，at-least-once）；poll 本身不抛。
struct OutboxConsumeResult {
    std::vector<ConsumedOutboxRecord> records;     // 本批拉到的事件
    std::vector<ConsumeDisposition> dispositions;  // 与 records 一一对应
    bool processed = false;        // 本批全部终态（含 dead-letter 落库）
    bool offsetCommitted = false;  // 仅在 processed 后才可能 true
    std::map<int32_t, int64_t> committedThrough;   // partition → 已提交 offset+1
    bool brokerOk = true;          // false = 本轮 fetch 阶段 broker 故障（无副作用）
};

class OutboxEventConsumer {
public:
    virtual ~OutboxEventConsumer() = default;
    virtual OutboxConsumeResult poll(int64_t deadlineMs) = 0;  // 不抛
};
