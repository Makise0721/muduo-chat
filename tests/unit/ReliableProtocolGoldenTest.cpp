// P3-06 协议 golden 一次冻结（docs/tasks/P3-06.md 决策表 1/2/3/5/6/10 行）：
// - msgid 11/12/13 与错误码 101..107 数值断言（冻结后不再变更，spec §2.5）；
// - 服务器回复（MESSAGE_ACCEPTED、错误响应）与 tests/fixtures/ golden JSON
//   字节级比对（nlohmann dump 默认 std::map 键序，golden 与之一致）；
// - 命令 fixture 经 parseChatMessage 语义解析（键序无关）；
// - errmsg 7 个字符串 pin（errno/errmsg 双通道一致）；
// - DELIVERY_ACK 形状（msgid=12+message_id，P3-07 handler 落地时复用）；
// - legacy 成功回显形状 = 原 Command + errno=0（字节随客户端，语义锁定）。
// 客户端 fixture（tests/fixtures/）与 golden 同步：改协议值必须先改 fixture。

#include "ChatService.hpp"
#include "app/DeliverySink.hpp"
#include "app/ProtocolCodec.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

namespace {

std::string repoRoot()
{
    std::string file(__FILE__);
    size_t pos = file.find("tests/unit/");
    if (pos == std::string::npos) {
        return "";
    }
    return file.substr(0, pos);
}

std::string readFixture(const std::string& name)
{
    std::ifstream in(repoRoot() + "tests/fixtures/" + name);
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string content = ss.str();
    while (!content.empty() && (content.back() == '\n' || content.back() == '\r')) {
        content.pop_back();
    }
    return content;
}

// 服务器回复 = dump() + "\n"（ChatService 发送格式）。fixture 不含换行，
// 比对时统一在 fixture 后补 \n。
std::string replyBytes(const std::string& fixture)
{
    return fixture + "\n";
}

const char* kErrmsg101 = "not a conversation member";
const char* kErrmsg102 = "too many recipients";
const char* kErrmsg103 = "idempotency conflict";
const char* kErrmsg104 = "dependency busy";
const char* kErrmsg105 = "content too long";
const char* kErrmsg106 = "target not found";
const char* kErrmsg107 = "invalid client_message_id";

} // namespace

// 决策表 1/2/3 行：msgid 11/12/13 冻结（B-22 不占 1..10）。
TEST(ReliableProtocolGolden, MsgidsFrozenAt11_12_13)
{
    EXPECT_EQ(11, MESSAGE_ACCEPTED_MSG);
    EXPECT_EQ(12, DELIVERY_ACK_MSG);
    EXPECT_EQ(13, ERROR_RESP_MSG);
    // B-22：新值不得占用 1..10（1..10 由既有枚举隐式占用）。
    EXPECT_GT(MESSAGE_ACCEPTED_MSG, 10);
    EXPECT_GT(DELIVERY_ACK_MSG, 10);
    EXPECT_GT(ERROR_RESP_MSG, 10);
}

// 决策表第 5 行：错误码 101..107 冻结。
TEST(ReliableProtocolGolden, ErrnosFrozenAt101To107)
{
    EXPECT_EQ(101, kErrnoNotConversationMember);
    EXPECT_EQ(102, kErrnoTooManyRecipients);
    EXPECT_EQ(103, kErrnoIdempotencyConflict);
    EXPECT_EQ(104, kErrnoDependencyBusy);
    EXPECT_EQ(105, kErrnoContentTooLong);
    EXPECT_EQ(106, kErrnoNotFound);
    EXPECT_EQ(107, kErrnoInvalidClientMessageId);
}

// 决策表第 6 行：content 上限 16KB（UTF-8 字节）。
TEST(ReliableProtocolGolden, ContentLimitFrozenAt16Kb)
{
    EXPECT_EQ(16u * 1024u, kMaxContentBytes);
}

// errmsg 字符串 pin：errno/errmsg 双通道，7 个 errmsg 不得漂移。
TEST(ReliableProtocolGolden, ErrmsgStringsPinned)
{
    EXPECT_STREQ(kErrmsg101, protocolErrmsg(kErrnoNotConversationMember));
    EXPECT_STREQ(kErrmsg102, protocolErrmsg(kErrnoTooManyRecipients));
    EXPECT_STREQ(kErrmsg103, protocolErrmsg(kErrnoIdempotencyConflict));
    EXPECT_STREQ(kErrmsg104, protocolErrmsg(kErrnoDependencyBusy));
    EXPECT_STREQ(kErrmsg105, protocolErrmsg(kErrnoContentTooLong));
    EXPECT_STREQ(kErrmsg106, protocolErrmsg(kErrnoNotFound));
    EXPECT_STREQ(kErrmsg107, protocolErrmsg(kErrnoInvalidClientMessageId));
    EXPECT_STREQ("protocol error", protocolErrmsg(0));
    EXPECT_STREQ("protocol error", protocolErrmsg(999));
}

// MESSAGE_ACCEPTED（msgid=11）五字段 + duplicate 字节级 golden。
TEST(ReliableProtocolGolden, MessageAcceptedMatchesFixtureBytes)
{
    const std::string fixture = readFixture("reply_message_accepted.json");
    const std::string got = buildMessageAcceptedReply(MESSAGE_ACCEPTED_MSG, "cm-001", 42, 9, 3,
                                                      false)
                                .dump() +
                            "\n";
    EXPECT_EQ(replyBytes(fixture), got);
}

// 错误响应（msgid=13）全表字节级 golden：101..107 各一 fixture。
TEST(ReliableProtocolGolden, ErrorRepliesMatchFixtureBytes)
{
    struct Row {
        int errnoCode;
        const char* fixture;
    };
    const Row rows[] = {
        {kErrnoNotConversationMember, "reply_error_101.json"},
        {kErrnoTooManyRecipients, "reply_error_102.json"},
        {kErrnoIdempotencyConflict, "reply_error_103.json"},
        {kErrnoDependencyBusy, "reply_error_104.json"},
        {kErrnoContentTooLong, "reply_error_105.json"},
        {kErrnoNotFound, "reply_error_106.json"},
        {kErrnoInvalidClientMessageId, "reply_error_107.json"},
    };
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i) {
        // 107 的 client_message_id 回显为非法原值（65 字节，非 ASCII 1..64）；
        // 其余回显合法 cmid。
        std::string cmid = (rows[i].errnoCode == kErrnoInvalidClientMessageId)
                               ? std::string(65, 'x')
                               : std::string("cm-001");
        const std::string fixture = readFixture(rows[i].fixture);
        const std::string got = buildErrorReply(ERROR_RESP_MSG, rows[i].errnoCode,
                                                protocolErrmsg(rows[i].errnoCode), &cmid)
                                    .dump() +
                                "\n";
        EXPECT_EQ(replyBytes(fixture), got) << "errno " << rows[i].errnoCode;
    }
}

// 错误响应省略 client_message_id（不可解析时 nullptr）。
TEST(ReliableProtocolGolden, ErrorReplyOmitsClientMessageIdWhenNull)
{
    nlohmann::json reply = buildErrorReply(ERROR_RESP_MSG, kErrnoNotFound,
                                           protocolErrmsg(kErrnoNotFound), nullptr);
    EXPECT_FALSE(reply.contains("client_message_id"));
    EXPECT_EQ(13, reply["msgid"].get<int>());
    EXPECT_EQ(106, reply["errno"].get<int>());
    EXPECT_EQ(kErrmsg106, reply["errmsg"].get<std::string>());
}

// DELIVERY_ACK 形状冻结（客户端→服务器；handler 属 P3-07，fixture 先 pin 形状）。
TEST(ReliableProtocolGolden, DeliveryAckShapePinned)
{
    nlohmann::json ack = nlohmann::json::parse(readFixture("reply_delivery_ack.json"));
    EXPECT_EQ(12, ack["msgid"].get<int>());
    EXPECT_TRUE(ack.contains("message_id"));
    EXPECT_EQ(2u, ack.size());  // 至少 msgid+message_id，不引入额外必填字段
}

// v2 命令解析（键序无关，fixture 为 canonical 字节）。
TEST(ReliableProtocolGolden, V2CommandsParseFromFixture)
{
    ParsedChatMessage parsed;
    nlohmann::json cmd = nlohmann::json::parse(readFixture("command_one_chat_v2.json"));
    ASSERT_EQ(0, parseChatMessage(ChatCommandKind::Direct, cmd, &parsed));
    EXPECT_FALSE(parsed.legacy);
    EXPECT_TRUE(parsed.hasClientMessageId);
    EXPECT_EQ("cm-001", parsed.clientMessageId);
    EXPECT_EQ("hello", parsed.content);
    EXPECT_EQ(2, parsed.directRecipient);

    ParsedChatMessage groupParsed;
    nlohmann::json gcmd = nlohmann::json::parse(readFixture("command_group_chat_v2.json"));
    ASSERT_EQ(0, parseChatMessage(ChatCommandKind::Group, gcmd, &groupParsed));
    EXPECT_FALSE(groupParsed.legacy);
    EXPECT_EQ("cm-002", groupParsed.clientMessageId);
    EXPECT_EQ("hi all", groupParsed.content);
    EXPECT_EQ(7, groupParsed.groupId);
}

// legacy 命令（缺 client_message_id）判定为 legacy，字段照常解析。
TEST(ReliableProtocolGolden, LegacyCommandParsesFromFixture)
{
    ParsedChatMessage parsed;
    nlohmann::json cmd = nlohmann::json::parse(readFixture("command_one_chat_legacy.json"));
    ASSERT_EQ(0, parseChatMessage(ChatCommandKind::Direct, cmd, &parsed));
    EXPECT_TRUE(parsed.legacy);
    EXPECT_FALSE(parsed.hasClientMessageId);
    EXPECT_EQ("hello", parsed.content);
    EXPECT_EQ(2, parsed.directRecipient);
}

// M2（对抗审查）：旧字段别名 msg→content（spec §5.1）——无 content 但 msg
// 存在的 legacy 命令照常解析（msg 作为 content，legacy 判定不受影响）。
TEST(ReliableProtocolGolden, LegacyMsgFieldAliasParsesFromFixture)
{
    ParsedChatMessage parsed;
    nlohmann::json cmd =
        nlohmann::json::parse(readFixture("command_one_chat_legacy_msg_alias.json"));
    ASSERT_EQ(0, parseChatMessage(ChatCommandKind::Direct, cmd, &parsed));
    EXPECT_TRUE(parsed.legacy);
    EXPECT_FALSE(parsed.hasClientMessageId);
    EXPECT_EQ("hello", parsed.content);
    EXPECT_EQ(2, parsed.directRecipient);
}

// legacy 成功回显形状：原 Command + errno=0（字节随客户端字段序，语义锁定）。
TEST(ReliableProtocolGolden, LegacySuccessEchoShapePinned)
{
    nlohmann::json cmd = nlohmann::json::parse(readFixture("command_one_chat_legacy.json"));
    nlohmann::json echo = nlohmann::json::parse(readFixture("reply_legacy_echo.json"));
    EXPECT_EQ(0, echo["errno"].get<int>());
    // 回显 = 原命令全字段 + errno=0；不发 MESSAGE_ACCEPTED（决策表第 10 行）。
    for (nlohmann::json::const_iterator it = cmd.begin(); it != cmd.end(); ++it) {
        EXPECT_TRUE(echo.contains(it.key())) << "echo missing " << it.key();
        EXPECT_EQ(it.value(), echo[it.key()]) << "echo differs on " << it.key();
    }
    EXPECT_EQ(echo.size(), cmd.size() + 1u);
}

namespace {

class MarkerDeliverySink : public DeliverySink {
public:
    explicit MarkerDeliverySink(DeliverDisposition disposition = DeliverDisposition::Accepted)
        : disposition_(disposition) {}

    DeliverDisposition deliver(const DeliveryAttempt&) override
    {
        return disposition_;
    }

private:
    DeliverDisposition disposition_;
};

} // namespace

TEST(ReliableProtocolGolden, DeliverySinkForwarderFailsClosedUntilBound)
{
    DelegatingDeliverySink forwarder;
    DeliveryAttempt attempt;
    EXPECT_EQ(DeliverDisposition::Closed, forwarder.deliver(attempt));

    MarkerDeliverySink marker;
    forwarder.bind(&marker);

    EXPECT_EQ(DeliverDisposition::Accepted, forwarder.deliver(attempt));
}

TEST(ReliableProtocolGolden, DeliverySinkForwarderBindsOnce)
{
    DelegatingDeliverySink forwarder;
    MarkerDeliverySink first;
    MarkerDeliverySink second(DeliverDisposition::Closed);
    DeliveryAttempt attempt;

    EXPECT_FALSE(forwarder.bind(nullptr));
    EXPECT_EQ(DeliverDisposition::Closed, forwarder.deliver(attempt));
    EXPECT_TRUE(forwarder.bind(&first));
    EXPECT_TRUE(forwarder.bind(&first));
    EXPECT_FALSE(forwarder.bind(&second));
    EXPECT_EQ(DeliverDisposition::Accepted, forwarder.deliver(attempt));
}
