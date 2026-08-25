// P5-03C OutboxBatchMarkContract RED→GREEN：批量 processed 标记 seam 契约
// （docs/tasks/P5-03C.md RED 节）。RED 引用尚不存在的
// `MessageStore::markOutboxEventsProcessed(const std::vector<uint64_t>&, int64_t)`
// → 编译失败（missing 成员）即合法 RED。
//
// 非构建目标（P5-03C FAILED/reverted；保留为候选规格）：批量 processed 标记
// 已接线落地（N→1 双证据成立、GREEN 全绿），但 reliable-direct after +5.84%
// 未达 ≥15% 主门槛且 CI 前后重叠 → 实现已 revert、本测试目标从 tests/CMakeLists.txt
// 构建集移除（沿 P5-03A 先例），文件保留为下次 accept 事务合并类削减候选的契约规格。
//
// 本文件只穿公开 interface（MessageStore outbox port + OutboxPublisher port +
// ReliableMessaging accept + LocalOutboxRelay runScan），不读私有容器。双 adapter
// 契约：InMemory（OutboxBatchMarkContractTest suite）+ 真实 MySQL
// （OutboxBatchMarkMySqlDb suite，chat_test 库，不 skip）。
//
// 冻结契约（GREEN 据此实现，卡「最小实现」节）：
// - MessageStore 新增虚方法
//     virtual void markOutboxEventsProcessed(const std::vector<uint64_t>& eventIds,
//                                            int64_t nowMs);
//   默认回退逐条 markOutboxProcessed（既有测试替身零伴改）。
// - InMemory：锁内循环等价，且带 `processedAtMs == 0` 守卫（与 SQL
//   `AND processed_at IS NULL` 一致的竞争幂等：已 processed 行绝不覆盖时间戳）。
// - MySQL：单条 `UPDATE OutboxEvent SET processed_at=FROM_UNIXTIME(?),
//   lease_owner=NULL, lease_until=NULL WHERE id IN (…) AND processed_at IS NULL`
//   （prepared 绑定，批大小 ≤ claimBatchSize=100）。
// - 批量标记绝不得参与 HOL/claimFor 判定；lease 语义不变（成功才标 processed，
//   失败事件保持未 processed、lease 保留由到期驱动重领——at-least-once 不破）。

#include "app/Clock.hpp"
#include "app/DomainTypes.hpp"
#include "app/InMemoryMessageStore.hpp"
#include "app/LocalOutboxRelay.hpp"
#include "app/LocalWakeupPublisher.hpp"
#include "app/MessageStore.hpp"
#include "app/MySQLMessageStore.hpp"
#include "app/OutboxPublisher.hpp"
#include "app/ReliableMessaging.hpp"

#include "FakeClock.hpp"
#include "MySqlTestFixture.hpp"
#include "RecordingDeliverySink.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

const UserId kAlice{1};
const UserId kBob{2};
const int64_t kNow = 1000000;
// Delivery lease 显著大于 outbox lease（测试注入 1000ms）：重放时 Delivery 仍在
// lease 内被 fencing（幂等 wakeup 依赖，沿 OutboxRelayTest 冻结参数）。
const uint64_t kLeaseMs = 10000;
const uint64_t kFanOutCap = 100;

RetryConfig outboxRetryConfig()
{
    RetryConfig c;
    c.cleanupCycleMs = 0;  // 不触发 Expired/cleanup，隔离 outbox 关注点
    return c;
}

OutboxConfig testOutboxConfig()
{
    OutboxConfig c;
    c.claimBatchSize = 10;
    c.scanIntervalMs = 10;
    c.claimLeaseMs = 1000;
    return c;
}

SendMessageCommand directTo(UserId recipient, const std::string& cmid, const std::string& content)
{
    SendMessageCommand cmd;
    cmd.clientMessageId = ClientMessageId(cmid);
    cmd.kind = SendMessageCommand::Kind::Direct;
    cmd.directRecipient = recipient;
    cmd.content = content;
    return cmd;
}

// 种子事件：accept 写入 n 个 outbox 事件（双 adapter 同构：insertMessage 同点写
// OutboxEvent，available_at=now），随后 claim（attempt+1、lease 写入），返回事件
// id（claim 顺序 = id 顺序）。claim 失败（种子缺行/存储故障）经 EXPECT 暴露，
// 由下游断言放大。
std::vector<uint64_t> seedAndClaim(ReliableMessaging& rm, MessageStore& store, Clock& clock,
                                   int n, const std::string& prefix)
{
    const SessionIdentity alice{kAlice, 1};
    for (int i = 0; i < n; ++i) {
        AcceptOutcome a = rm.accept(alice, directTo(kBob, prefix + "-" + std::to_string(i), "x"));
        EXPECT_TRUE(a.ok) << prefix << "-" << i;
    }
    const int64_t now = clock.nowMs();
    std::vector<OutboxEvent> claimed =
        store.claimOutboxEvents(now, "mark-owner", now + 10000, static_cast<uint64_t>(n));
    std::vector<uint64_t> ids;
    ids.reserve(claimed.size());
    for (size_t i = 0; i < claimed.size(); ++i) {
        EXPECT_EQ(1u, claimed[i].attemptCount);
        ids.push_back(claimed[i].id);
    }
    EXPECT_EQ(static_cast<size_t>(n), ids.size()) << prefix;
    return ids;
}

void expectProcessed(MessageStore& store, uint64_t eventId)
{
    std::shared_ptr<const OutboxEvent> e = store.findOutboxEvent(eventId);
    ASSERT_TRUE(e) << "event " << eventId;
    EXPECT_NE(0, e->processedAtMs) << "event " << eventId << " must be processed";
    EXPECT_TRUE(e->leaseOwner.empty()) << "processed event lease must be released";
    EXPECT_EQ(0, e->leaseUntilMs);
    EXPECT_EQ(1u, e->attemptCount);
}

void expectUnprocessed(MessageStore& store, uint64_t eventId)
{
    std::shared_ptr<const OutboxEvent> e = store.findOutboxEvent(eventId);
    ASSERT_TRUE(e) << "event " << eventId;
    EXPECT_EQ(0, e->processedAtMs) << "event " << eventId << " must stay unprocessed";
    // 失败/未标记事件：lease 保留，到期可重领（at-least-once 语义不破）。
    EXPECT_FALSE(e->leaseOwner.empty()) << "unprocessed event must keep its lease";
    EXPECT_NE(0, e->leaseUntilMs);
}

// ---- 批量 processed 标记契约（双 adapter 共用；时钟确定性由调用方保证）----

// MarkBatchMarksAll：一批 N 事件一次批量标记全部 processed，无遗漏。
void runMarkBatchMarksAll(ReliableMessaging& rm, MessageStore& store, Clock& clock)
{
    std::vector<uint64_t> ids = seedAndClaim(rm, store, clock, 5, "marks-all");
    store.markOutboxEventsProcessed(ids, clock.nowMs());
    for (size_t i = 0; i < ids.size(); ++i) {
        expectProcessed(store, ids[i]);
    }
    EXPECT_EQ(0u, store.countUnprocessedOutboxEvents());
}

// MarkBatchIdempotent：重复批量无副作用、时间戳不重置（已 processed 行不被覆盖）。
void runMarkBatchIdempotent(ReliableMessaging& rm, MessageStore& store, Clock& clock)
{
    std::vector<uint64_t> ids = seedAndClaim(rm, store, clock, 3, "idem");
    const int64_t t1 = clock.nowMs();
    store.markOutboxEventsProcessed(ids, t1);
    std::vector<int64_t> firstMarked;
    for (size_t i = 0; i < ids.size(); ++i) {
        std::shared_ptr<const OutboxEvent> e = store.findOutboxEvent(ids[i]);
        ASSERT_TRUE(e);
        EXPECT_NE(0, e->processedAtMs);
        firstMarked.push_back(e->processedAtMs);
    }
    const int64_t t2 = t1 + 1500;  // 跨 DATETIME(0) 秒边界（MySQL 秒精度可分辨）
    store.markOutboxEventsProcessed(ids, t2);
    for (size_t i = 0; i < ids.size(); ++i) {
        std::shared_ptr<const OutboxEvent> e = store.findOutboxEvent(ids[i]);
        ASSERT_TRUE(e);
        EXPECT_EQ(firstMarked[i], e->processedAtMs)
            << "repeated batch must not reset already-processed timestamp";
        EXPECT_TRUE(e->leaseOwner.empty());
        EXPECT_EQ(0, e->leaseUntilMs);
        EXPECT_EQ(1u, e->attemptCount);
    }
}

// MarkBatchPartialIdsValid：批中含已 processed/未 processed 混合 → 只标未处理，
// 已 processed 行不被覆盖；未入批的未处理事件保持未 processed、lease 保留。
void runMarkBatchPartialIdsValid(ReliableMessaging& rm, MessageStore& store, Clock& clock)
{
    std::vector<uint64_t> ids = seedAndClaim(rm, store, clock, 4, "partial");
    store.markOutboxEventsProcessed(ids, clock.nowMs());
    std::vector<int64_t> already;
    for (size_t i = 0; i < ids.size(); ++i) {
        std::shared_ptr<const OutboxEvent> e = store.findOutboxEvent(ids[i]);
        ASSERT_TRUE(e);
        already.push_back(e->processedAtMs);
    }
    std::vector<uint64_t> more = seedAndClaim(rm, store, clock, 2, "partial-more");
    std::vector<uint64_t> mix;
    mix.push_back(ids[0]);    // 已 processed
    mix.push_back(more[0]);   // 未 processed
    store.markOutboxEventsProcessed(mix, clock.nowMs());
    // 已 processed 行不被覆盖（时间戳不变）。
    std::shared_ptr<const OutboxEvent> e0 = store.findOutboxEvent(ids[0]);
    ASSERT_TRUE(e0);
    EXPECT_EQ(already[0], e0->processedAtMs);
    // 批内未处理行被标记；未入批的未处理行保持未 processed、lease 保留。
    expectProcessed(store, more[0]);
    expectUnprocessed(store, more[1]);
    // 已 processed 且不在本批中的行不受影响。
    expectProcessed(store, ids[1]);
}

// MarkBatchUnknownIdsNoOp：不存在的 id 无副作用（不崩溃、不影响其它行）。
void runMarkBatchUnknownIdsNoOp(ReliableMessaging& rm, MessageStore& store, Clock& clock)
{
    std::vector<uint64_t> ids = seedAndClaim(rm, store, clock, 2, "unknown");
    std::vector<uint64_t> mix;
    mix.push_back(ids[0]);
    mix.push_back(1000000000000ULL);  // 不存在
    mix.push_back(ids[1]);
    mix.push_back(2000000000000ULL);  // 不存在
    store.markOutboxEventsProcessed(mix, clock.nowMs());
    expectProcessed(store, ids[0]);
    expectProcessed(store, ids[1]);
    EXPECT_EQ(0u, store.countUnprocessedOutboxEvents());
}

// MarkBatchEquivalentToIndividual：批量 vs 逐条 markOutboxProcessed 对 outbox 状态
// 机结果完全一致（processed/lease/attempt 形状一致，同 now 时间戳一致）。
void runMarkBatchEquivalentToIndividual(ReliableMessaging& rm, MessageStore& store, Clock& clock)
{
    std::vector<uint64_t> ids = seedAndClaim(rm, store, clock, 6, "equiv");
    std::vector<uint64_t> batch(ids.begin(), ids.begin() + 3);
    const int64_t now = clock.nowMs();
    store.markOutboxEventsProcessed(batch, now);
    for (size_t i = 3; i < 6; ++i) {
        store.markOutboxProcessed(ids[i], now);
    }
    for (size_t i = 0; i < ids.size(); ++i) {
        expectProcessed(store, ids[i]);
    }
    // 批量 vs 逐条：processed_at（同 now 秒精度一致）与 attempt_count 完全一致。
    std::shared_ptr<const OutboxEvent> b = store.findOutboxEvent(ids[0]);
    std::shared_ptr<const OutboxEvent> in = store.findOutboxEvent(ids[3]);
    ASSERT_TRUE(b);
    ASSERT_TRUE(in);
    EXPECT_EQ(b->processedAtMs, in->processedAtMs);
    EXPECT_EQ(b->attemptCount, in->attemptCount);
}

// MarkBatchPartialSubset（卡 BatchMarkPartialSafety）：只传 ok 子集 id → 仅子集被
// 标记；失败事件保持未 processed、lease 保留由到期驱动重领（at-least-once 不破）。
void runMarkBatchPartialSubset(ReliableMessaging& rm, MessageStore& store, Clock& clock)
{
    std::vector<uint64_t> ids = seedAndClaim(rm, store, clock, 3, "subset");
    std::vector<uint64_t> ok(ids.begin(), ids.begin() + 2);  // 仅 ok 子集
    store.markOutboxEventsProcessed(ok, clock.nowMs());
    expectProcessed(store, ids[0]);
    expectProcessed(store, ids[1]);
    expectUnprocessed(store, ids[2]);
}

// MarkBatchBoundedByClaimBatchSize：批大小 ≤ claimBatchSize（生产默认 100）有界，
// 一次批量标记完整 claim 批全部生效。
void runMarkBatchBoundedByClaimBatchSize(ReliableMessaging& rm, MessageStore& store, Clock& clock)
{
    const int kBatch = 100;  // OutboxConfig::claimBatchSize 生产默认
    std::vector<uint64_t> ids = seedAndClaim(rm, store, clock, kBatch, "bounded");
    ASSERT_EQ(static_cast<size_t>(kBatch), ids.size());
    store.markOutboxEventsProcessed(ids, clock.nowMs());
    for (size_t i = 0; i < ids.size(); ++i) {
        expectProcessed(store, ids[i]);
    }
    EXPECT_EQ(0u, store.countUnprocessedOutboxEvents());
}

// MarkBatchConcurrentSafe：两并发批（重叠全集）标记同一批 → 无丢失（全部
// processed）无重复（单行终态、无残留未处理）。storeA/storeB 为两 store 实例：
// InMemory 传同一实例（GREEN 锁内串行化）；MySQL 传共享 pool 的两实例（SQL 守卫
// + InnoDB 行锁跨实例串行化——真实 relay 竞争形态）。
void runMarkBatchConcurrentSafe(MessageStore& storeA, MessageStore& storeB,
                                ReliableMessaging& rm, Clock& clock)
{
    std::vector<uint64_t> ids = seedAndClaim(rm, storeA, clock, 8, "concurrent");
    const int64_t now = clock.nowMs();
    std::vector<std::thread> threads;
    threads.emplace_back([&storeA, &ids, now] { storeA.markOutboxEventsProcessed(ids, now); });
    threads.emplace_back([&storeB, &ids, now] { storeB.markOutboxEventsProcessed(ids, now); });
    for (size_t t = 0; t < threads.size(); ++t) {
        threads[t].join();
    }
    for (size_t i = 0; i < ids.size(); ++i) {
        std::shared_ptr<const OutboxEvent> e = storeA.findOutboxEvent(ids[i]);
        ASSERT_TRUE(e);
        EXPECT_NE(0, e->processedAtMs) << "no loss under concurrent batch mark";
        EXPECT_TRUE(e->leaseOwner.empty());
        EXPECT_EQ(0, e->leaseUntilMs);
    }
    EXPECT_EQ(0u, storeA.countUnprocessedOutboxEvents());
}

// ---- InMemory adapter 契约（OutboxBatchMarkContractTest suite）----

struct OutboxBatchHarness {
    FakeClock clock;
    InMemoryMessageStore store;
    RecordingDeliverySink sink;
    ReliableMessaging rm;

    OutboxBatchHarness() : rm(store, sink, clock, kLeaseMs, outboxRetryConfig())
    {
        clock.set(kNow);
    }
};

TEST(OutboxBatchMarkContractTest, MarkBatchMarksAll)
{
    OutboxBatchHarness h;
    runMarkBatchMarksAll(h.rm, h.store, h.clock);
}

TEST(OutboxBatchMarkContractTest, MarkBatchIdempotent)
{
    OutboxBatchHarness h;
    runMarkBatchIdempotent(h.rm, h.store, h.clock);
}

TEST(OutboxBatchMarkContractTest, MarkBatchPartialIdsValid)
{
    OutboxBatchHarness h;
    runMarkBatchPartialIdsValid(h.rm, h.store, h.clock);
}

TEST(OutboxBatchMarkContractTest, MarkBatchUnknownIdsNoOp)
{
    OutboxBatchHarness h;
    runMarkBatchUnknownIdsNoOp(h.rm, h.store, h.clock);
}

TEST(OutboxBatchMarkContractTest, MarkBatchEquivalentToIndividual)
{
    OutboxBatchHarness h;
    runMarkBatchEquivalentToIndividual(h.rm, h.store, h.clock);
}

TEST(OutboxBatchMarkContractTest, MarkBatchPartialSubset)
{
    OutboxBatchHarness h;
    runMarkBatchPartialSubset(h.rm, h.store, h.clock);
}

TEST(OutboxBatchMarkContractTest, MarkBatchBoundedByClaimBatchSize)
{
    OutboxBatchHarness h;
    runMarkBatchBoundedByClaimBatchSize(h.rm, h.store, h.clock);
}

TEST(OutboxBatchMarkContractTest, MarkBatchConcurrentSafe)
{
    OutboxBatchHarness h;
    runMarkBatchConcurrentSafe(h.store, h.store, h.rm, h.clock);
}

// ---- 接线落地证据契约定点（卡「接线落地证据」）：同一 scan 批 N 个 ok 事件 →
// store 的批量 mark 调用恰 1 次、逐条 mark 0 次（防 P5-03A「实现未接线」复发）----

// 计数 MessageStore 装饰器（ReliableMessagingContractTest::ThrowOnceOnUpdateStore
// 同模式）：只计数 mark 两 seam，其余委托 InMemoryMessageStore。
class CountingMarkStore : public MessageStore {
public:
    std::shared_ptr<const Message> findAccepted(const ClientMessageId& id, UserId sender) override
    {
        return delegate_.findAccepted(id, sender);
    }
    ConversationId getOrCreateConversation(const SessionIdentity& sender,
                                           const SendMessageCommand& cmd) override
    {
        return delegate_.getOrCreateConversation(sender, cmd);
    }
    Message insertMessage(const Message& draft) override { return delegate_.insertMessage(draft); }
    void insertDelivery(const Delivery& delivery) override { delegate_.insertDelivery(delivery); }
    void updateDelivery(const Delivery& delivery) override { delegate_.updateDelivery(delivery); }
    std::vector<Delivery> deliveriesByRecipient(UserId recipient) override
    {
        return delegate_.deliveriesByRecipient(recipient);
    }
    std::vector<Delivery> deliveriesByMessage(MessageId messageId) override
    {
        return delegate_.deliveriesByMessage(messageId);
    }
    std::shared_ptr<const Message> findMessage(MessageId messageId) override
    {
        return delegate_.findMessage(messageId);
    }
    std::vector<OutboxEvent> claimOutboxEvents(int64_t nowMs, const std::string& leaseOwner,
                                               int64_t leaseUntilMs, uint64_t limit) override
    {
        return delegate_.claimOutboxEvents(nowMs, leaseOwner, leaseUntilMs, limit);
    }
    void markOutboxProcessed(uint64_t eventId, int64_t nowMs) override
    {
        ++individualCalls_;
        delegate_.markOutboxProcessed(eventId, nowMs);
    }
    void markOutboxEventsProcessed(const std::vector<uint64_t>& ids, int64_t nowMs) override
    {
        ++batchCalls_;
        delegate_.markOutboxEventsProcessed(ids, nowMs);
    }
    void markOutboxPoisoned(uint64_t eventId, int64_t nowMs) override
    {
        delegate_.markOutboxPoisoned(eventId, nowMs);
    }
    std::shared_ptr<const OutboxEvent> findOutboxEvent(uint64_t eventId) override
    {
        return delegate_.findOutboxEvent(eventId);
    }
    std::vector<OutboxEvent> poisonedOutboxEvents(uint64_t limit) override
    {
        return delegate_.poisonedOutboxEvents(limit);
    }
    uint64_t countUnprocessedOutboxEvents() override
    {
        return delegate_.countUnprocessedOutboxEvents();
    }

    size_t batchCalls() const { return batchCalls_; }
    size_t individualCalls() const { return individualCalls_; }

private:
    InMemoryMessageStore delegate_;
    size_t batchCalls_ = 0;
    size_t individualCalls_ = 0;
};

// P4-03：relay 出口为 OutboxPublisher port。测试 publisher（生产 LocalWakeupPublisher
// 语义 + 记录，沿 OutboxRelayTest 同构）：记录每次 publish 请求；默认全成功。
class RecordingWakeupPublisher : public OutboxPublisher {
public:
    explicit RecordingWakeupPublisher(MessageStore& store) : store_(store) {}

    std::vector<OutboxPublishOutcome> publish(
        const std::vector<OutboxPublishRequest>& batch, int64_t deadlineMs) override
    {
        (void)deadlineMs;
        std::vector<OutboxPublishOutcome> out;
        out.reserve(batch.size());
        for (size_t i = 0; i < batch.size(); ++i) {
            const OutboxPublishRequest& req = batch[i];
            requests_.push_back(req);
            OutboxPublishOutcome oc;
            const std::shared_ptr<const Message> msg =
                store_.findMessage(req.event.aggregateMessageId);
            if (!msg) {
                oc.failure = PublishFailure::Other;
                oc.error = "outbox event without message";
            } else {
                oc.ok = true;
                oc.failure = PublishFailure::None;
            }
            out.push_back(oc);
        }
        return out;
    }

    const std::vector<OutboxPublishRequest>& requests() const { return requests_; }

private:
    MessageStore& store_;
    std::vector<OutboxPublishRequest> requests_;
};

TEST(OutboxBatchMarkContractTest, RelayMarkBatchCalledOnceForBatch)
{
    FakeClock clock;
    clock.set(kNow);
    CountingMarkStore store;
    RecordingDeliverySink sink;
    ReliableMessaging rm(store, sink, clock, kLeaseMs, outboxRetryConfig());
    RecordingWakeupPublisher pub(store);
    LocalOutboxRelay relay(store, pub, clock, testOutboxConfig());

    const SessionIdentity alice{kAlice, 1};
    for (int i = 0; i < 3; ++i) {
        AcceptOutcome a = rm.accept(alice, directTo(kBob, "relay-batch-" + std::to_string(i), "x"));
        ASSERT_TRUE(a.ok);
    }
    ASSERT_EQ(3, relay.runScan());
    ASSERT_EQ(3u, pub.requests().size());

    // 接线落地证据：整批 ok 事件 → 批量 mark 恰 1 次，逐条 mark 0 次。
    EXPECT_EQ(1u, store.batchCalls());
    EXPECT_EQ(0u, store.individualCalls());
    EXPECT_EQ(0u, store.countUnprocessedOutboxEvents());
}

// ---- MySQL adapter 契约（OutboxBatchMarkMySqlDb suite，chat_test 库，不 skip）----

class OutboxBatchMarkMySqlDb : public ::testing::Test {
protected:
    static void SetUpTestSuite()
    {
        MySqlTestFixture::resetSchema();
        MySQL admin;
        // 沿 BatchedAckContractMySqlTest 既有模式：seed User 需选中 chat_test 库
        // （MessageDelivery/OutboxEvent 有 User FK）。
        ASSERT_TRUE(admin.connect("127.0.0.1", "root", MySqlTestFixture::password(),
                                  "chat_test", 3306));
        ASSERT_TRUE(admin.update("INSERT INTO User(id,name,password) VALUES (1,'u1','x'),"
                                 "(2,'u2','x')"));
        (void)MySqlTestFixture::pool();
    }

    // 用例间隔离：清空可靠消息表（ChatMessage 级联 MessageDelivery/OutboxEvent；
    // 再删链接表与 Conversation），使前用例遗留行不干扰后用例 claim。
    void TearDown() override
    {
        MySQL conn;
        ASSERT_TRUE(conn.connect("127.0.0.1", "root", MySqlTestFixture::password(), "chat_test",
                                 3306));
        ASSERT_TRUE(conn.update("DELETE FROM ChatMessage"));
        ASSERT_TRUE(conn.update("DELETE FROM DirectConversation"));
        ASSERT_TRUE(conn.update("DELETE FROM GroupConversation"));
        ASSERT_TRUE(conn.update("DELETE FROM Conversation"));
    }
};

TEST_F(OutboxBatchMarkMySqlDb, MarkBatchMarksAll)
{
    UnixEpochClock clock;
    RecordingDeliverySink sink;
    MySQLMessageStore store(MySqlTestFixture::pool(), kFanOutCap);
    ReliableMessaging rm(store, sink, clock, kLeaseMs);
    runMarkBatchMarksAll(rm, store, clock);
    rm.stop(0);
}

TEST_F(OutboxBatchMarkMySqlDb, MarkBatchIdempotent)
{
    UnixEpochClock clock;
    RecordingDeliverySink sink;
    MySQLMessageStore store(MySqlTestFixture::pool(), kFanOutCap);
    ReliableMessaging rm(store, sink, clock, kLeaseMs);
    runMarkBatchIdempotent(rm, store, clock);
    rm.stop(0);
}

TEST_F(OutboxBatchMarkMySqlDb, MarkBatchPartialIdsValid)
{
    UnixEpochClock clock;
    RecordingDeliverySink sink;
    MySQLMessageStore store(MySqlTestFixture::pool(), kFanOutCap);
    ReliableMessaging rm(store, sink, clock, kLeaseMs);
    runMarkBatchPartialIdsValid(rm, store, clock);
    rm.stop(0);
}

TEST_F(OutboxBatchMarkMySqlDb, MarkBatchUnknownIdsNoOp)
{
    UnixEpochClock clock;
    RecordingDeliverySink sink;
    MySQLMessageStore store(MySqlTestFixture::pool(), kFanOutCap);
    ReliableMessaging rm(store, sink, clock, kLeaseMs);
    runMarkBatchUnknownIdsNoOp(rm, store, clock);
    rm.stop(0);
}

TEST_F(OutboxBatchMarkMySqlDb, MarkBatchEquivalentToIndividual)
{
    UnixEpochClock clock;
    RecordingDeliverySink sink;
    MySQLMessageStore store(MySqlTestFixture::pool(), kFanOutCap);
    ReliableMessaging rm(store, sink, clock, kLeaseMs);
    runMarkBatchEquivalentToIndividual(rm, store, clock);
    rm.stop(0);
}

TEST_F(OutboxBatchMarkMySqlDb, MarkBatchPartialSubset)
{
    UnixEpochClock clock;
    RecordingDeliverySink sink;
    MySQLMessageStore store(MySqlTestFixture::pool(), kFanOutCap);
    ReliableMessaging rm(store, sink, clock, kLeaseMs);
    runMarkBatchPartialSubset(rm, store, clock);
    rm.stop(0);
}

TEST_F(OutboxBatchMarkMySqlDb, MarkBatchConcurrentSafe)
{
    UnixEpochClock clock;
    RecordingDeliverySink sink;
    MySQLMessageStore store(MySqlTestFixture::pool(), kFanOutCap);
    ReliableMessaging rm(store, sink, clock, kLeaseMs);
    // 两实例共享 pool + chat_test 库：SQL 守卫 + InnoDB 行锁跨实例串行化（真实
    // relay 竞争形态）。
    MySQLMessageStore store2(MySqlTestFixture::pool(), kFanOutCap);
    runMarkBatchConcurrentSafe(store, store2, rm, clock);
    rm.stop(0);
}

} // namespace