#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

// P3-02 可靠消息模块：领域值类型（计划 §3、§5 P3-02）。
// raw JSON 不进领域类型；codec/session 绑定在应用层（P3-05/P3-06）完成。
//
// 本头自包含且不声明 class Clock：mymuduo TimerQueue.h 的全局
// `using Clock = std::chrono::steady_clock` 与领域 class Clock（app/Clock.hpp）
// 同名冲突——投递 port（app/DeliverySink.hpp）被 mymuduo 侧 TU（如
// SessionDeliverySink）引入，只能依赖本头的值类型。

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
// 排序（DeliveryCoordinator 的 paused_/active 集合用）。
inline bool operator<(const SessionIdentity& a, const SessionIdentity& b)
{
    if (a.userId.value != b.userId.value) {
        return a.userId.value < b.userId.value;
    }
    return a.generation < b.generation;
}

// legacy identity 判定（spec §5.1："legacy:" 前缀）：legacy Delivery 在 socket
// 准入后自动视为已确认（implicit-ack），客户端不发送 DELIVERY_ACK。
inline bool isLegacyClientMessageId(const std::string& value)
{
    return value.compare(0, 7, "legacy:") == 0;
}

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
// sequence：Conversation 内序号（P3-07 由 store 在 deliveriesBy* 返回时填充——
// 供 claimFor 的 head-of-line 选择，避免按 delivery 逐条 findMessage 的 O(n) 扫描）。
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
    ConversationSequence sequence;  // 消息在 Conversation 内的序号（store 填充）
    UserId recipient;
    DeliveryState state = DeliveryState::Pending;
    SessionIdentity leaseOwner;  // 当前 lease 持有者（Pending 时为默认值）
    int64_t leaseUntilMs = 0;
    uint32_t attemptCount = 0;
    int64_t acknowledgedAtMs = 0;
    int64_t expiresAtMs = 0;
};
