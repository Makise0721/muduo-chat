// P3-06 应用切片（真实 chat_p306 库，不 skip）：
// - parseChatMessage：v2/legacy 判定、ASCII 1..64 校验（非法→107）、16KB 边界
//   （恰 16KB 通过 / 超 1 字节→105）、缺 content → 抛异常（B-25 静默契约）；
// - acceptChatCommand：成功五字段（MESSAGE_ACCEPTED）、同 key 重试返回原 identity
//   （duplicate=true）、同 key 不同 payload→103、目标不存在→106、非成员→101、
//   群不存在→106、超 fan-out cap→102、成员查询失败→104（B-18/B-19 收紧）；
// - legacy identity：legacy:<uid>:<计数> 格式 + legacy-mode 计数递增（spec §5.1）；
// - 错误映射全表 101..107（105/107 为 codec 层，其余为 accept 层）。
// 串行约束：本二进制独占 chat_p306 库与单例连接池（勿与其它二进制并行）。

#include "app/ChatApplication.hpp"
#include "app/GroupRepository.hpp"
#include "app/MySQLFriendRepository.hpp"
#include "app/MySQLGroupRepository.hpp"
#include "app/MySQLMessageRepository.hpp"
#include "app/MySQLUserRepository.hpp"
#include "app/ProtocolCodec.hpp"

#include "db/MySQL.hpp"
#include "db/SchemaMigration.hpp"

#include <gtest/gtest.h>
#include <mysql/mysql.h>

#include <cstdlib>
#include <string>

namespace {

const char* kTestDb = "chat_p306";

std::string MySqlPassword()
{
    const char* pwd = getenv("DB_PASSWORD");
    return pwd ? std::string(pwd) : std::string("123456");
}

std::string repoRoot()
{
    std::string file(__FILE__);
    size_t pos = file.find("tests/unit/");
    if (pos == std::string::npos) {
        return "";
    }
    return file.substr(0, pos);
}

void resetDb()
{
    MySQL admin;
    ASSERT_TRUE(admin.connect("127.0.0.1", "root", MySqlPassword(), "", 3306));
    ASSERT_TRUE(admin.update("DROP DATABASE IF EXISTS chat_p306"));
    ASSERT_TRUE(admin.update("CREATE DATABASE chat_p306 DEFAULT CHARSET utf8"));
    schema_migration::Migrator migrator("127.0.0.1", "root", MySqlPassword(), kTestDb, 3306);
    schema_migration::MigrateResult r = migrator.migrateTo(repoRoot() + "sql/migrations", "", 30);
    ASSERT_TRUE(r.ok) << r.error;
}

// 显式 id：accept 的 FK 需要 User 行；群成员资格经 GroupUser 查询（acceptChatCommand
// 的上层预检），故按用例 seed 群成员表。
void seed()
{
    MySQL conn;
    ASSERT_TRUE(conn.connect("127.0.0.1", "root", MySqlPassword(), kTestDb, 3306));
    ASSERT_TRUE(conn.update(
        "INSERT INTO User(id, name, password) VALUES (1,'u1','x'),(2,'u2','x'),(3,'u3','x')"));
    for (int i = 4; i <= 120; ++i) {
        std::string sql = "INSERT INTO User(id, name, password) VALUES (" + std::to_string(i) +
                          ", 'u" + std::to_string(i) + "', 'x')";
        ASSERT_TRUE(conn.update(sql));
    }
    // 群 7：成员 {1,2}（正常群）；群 8：成员 {2}（1 非成员→101）；
    // 群 700：成员 1..101（101 > fan-out cap 100 → 102）。
    ASSERT_TRUE(conn.update(
        "INSERT INTO AllGroup(id, groupname) VALUES (7,'g7'),(8,'g8'),(700,'g700')"));
    ASSERT_TRUE(conn.update(
        "INSERT INTO GroupUser(groupid, userid) VALUES (7,1),(7,2),(8,2)"));
    std::string capSql = "INSERT INTO GroupUser(groupid, userid) VALUES ";
    for (int i = 1; i <= 101; ++i) {
        capSql += (i == 1 ? "(700," : ",(700,") + std::to_string(i) + ")";
    }
    ASSERT_TRUE(conn.update(capSql));
}

// 单例连接池：必须在 resetDb 之后首次初始化（池连接持有 DATABASE 句柄）。
ConnectionPool& pool()
{
    static ConnectionPool* instance = [] {
        ConnectionPool* p = &ConnectionPool::getInstance();
        p->init("127.0.0.1", "root", MySqlPassword(), kTestDb, 3306, 4);
        return p;
    }();
    return *instance;
}

struct AppHarness
{
    MySQLUserRepository users;
    MySQLFriendRepository friends;
    MySQLGroupRepository groups;
    MySQLMessageRepository messages;
    ChatApplication app;

    AppHarness()
        : users(pool()), friends(pool()), groups(pool()), messages(pool()),
          app(&users, &friends, &groups, &messages)
    {
    }
};

// B-19 收紧：成员查询失败 → 104（DependencyBusy），不再假装成功（errno=0 退役）。
class FailingGroupRepository : public GroupRepository {
public:
    CreateGroupResult create(int64_t, const std::string&, const std::string&) override
    {
        return CreateGroupResult();
    }
    JoinGroupResult join(int64_t, int64_t) override { return JoinGroupResult(); }
    MembersResult members(int64_t) override { return MembersResult(); }  // ok=false
};

class MessageAcceptanceApplication : public ::testing::Test {
protected:
    static void SetUpTestSuite()
    {
        resetDb();  // 连接池初始化前重建库
        seed();
        (void)pool();
    }
};

// 解析+accept 一体化 helper：codec 通过后走 worker 语义（app 层预检 + ledger）。
AcceptResultView acceptCommand(ChatApplication* app, int64_t userId, int64_t generation,
                               ChatCommandKind kind, const nlohmann::json& cmd)
{
    ParsedChatMessage parsed;
    int codecErr = parseChatMessage(kind, cmd, &parsed);
    EXPECT_EQ(0, codecErr) << cmd.dump();
    AcceptResultView view;
    acceptChatCommand(app, userId, generation, parsed, kind, &view);
    return view;
}

nlohmann::json directCommand(const std::string& cmid, int64_t toid, const std::string& content,
                             bool legacy = false)
{
    nlohmann::json cmd;
    cmd["msgid"] = 6;
    cmd["id"] = 1;
    cmd["toid"] = toid;
    if (!legacy) {
        cmd["client_message_id"] = cmid;
    }
    cmd["content"] = content;
    return cmd;
}

nlohmann::json groupCommand(const std::string& cmid, int64_t groupid, const std::string& content,
                            bool legacy = false)
{
    nlohmann::json cmd;
    cmd["msgid"] = 10;
    cmd["id"] = 1;
    cmd["groupid"] = groupid;
    if (!legacy) {
        cmd["client_message_id"] = cmid;
    }
    cmd["content"] = content;
    return cmd;
}

} // namespace

// ---- codec 层（无 DB 依赖）----

TEST_F(MessageAcceptanceApplication, ParseRejectsInvalidClientMessageIdWith107)
{
    const std::string tooLong(65, 'x');
    const std::string nonAscii("id-\xFF\x01");
    for (size_t i = 0; i < 2; ++i) {
        const std::string& bad = (i == 0) ? tooLong : nonAscii;
        ParsedChatMessage parsed;
        EXPECT_EQ(kErrnoInvalidClientMessageId,
                  parseChatMessage(ChatCommandKind::Direct,
                                   directCommand(bad, 2, "hello"), &parsed))
            << "cmid len=" << bad.size();
    }
    // 空串 ≠ legacy：字段存在即判定非 legacy，再校验格式。
    ParsedChatMessage parsed;
    EXPECT_EQ(kErrnoInvalidClientMessageId,
              parseChatMessage(ChatCommandKind::Direct, directCommand("", 2, "hello"), &parsed));
}

TEST_F(MessageAcceptanceApplication, ParseRejectsContentOver16KbWith105)
{
    const std::string boundary(kMaxContentBytes, 'a');
    ParsedChatMessage parsed;
    EXPECT_EQ(0, parseChatMessage(ChatCommandKind::Direct, directCommand("cm-b", 2, boundary),
                                  &parsed));
    EXPECT_EQ(boundary, parsed.content);

    const std::string over(kMaxContentBytes + 1, 'a');
    EXPECT_EQ(kErrnoContentTooLong,
              parseChatMessage(ChatCommandKind::Direct, directCommand("cm-b", 2, over), &parsed));
}

TEST_F(MessageAcceptanceApplication, ParseThrowsOnMissingOrWrongTypedContent)
{
    nlohmann::json noContent;
    noContent["msgid"] = 6;
    noContent["id"] = 1;
    noContent["toid"] = 2;
    ParsedChatMessage parsed;
    EXPECT_THROW(parseChatMessage(ChatCommandKind::Direct, noContent, &parsed),
                 std::exception);

    nlohmann::json wrongType = directCommand("cm-c", 2, "hello");
    wrongType["content"] = 12345;
    EXPECT_THROW(parseChatMessage(ChatCommandKind::Direct, wrongType, &parsed),
                 std::exception);
}

TEST_F(MessageAcceptanceApplication, ParseMissingClientMessageIdIsLegacy)
{
    ParsedChatMessage parsed;
    ASSERT_EQ(0, parseChatMessage(ChatCommandKind::Direct, directCommand("", 0, "hi", true),
                                  &parsed));
    EXPECT_TRUE(parsed.legacy);
    EXPECT_FALSE(parsed.hasClientMessageId);
    EXPECT_EQ("hi", parsed.content);
    EXPECT_EQ(0, parsed.directRecipient);
}

// ---- accept 层（chat_p306）----

TEST_F(MessageAcceptanceApplication, AcceptDirectReturnsFiveFieldAccepted)
{
    AppHarness h;
    AcceptResultView view = acceptCommand(&h.app, 1, 1, ChatCommandKind::Direct,
                                          directCommand("acc-001", 2, "hello"));
    ASSERT_TRUE(view.ok);
    EXPECT_FALSE(view.duplicate);
    EXPECT_GT(view.messageId, 0u);
    EXPECT_GT(view.conversationId, 0u);
    EXPECT_EQ(1u, view.sequence);  // 全新 (1,2) 对话首条
    EXPECT_EQ("acc-001", view.clientMessageId);

    // spec §2.2：五字段 + duplicate 的 MESSAGE_ACCEPTED（msgid=11 冻结值见
    // ChatService.hpp EnMsgType；数值由 ReliableProtocolGoldenTest pin）。
    nlohmann::json reply = buildMessageAcceptedReply(11, view.clientMessageId,
                                                     view.messageId, view.conversationId,
                                                     view.sequence, view.duplicate);
    EXPECT_EQ(11, reply["msgid"].get<int>());
    EXPECT_EQ("acc-001", reply["client_message_id"].get<std::string>());
    EXPECT_EQ(view.messageId, reply["message_id"].get<uint64_t>());
    EXPECT_EQ(view.conversationId, reply["conversation_id"].get<uint64_t>());
    EXPECT_EQ(1u, reply["sequence"].get<uint64_t>());
    EXPECT_FALSE(reply["duplicate"].get<bool>());
    EXPECT_EQ(6u, reply.size());
}

// 故障点 1（spec §4）：accept 回复丢失 → 同 command 重试返回原 identity（duplicate=true）。
TEST_F(MessageAcceptanceApplication, RetrySameCommandReturnsOriginalIdentity)
{
    AppHarness h;
    AcceptResultView first = acceptCommand(&h.app, 1, 1, ChatCommandKind::Direct,
                                           directCommand("acc-retry", 2, "hello"));
    ASSERT_TRUE(first.ok);
    AcceptResultView retry = acceptCommand(&h.app, 1, 1, ChatCommandKind::Direct,
                                           directCommand("acc-retry", 2, "hello"));
    ASSERT_TRUE(retry.ok);
    EXPECT_TRUE(retry.duplicate);
    EXPECT_EQ(first.messageId, retry.messageId);
    EXPECT_EQ(first.conversationId, retry.conversationId);
    EXPECT_EQ(first.sequence, retry.sequence);
    EXPECT_EQ(first.clientMessageId, retry.clientMessageId);
}

// 同 key 不同 payload：IdempotencyConflict → 103（不得被当作 duplicate=true）。
TEST_F(MessageAcceptanceApplication, SameKeyDifferentPayloadReturns103)
{
    AppHarness h;
    AcceptResultView first = acceptCommand(&h.app, 1, 1, ChatCommandKind::Direct,
                                           directCommand("acc-conflict", 2, "hello"));
    ASSERT_TRUE(first.ok);
    AcceptResultView second = acceptCommand(&h.app, 1, 1, ChatCommandKind::Direct,
                                            directCommand("acc-conflict", 2, "different"));
    ASSERT_FALSE(second.ok);
    EXPECT_EQ(kErrnoIdempotencyConflict, second.errnoCode);
}

TEST_F(MessageAcceptanceApplication, AcceptDirectToMissingUserReturns106)
{
    AppHarness h;
    AcceptResultView view = acceptCommand(&h.app, 1, 1, ChatCommandKind::Direct,
                                          directCommand("acc-missing", 99999999, "hello"));
    ASSERT_FALSE(view.ok);
    EXPECT_EQ(kErrnoNotFound, view.errnoCode);
}

TEST_F(MessageAcceptanceApplication, AcceptGroupByMemberReturnsAccepted)
{
    AppHarness h;
    AcceptResultView view = acceptCommand(&h.app, 1, 1, ChatCommandKind::Group,
                                          groupCommand("acc-g7", 7, "hi all"));
    ASSERT_TRUE(view.ok);
    EXPECT_FALSE(view.duplicate);
    EXPECT_GT(view.messageId, 0u);
    EXPECT_EQ(1u, view.sequence);  // 全新群对话首条
}

// B-18 收紧：非成员发送群聊 → 101（NotConversationMember），不再 errno=0。
TEST_F(MessageAcceptanceApplication, AcceptGroupByNonMemberReturns101)
{
    AppHarness h;
    AcceptResultView view = acceptCommand(&h.app, 1, 1, ChatCommandKind::Group,
                                          groupCommand("acc-g8", 8, "hi all"));
    ASSERT_FALSE(view.ok);
    EXPECT_EQ(kErrnoNotConversationMember, view.errnoCode);
}

TEST_F(MessageAcceptanceApplication, AcceptMissingGroupReturns106)
{
    AppHarness h;
    AcceptResultView view = acceptCommand(&h.app, 1, 1, ChatCommandKind::Group,
                                          groupCommand("acc-gm", 99999999, "hi all"));
    ASSERT_FALSE(view.ok);
    EXPECT_EQ(kErrnoNotFound, view.errnoCode);
}

// 超 fan-out cap（100，P3-04 冻结）：整体拒绝 → 102，同 key 重试返回同一错误。
TEST_F(MessageAcceptanceApplication, AcceptGroupOverFanOutCapReturns102)
{
    AppHarness h;
    AcceptResultView view = acceptCommand(&h.app, 1, 1, ChatCommandKind::Group,
                                          groupCommand("acc-g700", 700, "hi all"));
    ASSERT_FALSE(view.ok);
    EXPECT_EQ(kErrnoTooManyRecipients, view.errnoCode);
    AcceptResultView retry = acceptCommand(&h.app, 1, 1, ChatCommandKind::Group,
                                           groupCommand("acc-g700", 700, "hi all"));
    EXPECT_EQ(kErrnoTooManyRecipients, retry.errnoCode);
}

// B-19 收紧：成员查询失败 → 104（DependencyBusy），不再无条件 errno=0。
TEST_F(MessageAcceptanceApplication, GroupMembershipQueryFailureReturns104)
{
    MySQLUserRepository users(pool());
    MySQLFriendRepository friends(pool());
    FailingGroupRepository groups;
    MySQLMessageRepository messages(pool());
    ChatApplication app(&users, &friends, &groups, &messages);
    AcceptResultView view = acceptCommand(&app, 1, 1, ChatCommandKind::Group,
                                          groupCommand("acc-104", 7, "hi all"));
    ASSERT_FALSE(view.ok);
    EXPECT_EQ(kErrnoDependencyBusy, view.errnoCode);
}

// legacy 命令走同一 ledger：identity 为 legacy:<uid>:<计数>，计数递增（spec §5.1）。
TEST_F(MessageAcceptanceApplication, LegacyAcceptGeneratesIdentityAndCounts)
{
    AppHarness h;
    uint64_t before = legacyModeCount();
    AcceptResultView view = acceptCommand(&h.app, 1, 1, ChatCommandKind::Direct,
                                          directCommand("", 3, "hi", true));
    ASSERT_TRUE(view.ok);
    EXPECT_EQ(before + 1, legacyModeCount());
    EXPECT_EQ(1u, view.sequence);  // 全新 (1,3) 对话首条
    const std::string prefix = "legacy:1:";
    EXPECT_EQ(0, view.clientMessageId.compare(0, prefix.size(), prefix));
}

// legacy identity 跨命令不稳定：重试不享幂等保证（能力差异），两次 accept 两个身份。
TEST_F(MessageAcceptanceApplication, LegacyAcceptNotIdempotentAcrossRetries)
{
    AppHarness h;
    uint64_t before = legacyModeCount();
    AcceptResultView first = acceptCommand(&h.app, 1, 1, ChatCommandKind::Direct,
                                           directCommand("", 3, "hi", true));
    AcceptResultView second = acceptCommand(&h.app, 1, 1, ChatCommandKind::Direct,
                                            directCommand("", 3, "hi", true));
    ASSERT_TRUE(first.ok);
    ASSERT_TRUE(second.ok);
    EXPECT_EQ(before + 2, legacyModeCount());
    EXPECT_NE(first.messageId, second.messageId);
    EXPECT_NE(first.clientMessageId, second.clientMessageId);
    EXPECT_EQ(first.sequence + 1, second.sequence);  // 同 (1,3) 对话连续两条
}
