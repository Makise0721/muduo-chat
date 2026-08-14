// P3-10 backfill runner 重入性/checkpoint 状态机/批次有界/哈希一致性
// （docs/tasks/P3-10.md RED 计划 3/5/6，与 LegacyOfflineMigrationTest 数据向
// 用例互补的最小集）：公开 runner interface（offline_backfill::Runner）经真实
// MySQL（独立库 chat_p310_reentrancy）驱动，不读私有容器、无固定 sleep。
// 现状无 db/OfflineBackfill.hpp → 本文件引用尚不存在的公开接口，编译失败即合法 RED。
// 用例：
// - RerunIsIdempotentAndHashStable：完整跑两遍 → ChatMessage 行数与 legacy id 集
//   相同（不增 Message）、checkpoint 不变、sourceHash 一致。
// - CheckpointAdvancesOnlyOnCompletedBatches：batchSize=2 分段（maxBatches=1 ×2）
//   再补全 → checkpoint 单调推进（2→4→5）、已迁移行不被重读、累计迁移守恒、
//   每段内部守恒（sourceRows = migrated + quarantine + skipped）。
// - DryRunMatchesRunCountsAndHash：dry-run 与实跑 migrated/quarantined/sourceHash
//   一致（dry-run 不写库）。
// - HashIndependentOfBatchSize：同一源，批次划分不同（batchSize 2/1 批 vs 全量）
//   → sourceHash 相同（hash 覆盖全部源行，与进度/批次无关）。
// - TransientBusyRowFailsRunWithoutQuarantine（P3-10 M4，RED→GREEN 卡记录）：
//   瞬态 DependencyBusy（1205/1213/断连/池超时，经 Runner 可选 FaultHook 注入点
//   在 InsertMessage 步骤注入一次）→ run() fail-fast 抛出（CLI exit 非零语义）、
//   该行不进 quarantine（quarantine 是数据层分类，瞬态依赖错误不得污染审查
//   噪音）、checkpoint 不推进；错误清除后重跑成功迁移该行（checkpoint 未写 →
//   自动重试）。GREEN：processRow 按 StoreErrorKind 分类，仅 DependencyBusy 重抛，
//   NotFound/Storage 等仍 quarantine。
// 串行约束：本二进制独占 chat_p310_reentrancy 库与单例连接池（SetUpTestSuite
// 重建），不与其它测试二进制混跑（CTest 默认串行满足，勿以 --parallel 混跑）。

#include "MySqlTestFixture.hpp"

#include "app/MySQLMessageStore.hpp"
#include "app/ReliableMessaging.hpp"

#include "db/OfflineBackfill.hpp"

#include <gtest/gtest.h>
#include <mysql/mysql.h>

#include <cstdlib>
#include <set>
#include <string>
#include <vector>

namespace {

const char* kTestDb = "chat_p310_reentrancy";
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
    ASSERT_TRUE(admin.update("DROP DATABASE IF EXISTS chat_p310_reentrancy"));
    ASSERT_TRUE(admin.update("CREATE DATABASE chat_p310_reentrancy DEFAULT CHARSET utf8"));
    schema_migration::Migrator migrator("127.0.0.1", "root", MySqlTestFixture::password(),
                                        kTestDb, 3306);
    schema_migration::MigrateResult r = migrator.migrateTo(migrationsDir(), "", 30);
    ASSERT_TRUE(r.ok) << r.error;
}

void makeConn(MySQL& conn)
{
    ASSERT_TRUE(conn.connect("127.0.0.1", "root", MySqlTestFixture::password(), kTestDb, 3306));
}

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
        "INSERT INTO User(id, name, password) VALUES (1,'u1','x'),(2,'u2','x')"));
}

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

std::string directPayload(int64_t sender, int64_t toid)
{
    return "{\"msgid\":6,\"id\":" + std::to_string(sender)
           + ",\"toid\":" + std::to_string(toid) + ",\"content\":\"m\"}";
}

void insertOffline(MySQL& conn, int64_t id, int64_t userId, const std::string& payload)
{
    ASSERT_TRUE(conn.update("INSERT INTO OfflineMessage(id,userid,message) VALUES("
                            + std::to_string(id) + "," + std::to_string(userId)
                            + ",'" + payload + "')"));
}

// P3-10 M4：瞬态 DependencyBusy 注入（1205 lock wait timeout 语义）。FaultHook
// 是 MySQLMessageStore 既有故障注入 seam（P3-04 文档化），经 Runner 可选构造参数
// 转递；只触发一次（transient），之后重跑不再失败。
class ThrowBusyOnceHook : public MySQLMessageStore::FaultHook {
public:
    explicit ThrowBusyOnceHook(MySQLMessageStore::Step step) : step_(step) {}
    void onStep(MySQLMessageStore::Step step) override
    {
        if (!fired_ && step == step_) {
            fired_ = true;
            throw MessageStoreError(StoreErrorKind::DependencyBusy,
                                    "injected transient lock wait timeout");
        }
    }
    bool fired() const { return fired_; }

private:
    MySQLMessageStore::Step step_;
    bool fired_ = false;
};

// 可重入数据集：n 条 direct 行（sender 1 → toid 2，userid=2）。
void seedDirectRows(int n)
{
    MySQL conn;
    makeConn(conn);
    for (int64_t i = 1; i <= n; ++i) {
        insertOffline(conn, i, 2, directPayload(1, 2));
    }
}

class BackfillReentrancyDb : public ::testing::Test {
protected:
    static void SetUpTestSuite()
    {
        resetDb();
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

TEST_F(BackfillReentrancyDb, RerunIsIdempotentAndHashStable)
{
    seedDirectRows(4);
    MySQL conn;
    makeConn(conn);
    ASSERT_EQ(4ll, queryInt(conn, "SELECT COUNT(*) FROM OfflineMessage"));

    offline_backfill::BackfillConfig cfg;
    cfg.batchSize = 2;
    offline_backfill::Runner runner(pool());
    offline_backfill::BackfillStats s1 = runner.run(cfg);
    EXPECT_EQ(4u, s1.migrated);
    EXPECT_EQ(4u, runner.checkpoint());
    EXPECT_EQ(4ll, queryInt(conn, "SELECT COUNT(*) FROM ChatMessage"));

    MySQLMessageStore store(pool(), kFanOutCap);
    std::shared_ptr<const Message> m1 =
        store.findAccepted(ClientMessageId("legacy:1"), UserId(1));
    ASSERT_TRUE(m1 != nullptr);

    // 完整重跑：checkpoint 已在高水位 → 无新迁移；sourceHash 覆盖全部源行，一致。
    offline_backfill::BackfillConfig full;
    offline_backfill::Runner runner2(pool());
    offline_backfill::BackfillStats s2 = runner2.run(full);
    EXPECT_EQ(0u, s2.sourceRows);
    EXPECT_EQ(0u, s2.migrated);
    EXPECT_EQ(0u, s2.quarantined);
    EXPECT_EQ(4u, runner2.checkpoint());
    EXPECT_EQ(s1.sourceHash, s2.sourceHash);
    EXPECT_EQ(4ll, queryInt(conn, "SELECT COUNT(*) FROM ChatMessage"));
    EXPECT_EQ((std::set<std::string>{"legacy:1", "legacy:2", "legacy:3", "legacy:4"}),
              columnSet(conn, "SELECT client_message_id FROM ChatMessage"));
    std::shared_ptr<const Message> m1again =
        store.findAccepted(ClientMessageId("legacy:1"), UserId(1));
    ASSERT_TRUE(m1again != nullptr);
    EXPECT_EQ(m1->id, m1again->id);
}

TEST_F(BackfillReentrancyDb, CheckpointAdvancesOnlyOnCompletedBatches)
{
    seedDirectRows(5);
    MySQL conn;
    makeConn(conn);

    offline_backfill::BackfillConfig cfg;
    cfg.batchSize = 2;
    cfg.maxBatches = 1;

    offline_backfill::Runner runner1(pool());
    offline_backfill::BackfillStats s1 = runner1.run(cfg);
    EXPECT_EQ(2u, s1.migrated);
    EXPECT_EQ(2u, runner1.checkpoint());

    // 新 runner（进程重启语义）继续下一批：checkpoint 只前进到已提交批的高水位。
    offline_backfill::Runner runner2(pool());
    offline_backfill::BackfillStats s2 = runner2.run(cfg);
    EXPECT_EQ(2u, s2.migrated);
    EXPECT_EQ(4u, runner2.checkpoint());

    offline_backfill::BackfillConfig full;
    offline_backfill::Runner runner3(pool());
    offline_backfill::BackfillStats s3 = runner3.run(full);
    EXPECT_EQ(1u, s3.migrated);
    EXPECT_EQ(5u, runner3.checkpoint());

    // 状态机：checkpoint 单调推进且不超前；累计迁移守恒；每段内部守恒。
    EXPECT_LE(s1.migrated, s1.sourceRows);
    EXPECT_LE(s2.migrated, s2.sourceRows);
    EXPECT_LE(s3.migrated, s3.sourceRows);
    EXPECT_EQ(5u, s1.migrated + s2.migrated + s3.migrated);
    EXPECT_EQ(5ll, queryInt(conn, "SELECT COUNT(*) FROM ChatMessage"));
    EXPECT_EQ((std::set<std::string>{"legacy:1", "legacy:2", "legacy:3",
                                     "legacy:4", "legacy:5"}),
              columnSet(conn, "SELECT client_message_id FROM ChatMessage"));
}

TEST_F(BackfillReentrancyDb, DryRunMatchesRunCountsAndHash)
{
    MySQL conn;
    makeConn(conn);
    insertOffline(conn, 1, 2, directPayload(1, 2));
    insertOffline(conn, 2, 2, directPayload(1, 2));
    insertOffline(conn, 3, 2, directPayload(1, 2));
    insertOffline(conn, 4, 2, "not json");
    insertOffline(conn, 5, 2, "{\"msgid\":6,\"id\":1,\"toid\":2,\"content\":\"x");
    ASSERT_EQ(5ll, queryInt(conn, "SELECT COUNT(*) FROM OfflineMessage"));

    offline_backfill::BackfillConfig dry;
    dry.dryRun = true;
    offline_backfill::Runner runner(pool());
    offline_backfill::BackfillStats sd = runner.run(dry);
    EXPECT_EQ(5u, sd.sourceRows);
    EXPECT_EQ(3u, sd.migrated);
    EXPECT_EQ(2u, sd.quarantined);
    EXPECT_EQ(0u, sd.skippedIdempotent);
    EXPECT_FALSE(sd.sourceHash.empty());

    // dry-run 不写库。
    EXPECT_EQ(0ll, queryInt(conn, "SELECT COUNT(*) FROM ChatMessage"));
    EXPECT_EQ(0u, runner.checkpoint());
    EXPECT_EQ(0u, runner.quarantine().size());

    // 实跑计数与 hash 与 dry-run 一致。
    offline_backfill::BackfillConfig full;
    offline_backfill::Runner runner2(pool());
    offline_backfill::BackfillStats s = runner2.run(full);
    EXPECT_EQ(3u, s.migrated);
    EXPECT_EQ(2u, s.quarantined);
    EXPECT_EQ(sd.sourceHash, s.sourceHash);
    EXPECT_EQ(3ll, queryInt(conn, "SELECT COUNT(*) FROM ChatMessage"));
    EXPECT_EQ(2u, runner2.quarantine().size());
}

TEST_F(BackfillReentrancyDb, HashIndependentOfBatchSize)
{
    seedDirectRows(5);
    MySQL conn;
    makeConn(conn);

    // 不同批次划分/进度下 sourceHash 相同：hash 覆盖全部源行（与 checkpoint 无关）。
    offline_backfill::BackfillConfig partial;
    partial.batchSize = 2;
    partial.maxBatches = 1;
    offline_backfill::Runner runner1(pool());
    offline_backfill::BackfillStats s1 = runner1.run(partial);
    ASSERT_EQ(2u, s1.migrated);

    offline_backfill::BackfillConfig full;
    offline_backfill::Runner runner2(pool());
    offline_backfill::BackfillStats s2 = runner2.run(full);
    EXPECT_EQ(3u, s2.migrated);
    EXPECT_EQ(s1.sourceHash, s2.sourceHash);

    // 单批全量（batchSize=100）的 hash 也一致。
    offline_backfill::Runner runner3(pool());
    offline_backfill::BackfillStats s3 = runner3.run(full);
    EXPECT_EQ(s1.sourceHash, s3.sourceHash);
}

// P3-10 M4 DependencyBusy fail-fast（RED 计划：现状 processRow 把任何写 ledger
// 异常都吞进 quarantine，瞬态依赖错误被误判为行数据问题——RED 断言失败）。
TEST_F(BackfillReentrancyDb, TransientBusyRowFailsRunWithoutQuarantine)
{
    seedDirectRows(2);
    MySQL conn;
    makeConn(conn);
    ASSERT_EQ(2ll, queryInt(conn, "SELECT COUNT(*) FROM OfflineMessage"));

    // 首跑注入一次瞬态 DependencyBusy（InsertMessage 步骤，1205 语义）。
    ThrowBusyOnceHook hook(MySQLMessageStore::Step::InsertMessage);
    offline_backfill::BackfillConfig cfg;
    cfg.batchSize = 2;
    offline_backfill::Runner runner(pool(), &hook);

    // fail-fast：run() 抛 DependencyBusy（CLI 映射 exit 1）；批未完成不写 checkpoint。
    EXPECT_THROW(runner.run(cfg), MessageStoreError);
    EXPECT_TRUE(hook.fired());

    // 该行不进 quarantine：瞬态依赖错误不是行数据问题（不污染审查噪音）。
    EXPECT_EQ(0u, runner.quarantine().size());
    // checkpoint 不推进：首行 accept 事务已回滚，重跑自动重试。
    EXPECT_EQ(0u, runner.checkpoint());
    EXPECT_EQ(0ll, queryInt(conn, "SELECT COUNT(*) FROM ChatMessage"));

    // 错误清除后重跑：成功迁移两行，checkpoint 推进到高水位（守恒成立）。
    offline_backfill::Runner runner2(pool());
    offline_backfill::BackfillStats s = runner2.run(cfg);
    EXPECT_EQ(2u, s.migrated);
    EXPECT_EQ(0u, s.quarantined);
    EXPECT_EQ(0u, s.skippedIdempotent);
    EXPECT_EQ(2u, runner2.checkpoint());
    EXPECT_EQ(2ll, queryInt(conn, "SELECT COUNT(*) FROM ChatMessage"));
}
