#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

// P3-02 可靠消息模块：领域值类型与对外 interface（计划 §3、§5 P3-02）。
// raw JSON 不进领域类型；codec/session 绑定在应用层（P3-05/P3-06）完成。

struct UserId {
    explicit UserId() = default;
    explicit UserId(uint64_t v) : value(v) {}
    uint64_t value = 0;
};
inline bool operator==(const UserId& a, const UserId& b) { return a.value == b.value; }
inline bool operator!=(const UserId& a, const UserId& b) { return !(a == b); }
inline bool operator<(const UserId& a, const UserId& b) { return a.value < b.value; }

struct GroupId {
    explicit GroupId() = default;
    explicit GroupId(uint64_t v) : value(v) {}
    uint64_t value = 0;
};
inline bool operator==(const GroupId& a, const GroupId& b) { return a.value == b.value; }
inline bool operator!=(const GroupId& a, const GroupId& b) { return !(a == b); }
inline bool operator<(const GroupId& a, const GroupId& b) { return a.value < b.value; }

struct MessageId {
    explicit MessageId() = default;
    explicit MessageId(uint64_t v) : value(v) {}
    uint64_t value = 0;
};
inline bool operator==(const MessageId& a, const MessageId& b) { return a.value == b.value; }
inline bool operator!=(const MessageId& a, const MessageId& b) { return !(a == b); }
inline bool operator<(const MessageId& a, const MessageId& b) { return a.value < b.value; }

struct ConversationId {
    explicit ConversationId() = default;
    explicit ConversationId(uint64_t v) : value(v) {}
    uint64_t value = 0;
};
inline bool operator==(const ConversationId& a, const ConversationId& b) { return a.value == b.value; }
inline bool operator!=(const ConversationId& a, const ConversationId& b) { return !(a == b); }
inline bool operator<(const ConversationId& a, const ConversationId& b) { return a.value < b.value; }

struct ConversationSequence {
    explicit ConversationSequence() = default;
    explicit ConversationSequence(uint64_t v) : value(v) {}
    uint64_t value = 0;
};
inline bool operator==(const ConversationSequence& a, const ConversationSequence& b)
{
    return a.value == b.value;
}
inline bool operator!=(const ConversationSequence& a, const ConversationSequence& b)
{
    return !(a == b);
}
inline bool operator<(const ConversationSequence& a, const ConversationSequence& b)
{
    return a.value < b.value;
}

// 会话身份占位：完整 AuthenticatedSession 是 P3-05 产物，本任务不提前实现 SessionRegistry。
struct SessionIdentity {
    SessionIdentity() = default;
    SessionIdentity(UserId user, uint64_t gen) : userId(user), generation(gen) {}
    UserId userId;
    uint64_t generation = 0;
};
inline bool operator==(const SessionIdentity& a, const SessionIdentity& b)
{
    return a.userId == b.userId && a.generation == b.generation;
}
inline bool operator!=(const SessionIdentity& a, const SessionIdentity& b) { return !(a == b); }

// 发送端稳定幂等标识：可打印 ASCII（0x20..0x7E），长度 1..64（schema ASCII(1..64)）。
class ClientMessageId {
public:
    explicit ClientMessageId(const std::string& value) : value_(value)
    {
        if (!isValid(value)) {
            throw std::invalid_argument("ClientMessageId must be printable ASCII, length 1..64");
        }
    }

    static bool isValid(const std::string& value)
    {
        if (value.empty() || value.size() > 64) {
            return false;
        }
        for (size_t i = 0; i < value.size(); ++i) {
            const unsigned char c = static_cast<unsigned char>(value[i]);
            if (c < 0x20 || c > 0x7E) {
                return false;
            }
        }
        return true;
    }

    const std::string& value() const { return value_; }

private:
    std::string value_;
};
inline bool operator==(const ClientMessageId& a, const ClientMessageId& b)
{
    return a.value() == b.value();
}
inline bool operator!=(const ClientMessageId& a, const ClientMessageId& b) { return !(a == b); }
inline bool operator<(const ClientMessageId& a, const ClientMessageId& b)
{
    return a.value() < b.value();
}

// 发送命令：群消息接受时快照接收者，成员列表由命令携带。
struct SendMessageCommand {
    enum class Kind {
        Direct,
        Group,
    };
    // 默认值为合法占位（"0"），默认构造所需；业务路径总是显式赋值。
    SendMessageCommand() = default;
    SendMessageCommand(ClientMessageId cmid, Kind k, UserId to, GroupId gid,
                       const std::vector<UserId>& m, const std::string& c)
        : clientMessageId(cmid), kind(k), directRecipient(to), groupId(gid), members(m), content(c)
    {
    }
    ClientMessageId clientMessageId = ClientMessageId("0");
    Kind kind = Kind::Direct;
    UserId directRecipient;       // Kind::Direct 的目标用户
    GroupId groupId;              // Kind::Group 的群
    std::vector<UserId> members;  // Kind::Group：接受事务内的接收者快照
    std::string content;
};

// 已接受 Message：接受时快照的命令 + 服务器生成身份。
struct Message {
    MessageId id;
    ConversationId conversationId;
    ConversationSequence sequence;
    UserId senderId;
    SendMessageCommand command;
};

// 幂等键下 payload 是否一致（同一消息意图）：kind/目标/成员/content 全部相同。
inline bool samePayload(const SendMessageCommand& a, const SendMessageCommand& b)
{
    if (a.kind != b.kind || a.content != b.content || a.clientMessageId != b.clientMessageId) {
        return false;
    }
    if (a.kind == SendMessageCommand::Kind::Direct) {
        return a.directRecipient == b.directRecipient;
    }
    if (a.groupId != b.groupId || a.members.size() != b.members.size()) {
        return false;
    }
    for (size_t i = 0; i < a.members.size(); ++i) {
        if (a.members[i] != b.members[i]) {
            return false;
        }
    }
    return true;
}

// 错误分类（message-reliability.md §2.4）；TooManyRecipients/NotConversationMember/
// NotFound/DependencyBusy 由 adapter 与 P3-04+ 引入，本任务不产生。
enum class AcceptError {
    DependencyBusy,
    IdempotencyConflict,
    TooManyRecipients,
    NotConversationMember,
    NotFound,
    InvalidClientMessageId,
};

struct AcceptOutcome {
    bool ok = false;
    bool duplicate = false;  // 同 key 重试：true；不同 payload 复用 key 不是 duplicate
    MessageId messageId;
    ConversationId conversationId;
    ConversationSequence sequence;
    AcceptError error = AcceptError::DependencyBusy;  // ok=false 时有效
};

// 接收端显式确认结果：重复 ACK 幂等且不越权（message-reliability.md §4 故障点 4/5）。
enum class AckResult {
    Acknowledged,  // 首次使该 Delivery 进入 Acknowledged
    Idempotent,    // 已 Acknowledged，重复 ACK，无副作用
    NotRecipient,  // ACK 主体不是该 Message 的接收者，忽略
    NotFound,      // MessageId 不存在
};

struct AckOutcome {
    AckResult result = AckResult::NotFound;
};

enum class DeliveryState {
    Pending,
    InFlight,
    Acknowledged,
    Expired,  // 转移逻辑 P3-08（retention）；本任务不产生
};

// 一条已接受 Message 对一个接收者的待履行投递义务。
struct Delivery {
    Delivery() = default;
    Delivery(MessageId mid, ConversationId cid, UserId r, DeliveryState st, SessionIdentity owner,
             int64_t leaseUntil, uint32_t attempts, int64_t ackedAt, int64_t expiresAt)
        : messageId(mid), conversationId(cid), recipient(r), state(st), leaseOwner(owner),
          leaseUntilMs(leaseUntil), attemptCount(attempts), acknowledgedAtMs(ackedAt),
          expiresAtMs(expiresAt)
    {
    }
    MessageId messageId;
    ConversationId conversationId;
    UserId recipient;
    DeliveryState state = DeliveryState::Pending;
    SessionIdentity leaseOwner;  // 当前 lease 持有者（Pending 时为默认值）
    int64_t leaseUntilMs = 0;
    uint32_t attemptCount = 0;
    int64_t acknowledgedAtMs = 0;
    int64_t expiresAtMs = 0;
};

// 一次 DeliveryAttempt：TCP 写入只是尝试，本值类型无"成功送达"语义。
struct DeliveryAttempt {
    MessageId messageId;
    ConversationId conversationId;
    ConversationSequence sequence;
    UserId recipient;
    std::string content;
    uint32_t attemptNumber = 0;
};

// 模块内部确定性测试 seam；生产系统时钟实现 P3-08 引入。
class Clock {
public:
    virtual ~Clock() = default;
    virtual int64_t nowMs() = 0;
};

class MessageStore;
class DeliverySink;

// P3 深模块（计划 §3）：网络层只学习 accept/acknowledge/sessionAvailable/sessionClosed，
// 重试、租约、Conversation 分配、幂等与状态机全部在模块内部。
// 同一实现将以 MySQL MessageStore 复用（P3-04），测试只穿过本 interface。
// 线程约束：实例须由单一调用者串行驱动（P3-05 接入后为 SessionSerialExecutor 或等价串行执行器）；
// 接口内部非原子（读-改-写），并发调用未定义行为。
class ReliableMessaging {
public:
    ReliableMessaging(MessageStore& store, DeliverySink& sink, Clock& clock, uint64_t leaseMs);
    ReliableMessaging(const ReliableMessaging&) = delete;
    ReliableMessaging& operator=(const ReliableMessaging&) = delete;

    // 持久接受：同 (sender, ClientMessageId) 幂等返回原结果；
    // 不同 payload 复用 key → IdempotencyConflict。
    // 存储层故障（并发竞争、依赖忙、cap 超限等）时 accept/acknowledge 可能抛出
    // MessageStoreError（或 store 定义的异常类型）；调用方（P3-06 协议层）负责
    // 映射为 AcceptOutcome 错误结果。
    AcceptOutcome accept(const SessionIdentity& sender, const SendMessageCommand& cmd);

    // 接收端按 MessageId 显式确认；ACK 主体只来自 Session，他人 ACK 不越权。
    AckOutcome acknowledge(const SessionIdentity& acker, MessageId messageId);

    // Session 上线：claim 自己名下 Pending Delivery（含租约到期重领）并交给 DeliverySink。
    void sessionAvailable(const SessionIdentity& session);

    // Session 下线：名下 InFlight 立即回 Pending（lease 释放，message-reliability.md §3）。
    void sessionClosed(const SessionIdentity& session);

    void start();

    // in-memory 无后台任务，stop 为空操作；P3-08 引入 timer 后有界退出。
    void stop(int64_t deadlineMs);

private:
    // 单在途 head-of-line claim：每 (recipient, conversation) 至多一个未确认 sequence。
    void claimFor(const SessionIdentity& session);

    MessageStore& store_;
    DeliverySink& sink_;
    Clock& clock_;
    uint64_t leaseMs_;
};
