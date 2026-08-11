// P3-04 MySQL MessageStore adapter 集成契约（真实 chat_p304 库，不 skip）：
// 契约双跑、每 SQL 步骤故障注入无部分状态、fan-out cap（100，任务卡 RED 节冻结）、
// FK 1452 → NotFound、错误码映射。
// 用例依赖声明顺序：MySqlAdapterSatisfiesContract 最先（需要全新库的绝对 sequence
// 断言）；其余用例只用自隔离 key/对话对断言，无全局计数（不依赖顺序）。
// 串行约束：本文件与 MessageAcceptanceConcurrencyTest.cpp 共享 chat_p304 库
// （SetUpTestSuite 重建）与单例连接池，两测试二进制必须串行执行（CTest 默认
// 串行满足，勿以 --parallel 混跑两个二进制）。

#include "MySqlTestFixture.hpp"

#include "app/MySQLMessageStore.hpp"
#include "app/ReliableMessaging.hpp"

#include "FakeClock.hpp"
#include "RecordingDeliverySink.hpp"
#include "ReliableMessagingContract.hpp"

#include "db/SchemaMigration.hpp"

#include <gtest/gtest.h>
#include <mysql/mysql.h>

#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

const char* kTestDb = "chat_p304";
const UserId kAlice{1};
const UserId kBob{2};
const UserId kCarol{3};
const uint64_t kLeaseMs = 1000;
const int64_t kNow = 1000000;
const uint64_t kCap = 100;  // 与 docs/tasks/P3-04.md RED 节冻结值一致

std::string repoRoot()
{
    std::string file(__FILE__);
    size_t pos = file.find("tests/unit/");
    if (pos == std::string::npos) {
        return "";
    }
    return file.substr(0, pos);
}

std::string migrationsDir()
{
    return repoRoot() + "sql/migrations";
}

void resetDb()
{
    MySQL admin;
    ASSERT_TRUE(admin.connect("127.0.0.1", "root", MySqlTestFixture::password(), "", 3306));
    ASSERT_TRUE(admin.update("DROP DATABASE IF EXISTS chat_p304"));
    ASSERT_TRUE(admin.update("CREATE DATABASE chat_p304 DEFAULT CHARSET utf8"));
    schema_migration::Migrator migrator("127.0.0.1", "root", MySqlTestFixture::password(),
                                        kTestDb, 3306);
    schema_migration::MigrateResult r = migrator.migrateTo(migrationsDir(), "", 30);
    ASSERT_TRUE(r.ok) << r.error;
}

// 显式 id：契约/并发用例引用固定 UserId（FK 需要 User 行存在）。
void seedUsers()
{
    MySQL conn;
    ASSERT_TRUE(conn.connect("127.0.0.1", "root", MySqlTestFixture::password(), kTestDb, 3306));
    ASSERT_TRUE(conn.update(
        "INSERT INTO User(id, name, password) VALUES (1,'u1','x'),(2,'u2','x'),(3,'u3','x')"));
    for (int i = 4; i <= 120; ++i) {
        std::string sql = "INSERT INTO User(id, name, password) VALUES (" + std::to_string(i) +
                          ", 'u" + std::to_string(i) + "', 'x')";
        ASSERT_TRUE(conn.update(sql));
    }
    // 群 id：契约用 7，随机序列契约用 1..3，cap 测试用 700。
    ASSERT_TRUE(conn.update(
        "INSERT INTO AllGroup(id, groupname) VALUES (1,'g1'),(2,'g2'),(3,'g3'),(7,'g7'),(700,'g700')"));
}

// 单例连接池：必须在 resetDb 之后首次初始化（池连接持有 DATABASE 句柄，
// 库被 drop 后旧连接会 1049）。
ConnectionPool& pool()
{
    static ConnectionPool* instance = [] {
        ConnectionPool* p = &ConnectionPool::getInstance();
        p->init("127.0.0.1", "root", MySqlTestFixture::password(), kTestDb, 3306, 40);
        return p;
    }();
    return *instance;
}

// 每 SQL 步骤后注入失败：hook 抛异常模拟该步骤之后的故障。
// fault 注入是 adapter 构造参数（adapter 内部 seam，不进 MessageStore/
// ReliableMessaging 公共接口——计划 §10 停止条件）。
class ThrowAtStepHook : public MySQLMessageStore::FaultHook {
public:
    explicit ThrowAtStepHook(MySQLMessageStore::Step step) : step_(step) {}
    void onStep(MySQLMessageStore::Step step) override
    {
        if (step == step_) {
            throw std::runtime_error("injected fault after sql step");
        }
    }

private:
    MySQLMessageStore::Step step_;
};

// COMMIT 语句失败注入：跳过真实 COMMIT、模拟其失败（如 1205 lock wait
// timeout），走 commit() 的显式回滚路径（与 onStep(Commit) 的"已提交但确认
// 未达"语义区分）。
class FailCommitHook : public MySQLMessageStore::FaultHook {
public:
    void onStep(MySQLMessageStore::Step) override {}
    bool failCommit() override { return true; }
};

long long countQuery(MySQL& conn, const std::string& sql)
{
    MYSQL_RES* res = conn.query(sql);
    if (!res) {
        return -1;
    }
    MYSQL_ROW row = mysql_fetch_row(res);
    long long n = (row && row[0]) ? atoll(row[0]) : -1;
    mysql_free_result(res);
    return n;
}

SendMessageCommand directCommand(const std::string& key, UserId recipient)
{
    SendMessageCommand cmd;
    cmd.clientMessageId = ClientMessageId(key);
    cmd.kind = SendMessageCommand::Kind::Direct;
    cmd.directRecipient = recipient;
    cmd.content = "payload";
    return cmd;
}

// 随机序列契约在共享库上重跑前清空可靠消息表（ChatMessage 级联删除
// MessageDelivery/OutboxEvent；再删链接表与 Conversation），使 attempt/sequence
// 不变量从干净状态开始（与 InMemory 版每测试全新 store 语义对齐）。
void clearReliableTables()
{
    MySQL conn;
    ASSERT_TRUE(conn.connect("127.0.0.1", "root", MySqlTestFixture::password(), kTestDb, 3306));
    ASSERT_TRUE(conn.update("DELETE FROM ChatMessage"));
    ASSERT_TRUE(conn.update("DELETE FROM DirectConversation"));
    ASSERT_TRUE(conn.update("DELETE FROM GroupConversation"));
    ASSERT_TRUE(conn.update("DELETE FROM Conversation"));
}

class MySqlReliableMessagingDb : public ::testing::Test {
protected:
    static void SetUpTestSuite()
    {
        resetDb();  // 连接池初始化前重建库
        seedUsers();
        (void)pool();
    }
};

} // namespace

TEST_F(MySqlReliableMessagingDb, MySqlAdapterSatisfiesContract)
{
    FakeClock clock;
    clock.set(kNow);
    MySQLMessageStore store(pool(), kCap);
    RecordingDeliverySink sink;
    ReliableMessaging rm(store, sink, clock, kLeaseMs);
    runReliableMessagingContract(rm, clock, sink);
}

TEST_F(MySqlReliableMessagingDb, MySqlAdapterSatisfiesRandomOpsContract)
{
    clearReliableTables();  // 契约用例已遗留 attempt>0 的 Delivery，先清空
    FakeClock clock;
    clock.set(kNow);
    MySQLMessageStore store(pool(), kCap);
    RecordingDeliverySink sink;
    ReliableMessaging rm(store, sink, clock, kLeaseMs);
    runReliableMessagingRandomOps(rm, clock, sink);
}

TEST_F(MySqlReliableMessagingDb, FaultAfterEverySqlStepLeavesNoPartialState)
{
    const SessionIdentity sender{UserId{30}, 1};  // 专属对话 (30,31)，与其它用例隔离
    const UserId recipient{31};

    // --- 全新对话模式（9 个步骤全部可命中）；Commit 必须最后（会真实提交）---
    const MySQLMessageStore::Step freshSteps[] = {
        MySQLMessageStore::Step::FindConversation,
        MySQLMessageStore::Step::CreateConversation,
        MySQLMessageStore::Step::CreateConversationLink,
        MySQLMessageStore::Step::LockConversation,
        MySQLMessageStore::Step::AdvanceSequence,
        MySQLMessageStore::Step::InsertMessage,
        MySQLMessageStore::Step::InsertOutboxEvent,
        MySQLMessageStore::Step::InsertDelivery,
        MySQLMessageStore::Step::Commit,
    };
    const size_t kFreshCount = sizeof(freshSteps) / sizeof(freshSteps[0]);

    for (size_t i = 0; i < kFreshCount; ++i) {
        const std::string key = "fault-fresh-" + std::to_string(i);
        ThrowAtStepHook hook(freshSteps[i]);
        MySQLMessageStore faultStore(pool(), kCap, &hook);
        FakeClock clock;
        clock.set(kNow);
        RecordingDeliverySink sink;
        ReliableMessaging rm(faultStore, sink, clock, kLeaseMs);

        bool threw = false;
        try {
            rm.accept(sender, directCommand(key, recipient));
        } catch (const std::runtime_error&) {
            threw = true;
        }
        EXPECT_TRUE(threw) << "fresh mode step " << i << " did not fail";

        MySQL conn;
        ASSERT_TRUE(conn.connect("127.0.0.1", "root", MySqlTestFixture::password(), kTestDb, 3306));
        const bool isCommit = (i + 1 == kFreshCount);
        if (!isCommit) {
            EXPECT_EQ(0, countQuery(conn, "SELECT COUNT(*) FROM ChatMessage WHERE sender_id=30 "
                                          "AND client_message_id='" + key + "'"))
                << "fresh step " << i << " left a ChatMessage";
            EXPECT_EQ(0, countQuery(conn, "SELECT COUNT(*) FROM MessageDelivery d JOIN ChatMessage m "
                                          "ON m.id=d.message_id WHERE m.sender_id=30 "
                                          "AND m.client_message_id='" + key + "'"))
                << "fresh step " << i << " left a MessageDelivery";
            EXPECT_EQ(0, countQuery(conn, "SELECT COUNT(*) FROM OutboxEvent o JOIN ChatMessage m "
                                          "ON m.id=o.aggregate_message_id WHERE m.sender_id=30 "
                                          "AND m.client_message_id='" + key + "'"))
                << "fresh step " << i << " left an OutboxEvent";
            EXPECT_EQ(0, countQuery(conn, "SELECT COUNT(*) FROM DirectConversation "
                                          "WHERE user_low_id=30 AND user_high_id=31"))
                << "fresh step " << i << " left a Conversation";
        } else {
            // Commit 后故障 = "事务已提交但确认未达"：行存在，同 key 重试返回原结果。
            EXPECT_EQ(1, countQuery(conn, "SELECT COUNT(*) FROM ChatMessage WHERE sender_id=30 "
                                          "AND client_message_id='" + key + "'"));
            EXPECT_EQ(1, countQuery(conn, "SELECT COUNT(*) FROM MessageDelivery d JOIN ChatMessage m "
                                          "ON m.id=d.message_id WHERE m.sender_id=30 "
                                          "AND m.client_message_id='" + key + "'"));
            EXPECT_EQ(1, countQuery(conn, "SELECT COUNT(*) FROM OutboxEvent o JOIN ChatMessage m "
                                          "ON m.id=o.aggregate_message_id WHERE m.sender_id=30 "
                                          "AND m.client_message_id='" + key + "'"));
            EXPECT_EQ(1, countQuery(conn, "SELECT COUNT(*) FROM DirectConversation "
                                          "WHERE user_low_id=30 AND user_high_id=31"));
            EXPECT_EQ(1, countQuery(conn, "SELECT c.next_sequence FROM Conversation c JOIN "
                                          "DirectConversation d ON d.conversation_id=c.id "
                                          "WHERE d.user_low_id=30 AND d.user_high_id=31"));

            FakeClock clock2;
            clock2.set(kNow);
            MySQLMessageStore plainStore(pool(), kCap);
            RecordingDeliverySink sink2;
            ReliableMessaging rm2(plainStore, sink2, clock2, kLeaseMs);
            AcceptOutcome retry = rm2.accept(sender, directCommand(key, recipient));
            EXPECT_TRUE(retry.ok);
            EXPECT_TRUE(retry.duplicate);
            EXPECT_NE(0u, retry.messageId.value);
            EXPECT_EQ(1u, retry.sequence.value);
        }
    }

    // --- 既有对话模式（对话已由 Commit 步提交）：CreateConversation*/Recover* 不触发 ---
    const MySQLMessageStore::Step existingSteps[] = {
        MySQLMessageStore::Step::FindConversation,
        MySQLMessageStore::Step::LockConversation,
        MySQLMessageStore::Step::AdvanceSequence,
        MySQLMessageStore::Step::InsertMessage,
        MySQLMessageStore::Step::InsertOutboxEvent,
        MySQLMessageStore::Step::InsertDelivery,
        MySQLMessageStore::Step::Commit,
    };
    const size_t kExistingCount = sizeof(existingSteps) / sizeof(existingSteps[0]);

    for (size_t i = 0; i < kExistingCount; ++i) {
        const std::string key = "fault-exist-" + std::to_string(i);
        ThrowAtStepHook hook(existingSteps[i]);
        MySQLMessageStore faultStore(pool(), kCap, &hook);
        FakeClock clock;
        clock.set(kNow);
        RecordingDeliverySink sink;
        ReliableMessaging rm(faultStore, sink, clock, kLeaseMs);

        bool threw = false;
        try {
            rm.accept(sender, directCommand(key, recipient));
        } catch (const std::runtime_error&) {
            threw = true;
        }
        EXPECT_TRUE(threw) << "existing mode step " << i << " did not fail";

        MySQL conn;
        ASSERT_TRUE(conn.connect("127.0.0.1", "root", MySqlTestFixture::password(), kTestDb, 3306));
        const bool isCommit = (i + 1 == kExistingCount);
        if (!isCommit) {
            EXPECT_EQ(0, countQuery(conn, "SELECT COUNT(*) FROM ChatMessage WHERE sender_id=30 "
                                          "AND client_message_id='" + key + "'"))
                << "existing step " << i << " left a ChatMessage";
            EXPECT_EQ(1, countQuery(conn, "SELECT c.next_sequence FROM Conversation c JOIN "
                                          "DirectConversation d ON d.conversation_id=c.id "
                                          "WHERE d.user_low_id=30 AND d.user_high_id=31"))
                << "existing step " << i << " advanced next_sequence";
        } else {
            EXPECT_EQ(1, countQuery(conn, "SELECT COUNT(*) FROM ChatMessage WHERE sender_id=30 "
                                          "AND client_message_id='" + key + "'"));
            EXPECT_EQ(1, countQuery(conn, "SELECT COUNT(*) FROM MessageDelivery d JOIN ChatMessage m "
                                          "ON m.id=d.message_id WHERE m.sender_id=30 "
                                          "AND m.client_message_id='" + key + "'"));
            EXPECT_EQ(1, countQuery(conn, "SELECT COUNT(*) FROM OutboxEvent o JOIN ChatMessage m "
                                          "ON m.id=o.aggregate_message_id WHERE m.sender_id=30 "
                                          "AND m.client_message_id='" + key + "'"));
            EXPECT_EQ(2, countQuery(conn, "SELECT c.next_sequence FROM Conversation c JOIN "
                                          "DirectConversation d ON d.conversation_id=c.id "
                                          "WHERE d.user_low_id=30 AND d.user_high_id=31"));

            FakeClock clock2;
            clock2.set(kNow);
            MySQLMessageStore plainStore(pool(), kCap);
            RecordingDeliverySink sink2;
            ReliableMessaging rm2(plainStore, sink2, clock2, kLeaseMs);
            AcceptOutcome retry = rm2.accept(sender, directCommand(key, recipient));
            EXPECT_TRUE(retry.ok);
            EXPECT_TRUE(retry.duplicate);
            EXPECT_NE(0u, retry.messageId.value);
            EXPECT_EQ(2u, retry.sequence.value);
        }
    }
}

TEST_F(MySqlReliableMessagingDb, CommitFailureRollsBackWithoutHangingTransaction)
{
    // 专属对话 (50,51)，与其它用例隔离。
    const SessionIdentity sender{UserId{50}, 1};
    const UserId recipient{51};

    FailCommitHook hook;
    MySQLMessageStore faultStore(pool(), kCap, &hook);
    FakeClock clock;
    clock.set(kNow);
    RecordingDeliverySink sink;
    ReliableMessaging rm(faultStore, sink, clock, kLeaseMs);

    const std::string key1 = "commit-fail";
    bool threw = false;
    try {
        rm.accept(sender, directCommand(key1, recipient));
    } catch (const std::runtime_error&) {
        threw = true;
    }
    ASSERT_TRUE(threw) << "commit failure must fail the accept";

    // 无部分数据：失败 accept 的一切写入已回滚。
    MySQL conn;
    ASSERT_TRUE(conn.connect("127.0.0.1", "root", MySqlTestFixture::password(), kTestDb, 3306));
    EXPECT_EQ(0, countQuery(conn, "SELECT COUNT(*) FROM ChatMessage WHERE sender_id=50 "
                                  "AND client_message_id='" + key1 + "'"));
    EXPECT_EQ(0, countQuery(conn, "SELECT COUNT(*) FROM MessageDelivery d JOIN ChatMessage m "
                                  "ON m.id=d.message_id WHERE m.sender_id=50 "
                                  "AND m.client_message_id='" + key1 + "'"));
    EXPECT_EQ(0, countQuery(conn, "SELECT COUNT(*) FROM OutboxEvent o JOIN ChatMessage m "
                                  "ON m.id=o.aggregate_message_id WHERE m.sender_id=50 "
                                  "AND m.client_message_id='" + key1 + "'"));
    EXPECT_EQ(0, countQuery(conn, "SELECT COUNT(*) FROM DirectConversation "
                                  "WHERE user_low_id=50 AND user_high_id=51"));
    // 无悬挂事务：归还池的连接不得带未提交事务（否则下一次 START TRANSACTION
    // 隐式提交会复活失败 accept 的部分数据）。
    EXPECT_EQ(0, countQuery(conn, "SELECT COUNT(*) FROM information_schema.innodb_trx"))
        << "pool holds a connection with an open transaction";

    // 后续事务正常：同一线程下一次 accept 成功且真正提交。
    MySQLMessageStore plainStore(pool(), kCap);
    FakeClock clock2;
    clock2.set(kNow);
    RecordingDeliverySink sink2;
    ReliableMessaging rm2(plainStore, sink2, clock2, kLeaseMs);
    AcceptOutcome out = rm2.accept(sender, directCommand("commit-fail-2", recipient));
    ASSERT_TRUE(out.ok) << "subsequent accept must succeed after a commit failure";
    EXPECT_EQ(1, countQuery(conn, "SELECT COUNT(*) FROM ChatMessage WHERE sender_id=50 "
                                  "AND client_message_id='commit-fail-2'"));
    EXPECT_EQ(0, countQuery(conn, "SELECT COUNT(*) FROM ChatMessage WHERE sender_id=50 "
                                  "AND client_message_id='" + key1 + "'"))
        << "failed accept data must not be resurrected";
    EXPECT_EQ(0, countQuery(conn, "SELECT COUNT(*) FROM information_schema.innodb_trx"));
}

TEST_F(MySqlReliableMessagingDb, EmptyGroupSnapshotCommitsBeforeAcceptReturns)
{
    // 0 成员快照（空群）：accept 返回 ok 前事务必须已提交（跨连接立即可见）。
    const SessionIdentity alice{kAlice, 1};
    SendMessageCommand cmd;
    cmd.clientMessageId = ClientMessageId("empty-group");
    cmd.kind = SendMessageCommand::Kind::Group;
    cmd.groupId = GroupId{7};  // 已种子；0 成员快照
    cmd.content = "no members";

    MySQLMessageStore store(pool(), kCap);
    FakeClock clock;
    clock.set(kNow);
    RecordingDeliverySink sink;
    ReliableMessaging rm(store, sink, clock, kLeaseMs);

    AcceptOutcome out = rm.accept(alice, cmd);
    ASSERT_TRUE(out.ok) << "empty snapshot must be accepted";

    MySQL conn;
    ASSERT_TRUE(conn.connect("127.0.0.1", "root", MySqlTestFixture::password(), kTestDb, 3306));
    EXPECT_EQ(1, countQuery(conn, "SELECT COUNT(*) FROM ChatMessage WHERE sender_id=1 "
                                  "AND client_message_id='empty-group'"));
    EXPECT_EQ(0, countQuery(conn, "SELECT COUNT(*) FROM MessageDelivery d JOIN ChatMessage m "
                                  "ON m.id=d.message_id WHERE m.sender_id=1 "
                                  "AND m.client_message_id='empty-group'"));
    EXPECT_EQ(1, countQuery(conn, "SELECT COUNT(*) FROM OutboxEvent o JOIN ChatMessage m "
                                  "ON m.id=o.aggregate_message_id WHERE m.sender_id=1 "
                                  "AND m.client_message_id='empty-group'"));
    // 无悬挂事务：0 成员 accept 不得遗留开放事务到下一操作边界/线程退出。
    EXPECT_EQ(0, countQuery(conn, "SELECT COUNT(*) FROM information_schema.innodb_trx"))
        << "empty snapshot accept left an open transaction";
}

TEST_F(MySqlReliableMessagingDb, DirectAcceptCommitsBeforeReturn)
{
    // direct 场景对照：accept 返回 ok 时事务同样必须已提交（无悬挂）。
    const SessionIdentity alice{kAlice, 1};
    MySQLMessageStore store(pool(), kCap);
    FakeClock clock;
    clock.set(kNow);
    RecordingDeliverySink sink;
    ReliableMessaging rm(store, sink, clock, kLeaseMs);

    AcceptOutcome out = rm.accept(alice, directCommand("direct-committed", kBob));
    ASSERT_TRUE(out.ok);

    MySQL conn;
    ASSERT_TRUE(conn.connect("127.0.0.1", "root", MySqlTestFixture::password(), kTestDb, 3306));
    EXPECT_EQ(1, countQuery(conn, "SELECT COUNT(*) FROM ChatMessage WHERE sender_id=1 "
                                  "AND client_message_id='direct-committed'"));
    EXPECT_EQ(1, countQuery(conn, "SELECT COUNT(*) FROM MessageDelivery d JOIN ChatMessage m "
                                  "ON m.id=d.message_id WHERE m.sender_id=1 "
                                  "AND m.client_message_id='direct-committed' AND d.recipient_id=2"));
    EXPECT_EQ(0, countQuery(conn, "SELECT COUNT(*) FROM information_schema.innodb_trx"))
        << "direct accept left an open transaction";
}

TEST_F(MySqlReliableMessagingDb, SenderInMembersGetsOwnDeliveryRow)
{
    // 群快照含 sender 时现状：给 sender 插 Delivery 行（自投递；P3-06 决定是否
    // 过滤——本用例锁定现状行为）。
    const SessionIdentity alice{kAlice, 1};
    SendMessageCommand cmd;
    cmd.clientMessageId = ClientMessageId("self-delivery");
    cmd.kind = SendMessageCommand::Kind::Group;
    cmd.groupId = GroupId{7};
    cmd.members.push_back(kAlice);  // sender 在 members 中
    cmd.members.push_back(kBob);
    cmd.content = "includes sender";

    MySQLMessageStore store(pool(), kCap);
    FakeClock clock;
    clock.set(kNow);
    RecordingDeliverySink sink;
    ReliableMessaging rm(store, sink, clock, kLeaseMs);

    AcceptOutcome out = rm.accept(alice, cmd);
    ASSERT_TRUE(out.ok);

    MySQL conn;
    ASSERT_TRUE(conn.connect("127.0.0.1", "root", MySqlTestFixture::password(), kTestDb, 3306));
    EXPECT_EQ(2, countQuery(conn, "SELECT COUNT(*) FROM MessageDelivery d JOIN ChatMessage m "
                                  "ON m.id=d.message_id WHERE m.sender_id=1 "
                                  "AND m.client_message_id='self-delivery'"));
    EXPECT_EQ(1, countQuery(conn, "SELECT COUNT(*) FROM MessageDelivery d JOIN ChatMessage m "
                                  "ON m.id=d.message_id WHERE m.sender_id=1 "
                                  "AND m.client_message_id='self-delivery' AND d.recipient_id=1"));
}

TEST_F(MySqlReliableMessagingDb, TooManyRecipientsRejectedAndRetriedStably)
{
    const SessionIdentity alice{kAlice, 1};
    SendMessageCommand cmd;
    cmd.clientMessageId = ClientMessageId("cap-1");
    cmd.kind = SendMessageCommand::Kind::Group;
    cmd.groupId = GroupId{700};
    for (int i = 1; i <= 101; ++i) {
        cmd.members.push_back(UserId{static_cast<uint64_t>(i)});
    }
    cmd.content = "big fanout";
    ASSERT_EQ(101u, cmd.members.size());

    MySQLMessageStore store(pool(), kCap);
    FakeClock clock;
    clock.set(kNow);
    RecordingDeliverySink sink;
    ReliableMessaging rm(store, sink, clock, kLeaseMs);

    // 同 key 重试返回同一错误，不得悄悄当重复成功。
    for (int attempt = 0; attempt < 2; ++attempt) {
        bool threw = false;
        try {
            rm.accept(alice, cmd);
        } catch (const MessageStoreError& e) {
            threw = true;
            EXPECT_EQ(StoreErrorKind::TooManyRecipients, e.kind()) << "attempt " << attempt;
        }
        EXPECT_TRUE(threw) << "attempt " << attempt << " must not silently succeed";
    }

    MySQL conn;
    ASSERT_TRUE(conn.connect("127.0.0.1", "root", MySqlTestFixture::password(), kTestDb, 3306));
    EXPECT_EQ(0, countQuery(conn, "SELECT COUNT(*) FROM ChatMessage WHERE sender_id=1 "
                                  "AND client_message_id='cap-1'"));
    EXPECT_EQ(0, countQuery(conn, "SELECT COUNT(*) FROM MessageDelivery d JOIN ChatMessage m "
                                  "ON m.id=d.message_id WHERE m.sender_id=1 "
                                  "AND m.client_message_id='cap-1'"));
    EXPECT_EQ(0, countQuery(conn, "SELECT COUNT(*) FROM OutboxEvent o JOIN ChatMessage m "
                                  "ON m.id=o.aggregate_message_id WHERE m.sender_id=1 "
                                  "AND m.client_message_id='cap-1'"));
    EXPECT_EQ(0, countQuery(conn, "SELECT COUNT(*) FROM GroupConversation WHERE group_id=700"));
}

TEST_F(MySqlReliableMessagingDb, CapBoundaryAcceptsExactlyCapMembers)
{
    const SessionIdentity alice{kAlice, 1};
    SendMessageCommand cmd;
    cmd.clientMessageId = ClientMessageId("cap-100");
    cmd.kind = SendMessageCommand::Kind::Group;
    cmd.groupId = GroupId{700};
    for (int i = 1; i <= 100; ++i) {
        cmd.members.push_back(UserId{static_cast<uint64_t>(i)});
    }
    cmd.content = "boundary";

    MySQLMessageStore store(pool(), kCap);
    FakeClock clock;
    clock.set(kNow);
    RecordingDeliverySink sink;
    ReliableMessaging rm(store, sink, clock, kLeaseMs);

    AcceptOutcome out = rm.accept(alice, cmd);
    EXPECT_TRUE(out.ok);
    EXPECT_FALSE(out.duplicate);

    MySQL conn;
    ASSERT_TRUE(conn.connect("127.0.0.1", "root", MySqlTestFixture::password(), kTestDb, 3306));
    EXPECT_EQ(1, countQuery(conn, "SELECT COUNT(*) FROM ChatMessage WHERE sender_id=1 "
                                  "AND client_message_id='cap-100'"));
    EXPECT_EQ(100, countQuery(conn, "SELECT COUNT(*) FROM MessageDelivery d JOIN ChatMessage m "
                                    "ON m.id=d.message_id WHERE m.sender_id=1 "
                                    "AND m.client_message_id='cap-100'"));
    EXPECT_EQ(1, countQuery(conn, "SELECT COUNT(*) FROM GroupConversation WHERE group_id=700"));
}

TEST_F(MySqlReliableMessagingDb, NonexistentGroupMapsToNotFoundAndRollsBack)
{
    const SessionIdentity alice{kAlice, 1};
    SendMessageCommand cmd;
    cmd.clientMessageId = ClientMessageId("nofk-group");
    cmd.kind = SendMessageCommand::Kind::Group;
    cmd.groupId = GroupId{900};  // 未种子 → GroupConversation FK 1452
    cmd.members.push_back(kBob);
    cmd.content = "to nowhere";

    MySQLMessageStore store(pool(), kCap);
    FakeClock clock;
    clock.set(kNow);
    RecordingDeliverySink sink;
    ReliableMessaging rm(store, sink, clock, kLeaseMs);

    bool threw = false;
    try {
        rm.accept(alice, cmd);
    } catch (const MessageStoreError& e) {
        threw = true;
        EXPECT_EQ(StoreErrorKind::NotFound, e.kind());
    }
    EXPECT_TRUE(threw) << "nonexistent group must not silently succeed";

    MySQL conn;
    ASSERT_TRUE(conn.connect("127.0.0.1", "root", MySqlTestFixture::password(), kTestDb, 3306));
    EXPECT_EQ(0, countQuery(conn, "SELECT COUNT(*) FROM ChatMessage WHERE sender_id=1 "
                                  "AND client_message_id='nofk-group'"));
    EXPECT_EQ(0, countQuery(conn, "SELECT COUNT(*) FROM MessageDelivery d JOIN ChatMessage m "
                                  "ON m.id=d.message_id WHERE m.sender_id=1 "
                                  "AND m.client_message_id='nofk-group'"));
    EXPECT_EQ(0, countQuery(conn, "SELECT COUNT(*) FROM OutboxEvent o JOIN ChatMessage m "
                                  "ON m.id=o.aggregate_message_id WHERE m.sender_id=1 "
                                  "AND m.client_message_id='nofk-group'"));
    EXPECT_EQ(0, countQuery(conn, "SELECT COUNT(*) FROM GroupConversation WHERE group_id=900"));
}

TEST_F(MySqlReliableMessagingDb, NonexistentDirectRecipientMapsToNotFoundAndRollsBack)
{
    const SessionIdentity alice{kAlice, 1};
    SendMessageCommand cmd = directCommand("nofk-direct", UserId{999});

    MySQLMessageStore store(pool(), kCap);
    FakeClock clock;
    clock.set(kNow);
    RecordingDeliverySink sink;
    ReliableMessaging rm(store, sink, clock, kLeaseMs);

    bool threw = false;
    try {
        rm.accept(alice, cmd);
    } catch (const MessageStoreError& e) {
        threw = true;
        EXPECT_EQ(StoreErrorKind::NotFound, e.kind());
    }
    EXPECT_TRUE(threw) << "nonexistent recipient must not silently succeed";

    MySQL conn;
    ASSERT_TRUE(conn.connect("127.0.0.1", "root", MySqlTestFixture::password(), kTestDb, 3306));
    EXPECT_EQ(0, countQuery(conn, "SELECT COUNT(*) FROM ChatMessage WHERE sender_id=1 "
                                  "AND client_message_id='nofk-direct'"));
    EXPECT_EQ(0, countQuery(conn, "SELECT COUNT(*) FROM DirectConversation "
                                  "WHERE user_low_id=1 AND user_high_id=999"));
}

TEST_F(MySqlReliableMessagingDb, MapStoreErrorClassifiesMySqlErrors)
{
    EXPECT_EQ(StoreErrorKind::DependencyBusy, mapStoreError(1205));  // lock wait timeout
    EXPECT_EQ(StoreErrorKind::DependencyBusy, mapStoreError(1213));  // deadlock
    EXPECT_EQ(StoreErrorKind::DependencyBusy, mapStoreError(2006));  // server gone
    EXPECT_EQ(StoreErrorKind::NotFound, mapStoreError(1452));        // FK 目标不存在
    EXPECT_EQ(StoreErrorKind::Storage, mapStoreError(1406));         // data too long
    EXPECT_EQ(StoreErrorKind::Storage, mapStoreError(1062));         // dup（期望处先按 1062 处理）
}
