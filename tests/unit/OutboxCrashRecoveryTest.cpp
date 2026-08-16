// P3-09 OutboxCrashRecovery RED→GREEN：进程 kill 语义（commit 后 relay 前 kill、
// 处理后 processed 标记前 kill、重放幂等）。穿过 LocalOutboxRelay 公开 interface +
// MessageStore outbox port + FakeClock + RecordingWakeupPublisher/LocalWakeupPublisher +
// InMemoryMessageStore + DeliveryCoordinator，无固定 sleep、不读私有容器。
//
// RED 依据（现状）：与 OutboxRelayTest 相同——LocalOutboxRelay/OutboxEvent/
// MessageStore outbox port 尚不存在，本文件引用即编译失败（合法 RED）。
//
// P4-03 伴改：relay 出口从 MessageAcceptedConsumer 替换为 OutboxPublisher port；
// RecordingOutboxConsumer/WakeupConsumer/RMWakeupConsumer 伴改为本文件内的
// RecordingWakeupPublisher（记录 publish 请求 + 派生接收者 + 可选 wakeup 转发，
// 与 LocalWakeupPublisher 语义同构）与生产 LocalWakeupPublisher（RED-M 直接覆盖
// 生产 adapter）。
//
// 实现契约与 OutboxRelayTest.cpp 顶部一致（OutboxEvent 值类型、MessageStore
// claimOutboxEvents/markOutboxProcessed/markOutboxPoisoned/findOutboxEvent/
// poisonedOutboxEvents、LocalOutboxRelay(store, publisher, clock, config)/
// OutboxPublisher::publish(batch, deadline) → 逐事件结果）。
//
// 事件种子经 ReliableMessaging::accept（GREEN 使 InMemory 双 adapter 在
// insertMessage 同点写 OutboxEvent，与 MySQLMessageStore 对称）。

#include "app/Clock.hpp"
#include "app/DeliveryCoordinator.hpp"
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
#include <string>
#include <vector>

namespace {

const UserId kAlice{1};
const UserId kBob{2};
const int64_t kNow = 1000000;
// Delivery lease 显著大于 outbox lease（测试注入 1000ms）：重放时 Delivery 仍在
// lease 内被 claimFor fencing（幂等 wakeup 依赖，P3-09 §Interface）。
const uint64_t kLeaseMs = 10000;

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
    c.cleanupCycleMs = 0;
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
// LocalWakeupPublisher 语义 + 记录）：从 Message 命令派生接收者（outboxRecipientsFor，
// 与生产同构）并记录每次 publish 请求与派生接收者；可选转发 wakeup（coordinator/
// ReliableMessaging），转发抛异常 → 该事件返回失败（不抛、逐事件）。
class RecordingWakeupPublisher : public OutboxPublisher {
public:
    explicit RecordingWakeupPublisher(MessageStore& store) : store_(store) {}

    void setForward(std::function<void(const std::vector<UserId>&)> f) { forward_ = std::move(f); }

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
                const std::vector<UserId> recipients = outboxRecipientsFor(msg->command);
                recipients_.push_back(recipients);
                ++wakeups_;
                oc.ok = true;
                oc.failure = PublishFailure::None;
                if (forward_) {
                    try {
                        forward_(recipients);
                    } catch (const std::exception& e) {
                        oc.ok = false;
                        oc.failure = PublishFailure::Other;
                        oc.error = e.what();
                    }
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
    std::function<void(const std::vector<UserId>&)> forward_;
    std::vector<OutboxPublishRequest> requests_;
    std::vector<std::vector<UserId> > recipients_;
    size_t wakeups_ = 0;
};

// 公开 MessageStore 装饰器（ReliableMessagingContractTest::ThrowOnceOnUpdateStore
// 同模式）：只在 wakeup 路径（relay 消费 → wakeupAccepted → coordinator_.onAccepted
// → claimFor → updateDelivery）注入一次存储故障；默认不武装，accept/sessionAvailable
// 路径不受影响（P3-07"异常不反转已提交结果"冻结语义保持）。
class ThrowOnceOnWakeupUpdateStore : public MessageStore {
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
    void updateDelivery(const Delivery& delivery) override
    {
        ++updateCalls_;
        if (failNextUpdate_) {
            failNextUpdate_ = false;
            throw std::runtime_error("injected wakeup updateDelivery failure");
        }
        delegate_.updateDelivery(delivery);
    }
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
    std::vector<Delivery> deliveriesDueForRetry(int64_t nowMs, uint64_t limit) override
    {
        return delegate_.deliveriesDueForRetry(nowMs, limit);
    }
    uint32_t expireDeliveries(int64_t nowMs, uint64_t limit) override
    {
        return delegate_.expireDeliveries(nowMs, limit);
    }
    uint32_t cleanupDeliveries(int64_t ackedBeforeMs, int64_t expiredBeforeMs,
                               uint64_t limit) override
    {
        return delegate_.cleanupDeliveries(ackedBeforeMs, expiredBeforeMs, limit);
    }
    uint32_t timeGranularityMs() override { return delegate_.timeGranularityMs(); }
    std::vector<OutboxEvent> claimOutboxEvents(int64_t nowMs, const std::string& leaseOwner,
                                               int64_t leaseUntilMs, uint64_t limit) override
    {
        return delegate_.claimOutboxEvents(nowMs, leaseOwner, leaseUntilMs, limit);
    }
    void markOutboxProcessed(uint64_t eventId, int64_t nowMs) override
    {
        delegate_.markOutboxProcessed(eventId, nowMs);
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

    void armUpdateDeliveryFailure() { failNextUpdate_ = true; }

private:
    InMemoryMessageStore delegate_;
    bool failNextUpdate_ = false;
    size_t updateCalls_ = 0;
};

// RED 1：commit 后 relay 前 kill——accept 事务已提交（Message+Deliveries+OutboxEvent
// 原子持久化）但 relay 从未消费；重启后新 relay 实例仍可 claim 并重放。
// 现状无 relay/outbox port → 编译失败。
TEST(OutboxCrashRecoveryTest, KillAfterCommitBeforeRelayKeepsEvent)
{
    // 进程 A：recipient 离线（accept 路径 best-effort wakeup 为空操作），随后在
    // relay 消费前 kill——事件未处理、无 lease，留在 store。
    OutboxHarness h;
    const SessionIdentity alice{kAlice, 1};
    AcceptOutcome a = h.rm.accept(alice, directTo(kBob, "crash-1", "hello"));
    ASSERT_TRUE(a.ok);
    h.clock.set(kNow);

    // 重启：新 relay 周期扫描 claim 该事件并唤醒（derived recipient = bob）。
    RecordingWakeupPublisher c(h.store);
    LocalOutboxRelay relay(h.store, c, h.clock, h.outbox);
    ASSERT_EQ(1, relay.runScan());
    ASSERT_EQ(1u, c.wakeups());
    ASSERT_EQ(1u, c.lastRecipients().size());
    EXPECT_EQ(kBob.value, c.lastRecipients()[0].value);

    // durable acceptance 完好：同 key 回读原 identity，Delivery 行未变。
    std::shared_ptr<const Message> back =
        h.store.findAccepted(ClientMessageId("crash-1"), alice.userId);
    ASSERT_TRUE(back);
    EXPECT_EQ(a.messageId.value, back->id.value);
    std::vector<Delivery> rows = h.store.deliveriesByMessage(a.messageId);
    ASSERT_EQ(1u, rows.size());

    // 事件由新 relay 处理（首次 claim：attempt=1，processed 写入）。
    std::shared_ptr<const OutboxEvent> done = h.store.findOutboxEvent(c.lastEvent().id);
    ASSERT_TRUE(done);
    EXPECT_EQ(1u, done->attemptCount);
    EXPECT_NE(0, done->processedAtMs);
}

// RED 2：处理后 processed 标记前 kill——wakeup 成功但 processed_at 写入前进程退出；
// 重启后同一事件可被重领（attempt_count 递增、owner 更换），最终才标 processed。
TEST(OutboxCrashRecoveryTest, KillAfterProcessBeforeMarkedProcessedReplays)
{
    OutboxHarness h;
    AcceptOutcome a = h.rm.accept(SessionIdentity{kAlice, 1}, directTo(kBob, "crash-2", "hello"));
    ASSERT_TRUE(a.ok);
    h.clock.set(kNow);

    // relay A：claim + 处理（wakeup 成功），但在写 processed_at 前 kill——store
    // port 模拟 A 的 claim，随后不调 markOutboxProcessed（崩溃窗口）。
    std::vector<OutboxEvent> claimed =
        h.store.claimOutboxEvents(kNow, "relay-A", kNow + h.outbox.claimLeaseMs, 10);
    ASSERT_EQ(1u, claimed.size());
    EXPECT_EQ(1u, claimed[0].attemptCount);

    // 重启：新 relay B 在 lease 有效期内无法重领；lease 到期后重领同一事件。
    RecordingWakeupPublisher cB(h.store);
    LocalOutboxRelay relayB(h.store, cB, h.clock, h.outbox);
    ASSERT_EQ(0, relayB.runScan());
    h.clock.advance(h.outbox.claimLeaseMs + 1);
    ASSERT_EQ(1, relayB.runScan());
    ASSERT_EQ(1u, cB.wakeups());
    ASSERT_EQ(1u, cB.requests().size());
    EXPECT_EQ(a.messageId.value, cB.requests()[0].event.aggregateMessageId.value);

    // 同一事件被重放：attempt_count 递增、owner 更换、最终 processed。
    std::shared_ptr<const OutboxEvent> after = h.store.findOutboxEvent(claimed[0].id);
    ASSERT_TRUE(after);
    EXPECT_EQ(2u, after->attemptCount);
    EXPECT_NE(std::string("relay-A"), after->leaseOwner);
    EXPECT_NE(0, after->processedAtMs);
}

// RED 3：同一 MessageAccepted 事件重复处理只产生幂等 wakeup——第二次 wakeup 后
// 不新增 DeliveryAttempt、不新建 Message/Delivery（DeliveryCoordinator claimFor
// fencing + P3-08 scheduler 幂等）。
TEST(OutboxCrashRecoveryTest, ReplayedMessageAcceptedProducesIdempotentWakeup)
{
    FakeClock clock;
    clock.set(kNow);
    InMemoryMessageStore store;
    RecordingDeliverySink sink;
    RetryConfig rcfg = outboxRetryConfig();

    // bob 经 coordinator 在线；ReliableMessaging 的 coordinator 不知道 bob →
    // accept 路径的 best-effort wakeup 是空操作，投递只可能来自 relay 的 wakeup。
    DeliveryCoordinator coordinator(store, sink, clock, kLeaseMs, rcfg, 0x1001);
    coordinator.sessionAvailable(SessionIdentity{kBob, 1});

    // P4-03：relay 出口为 OutboxPublisher port；publisher 派生接收者并转发 wakeup
    // 到 coordinator（生产 LocalWakeupPublisher 语义同构）。
    OutboxConfig ocfg = testOutboxConfig();
    RecordingWakeupPublisher pub(store);
    pub.setForward([&coordinator](const std::vector<UserId>& r) { coordinator.onAccepted(r); });
    LocalOutboxRelay relay(store, pub, clock, ocfg);

    ReliableMessaging rm(store, sink, clock, kLeaseMs, rcfg);
    AcceptOutcome a = rm.accept(SessionIdentity{kAlice, 1}, directTo(kBob, "idem", "hello"));
    ASSERT_TRUE(a.ok);

    // relay A：claim + publish（wakeup 驱动 coordinator 投递 bob），随后未标 processed
    // 前 kill。手动注入"消费成功但未标 processed"的崩溃窗口。
    std::vector<OutboxEvent> claimed =
        store.claimOutboxEvents(kNow, "relay-A", kNow + ocfg.claimLeaseMs, 10);
    ASSERT_EQ(1u, claimed.size());
    std::shared_ptr<const Message> m = store.findMessage(claimed[0].aggregateMessageId);
    ASSERT_TRUE(m);
    OutboxPublishRequest req;
    req.event = claimed[0];
    req.conversationId = m->conversationId;
    req.sequence = m->sequence.value;
    req.topic = "muduo-outbox";
    std::vector<OutboxPublishRequest> one;
    one.push_back(req);
    std::vector<OutboxPublishOutcome> outcomes = pub.publish(one, 5000);
    ASSERT_EQ(1u, outcomes.size());
    EXPECT_TRUE(outcomes[0].ok);
    ASSERT_EQ(1u, pub.wakeups());
    ASSERT_EQ(1u, sink.attempts().size());
    EXPECT_EQ(a.messageId.value, sink.attempts()[0].messageId.value);

    // 重启：lease 到期后同一事件被重放（第二次 wakeup），但投递幂等——不新增
    // DeliveryAttempt、不新建 Message/Delivery。
    clock.advance(ocfg.claimLeaseMs + 1);
    ASSERT_EQ(1, relay.runScan());
    ASSERT_EQ(2u, pub.wakeups());
    ASSERT_EQ(1u, sink.attempts().size());

    EXPECT_EQ(1u, store.deliveriesByMessage(a.messageId).size());
    std::shared_ptr<const Message> dup = store.findAccepted(ClientMessageId("idem"), kAlice);
    ASSERT_TRUE(dup);
    EXPECT_EQ(a.messageId.value, dup->id.value);

    // 事件最终 processed（第二次处理成功）。
    std::shared_ptr<const OutboxEvent> done = store.findOutboxEvent(claimed[0].id);
    ASSERT_TRUE(done);
    EXPECT_NE(0, done->processedAtMs);
}

// 生产 relay 消费出口接 ReliableMessaging::wakeupAccepted（幂等 onAccepted 重唤醒）：
// 同一批接收者的重放不产生重复投递——claimFor 对已 InFlight/Acknowledged 行 fencing。
TEST(OutboxCrashRecoveryTest, WakeupAcceptedIsIdempotent)
{
    FakeClock clock;
    clock.set(kNow);
    InMemoryMessageStore store;
    RecordingDeliverySink sink;
    RetryConfig rcfg = outboxRetryConfig();
    ReliableMessaging rm(store, sink, clock, kLeaseMs, rcfg);

    // recipient 在 accept 时离线（accept 路径 best-effort wakeup 空操作）。
    AcceptOutcome a = rm.accept(SessionIdentity{kAlice, 1}, directTo(kBob, "wakeup-idem", "hello"));
    ASSERT_TRUE(a.ok);

    // bob 上线 claim；随后 relay 重放（outbox 出口）同一接收者列表 → 幂等。
    rm.sessionAvailable(SessionIdentity{kBob, 1});
    ASSERT_EQ(1u, sink.attempts().size());
    std::vector<UserId> recipients;
    recipients.push_back(kBob);
    rm.wakeupAccepted(recipients);
    rm.wakeupAccepted(recipients);
    ASSERT_EQ(1u, sink.attempts().size());
    EXPECT_EQ(1u, store.deliveriesByMessage(a.messageId).size());
}

// RED（M）：wakeup 路径的瞬时存储故障不得使事件被消费——relay 消费时
// coordinator_.onAccepted（claimFor → updateDelivery）抛一次存储异常，事件必须
// 保持未 processed（lease 到期可重试）。P4-03：生产 LocalWakeupPublisher 把
// wakeupAccepted 的存储异常转为该事件失败（not-ok，不抛）→ relay 不标 processed
// （语义与 P3-09 完全一致）。
// accept 路径不触发装饰器（recipient 离线、装饰器在 accept 后武装）——P3-07
// "异常不反转已提交结果"冻结语义保持。
TEST(OutboxCrashRecoveryTest, WakeupStoreFailureKeepsEventUnprocessed)
{
    FakeClock clock;
    clock.set(kNow);
    ThrowOnceOnWakeupUpdateStore store;
    RecordingDeliverySink sink;
    RetryConfig rcfg = outboxRetryConfig();
    ReliableMessaging rm(store, sink, clock, kLeaseMs, rcfg);

    // 离线 accept：best-effort wakeup 空操作，提交的 Message/Deliveries/OutboxEvent
    // 完好；装饰器未武装，accept 路径不受影响。
    const SessionIdentity alice{kAlice, 1};
    AcceptOutcome a = rm.accept(alice, directTo(kBob, "wakeup-fail", "hello"));
    ASSERT_TRUE(a.ok);
    ASSERT_TRUE(store.findAccepted(ClientMessageId("wakeup-fail"), alice.userId));

    // bob 上线 claim：m1 进入 InFlight（投递成功），装饰器仍未武装。
    rm.sessionAvailable(SessionIdentity{kBob, 1});
    ASSERT_EQ(1u, sink.attempts().size());

    // 武装装饰器并推进时钟越过 Delivery lease：wakeup 的 claimFor 会重投该行
    // （InFlight 到期 → claimable），其 updateDelivery 命中注入故障。
    store.armUpdateDeliveryFailure();
    clock.advance(static_cast<int64_t>(kLeaseMs) + 1);

    // relay 消费：生产 LocalWakeupPublisher 的 wakeup 存储异常 → 该事件失败
    // （not-ok）→ 事件不被标 processed。
    OutboxConfig ocfg = testOutboxConfig();
    LocalWakeupPublisher pub(store, rm);
    LocalOutboxRelay relay(store, pub, clock, ocfg);
    ASSERT_EQ(1, relay.runScan());

    // 事件保持未 processed：lease 保留，到期可重试（未被静默消费）。
    std::vector<OutboxEvent> pending = store.poisonedOutboxEvents(10);
    ASSERT_EQ(1u, pending.size());
    EXPECT_EQ(0, pending[0].processedAtMs);
    EXPECT_EQ(1u, pending[0].attemptCount);
    EXPECT_FALSE(pending[0].leaseOwner.empty());

    // lease 到期后重试成功（故障已解除）：事件最终 processed（attempt_count 递增）。
    clock.advance(ocfg.claimLeaseMs + 1);
    ASSERT_EQ(1, relay.runScan());
    std::shared_ptr<const OutboxEvent> done = store.findOutboxEvent(pending[0].id);
    ASSERT_TRUE(done);
    EXPECT_EQ(2u, done->attemptCount);
    EXPECT_NE(0, done->processedAtMs);
}

} // namespace
