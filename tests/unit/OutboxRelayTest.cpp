// P3-09 OutboxRelay RED→GREEN：穿过 LocalOutboxRelay 公开 interface +
// MessageStore outbox port + FakeClock + RecordingPublisher + InMemoryMessageStore，
// 无固定 sleep、不读私有容器。
//
// P4-03 伴改：relay 出口从 MessageAcceptedConsumer 替换为 OutboxPublisher port；
// RecordingOutboxConsumer/ScriptedOutboxConsumer 伴改为本文件内的
// RecordingWakeupPublisher（记录 publish 请求与派生接收者——接收者断言经
// LocalWakeupPublisher 同构的 outboxRecipientsFor 派生保留；可脚本化注入失败）。
//
// RED 依据（现状）：docs/tasks/P3-09.md §Interface——尚无 LocalOutboxRelay /
// OutboxConfig / OutboxEvent 值类型，MessageStore 亦无
// claimOutboxEvents/markOutboxProcessed/markOutboxPoisoned/findOutboxEvent/
// poisonedOutboxEvents 端口；本文件引用这些尚不存在的类型与方法 → 编译失败即合法 RED。
//
// 本文件冻结的实现契约（GREEN 据此实现）：
// - chatserver/include/app/DomainTypes.hpp 增加 OutboxEvent 值类型：
//     struct OutboxEvent {
//         uint64_t id;                 // store 分配的事件 id
//         MessageId aggregateMessageId; // 关联 ChatMessage.id
//         std::string eventType;        // "MessageAccepted"
//         std::string payload;          // P3-04 冻结的命令快照 JSON
//         int64_t availableAtMs;        // 可见时刻（claim 判定 available_at<=now）
//         std::string leaseOwner;       // "" = 未租用
//         int64_t leaseUntilMs;         // 0 = 未租用
//         uint32_t attemptCount;        // 每次 claim +1
//         int64_t processedAtMs;        // 0 = 未处理（处理成功才写）
//     };
// - chatserver/include/app/MessageStore.hpp 增加 outbox port（默认空实现，沿用
//   deliveriesDueForRetry 模式，避免既有测试替身如 ReliableMessagingContractTest::
//   ThrowOnceOnUpdateStore 编译失败；InMemory/MySQL 双 adapter override）：
//     claimOutboxEvents(nowMs, leaseOwner, leaseUntilMs, limit)
//         → processedAt==0 && availableAt<=nowMs && (leaseOwner 空 || leaseUntil<=nowMs)
//           的行写 owner+lease_until、attempt_count+1，LIMIT limit 返回；
//     markOutboxProcessed(eventId, nowMs);      // 处理成功后才调用
//     markOutboxPoisoned(eventId, nowMs);       // port 保留（P3-12 精确指标）；
//                                              // relay 不调用——处理失败统一保持未 processed
//     findOutboxEvent(eventId);                 // shared_ptr<const OutboxEvent>
//     poisonedOutboxEvents(limit);              // poison 可查询谓词
// - chatserver/include/app/OutboxPublisher.hpp 增加 OutboxPublisher port：
//     OutboxPublishRequest { event, conversationId, sequence, topic }
//     OutboxPublishOutcome { ok, failure, error }（逐事件结果）
//     OutboxPublisher::publish(batch, deadlineMs) → 逐事件结果（不抛、不写库）
// - chatserver/include/app/LocalOutboxRelay.hpp 增加：
//     OutboxConfig { claimBatchSize=100, scanIntervalMs=5000, claimLeaseMs=30000 }
//       （冻结参数生产默认，见 P3-09 §冻结参数；测试注入小值）
//     LocalOutboxRelay(MessageStore&, OutboxPublisher&, Clock&, const OutboxConfig&)
//       （P4-03：relay 出口为 OutboxPublisher port；逐事件结果 ok 才标 processed，
//        失败保持未 processed、lease 到期重领重试）
//     start() / stop(int64_t deadlineMs) / runScan()（单轮扫描，返回本轮 claim 数）
//     每个 relay 实例须有唯一 leaseOwner（实例标识），使并发 relay 经 lease 竞争。
//
// 事件种子经 ReliableMessaging::accept 写入（GREEN 使 InMemory 在 insertMessage
// 同点写 OutboxEvent，与 MySQLMessageStore 事务内写入对称；available_at=nowMs）。

#include "app/Clock.hpp"
#include "app/DeliverySink.hpp"
#include "app/DomainTypes.hpp"
#include "app/InMemoryMessageStore.hpp"
#include "app/LocalOutboxRelay.hpp"
#include "app/LocalWakeupPublisher.hpp"
#include "app/MessageStore.hpp"
#include "app/OutboxPublisher.hpp"
#include "app/ReliableMessaging.hpp"

#include "FakeClock.hpp"
#include "RecordingDeliverySink.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

const UserId kAlice{1};
const UserId kBob{2};
const int64_t kNow = 1000000;
// Delivery lease（DeliveryCoordinator）：显著大于 outbox lease（测试注入 1000ms），
// 保证重放时 Delivery 仍在 lease 内被 fencing（P3-09 幂等 wakeup 依赖）。
const uint64_t kLeaseMs = 10000;

// P3-09 冻结参数（测试注入小值，不得成为生产默认）。
OutboxConfig testOutboxConfig()
{
    OutboxConfig c;
    c.claimBatchSize = 10;
    c.scanIntervalMs = 10;
    c.claimLeaseMs = 1000;
    return c;
}

RetryConfig outboxRetryConfig()
{
    RetryConfig c;
    c.cleanupCycleMs = 0;  // 不触发 Expired/cleanup，隔离 outbox 关注点
    return c;
}

struct OutboxHarness {
    FakeClock clock;
    InMemoryMessageStore store;
    RecordingDeliverySink sink;
    ReliableMessaging rm;
    OutboxConfig outbox;

    OutboxHarness() : rm(store, sink, clock, kLeaseMs, outboxRetryConfig()), outbox(testOutboxConfig())
    {
        clock.set(kNow);
    }
};

SendMessageCommand directTo(UserId recipient, const std::string& cmid, const std::string& content)
{
    SendMessageCommand cmd;
    cmd.clientMessageId = ClientMessageId(cmid);
    cmd.kind = SendMessageCommand::Kind::Direct;
    cmd.directRecipient = recipient;
    cmd.content = content;
    return cmd;
}

// P4-03：relay 出口为 OutboxPublisher port。测试 publisher adapter（生产
// LocalWakeupPublisher 语义 + 记录）：记录每次 publish 请求与派生接收者（从
// Message 命令派生，与 LocalWakeupPublisher 同构的 outboxRecipientsFor）；默认全
// 成功，可脚本化注入失败（poisonOn，按 aggregateMessageId），失败不阻断同批。
class RecordingWakeupPublisher : public OutboxPublisher {
public:
    explicit RecordingWakeupPublisher(MessageStore& store) : store_(store) {}

    void poisonOn(uint64_t aggregateMessageId) { poison_.insert(aggregateMessageId); }
    void poisonOff(uint64_t aggregateMessageId) { poison_.erase(aggregateMessageId); }

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
            if (poison_.count(req.event.aggregateMessageId.value) != 0) {
                oc.failure = PublishFailure::Other;
                oc.error = "scripted poison";
            } else {
                const std::shared_ptr<const Message> msg =
                    store_.findMessage(req.event.aggregateMessageId);
                if (!msg) {
                    oc.failure = PublishFailure::Other;
                    oc.error = "outbox event without message";
                } else {
                    recipients_.push_back(outboxRecipientsFor(msg->command));
                    ++wakeups_;
                    oc.ok = true;
                    oc.failure = PublishFailure::None;
                }
            }
            out.push_back(oc);
        }
        return out;
    }

    size_t wakeups() const { return wakeups_; }
    const std::vector<OutboxPublishRequest>& requests() const { return requests_; }
    const OutboxEvent& lastEvent() const { return requests_[requests_.size() - 1].event; }
    const std::vector<UserId>& lastRecipients() const
    {
        return recipients_[recipients_.size() - 1];
    }

private:
    MessageStore& store_;
    std::set<uint64_t> poison_;
    std::vector<OutboxPublishRequest> requests_;
    std::vector<std::vector<UserId> > recipients_;
    size_t wakeups_ = 0;
};

// RED 1：claim 只返回 available_at<=now 的未处理事件（含 lease 到期可重领），
// batch 有界（limit 封顶）。现状无 claimOutboxEvents/OutboxEvent → 编译失败。
TEST(OutboxRelayTest, ClaimBatchReturnsDueEvents)
{
    OutboxHarness h;
    const SessionIdentity alice{kAlice, 1};
    const int64_t t0 = h.clock.nowMs();

    AcceptOutcome a1 = h.rm.accept(alice, directTo(kBob, "due-1", "hello"));
    ASSERT_TRUE(a1.ok);
    h.clock.advance(1000);
    AcceptOutcome a2 = h.rm.accept(alice, directTo(kBob, "due-2", "world"));
    ASSERT_TRUE(a2.ok);

    // t0：只有 available_at(=t0)<=now 的 a1 到期；a2(available_at=t0+1000) 未来。
    h.clock.set(t0);
    std::vector<OutboxEvent> due =
        h.store.claimOutboxEvents(t0, "owner-1", t0 + h.outbox.claimLeaseMs, 10);
    ASSERT_EQ(1u, due.size());
    EXPECT_EQ(a1.messageId.value, due[0].aggregateMessageId.value);
    EXPECT_EQ("owner-1", due[0].leaseOwner);
    EXPECT_EQ(t0 + h.outbox.claimLeaseMs, due[0].leaseUntilMs);
    EXPECT_EQ(1u, due[0].attemptCount);
    EXPECT_EQ(0, due[0].processedAtMs);

    // 有效 lease（未到期）的事件不再被 claim（同 owner 与其它 owner 均不行）。
    EXPECT_TRUE(h.store.claimOutboxEvents(t0, "owner-1", t0 + h.outbox.claimLeaseMs, 10).empty());
    EXPECT_TRUE(h.store.claimOutboxEvents(t0, "owner-2", t0 + h.outbox.claimLeaseMs, 10).empty());

    // lease 到期（t0+lease+1）后 a1 可重领，且 a2 同时到期 → 两行都 due；limit 封顶。
    h.clock.set(t0 + h.outbox.claimLeaseMs + 1);
    std::vector<OutboxEvent> capped = h.store.claimOutboxEvents(
        t0 + h.outbox.claimLeaseMs + 1, "owner-2",
        t0 + h.outbox.claimLeaseMs + 1 + h.outbox.claimLeaseMs, 1);
    ASSERT_EQ(1u, capped.size());
    std::vector<OutboxEvent> rest = h.store.claimOutboxEvents(
        t0 + h.outbox.claimLeaseMs + 1, "owner-3",
        t0 + h.outbox.claimLeaseMs + 1 + h.outbox.claimLeaseMs, 10);
    ASSERT_EQ(1u, rest.size());
    // 归属顺序未定义：断言两事件合计 attempt 和 == 3（重领 2 + 首领 1）。
    EXPECT_EQ(3u, capped[0].attemptCount + rest[0].attemptCount);
}

// RED 4：两个 relay 实例竞争同一批，恰一个 claim 成功；败者不 claim、不处理。
// 确定性：两 relay 共享同一 InMemory store（store 串行化），先扫描者得 lease 并处理，
// 后者在同一轮无可用行（已 processed/已 lease）。
TEST(OutboxRelayTest, TwoRelaysOnlyOneWinsLease)
{
    OutboxHarness h;
    AcceptOutcome a = h.rm.accept(SessionIdentity{kAlice, 1}, directTo(kBob, "race", "hello"));
    ASSERT_TRUE(a.ok);
    h.clock.set(kNow);

    RecordingWakeupPublisher c1(h.store);
    RecordingWakeupPublisher c2(h.store);
    LocalOutboxRelay r1(h.store, c1, h.clock, h.outbox);
    LocalOutboxRelay r2(h.store, c2, h.clock, h.outbox);

    ASSERT_EQ(1, r1.runScan());
    ASSERT_EQ(1u, c1.requests().size());
    ASSERT_EQ(0, r2.runScan());
    EXPECT_EQ(0u, c2.requests().size());

    // 胜者处理完成（processed_at 写入），事件不可再 claim。
    ASSERT_EQ(1u, c1.requests().size());
    std::shared_ptr<const OutboxEvent> done = h.store.findOutboxEvent(c1.requests()[0].event.id);
    ASSERT_TRUE(done);
    EXPECT_NE(0, done->processedAtMs);
    EXPECT_EQ(1u, done->attemptCount);
    EXPECT_EQ(0, r2.runScan());
}

// RED 5：lease 到期后允许重领（崩溃无清理路径依赖到期重领），attempt_count 递增。
TEST(OutboxRelayTest, LeaseExpiryAllowsReclaim)
{
    OutboxHarness h;
    AcceptOutcome a = h.rm.accept(SessionIdentity{kAlice, 1}, directTo(kBob, "lease", "hello"));
    ASSERT_TRUE(a.ok);
    h.clock.set(kNow);

    // relay A 的 claim（store port 直接驱动：已写 lease，未写 processed——崩溃点）。
    std::vector<OutboxEvent> claimed =
        h.store.claimOutboxEvents(kNow, "relay-A", kNow + h.outbox.claimLeaseMs, 10);
    ASSERT_EQ(1u, claimed.size());
    EXPECT_EQ(1u, claimed[0].attemptCount);

    // 新 relay 在 lease 有效期内无法重领。
    RecordingWakeupPublisher c(h.store);
    LocalOutboxRelay r(h.store, c, h.clock, h.outbox);
    ASSERT_EQ(0, r.runScan());
    EXPECT_EQ(0u, c.requests().size());

    // lease 到期后重领并处理：attempt_count 递增、owner 更换、最终 processed。
    h.clock.advance(h.outbox.claimLeaseMs + 1);
    ASSERT_EQ(1, r.runScan());
    ASSERT_EQ(1u, c.requests().size());

    std::shared_ptr<const OutboxEvent> after = h.store.findOutboxEvent(claimed[0].id);
    ASSERT_TRUE(after);
    EXPECT_EQ(2u, after->attemptCount);
    EXPECT_NE(std::string("relay-A"), after->leaseOwner);
    EXPECT_NE(0, after->processedAtMs);
}

// RED 6：lost wakeup 恢复——accept 提交后、relay 消费前 kill；无任何显式通知，
// 新 relay 的周期扫描仍 claim 并重放 MessageAccepted（在线接收者收到派生 wakeup）。
TEST(OutboxRelayTest, LostWakeupRecoveredByPeriodicScan)
{
    OutboxHarness h;
    // recipient 离线：accept 路径的 best-effort wakeup 为空操作（wakeup 丢失），
    // 进程随即"崩溃"，relay 从未被通知。
    AcceptOutcome a = h.rm.accept(SessionIdentity{kAlice, 1}, directTo(kBob, "lost-wakeup", "hello"));
    ASSERT_TRUE(a.ok);
    h.clock.set(kNow);

    RecordingWakeupPublisher c(h.store);
    LocalOutboxRelay relay(h.store, c, h.clock, h.outbox);
    // 无显式通知：周期扫描自行 claim 并唤醒（runScan 即 timer 驱动的单轮）。
    ASSERT_EQ(1, relay.runScan());
    ASSERT_EQ(1u, c.requests().size());
    ASSERT_EQ(1u, c.lastRecipients().size());
    EXPECT_EQ(kBob.value, c.lastRecipients()[0].value);
    EXPECT_EQ(a.messageId.value, c.lastEvent().aggregateMessageId.value);

    // 已处理：再次扫描无剩余未处理事件。
    ASSERT_EQ(0, relay.runScan());
    std::shared_ptr<const OutboxEvent> done = h.store.findOutboxEvent(c.lastEvent().id);
    ASSERT_TRUE(done);
    EXPECT_NE(0, done->processedAtMs);
}

// RED 7：poison 事件不静默丢弃、可查询（poisonedOutboxEvents 谓词命中）、不阻断
// 同批后续事件与后续批次。
TEST(OutboxRelayTest, PoisonEventVisibleAndDoesNotBlockBatch)
{
    OutboxHarness h;
    const SessionIdentity alice{kAlice, 1};
    std::vector<uint64_t> mids;
    for (int i = 0; i < 3; ++i) {
        AcceptOutcome a = h.rm.accept(alice, directTo(kBob, "poison-" + std::to_string(i), "x"));
        ASSERT_TRUE(a.ok);
        mids.push_back(a.messageId.value);
    }
    h.clock.set(kNow);

    // P4-03：publish 注入失败（poisonOn）→ 该事件逐事件结果失败 → relay 不标
    // processed（P3-09 抛异常语义等价）。
    RecordingWakeupPublisher c(h.store);
    c.poisonOn(mids[1]);  // 中间事件发布失败
    LocalOutboxRelay relay(h.store, c, h.clock, h.outbox);

    // 单批全部 claim（batch 10 >= 3）；E1 失败不阻断同批 E0/E2。
    ASSERT_EQ(3, relay.runScan());
    ASSERT_EQ(3u, c.requests().size());

    // E1 poison 可查询且未 processed；E0/E2 processed。
    std::vector<OutboxEvent> poisoned = h.store.poisonedOutboxEvents(10);
    ASSERT_EQ(1u, poisoned.size());
    EXPECT_EQ(mids[1], poisoned[0].aggregateMessageId.value);
    EXPECT_EQ(0, poisoned[0].processedAtMs);

    std::shared_ptr<const OutboxEvent> done0 = h.store.findOutboxEvent(c.requests()[0].event.id);
    std::shared_ptr<const OutboxEvent> done2 = h.store.findOutboxEvent(c.requests()[2].event.id);
    ASSERT_TRUE(done0);
    ASSERT_TRUE(done2);
    EXPECT_NE(0, done0->processedAtMs);
    EXPECT_NE(0, done2->processedAtMs);

    // 后续批次继续推进：新事件 E3 被处理（E1 仍被有效 lease 挡住）。
    AcceptOutcome a3 = h.rm.accept(alice, directTo(kBob, "poison-after", "y"));
    ASSERT_TRUE(a3.ok);
    ASSERT_EQ(1, relay.runScan());
    ASSERT_EQ(4u, c.requests().size());

    // E1 未静默丢弃：lease 到期后仍被重领（attempt+1）并再次失败（可查询）。
    h.clock.advance(h.outbox.claimLeaseMs + 1);
    ASSERT_EQ(1, relay.runScan());
    std::vector<OutboxEvent> poisonedAgain = h.store.poisonedOutboxEvents(10);
    ASSERT_EQ(1u, poisonedAgain.size());
    EXPECT_EQ(mids[1], poisonedAgain[0].aggregateMessageId.value);
    EXPECT_EQ(2u, poisonedAgain[0].attemptCount);
    EXPECT_EQ(0, poisonedAgain[0].processedAtMs);
}

// RED 8：处理成功后才标 processed；失败（publish 注入失败）保持未 processed、
// lease 到期可重试。
TEST(OutboxRelayTest, ProcessedOnlyAfterSuccess)
{
    OutboxHarness h;
    AcceptOutcome a = h.rm.accept(SessionIdentity{kAlice, 1}, directTo(kBob, "retry", "hello"));
    ASSERT_TRUE(a.ok);
    h.clock.set(kNow);

    RecordingWakeupPublisher c(h.store);
    c.poisonOn(a.messageId.value);  // 首次发布失败
    LocalOutboxRelay relay(h.store, c, h.clock, h.outbox);

    ASSERT_EQ(1, relay.runScan());
    std::vector<OutboxEvent> p = h.store.poisonedOutboxEvents(10);
    ASSERT_EQ(1u, p.size());
    EXPECT_EQ(0, p[0].processedAtMs);  // 失败：绝不标 processed
    EXPECT_EQ(1u, p[0].attemptCount);

    // 失败保持可重试：lease 到期后重领，第二次成功 → 才标 processed。
    h.clock.advance(h.outbox.claimLeaseMs + 1);
    c.poisonOff(a.messageId.value);
    ASSERT_EQ(1, relay.runScan());
    std::shared_ptr<const OutboxEvent> done = h.store.findOutboxEvent(p[0].id);
    ASSERT_TRUE(done);
    EXPECT_EQ(2u, done->attemptCount);
    EXPECT_NE(0, done->processedAtMs);
    EXPECT_TRUE(h.store.poisonedOutboxEvents(10).empty());
}

// RED 9：stop 有界（单轮 batch 有界 → join 有界）、幂等（重复调用直接返回）；
// stop 后公开 seam（runScan）仍可用。
TEST(OutboxRelayTest, StopIsBoundedIdempotent)
{
    OutboxHarness h;
    RecordingWakeupPublisher c(h.store);
    LocalOutboxRelay relay(h.store, c, h.clock, h.outbox);

    relay.stop(h.clock.nowMs() + 1000);  // 未 start：幂等返回
    relay.start();
    relay.stop(h.clock.nowMs() + 1000);
    relay.stop(h.clock.nowMs() + 1000);  // 重复：直接返回，无悬挂 join

    AcceptOutcome a = h.rm.accept(SessionIdentity{kAlice, 1}, directTo(kBob, "stop", "x"));
    ASSERT_TRUE(a.ok);
    ASSERT_EQ(1, relay.runScan());
    ASSERT_EQ(1u, c.requests().size());
}

// RED 10：批次/队列有界——超量事件按 batch 上限分批处理，单轮 claim 不超过上限。
TEST(OutboxRelayTest, BatchQueueBounded)
{
    OutboxHarness h;
    const SessionIdentity alice{kAlice, 1};
    for (int i = 0; i < 25; ++i) {
        AcceptOutcome a = h.rm.accept(alice, directTo(kBob, "batch-" + std::to_string(i), "x"));
        ASSERT_TRUE(a.ok);
    }
    h.clock.set(kNow);

    RecordingWakeupPublisher c(h.store);
    LocalOutboxRelay relay(h.store, c, h.clock, h.outbox);  // claimBatchSize=10

    const int64_t round1 = relay.runScan();
    EXPECT_EQ(10, round1);
    const int64_t round2 = relay.runScan();
    EXPECT_EQ(10, round2);
    const int64_t round3 = relay.runScan();
    EXPECT_EQ(5, round3);
    EXPECT_EQ(0, relay.runScan());

    EXPECT_EQ(25u, c.requests().size());
}

} // namespace
