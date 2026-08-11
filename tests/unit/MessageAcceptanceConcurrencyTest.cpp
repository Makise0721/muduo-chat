// P3-04 并发接受：8/32 线程（每线程独立 ReliableMessaging/Clock/Sink，共享
// MySQLMessageStore + 连接池；adapter 事务上下文 thread_local 按线程隔离）。
// 真实 chat_p304 库，不 skip；DependencyBusy 按计划 §2.4 以同 key 重试。
// 每场景使用专属 key/对话对，断言只按自身范围，无跨用例全局计数。
// 串行约束：本文件与 MySQLReliableMessagingTest.cpp 共享 chat_p304 库
// （SetUpTestSuite 重建）与单例连接池，两测试二进制必须串行执行（CTest 默认
// 串行满足，勿以 --parallel 混跑两个二进制）。

#include "MySqlTestFixture.hpp"

#include "app/MySQLMessageStore.hpp"
#include "app/ReliableMessaging.hpp"

#include "FakeClock.hpp"
#include "RecordingDeliverySink.hpp"

#include "db/SchemaMigration.hpp"

#include <gtest/gtest.h>
#include <mysql/mysql.h>

#include <atomic>
#include <cstdlib>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

const char* kTestDb = "chat_p304";
const UserId kAlice{1};
const UserId kBob{2};
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
}

// 单例连接池：必须在 resetDb 之后首次初始化（池连接持有 DATABASE 句柄）。
ConnectionPool& pool()
{
    static ConnectionPool* instance = [] {
        ConnectionPool* p = &ConnectionPool::getInstance();
        p->init("127.0.0.1", "root", MySqlTestFixture::password(), kTestDb, 3306, 40);
        return p;
    }();
    return *instance;
}

struct AcceptResult {
    bool ok = false;
    uint64_t messageId = 0;
    uint64_t conversationId = 0;
    uint64_t sequence = 0;
};

// 每线程独立 ReliableMessaging；DependencyBusy 以同 key 重试（计划 §2.4）。
AcceptResult acceptWithRetry(MySQLMessageStore& store, const SessionIdentity& sender,
                             const SendMessageCommand& cmd)
{
    FakeClock clock;
    clock.set(kNow);
    RecordingDeliverySink sink;
    ReliableMessaging rm(store, sink, clock, kLeaseMs);
    for (int attempt = 0; attempt < 16; ++attempt) {
        try {
            AcceptOutcome out = rm.accept(sender, cmd);
            AcceptResult r;
            r.ok = out.ok;
            r.messageId = out.messageId.value;
            r.conversationId = out.conversationId.value;
            r.sequence = out.sequence.value;
            return r;
        } catch (const MessageStoreError& e) {
            if (e.kind() != StoreErrorKind::DependencyBusy) {
                throw;
            }
        }
    }
    AcceptResult r;  // 16 次依赖忙重试仍失败 → ok=false
    return r;
}

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

class MessageAcceptanceConcurrencyDb : public ::testing::Test {
protected:
    static void SetUpTestSuite()
    {
        resetDb();
        seedUsers();
        (void)pool();
    }
};

// 同 sender+同 key：恰 1 行 Message/Delivery/Outbox，其余线程幂等返回原结果
// （全部 identity 相等；库内唯一键行数=1）。
void runSameKey(int threads)
{
    MySQLMessageStore store(pool(), kCap);
    const SessionIdentity alice{kAlice, 1};
    SendMessageCommand cmd;
    cmd.clientMessageId = ClientMessageId("race-key");
    cmd.kind = SendMessageCommand::Kind::Direct;
    cmd.directRecipient = UserId{40};  // 专属对话 (1,40)
    cmd.content = "same key race";

    std::atomic<int> go{0};
    std::mutex mu;
    std::vector<AcceptResult> results;
    std::vector<std::thread> poolThreads;
    for (int i = 0; i < threads; ++i) {
        poolThreads.emplace_back([&] {
            while (go.load() == 0) {
                std::this_thread::yield();
            }
            AcceptResult r = acceptWithRetry(store, alice, cmd);
            std::lock_guard<std::mutex> lock(mu);
            results.push_back(r);
        });
    }
    go.store(1);
    for (size_t i = 0; i < poolThreads.size(); ++i) {
        poolThreads[i].join();
    }

    ASSERT_EQ(static_cast<size_t>(threads), results.size());
    for (size_t i = 0; i < results.size(); ++i) {
        EXPECT_TRUE(results[i].ok) << "thread " << i << " did not converge";
        EXPECT_NE(0u, results[i].messageId) << "thread " << i;
    }
    for (size_t i = 1; i < results.size(); ++i) {
        EXPECT_EQ(results[0].messageId, results[i].messageId)
            << "racing duplicate must return original message id";
        EXPECT_EQ(results[0].conversationId, results[i].conversationId);
        EXPECT_EQ(results[0].sequence, results[i].sequence);
    }

    MySQL conn;
    ASSERT_TRUE(conn.connect("127.0.0.1", "root", MySqlTestFixture::password(), kTestDb, 3306));
    EXPECT_EQ(1, countQuery(conn, "SELECT COUNT(*) FROM ChatMessage WHERE sender_id=1 "
                                  "AND client_message_id='race-key'"));
    EXPECT_EQ(1, countQuery(conn, "SELECT COUNT(*) FROM MessageDelivery d JOIN ChatMessage m "
                                  "ON m.id=d.message_id WHERE m.sender_id=1 "
                                  "AND m.client_message_id='race-key'"));
    EXPECT_EQ(1, countQuery(conn, "SELECT COUNT(*) FROM OutboxEvent o JOIN ChatMessage m "
                                  "ON m.id=o.aggregate_message_id WHERE m.sender_id=1 "
                                  "AND m.client_message_id='race-key'"));
    EXPECT_EQ(1, countQuery(conn, "SELECT COUNT(*) FROM DirectConversation "
                                  "WHERE user_low_id=1 AND user_high_id=40"));
}

// 同 Conversation 不同 key：FOR UPDATE 串行分配 sequence，严格递增、无重复。
void runSameConversation(int threads)
{
    MySQLMessageStore store(pool(), kCap);
    const SessionIdentity alice{kAlice, 1};

    std::atomic<int> go{0};
    std::mutex mu;
    std::vector<AcceptResult> results;
    std::vector<std::thread> poolThreads;
    for (int i = 0; i < threads; ++i) {
        poolThreads.emplace_back([&, i] {
            while (go.load() == 0) {
                std::this_thread::yield();
            }
            SendMessageCommand cmd;
            cmd.clientMessageId = ClientMessageId("seq-" + std::to_string(i));
            cmd.kind = SendMessageCommand::Kind::Direct;
            cmd.directRecipient = kBob;
            cmd.content = "same conversation";
            AcceptResult r = acceptWithRetry(store, alice, cmd);
            std::lock_guard<std::mutex> lock(mu);
            results.push_back(r);
        });
    }
    go.store(1);
    for (size_t i = 0; i < poolThreads.size(); ++i) {
        poolThreads[i].join();
    }

    ASSERT_EQ(static_cast<size_t>(threads), results.size());
    std::set<uint64_t> sequences;
    for (size_t i = 0; i < results.size(); ++i) {
        EXPECT_TRUE(results[i].ok) << "thread " << i << " did not converge";
        EXPECT_NE(0u, results[i].sequence) << "thread " << i;
        sequences.insert(results[i].sequence);
    }
    for (size_t i = 1; i < results.size(); ++i) {
        EXPECT_EQ(results[0].conversationId, results[i].conversationId)
            << "all threads must resolve the same conversation";
    }
    EXPECT_EQ(static_cast<size_t>(threads), sequences.size()) << "sequences must be unique";
    for (uint64_t i = 1; i <= static_cast<uint64_t>(threads); ++i) {
        EXPECT_TRUE(sequences.count(i)) << "missing sequence " << i;
    }

    MySQL conn;
    ASSERT_TRUE(conn.connect("127.0.0.1", "root", MySqlTestFixture::password(), kTestDb, 3306));
    EXPECT_EQ(threads, countQuery(conn, "SELECT COUNT(*) FROM ChatMessage WHERE conversation_id = "
                                        "(SELECT conversation_id FROM DirectConversation "
                                        "WHERE user_low_id=1 AND user_high_id=2)"));
    EXPECT_EQ(0, countQuery(conn, "SELECT COUNT(*) FROM (SELECT sequence, COUNT(*) c FROM ChatMessage "
                                  "WHERE conversation_id = (SELECT conversation_id FROM "
                                  "DirectConversation WHERE user_low_id=1 AND user_high_id=2) "
                                  "GROUP BY sequence HAVING c > 1) dup"));
}

// 不同 Conversation 并行提交：每线程专属 pair，互不阻塞。
void runDifferentConversations(int threads)
{
    MySQLMessageStore store(pool(), kCap);
    const SessionIdentity alice{kAlice, 1};

    std::atomic<int> go{0};
    std::mutex mu;
    std::vector<AcceptResult> results;
    std::vector<std::thread> poolThreads;
    for (int i = 0; i < threads; ++i) {
        poolThreads.emplace_back([&, i] {
            while (go.load() == 0) {
                std::this_thread::yield();
            }
            SendMessageCommand cmd;
            cmd.clientMessageId = ClientMessageId("par-" + std::to_string(i));
            cmd.kind = SendMessageCommand::Kind::Direct;
            cmd.directRecipient = UserId{static_cast<uint64_t>(3 + i)};  // 专属 pair (1, 3+i)
            cmd.content = "parallel conversations";
            AcceptResult r = acceptWithRetry(store, alice, cmd);
            std::lock_guard<std::mutex> lock(mu);
            results.push_back(r);
        });
    }
    go.store(1);
    for (size_t i = 0; i < poolThreads.size(); ++i) {
        poolThreads[i].join();
    }

    ASSERT_EQ(static_cast<size_t>(threads), results.size());
    std::set<uint64_t> conversations;
    for (size_t i = 0; i < results.size(); ++i) {
        EXPECT_TRUE(results[i].ok) << "thread " << i << " did not converge";
        EXPECT_NE(0u, results[i].conversationId) << "thread " << i;
        EXPECT_EQ(1u, results[i].sequence) << "thread " << i << " must be its own conversation";
        conversations.insert(results[i].conversationId);
    }
    EXPECT_EQ(static_cast<size_t>(threads), conversations.size())
        << "each thread must get its own conversation";

    MySQL conn;
    ASSERT_TRUE(conn.connect("127.0.0.1", "root", MySqlTestFixture::password(), kTestDb, 3306));
    EXPECT_EQ(threads, countQuery(conn, "SELECT COUNT(*) FROM DirectConversation WHERE user_low_id=1 "
                                        "AND user_high_id BETWEEN 3 AND "
                                        + std::to_string(2 + threads)));
}

} // namespace

TEST_F(MessageAcceptanceConcurrencyDb, SameKeyEightThreads)
{
    runSameKey(8);
}

TEST_F(MessageAcceptanceConcurrencyDb, SameKeyThirtyTwoThreads)
{
    runSameKey(32);
}

TEST_F(MessageAcceptanceConcurrencyDb, SameConversationSequencesEightThreads)
{
    runSameConversation(8);
}

TEST_F(MessageAcceptanceConcurrencyDb, SameConversationSequencesThirtyTwoThreads)
{
    runSameConversation(32);
}

TEST_F(MessageAcceptanceConcurrencyDb, DifferentConversationsEightThreads)
{
    runDifferentConversations(8);
}

TEST_F(MessageAcceptanceConcurrencyDb, DifferentConversationsThirtyTwoThreads)
{
    runDifferentConversations(32);
}
