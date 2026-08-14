// P3-10 旧 OfflineMessage 迁移 backfill 真实 MySQL 契约（docs/tasks/P3-10.md
// RED 计划 1/2/4/6/7/8，spec message-reliability.md §5.2）。全部在独立库
// chat_p310_backfill 执行，不 skip；断言失败即失败。
// 现状无 db/OfflineBackfill.hpp（OfflineBackfill runner / checkpoint /
// quarantine / cutover 开关均不存在）→ 本文件引用尚不存在的公开接口，编译失败
// 即合法 RED（docs/tasks/P3-10.md RED 计划：现状接口不存在而 compile 失败）。
// 用例：
// - BackfillMigratesDirectAndGroupRows：direct/group 行 → ledger 产生
//   Message+Delivery，client_message_id = legacy:<offline_id>，content 按旧
//   payload 解析原样保留；group 每行（旧表 per-recipient 形状）各成独立
//   Message+Delivery（recipient = userid）。
// - DuplicateRowsIdempotentByLegacyId：同 payload 重复行 → 两条独立 Message（旧表
//   不去重）且幂等键互不冲突；重复运行不增 Message；已存在同键 → 跳过并返回原
//   MessageId（legacy:<offline_id> 幂等键，spec §5.1）。
// - BadJsonRowsGoToQuarantine：坏 JSON/截断 JSON/乱码/非 UTF-8（MySQL 8.0 写时
//   净化后的非 JSON 值）/缺字段/未知 msgid/超界（>500 字节）→ quarantine 可查询、
//   绝不静默删、不计入 migrated。注：MySQL 8.0 utf8 列写时校验 UTF-8，非 UTF-8
//   只能以"净化后非 JSON"近似构造；"超界"以 >500 字节但 ≤500 字符的合法 JSON
//   （240 × 'é'，utf8 列按字符计数）构造，已实测字节精确往返。
// - SourceCountConservation：源行 = migrated + quarantine（含中断重跑后守恒）。
// - EmptyDatabaseBackfillIsNoop：空库 dry-run/实跑均 0 行、幂等。
// - DryRunDoesNotMutate：dry-run 只报告计数/hash 不改数据。
// - CheckpointResumeAfterInterruption：批次中间断 → 重入从 checkpoint 继续、
//   不重复迁移、原 MessageId 不变。
// 串行约束：本二进制独占 chat_p310_backfill 库与单例连接池（SetUpTestSuite 重建），
// 不与其它测试二进制混跑（CTest 默认串行满足，勿以 --parallel 混跑）。

#include "MySqlTestFixture.hpp"

#include "app/MySQLMessageStore.hpp"
#include "app/ReliableMessaging.hpp"

#include "db/OfflineBackfill.hpp"

#include <gtest/gtest.h>
#include <mysql/mysql.h>

#include <cstdint>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

namespace {

const char* kTestDb = "chat_p310_backfill";
const uint64_t kFanOutCap = 100;

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
    ASSERT_TRUE(admin.update("DROP DATABASE IF EXISTS chat_p310_backfill"));
    ASSERT_TRUE(admin.update("CREATE DATABASE chat_p310_backfill DEFAULT CHARSET utf8"));
    schema_migration::Migrator migrator("127.0.0.1", "root", MySqlTestFixture::password(),
                                        kTestDb, 3306);
    schema_migration::MigrateResult r = migrator.migrateTo(migrationsDir(), "", 30);
    ASSERT_TRUE(r.ok) << r.error;
}

void makeConn(MySQL& conn)
{
    ASSERT_TRUE(conn.connect("127.0.0.1", "root", MySqlTestFixture::password(), kTestDb, 3306));
}

// 单例连接池：必须在 resetDb 之后首次初始化（池连接持有 DATABASE 句柄，库被
// drop 后旧连接会 1049）。
ConnectionPool& pool()
{
    static ConnectionPool* instance = [] {
        ConnectionPool* p = &ConnectionPool::getInstance();
        p->init("127.0.0.1", "root", MySqlTestFixture::password(), kTestDb, 3306, 8);
        return p;
    }();
    return *instance;
}

void seedBase()
{
    MySQL conn;
    makeConn(conn);
    ASSERT_TRUE(conn.update(
        "INSERT INTO User(id, name, password) VALUES (1,'u1','x'),(2,'u2','x'),"
        "(3,'u3','x'),(4,'u4','x')"));
    ASSERT_TRUE(conn.update("INSERT INTO AllGroup(id, groupname) VALUES (1,'g1')"));
}

// 用例间隔离：清空可靠消息六表与旧表（ChatMessage 级联 MessageDelivery/OutboxEvent；
// 再删链接表与 Conversation），并 DROP runner 自建辅助表（checkpoint/quarantine，
// 由下一次 run 重建），使跨用例不互相污染。
void clearTables(MySQL& conn)
{
    ASSERT_TRUE(conn.update("DELETE FROM ChatMessage"));
    ASSERT_TRUE(conn.update("DELETE FROM DirectConversation"));
    ASSERT_TRUE(conn.update("DELETE FROM GroupConversation"));
    ASSERT_TRUE(conn.update("DELETE FROM Conversation"));
    ASSERT_TRUE(conn.update("DELETE FROM OfflineMessage"));
    ASSERT_TRUE(conn.update("DROP TABLE IF EXISTS " + std::string(offline_backfill::kCheckpointTable)));
    ASSERT_TRUE(conn.update("DROP TABLE IF EXISTS " + std::string(offline_backfill::kQuarantineTable)));
}

long long queryInt(MySQL& conn, const std::string& sql)
{
    MYSQL_RES* res = conn.query(sql);
    if (!res) {
        return -1;
    }
    MYSQL_ROW row = mysql_fetch_row(res);
    long long v = (row && row[0]) ? atoll(row[0]) : -1;
    mysql_free_result(res);
    return v;
}

std::set<std::string> columnSet(MySQL& conn, const std::string& sql)
{
    std::set<std::string> out;
    MYSQL_RES* res = conn.query(sql);
    if (!res) {
        return out;
    }
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res))) {
        if (row[0]) {
            out.insert(row[0]);
        }
    }
    mysql_free_result(res);
    return out;
}

// ChatMessage.content 为 MEDIUMBLOB（二进制），按字节取回。
std::string fetchContent(MySQL& conn, const std::string& cmid)
{
    MYSQL_RES* res = conn.query("SELECT content FROM ChatMessage WHERE client_message_id='"
                                + cmid + "'");
    if (!res) {
        return "";
    }
    MYSQL_ROW row = mysql_fetch_row(res);
    unsigned long* lens = mysql_fetch_lengths(res);
    std::string out;
    if (row && row[0] && lens) {
        out.assign(row[0], lens[0]);
    }
    mysql_free_result(res);
    return out;
}

std::string hexEncode(const std::string& data)
{
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(data.size() * 2);
    for (size_t i = 0; i < data.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(data[i]);
        out.push_back(hex[c >> 4]);
        out.push_back(hex[c & 15]);
    }
    return out;
}

// 旧表 payload（按 README.md 消息协议形状）：ONE_CHAT(6) direct / GROUP_CHAT(10)。
// content 为解析后的消息文本；迁移后 ChatMessage.content 应等于该文本原样。
std::string directPayload()
{
    return "{\"msgid\":6,\"id\":1,\"toid\":2,\"content\":\"hello\"}";
}

std::string groupPayload()
{
    return "{\"msgid\":10,\"id\":1,\"groupid\":1,\"content\":\"hi all\"}";
}

// 'é' 的 UTF-8 字节（0xC3A9），n 个。
std::string eTimes(int n)
{
    std::string s;
    for (int i = 0; i < n; ++i) {
        s += char(0xC3);
        s += char(0xA9);
    }
    return s;
}

// 超界行：合法 JSON 但 >500 字节（240 × 'é' = 480 字节 + JSON 外壳 ≈ 520 字节），
// utf8 列按"字符"计数（280 字符 < 500）可存入——旧 bug/旧应用 500 字节上限的
// 超限快照只能以字节超限出现。
std::string oversizedPayload()
{
    return "{\"msgid\":6,\"id\":1,\"toid\":2,\"content\":\"" + eTimes(240) + "\"}";
}

// 非 UTF-8 近似行：MySQL 8.0 utf8 列写时校验并净化非法字节（0xE9E9... 存为
// 空/部分保留），净化结果必非合法 JSON → 仍须 quarantine（绝不静默删）。
// 以 sql_mode='' 会话写入（严格模式直接报 1406）。
void insertRawBytes(MySQL& conn, int64_t id, int64_t userId, const std::string& hex)
{
    ASSERT_TRUE(conn.update("SET SESSION sql_mode=''"));
    ASSERT_TRUE(conn.update("INSERT INTO OfflineMessage(id,userid,message) VALUES("
                            + std::to_string(id) + "," + std::to_string(userId)
                            + ", _binary 0x" + hex + ")"));
}

void insertOffline(MySQL& conn, int64_t id, int64_t userId, const std::string& payload)
{
    ASSERT_TRUE(conn.update("INSERT INTO OfflineMessage(id,userid,message) VALUES("
                            + std::to_string(id) + "," + std::to_string(userId)
                            + ",'" + payload + "')"));
}

// payload 含非 ASCII 字节时经 hex 字面量写入（utf8 列转换校验后字节不变）。
void insertOfflineHex(MySQL& conn, int64_t id, int64_t userId, const std::string& payload)
{
    ASSERT_TRUE(conn.update("INSERT INTO OfflineMessage(id,userid,message) VALUES("
                            + std::to_string(id) + "," + std::to_string(userId)
                            + ", 0x" + hexEncode(payload) + ")"));
}

// 按字节取回源行 payload（CAST AS BINARY 免连接字符集转换，与 runner 读到的
// 原始字节一致）。
std::string storedPayload(MySQL& conn, int64_t id)
{
    MYSQL_RES* res = conn.query("SELECT CAST(message AS BINARY) FROM OfflineMessage WHERE id="
                                + std::to_string(id));
    if (!res) {
        return "";
    }
    MYSQL_ROW row = mysql_fetch_row(res);
    unsigned long* lens = mysql_fetch_lengths(res);
    std::string out;
    if (row && row[0] && lens) {
        out.assign(row[0], lens[0]);
    }
    mysql_free_result(res);
    return out;
}

class LegacyOfflineMigrationDb : public ::testing::Test {
protected:
    static void SetUpTestSuite()
    {
        resetDb();  // 连接池初始化前重建库
        seedBase();
        (void)pool();
    }

    void TearDown() override
    {
        MySQL conn;
        makeConn(conn);
        clearTables(conn);
    }
};

} // namespace

TEST_F(LegacyOfflineMigrationDb, BackfillMigratesDirectAndGroupRows)
{
    MySQL conn;
    makeConn(conn);
    insertOffline(conn, 1, 2, directPayload());   // direct：sender 1 → toid 2
    insertOffline(conn, 2, 3, groupPayload());    // group：groupid 1，离线成员 3
    insertOffline(conn, 3, 4, groupPayload());    // group：同一群消息另一成员 4
    ASSERT_EQ(3ll, queryInt(conn, "SELECT COUNT(*) FROM OfflineMessage"));

    offline_backfill::BackfillConfig cfg;
    cfg.batchSize = 2;
    offline_backfill::Runner runner(pool());
    offline_backfill::BackfillStats s = runner.run(cfg);

    EXPECT_EQ(3u, s.sourceRows);
    EXPECT_EQ(3u, s.migrated);
    EXPECT_EQ(0u, s.quarantined);
    EXPECT_EQ(0u, s.skippedIdempotent);
    EXPECT_FALSE(s.sourceHash.empty());

    // ledger：3 条 ChatMessage，幂等键 legacy:<offline_id>，sender=1，content 原样。
    EXPECT_EQ(3ll, queryInt(conn, "SELECT COUNT(*) FROM ChatMessage"));
    EXPECT_EQ((std::set<std::string>{"legacy:1", "legacy:2", "legacy:3"}),
              columnSet(conn, "SELECT client_message_id FROM ChatMessage"));
    EXPECT_EQ("hello", fetchContent(conn, "legacy:1"));
    EXPECT_EQ("hi all", fetchContent(conn, "legacy:2"));
    EXPECT_EQ("hi all", fetchContent(conn, "legacy:3"));
    EXPECT_EQ(3ll, queryInt(conn, "SELECT COUNT(*) FROM ChatMessage WHERE sender_id=1"));

    // Conversation：direct (1,2) 与 group(1) 各一。
    EXPECT_EQ(1ll, queryInt(conn, "SELECT COUNT(*) FROM DirectConversation "
                                  "WHERE user_low_id=1 AND user_high_id=2"));
    EXPECT_EQ(1ll, queryInt(conn, "SELECT COUNT(*) FROM GroupConversation WHERE group_id=1"));

    // MessageDelivery：每行一条，recipient = 旧表 userid。
    EXPECT_EQ((std::set<std::string>{"2", "3", "4"}),
              columnSet(conn, "SELECT recipient_id FROM MessageDelivery"));
    EXPECT_EQ(3ll, queryInt(conn, "SELECT COUNT(*) FROM MessageDelivery"));

    // 幂等键回读：legacy:1 的 Message 与旧行语义一致（delivery 重建所需字段齐备）。
    MySQLMessageStore store(pool(), kFanOutCap);
    std::shared_ptr<const Message> msg = store.findAccepted(ClientMessageId("legacy:1"), UserId(1));
    ASSERT_TRUE(msg != nullptr);
    EXPECT_EQ(UserId(2), msg->command.directRecipient);
    EXPECT_EQ("hello", msg->command.content);
}

TEST_F(LegacyOfflineMigrationDb, DuplicateRowsIdempotentByLegacyId)
{
    MySQL conn;
    makeConn(conn);
    // 重复行：同 payload 两行（旧表不去重语义，P2-07 保留），幂等键按 offline_id
    // 互不冲突。
    insertOffline(conn, 1, 2, directPayload());
    insertOffline(conn, 2, 2, directPayload());
    ASSERT_EQ(2ll, queryInt(conn, "SELECT COUNT(*) FROM OfflineMessage"));

    offline_backfill::BackfillConfig cfg;
    offline_backfill::Runner runner(pool());
    offline_backfill::BackfillStats s = runner.run(cfg);

    EXPECT_EQ(2u, s.migrated);
    EXPECT_EQ(2ll, queryInt(conn, "SELECT COUNT(*) FROM ChatMessage"));
    EXPECT_EQ((std::set<std::string>{"legacy:1", "legacy:2"}),
              columnSet(conn, "SELECT client_message_id FROM ChatMessage"));
    EXPECT_EQ("hello", fetchContent(conn, "legacy:1"));
    EXPECT_EQ("hello", fetchContent(conn, "legacy:2"));

    MySQLMessageStore store(pool(), kFanOutCap);
    std::shared_ptr<const Message> m1 = store.findAccepted(ClientMessageId("legacy:1"), UserId(1));
    std::shared_ptr<const Message> m2 = store.findAccepted(ClientMessageId("legacy:2"), UserId(1));
    ASSERT_TRUE(m1 != nullptr);
    ASSERT_TRUE(m2 != nullptr);
    EXPECT_NE(m1->id, m2->id);

    // 重复运行不增 Message：checkpoint 已到高水位，无新行。
    offline_backfill::Runner runner2(pool());
    offline_backfill::BackfillStats s2 = runner2.run(cfg);
    EXPECT_EQ(0u, s2.sourceRows);
    EXPECT_EQ(0u, s2.migrated);
    EXPECT_EQ(0u, s2.quarantined);
    EXPECT_EQ(2ll, queryInt(conn, "SELECT COUNT(*) FROM ChatMessage"));
    std::shared_ptr<const Message> m1again =
        store.findAccepted(ClientMessageId("legacy:1"), UserId(1));
    ASSERT_TRUE(m1again != nullptr);
    EXPECT_EQ(m1->id, m1again->id);  // 同 legacy id 返回原 MessageId

    // 已存在同键（此前迁移已落库但 checkpoint 未记录）→ 按幂等键跳过，不产生
    // 第二行，原 Message 原样保留。
    clearTables(conn);
    ASSERT_TRUE(conn.update("INSERT INTO Conversation(id,kind,next_sequence) "
                            "VALUES(1,'DIRECT',0)"));
    ASSERT_TRUE(conn.update("INSERT INTO DirectConversation(conversation_id,user_low_id,"
                            "user_high_id) VALUES(1,1,2)"));
    ASSERT_TRUE(conn.update("INSERT INTO ChatMessage(conversation_id,sender_id,"
                            "client_message_id,sequence,content) "
                            "VALUES(1,1,'legacy:1',1,'pre-seeded')"));
    insertOffline(conn, 1, 2, directPayload());
    offline_backfill::Runner runner3(pool());
    offline_backfill::BackfillStats s3 = runner3.run(cfg);
    EXPECT_EQ(1u, s3.sourceRows);
    EXPECT_EQ(0u, s3.migrated);
    EXPECT_EQ(1u, s3.skippedIdempotent);
    EXPECT_EQ(1ll, queryInt(conn, "SELECT COUNT(*) FROM ChatMessage"));
    std::shared_ptr<const Message> original =
        store.findAccepted(ClientMessageId("legacy:1"), UserId(1));
    ASSERT_TRUE(original != nullptr);
    EXPECT_EQ("pre-seeded", original->command.content);
}

TEST_F(LegacyOfflineMigrationDb, BadJsonRowsGoToQuarantine)
{
    MySQL conn;
    makeConn(conn);
    // 不可解析行：坏 JSON / 截断 JSON / 乱码 / 非 UTF-8（净化后非 JSON）/
    // 缺字段 / 未知 msgid / 超界（>500 字节合法 JSON）。
    insertOffline(conn, 1, 2, "this is not json");
    insertOffline(conn, 2, 2, "{\"msgid\":6,\"id\":1,\"toid\":2,\"content\":\"hi");
    insertOfflineHex(conn, 3, 2, "garbage \xC3\xA9\xC3\xA9\xC3\xA9");       // 乱码（合法 utf8 非 JSON）
    insertRawBytes(conn, 4, 2, "E9E9E9E9");                                 // 非 UTF-8 → 净化后非 JSON
    insertOffline(conn, 5, 2, "{\"msgid\":6,\"id\":1,\"toid\":2}");         // 缺 content/msg
    insertOffline(conn, 6, 2, "{\"msgid\":99,\"id\":1,\"toid\":2,\"content\":\"x\"}");  // 未知 msgid
    insertOfflineHex(conn, 7, 2, oversizedPayload());                       // 超界（>500 字节）
    // 正常行仍迁移。
    insertOffline(conn, 8, 2, directPayload());
    insertOffline(conn, 9, 3, groupPayload());
    ASSERT_EQ(9ll, queryInt(conn, "SELECT COUNT(*) FROM OfflineMessage"));

    offline_backfill::BackfillConfig cfg;
    offline_backfill::Runner runner(pool());
    offline_backfill::BackfillStats s = runner.run(cfg);

    // 计数：坏行全部 quarantine、不计入 migrated；正常行迁移。
    EXPECT_EQ(9u, s.sourceRows);
    EXPECT_EQ(2u, s.migrated);
    EXPECT_EQ(7u, s.quarantined);
    EXPECT_EQ(0u, s.skippedIdempotent);
    EXPECT_EQ(2ll, queryInt(conn, "SELECT COUNT(*) FROM ChatMessage"));
    EXPECT_EQ((std::set<std::string>{"legacy:8", "legacy:9"}),
              columnSet(conn, "SELECT client_message_id FROM ChatMessage"));

    // quarantine 可查询：每条含源 id/用户/原因/原 payload；绝不静默删（源行仍在）。
    std::vector<offline_backfill::QuarantineEntry> q = runner.quarantine();
    ASSERT_EQ(7u, q.size());
    std::set<int64_t> qIds;
    for (size_t i = 0; i < q.size(); ++i) {
        qIds.insert(q[i].offlineId);
        EXPECT_EQ(2, q[i].userId);
        EXPECT_FALSE(q[i].reason.empty());
        EXPECT_EQ(storedPayload(conn, q[i].offlineId), q[i].payload);
    }
    EXPECT_EQ((std::set<int64_t>{1, 2, 3, 4, 5, 6, 7}), qIds);
    EXPECT_EQ(9ll, queryInt(conn, "SELECT COUNT(*) FROM OfflineMessage"));
    EXPECT_EQ(2ll, queryInt(conn, "SELECT COUNT(*) FROM ChatMessage WHERE sender_id=1"));
    // 守恒：源行 = migrated + quarantine + skipped。
    EXPECT_EQ(s.sourceRows, s.migrated + s.quarantined + s.skippedIdempotent);
}

TEST_F(LegacyOfflineMigrationDb, SourceCountConservation)
{
    MySQL conn;
    makeConn(conn);
    insertOffline(conn, 1, 2, directPayload());
    insertOffline(conn, 2, 2, directPayload());
    insertOffline(conn, 3, 2, directPayload());
    insertOffline(conn, 4, 2, "this is not json");
    insertOffline(conn, 5, 2, "{\"msgid\":6,\"id\":1,\"toid\":2,\"content\":\"hi");
    ASSERT_EQ(5ll, queryInt(conn, "SELECT COUNT(*) FROM OfflineMessage"));

    // 中断：批 1 完成（2 行）后停止。
    offline_backfill::BackfillConfig cfg;
    cfg.batchSize = 2;
    cfg.maxBatches = 1;
    offline_backfill::Runner runner(pool());
    offline_backfill::BackfillStats s1 = runner.run(cfg);
    EXPECT_EQ(2u, s1.sourceRows);
    EXPECT_EQ(2u, s1.migrated);
    EXPECT_EQ(0u, s1.quarantined);
    EXPECT_EQ(2u, runner.checkpoint());

    // 重跑：从 checkpoint 继续，处理剩余 3 行（1 迁移 + 2 quarantine）。
    offline_backfill::BackfillConfig full;
    offline_backfill::Runner runner2(pool());
    offline_backfill::BackfillStats s2 = runner2.run(full);
    EXPECT_EQ(3u, s2.sourceRows);
    EXPECT_EQ(1u, s2.migrated);
    EXPECT_EQ(2u, s2.quarantined);
    EXPECT_EQ(5u, runner2.checkpoint());

    // 守恒：两段累计 migrated + quarantine = 源行 5；中断重跑不重复迁移。
    EXPECT_EQ(5u, s1.migrated + s2.migrated + s1.quarantined + s2.quarantined);
    EXPECT_EQ(s1.sourceRows, s1.migrated + s1.quarantined + s1.skippedIdempotent);
    EXPECT_EQ(s2.sourceRows, s2.migrated + s2.quarantined + s2.skippedIdempotent);
    EXPECT_EQ(3ll, queryInt(conn, "SELECT COUNT(*) FROM ChatMessage"));
    EXPECT_EQ(2u, runner2.quarantine().size());
    EXPECT_EQ(5ll, queryInt(conn, "SELECT COUNT(*) FROM OfflineMessage"));
}

TEST_F(LegacyOfflineMigrationDb, EmptyDatabaseBackfillIsNoop)
{
    MySQL conn;
    makeConn(conn);
    ASSERT_EQ(0ll, queryInt(conn, "SELECT COUNT(*) FROM OfflineMessage"));

    offline_backfill::BackfillConfig dry;
    dry.dryRun = true;
    offline_backfill::Runner runner(pool());
    offline_backfill::BackfillStats sd = runner.run(dry);
    EXPECT_EQ(0u, sd.sourceRows);
    EXPECT_EQ(0u, sd.migrated);
    EXPECT_EQ(0u, sd.quarantined);
    EXPECT_EQ(0u, runner.checkpoint());

    // 实跑同为空库：0 行、无任何 ledger/quarantine 写入，幂等可重跑。
    offline_backfill::BackfillConfig full;
    offline_backfill::Runner runner2(pool());
    offline_backfill::BackfillStats s = runner2.run(full);
    EXPECT_EQ(0u, s.sourceRows);
    EXPECT_EQ(0u, s.migrated);
    EXPECT_EQ(0u, s.quarantined);
    EXPECT_EQ(0u, s.skippedIdempotent);
    EXPECT_EQ(0u, runner2.checkpoint());
    EXPECT_EQ(0ll, queryInt(conn, "SELECT COUNT(*) FROM Conversation"));
    EXPECT_EQ(0ll, queryInt(conn, "SELECT COUNT(*) FROM ChatMessage"));
    EXPECT_EQ(0ll, queryInt(conn, "SELECT COUNT(*) FROM MessageDelivery"));
    EXPECT_EQ(0u, runner2.quarantine().size());
    offline_backfill::Runner runner3(pool());
    offline_backfill::BackfillStats s3 = runner3.run(full);
    EXPECT_EQ(0u, s3.sourceRows);
    EXPECT_EQ(0u, s3.migrated);
    EXPECT_EQ(0u, s3.quarantined);
}

TEST_F(LegacyOfflineMigrationDb, DryRunDoesNotMutate)
{
    MySQL conn;
    makeConn(conn);
    insertOffline(conn, 1, 2, directPayload());
    insertOffline(conn, 2, 2, groupPayload());
    insertOffline(conn, 3, 2, "this is not json");
    ASSERT_EQ(3ll, queryInt(conn, "SELECT COUNT(*) FROM OfflineMessage"));

    offline_backfill::BackfillConfig dry;
    dry.dryRun = true;
    offline_backfill::Runner runner(pool());
    offline_backfill::BackfillStats sd = runner.run(dry);
    EXPECT_EQ(3u, sd.sourceRows);
    EXPECT_EQ(2u, sd.migrated);
    EXPECT_EQ(1u, sd.quarantined);
    EXPECT_FALSE(sd.sourceHash.empty());

    // dry-run 不写任何 ledger/quarantine/checkpoint。
    EXPECT_EQ(0ll, queryInt(conn, "SELECT COUNT(*) FROM Conversation"));
    EXPECT_EQ(0ll, queryInt(conn, "SELECT COUNT(*) FROM ChatMessage"));
    EXPECT_EQ(0ll, queryInt(conn, "SELECT COUNT(*) FROM MessageDelivery"));
    EXPECT_EQ(0u, runner.quarantine().size());
    EXPECT_EQ(0u, runner.checkpoint());

    // 实跑产生与 dry-run 一致的计数。
    offline_backfill::BackfillConfig full;
    offline_backfill::Runner runner2(pool());
    offline_backfill::BackfillStats s = runner2.run(full);
    EXPECT_EQ(2u, s.migrated);
    EXPECT_EQ(1u, s.quarantined);
    EXPECT_EQ(sd.sourceHash, s.sourceHash);
    EXPECT_EQ(2ll, queryInt(conn, "SELECT COUNT(*) FROM ChatMessage"));
    EXPECT_EQ(1u, runner2.quarantine().size());
}

TEST_F(LegacyOfflineMigrationDb, CheckpointResumeAfterInterruption)
{
    MySQL conn;
    makeConn(conn);
    for (int64_t i = 1; i <= 5; ++i) {
        insertOffline(conn, i, 2, directPayload());
    }
    ASSERT_EQ(5ll, queryInt(conn, "SELECT COUNT(*) FROM OfflineMessage"));

    // 中断：batchSize=2、仅 1 批 → 迁移 2 行，checkpoint 落到 id=2。
    offline_backfill::BackfillConfig cfg;
    cfg.batchSize = 2;
    cfg.maxBatches = 1;
    offline_backfill::Runner runner(pool());
    offline_backfill::BackfillStats s1 = runner.run(cfg);
    EXPECT_EQ(2u, s1.migrated);
    EXPECT_EQ(2u, runner.checkpoint());
    EXPECT_EQ(2ll, queryInt(conn, "SELECT COUNT(*) FROM ChatMessage"));

    MySQLMessageStore store(pool(), kFanOutCap);
    std::shared_ptr<const Message> m1 =
        store.findAccepted(ClientMessageId("legacy:1"), UserId(1));
    ASSERT_TRUE(m1 != nullptr);

    // 新 runner（进程重启语义）从 checkpoint 继续：不重复迁移批 1，完成剩余。
    offline_backfill::BackfillConfig full;
    offline_backfill::Runner runner2(pool());
    offline_backfill::BackfillStats s2 = runner2.run(full);
    EXPECT_EQ(3u, s2.sourceRows);
    EXPECT_EQ(3u, s2.migrated);
    EXPECT_EQ(5u, runner2.checkpoint());
    EXPECT_EQ(5ll, queryInt(conn, "SELECT COUNT(*) FROM ChatMessage"));
    EXPECT_EQ((std::set<std::string>{"legacy:1", "legacy:2", "legacy:3",
                                     "legacy:4", "legacy:5"}),
              columnSet(conn, "SELECT client_message_id FROM ChatMessage"));
    // 批 1 未重复：legacy:1 仍返回首次迁移的 MessageId，content 原样。
    std::shared_ptr<const Message> m1again =
        store.findAccepted(ClientMessageId("legacy:1"), UserId(1));
    ASSERT_TRUE(m1again != nullptr);
    EXPECT_EQ(m1->id, m1again->id);
    EXPECT_EQ("hello", m1again->command.content);
}
