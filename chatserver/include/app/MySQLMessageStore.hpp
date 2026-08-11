#pragma once

#include "app/MessageStore.hpp"
#include "db/ConnectionPool.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>

// P3-04 MySQL MessageStore adapter（计划 §5 P3-04）：一个 accept 事务完成
// Conversation 查找/创建（direct low/high 归一化由 adapter 保证，P3-03 登记）、
// 成员快照（命令携带，只消费 SendMessageCommand 的接收者列表，不查群）、
// sequence 分配、ChatMessage、逐接收者 MessageDelivery、OutboxEvent。
// 成员快照顺序经 OutboxEvent.payload JSON 保序（MessageDelivery 无序号列，
// 0002 已冻结）；content 走 ChatMessage.content 列原样 round-trip。
//
// 错误通道：P3-02 MessageStore port 无错误返回值 → 并发竞争唯一键的
// IdempotencyConflict、DependencyBusy、TooManyRecipients 经 MessageStoreError
// 异常传递（串行重复路径仍由 findAccepted + ReliableMessaging 正常返回；
// P3-06 应用层把异常映射为 AcceptOutcome）。P3-04 冻结 fan-out cap=100
// （docs/tasks/P3-04.md RED 节，构造参数）。

enum class StoreErrorKind {
    DependencyBusy,       // 1205/1213/断连/池超时：调用方以同 key 重试
    IdempotencyConflict,  // 并发竞争：同 (sender, client_message_id) 已提交不同 payload
    TooManyRecipients,    // 群成员数超过 fan-out cap（100）
    NotFound,             // FK 1452：目标用户/群不存在
    Storage,              // 其它存储错误（数据超限、完整性等）
};

class MessageStoreError : public std::runtime_error {
public:
    MessageStoreError(StoreErrorKind kind, const std::string& what)
        : std::runtime_error(what), kind_(kind)
    {
    }
    StoreErrorKind kind() const { return kind_; }

private:
    StoreErrorKind kind_;
};

// 独立错误映射函数，便于单元测试错误分类（MySQLUserRepository 先例）。
StoreErrorKind mapStoreError(unsigned int mysqlErrno);

class MySQLMessageStore : public MessageStore {
public:
    // accept 事务内的 SQL 步骤（故障注入点，docs/tasks/P3-04.md RED 节；
    // P3-12 故障矩阵复用）。Recover* 只在并发唯一键竞争重试路径触发。
    enum class Step {
        FindConversation,     // SELECT DirectConversation/GroupConversation
        CreateConversation,   // INSERT Conversation
        CreateConversationLink,  // INSERT DirectConversation/GroupConversation
        RecoverConversation,  // 唯一键竞争后新事务内重读
        LockConversation,     // SELECT ... FOR UPDATE（sequence 串行点）
        AdvanceSequence,      // UPDATE next_sequence
        InsertMessage,        // INSERT ChatMessage
        RecoverMessage,       // 唯一键竞争后新事务内读已提交原 Message
        InsertOutboxEvent,    // INSERT OutboxEvent
        InsertDelivery,       // INSERT MessageDelivery（每接收者一次）
        Commit,               // COMMIT
    };

    // 测试故障注入：每个 SQL 步骤执行成功后回调；回调抛异常模拟该步骤后的失败。
    // adapter 构造参数（内部 seam），不进 MessageStore/ReliableMessaging 公共接口
    // （计划 §10 停止条件）；生产路径传 nullptr。回调须线程安全（并发测试不用）。
    struct FaultHook {
        virtual ~FaultHook() = default;
        virtual void onStep(Step step) = 0;
    };

    explicit MySQLMessageStore(ConnectionPool& pool, uint64_t fanOutCap,
                               FaultHook* faultHook = nullptr);

    std::shared_ptr<const Message> findAccepted(const ClientMessageId& clientMessageId,
                                                UserId sender) override;
    ConversationId getOrCreateConversation(const SessionIdentity& sender,
                                           const SendMessageCommand& cmd) override;
    Message insertMessage(const Message& draft) override;
    void insertDelivery(const Delivery& delivery) override;
    void updateDelivery(const Delivery& delivery) override;
    std::vector<Delivery> deliveriesByRecipient(UserId recipient) override;
    std::vector<Delivery> deliveriesByMessage(MessageId messageId) override;
    std::shared_ptr<const Message> findMessage(MessageId messageId) override;

private:
    void fire(Step step);
    void beginTx();
    void commitTx();
    void rollbackTx();
    void rollbackAndClear();
    void finishPending();  // 边界：非 accept 操作先提交遗留 accept 事务
    void ensureTx();

    uint64_t findDirectConversation(MySQL& m, int32_t low, int32_t high);
    uint64_t findGroupConversation(MySQL& m, int32_t groupId);
    uint64_t createDirectConversation(MySQL& m, int32_t low, int32_t high);
    uint64_t createGroupConversation(MySQL& m, int32_t groupId);

    std::shared_ptr<const Message> loadMessage(MySQL& m, const std::string& where,
                                               MYSQL_BIND* params, unsigned nParams);
    std::vector<Delivery> deliveriesWhere(MySQL& m, const std::string& where,
                                          MYSQL_BIND* params, unsigned nParams);

    ConnectionPool& pool_;
    uint64_t fanOutCap_;
    FaultHook* faultHook_;
};
