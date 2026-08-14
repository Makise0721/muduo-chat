// P3-08 真实 MySQL 时钟/lease round-trip（docs/tasks/P3-08.md 验证节，本卡必须
// 新增）：跨 store 实例持久 lease 可比对、到期可重领、旧格式 owner 安全方向。
// 全部在真实 chat_p308_lease 库执行，不 skip；断言失败即失败。
// 用例：
// - LeaseEpochRoundTripAcrossStoreInstances：store 实例 1（进程 1 语义，owner 带
//   bootId）写入 InFlight Delivery（leaseUntilMs = 真实 now + 5000），新 store 实例
//   2 读回：leaseUntilMs 与写入值差 <=1000ms（DATETIME(0) 秒精度）；owner 字符串
//   round-trip 一致（SessionIdentity + leaseBootId 精确相等）。
// - ExpiredLeaseReclaimableAcrossInstances：lease 已过期的 InFlight 行经 store 查询
//   可重领（真实 clock）；新 ReliableMessaging 实例 sessionAvailable 立即重投同
//   message_id（attempt+1，跨实例重领）。
// - LegacyTwoPartOwnerDecodes：旧格式 "uid:gen"（无 bootId）行读回 leaseBootId=0 且
//   owner 两段正确；跨进程 fencing 判定必不等 -> 未到期 lease 也立即回收（安全方向）。
// - NextAttemptAtNotDueEarlyAcrossStoreInstances：next_attempt_at 写入向上取整，
//   读回值不早于写入时刻、到期扫描不提前命中（ack-timeout 重投不得早于排程）。
// - ExpiresAtPersistsFromAccept：accept 注入的 expiresAtMs 随行持久化。
// 串行约束：本二进制独占 chat_p308_lease 库与单例连接池（SetUpTestSuite 重建），
// 不与其它测试二进制混跑（CTest 默认串行满足，勿以 --parallel 混跑）。

#include "MySqlTestFixture.hpp"

#include "app/Clock.hpp"
#include "app/MySQLMessageStore.hpp"
#include "app/ReliableMessaging.hpp"

#include "RecordingDeliverySink.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

namespace {

const char* kTestDb = "chat_p308_lease";
const UserId kAlice{1};
const UserId kBob{2};
const uint64_t kLeaseMs = 1000;
const uint64_t kCap = 100;
const int64_t kEpochLeaseMs = 5000;

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
    ASSERT_TRUE(admin.update("DROP DATABASE IF EXISTS chat_p308_lease"));
    ASSERT_TRUE(admin.update("CREATE DATABASE chat_p308_lease DEFAULT CHARSET utf8"));
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
        "INSERT INTO User(id, name, password) VALUES (1,'u1','x'),(2,'u2','x')"));
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

SendMessageCommand directTo(UserId recipient, const std::string& cmid)
{
    SendMessageCommand cmd;
    cmd.clientMessageId = ClientMessageId(cmid);
    cmd.kind = SendMessageCommand::Kind::Direct;
    cmd.directRecipient = recipient;
    cmd.content = "payload";
    return cmd;
}

class MySqlLeaseRoundTripDb : public ::testing::Test {
protected:
    static void SetUpTestSuite()
    {
        resetDb();  // 连接池初始化前重建库
        seedUsers();
        (void)pool();
    }

    // 用例间隔离：清空可靠消息表（ChatMessage 级联 MessageDelivery/OutboxEvent；
    // 再删链接表与 Conversation），使跨用例不互相 claim（否则前用例遗留的
    // InFlight 会被后一用例的 sessionAvailable 重领，污染 attempts）。
    void TearDown() override
    {
        MySQL conn;
        ASSERT_TRUE(conn.connect("127.0.0.1", "root", MySqlTestFixture::password(), kTestDb, 3306));
        ASSERT_TRUE(conn.update("DELETE FROM ChatMessage"));
        ASSERT_TRUE(conn.update("DELETE FROM DirectConversation"));
        ASSERT_TRUE(conn.update("DELETE FROM GroupConversation"));
        ASSERT_TRUE(conn.update("DELETE FROM Conversation"));
    }
};

} // namespace

TEST_F(MySqlLeaseRoundTripDb, LeaseEpochRoundTripAcrossStoreInstances)
{
    UnixEpochClock clock;
    RecordingDeliverySink sink;
    MySQLMessageStore store1(pool(), kCap);
    ReliableMessaging rm1(store1, sink, clock, kEpochLeaseMs);  // 进程 1 语义（持 boot id）

    const SessionIdentity alice{kAlice, 1};
    const SessionIdentity bob{kBob, 1};
    AcceptOutcome a = rm1.accept(alice, directTo(kBob, "lease-epoch-1"));
    ASSERT_TRUE(a.ok);
    ASSERT_TRUE(sink.attempts().empty());  // bob 离线：accept 不产生投递

    // 进程 1 写入 InFlight lease：真实 clock + 5000ms，owner 带 bootId。
    std::vector<Delivery> rows = store1.deliveriesByMessage(a.messageId);
    ASSERT_EQ(1u, rows.size());
    Delivery write = rows[0];
    write.state = DeliveryState::InFlight;
    write.leaseOwner = bob;
    write.leaseBootId = 0x0808;
    write.leaseUntilMs = clock.nowMs() + kEpochLeaseMs;
    store1.updateDelivery(write);

    // 新 store 实例 2（进程 2 读语义）读回。
    MySQLMessageStore store2(pool(), kCap);
    std::vector<Delivery> readback = store2.deliveriesByMessage(a.messageId);
    ASSERT_EQ(1u, readback.size());
    const Delivery& d = readback[0];
    EXPECT_EQ(DeliveryState::InFlight, d.state);
    // owner 字符串 round-trip："uid:gen:bootid" -> SessionIdentity + leaseBootId。
    EXPECT_EQ(bob, d.leaseOwner);
    EXPECT_EQ(0x0808, d.leaseBootId);
    // leaseUntilMs round-trip：DATETIME(0) 秒精度，允许 <=1000ms 差。
    EXPECT_GE(d.leaseUntilMs, write.leaseUntilMs - 1000)
        << "written " << write.leaseUntilMs << " read " << d.leaseUntilMs;
    EXPECT_LE(d.leaseUntilMs, write.leaseUntilMs + 1000)
        << "written " << write.leaseUntilMs << " read " << d.leaseUntilMs;
    rm1.stop(0);
}

TEST_F(MySqlLeaseRoundTripDb, ExpiredLeaseReclaimableAcrossInstances)
{
    UnixEpochClock clock;
    RecordingDeliverySink sink1;
    MySQLMessageStore store1(pool(), kCap);
    ReliableMessaging rm1(store1, sink1, clock, kLeaseMs);

    const SessionIdentity alice{kAlice, 1};
    const SessionIdentity bob{kBob, 1};
    AcceptOutcome a = rm1.accept(alice, directTo(kBob, "lease-expired-1"));
    ASSERT_TRUE(a.ok);

    // 进程 1 写入已过期 lease 的 InFlight 行（attempt=1，真实 clock）。
    std::vector<Delivery> rows = store1.deliveriesByMessage(a.messageId);
    ASSERT_EQ(1u, rows.size());
    Delivery expired = rows[0];
    expired.state = DeliveryState::InFlight;
    expired.leaseOwner = bob;
    expired.leaseBootId = 0x0A0A;
    expired.attemptCount = 1;
    expired.leaseUntilMs = clock.nowMs() - 1000;  // 已过期
    store1.updateDelivery(expired);

    // 新实例 sessionAvailable：lease 过期行经 store 查询可重领，重投同 message_id。
    RecordingDeliverySink sink2;
    MySQLMessageStore store2(pool(), kCap);
    ReliableMessaging rm2(store2, sink2, clock, kLeaseMs);
    rm2.sessionAvailable(bob);
    ASSERT_EQ(1u, sink2.attempts().size());
    EXPECT_EQ(a.messageId.value, sink2.attempts()[0].messageId.value);
    EXPECT_EQ(2u, sink2.attempts()[0].attemptNumber);  // 跨实例重领 attempt+1
    rm2.stop(0);
    rm1.stop(0);
}

TEST_F(MySqlLeaseRoundTripDb, LegacyTwoPartOwnerDecodes)
{
    UnixEpochClock clock;
    RecordingDeliverySink sink1;
    MySQLMessageStore store1(pool(), kCap);
    ReliableMessaging rm1(store1, sink1, clock, kLeaseMs);

    const SessionIdentity alice{kAlice, 1};
    const SessionIdentity bob{kBob, 1};
    AcceptOutcome a = rm1.accept(alice, directTo(kBob, "legacy-owner-1"));
    ASSERT_TRUE(a.ok);

    // 模拟启动前遗留行：lease_owner 为旧格式 "uid:gen"（无 boot id），lease 未过期。
    MySQL conn;
    ASSERT_TRUE(conn.connect("127.0.0.1", "root", MySqlTestFixture::password(), kTestDb, 3306));
    const int64_t nowSecs = clock.nowMs() / 1000;
    ASSERT_TRUE(conn.update(
        "UPDATE MessageDelivery SET state=1, attempt_count=1, "
        "lease_owner='" + std::to_string(kBob.value) + ":1', "
        "lease_until=FROM_UNIXTIME(" + std::to_string(nowSecs + 100) + ") "
        "WHERE message_id=" + std::to_string(a.messageId.value) +
        " AND recipient_id=" + std::to_string(kBob.value)));

    // 读回：bootId=0、owner 两段正确（旧格式 decode）。
    MySQLMessageStore store2(pool(), kCap);
    std::vector<Delivery> rows = store2.deliveriesByMessage(a.messageId);
    ASSERT_EQ(1u, rows.size());
    EXPECT_EQ(DeliveryState::InFlight, rows[0].state);
    EXPECT_EQ(bob, rows[0].leaseOwner);
    EXPECT_EQ(0u, rows[0].leaseBootId);
    EXPECT_GT(rows[0].leaseUntilMs, clock.nowMs());  // lease 未到期（排除到期路径）

    // 跨实例 sessionAvailable：遗留 owner（bootId=0）必不等 -> 未到期 lease 立即回收。
    RecordingDeliverySink sink2;
    ReliableMessaging rm2(store2, sink2, clock, kLeaseMs);
    rm2.sessionAvailable(bob);
    ASSERT_EQ(1u, sink2.attempts().size());
    EXPECT_EQ(a.messageId.value, sink2.attempts()[0].messageId.value);
    rm2.stop(0);
    rm1.stop(0);
}

// P3-08 补缺轮 M2：next_attempt_at 写入必须向上取整（ceil），不得早于写入时刻。
// 现状写 floor(ms/1000)、due 判定 `next_attempt_at <= floor(now/1000)` 使 ack-
// timeout 重投可早至 999ms（不安全方向：在 next_attempt_at 到达前重投）。写入
// nextAttemptAtMs = now + 300ms：读回值必须 >= 写入值；到期扫描在 now 不得把
// 该行判为到期。RED=floor 实现读回早于写入值/提前到期，GREEN=ceil 修复后通过。
TEST_F(MySqlLeaseRoundTripDb, NextAttemptAtNotDueEarlyAcrossStoreInstances)
{
    UnixEpochClock clock;
    RecordingDeliverySink sink1;
    MySQLMessageStore store1(pool(), kCap);
    ReliableMessaging rm1(store1, sink1, clock, kLeaseMs);

    const SessionIdentity alice{kAlice, 1};
    const SessionIdentity bob{kBob, 1};
    AcceptOutcome a = rm1.accept(alice, directTo(kBob, "next-attempt-1"));
    ASSERT_TRUE(a.ok);

    // 进程 1 写入下次重投时刻 = 真实 now + 300ms（DATETIME(0) 秒精度下 floor
    // 写入会把该秒截成 now 的秒，导致读回值早于写入时刻、到期扫描提前命中）。
    const int64_t now = clock.nowMs();
    std::vector<Delivery> rows = store1.deliveriesByMessage(a.messageId);
    ASSERT_EQ(1u, rows.size());
    Delivery write = rows[0];
    write.state = DeliveryState::InFlight;
    write.leaseOwner = bob;
    write.leaseBootId = 0x0B0B;
    write.attemptCount = 1;
    write.leaseUntilMs = now + kEpochLeaseMs;
    write.nextAttemptAtMs = now + 300;
    store1.updateDelivery(write);

    // 新 store 实例读回：持久化的 next_attempt_at 不得早于写入时刻（向上取整）。
    MySQLMessageStore store2(pool(), kCap);
    std::vector<Delivery> readback = store2.deliveriesByMessage(a.messageId);
    ASSERT_EQ(1u, readback.size());
    EXPECT_GE(readback[0].nextAttemptAtMs, write.nextAttemptAtMs)
        << "written " << write.nextAttemptAtMs << " read " << readback[0].nextAttemptAtMs;

    // ack-timeout 到期扫描在 now（写入时刻）不得把该行判为到期（提前重投）。
    std::vector<Delivery> due = store2.deliveriesDueForRetry(now, kCap);
    for (size_t i = 0; i < due.size(); ++i) {
        EXPECT_NE(a.messageId.value, due[i].messageId.value)
            << "delivery due for retry before nextAttemptAtMs=" << write.nextAttemptAtMs;
    }
    rm1.stop(0);
}

// P3-08 retention 前提：accept 时注入的 expiresAtMs 必须随 Delivery 行持久化
// （否则 MySQL 侧 expireDeliveries 的 `expires_at IS NOT NULL` 永不命中，Expired
// 转移与 acked/expired retention cleanup 在真实库上静默失效）。
TEST_F(MySqlLeaseRoundTripDb, ExpiresAtPersistsFromAccept)
{
    UnixEpochClock clock;
    RecordingDeliverySink sink;
    MySQLMessageStore store1(pool(), kCap);
    ReliableMessaging rm1(store1, sink, clock, kLeaseMs);  // 默认 retention（7 天）

    const SessionIdentity alice{kAlice, 1};
    const SessionIdentity bob{kBob, 1};
    AcceptOutcome a = rm1.accept(alice, directTo(kBob, "expires-persist-1"));
    ASSERT_TRUE(a.ok);

    // 基准 = accept 写入后 store1 读回的写入值（消除双时钟读数竞态：测试独立
    // now()+retention 与 accept 内部 now()+retention 两次真实时钟读数偏差，叠加
    // DATETIME(0) 秒截断可越过 1000ms 容差——×20 压力第 14 轮 10ms 边界失败）。
    std::vector<Delivery> writeRows = store1.deliveriesByMessage(a.messageId);
    ASSERT_EQ(1u, writeRows.size());
    const int64_t expectedExpiresMs = writeRows[0].expiresAtMs;

    // 新 store 实例读回：expiresAtMs 与写入值差 <=1000ms（秒精度）。
    MySQLMessageStore store2(pool(), kCap);
    std::vector<Delivery> rows = store2.deliveriesByMessage(a.messageId);
    ASSERT_EQ(1u, rows.size());
    EXPECT_NE(0, rows[0].expiresAtMs);  // RED：insertDelivery 未持久化 expires_at
    EXPECT_GE(rows[0].expiresAtMs, expectedExpiresMs - 1000);
    EXPECT_LE(rows[0].expiresAtMs, expectedExpiresMs + 1000);
    rm1.stop(0);
}
