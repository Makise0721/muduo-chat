// P3-08 MessageRetention RED：穿过 ReliableMessaging 公开 interface，注入
// FakeClock + ScriptedDeliverySink + InMemoryMessageStore，无固定 sleep。
// T6：Pending 越过 expires_at（accept_time + message_retention）后扫描转
// Expired 且可查询；恰在边界不 Expired。T7：Acknowledged 行 cleanup 幂等
// （第二次删 0 行，Pending/InFlight 不动）。cleanup 隐藏在模块内部（P3-08
// 最小实现：有界 batch scheduler），本测试经公共调用驱动的扫描推进、并以
// store 可查询行断言；现状无 Expired 转移、无 cleanup（预期 RED）。

#include "app/DeliverySink.hpp"
#include "app/Clock.hpp"
#include "app/InMemoryMessageStore.hpp"
#include "app/MessageStore.hpp"
#include "app/ReliableMessaging.hpp"

#include "FakeClock.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

namespace {

const UserId kAlice{1};
const UserId kBob{2};
const UserId kCarol{3};
const int64_t kNow = 1000000;
// P3-08 冻结参数（测试注入小值，不得成为生产默认）。
const int64_t kMessageRetentionMs = 10000;  // Pending/InFlight → Expired 期限
const int64_t kAckedRetentionMs = 1000;     // Acknowledged 行 cleanup 期限
const uint64_t kLeaseMs = 10000;

// 测试注入的 RetryConfig：保留期限用小值；cleanupCycleMs=0 使每次扫描都执行
// cleanup（幂等断言不依赖周期）。ack_timeout/backoff 保持生产默认（本文件
// 场景不触发重投循环）。
RetryConfig retentionRetryConfig()
{
    RetryConfig c;
    c.messageRetentionMs = kMessageRetentionMs;
    c.ackedRetentionMs = kAckedRetentionMs;
    c.expiredRetentionMs = kAckedRetentionMs;
    c.cleanupCycleMs = 0;
    return c;
}

// 脚本化 sink：按 enqueue 顺序消费 DeliverDisposition，耗尽后回退 def_。
class ScriptedDeliverySink : public DeliverySink {
public:
    void enqueue(DeliverDisposition d) { script_.push_back(d); }
    void setDefault(DeliverDisposition d) { def_ = d; }

    DeliverDisposition deliver(const DeliveryAttempt& attempt) override
    {
        attempts_.push_back(attempt);
        if (!script_.empty()) {
            const DeliverDisposition d = script_.front();
            script_.erase(script_.begin());
            return d;
        }
        return def_;
    }

    const std::vector<DeliveryAttempt>& attempts() const { return attempts_; }

private:
    std::vector<DeliverDisposition> script_;
    std::vector<DeliveryAttempt> attempts_;
    DeliverDisposition def_ = DeliverDisposition::Accepted;
};

struct Harness {
    FakeClock clock;
    InMemoryMessageStore store;
    ScriptedDeliverySink sink;
    ReliableMessaging rm;

    Harness() : rm(store, sink, clock, kLeaseMs, retentionRetryConfig())
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

TEST(MessageRetentionTest, ExpiryMovesDeliveryToExpiredAtBoundary)
{
    Harness h;
    AcceptOutcome a = h.rm.accept(SessionIdentity{kAlice, 1}, directTo(kBob, "m1", "hello"));
    ASSERT_TRUE(a.ok);
    // 接收者离线：Delivery 保持 Pending。
    std::vector<Delivery> rows = h.store.deliveriesByMessage(a.messageId);
    ASSERT_EQ(1u, rows.size());
    EXPECT_EQ(DeliveryState::Pending, rows[0].state);

    h.rm.start();

    // 恰在边界（now == accept_time + message_retention）：不转 Expired。
    // 推进扫描：无相关投递的中立 Session 上线驱动内部到期扫描（批量有界）。
    h.clock.set(kNow + kMessageRetentionMs);
    h.rm.sessionAvailable(SessionIdentity{kCarol, 1});
    rows = h.store.deliveriesByMessage(a.messageId);
    ASSERT_EQ(1u, rows.size());
    EXPECT_EQ(DeliveryState::Pending, rows[0].state);
    ASSERT_TRUE(h.sink.attempts().empty());

    // 越过边界：扫描后转 Expired，行可查询（不静默删除）。
    h.clock.advance(1);
    h.rm.sessionAvailable(SessionIdentity{kCarol, 1});
    rows = h.store.deliveriesByMessage(a.messageId);
    ASSERT_EQ(1u, rows.size());
    EXPECT_EQ(DeliveryState::Expired, rows[0].state);  // RED：现状无 Expired 转移
    EXPECT_EQ(a.messageId.value, rows[0].messageId.value);

    // Expired 不再重投。
    h.rm.sessionAvailable(SessionIdentity{kBob, 1});
    ASSERT_TRUE(h.sink.attempts().empty());

    h.rm.stop(h.clock.nowMs() + 1000);
}

TEST(MessageRetentionTest, CleanupIsIdempotent)
{
    Harness h;
    // m1：Acknowledged，过 acked_retention 后应被清理。
    AcceptOutcome acked =
        h.rm.accept(SessionIdentity{kAlice, 1}, directTo(kBob, "m1", "acked"));
    ASSERT_TRUE(acked.ok);
    EXPECT_EQ(AckResult::Acknowledged,
              h.rm.acknowledge(SessionIdentity{kBob, 1}, acked.messageId).result);

    // m2：Pending（carol→bob，不同 conversation），不应被清理。
    AcceptOutcome pending =
        h.rm.accept(SessionIdentity{kCarol, 1}, directTo(kBob, "m2", "pending"));
    ASSERT_TRUE(pending.ok);
    ASSERT_NE(acked.conversationId.value, pending.conversationId.value);

    // m3：InFlight（alice→carol，在线投递），不应被清理。
    h.rm.sessionAvailable(SessionIdentity{kCarol, 1});
    AcceptOutcome inflight =
        h.rm.accept(SessionIdentity{kAlice, 1}, directTo(kCarol, "m3", "inflight"));
    ASSERT_TRUE(inflight.ok);
    ASSERT_EQ(1u, h.sink.attempts().size());
    EXPECT_EQ(inflight.messageId.value, h.sink.attempts()[0].messageId.value);

    // 越过 acked_retention（m2/m3 未到 message_retention，保持 Pending/InFlight）。
    h.clock.set(kNow + kAckedRetentionMs + 1);

    // 第一次 cleanup（扫描）：m1 被清理。推进扫描：中立 Session 上线驱动
    // 内部 cleanup（有界 batch）。RED：现状无 cleanup，m1 仍可查询。
    h.rm.sessionAvailable(SessionIdentity{kCarol, 1});
    EXPECT_TRUE(h.store.deliveriesByMessage(acked.messageId).empty());

    // 第二次 cleanup：幂等，删 0 行，不崩溃。
    h.rm.sessionAvailable(SessionIdentity{kCarol, 1});
    EXPECT_TRUE(h.store.deliveriesByMessage(acked.messageId).empty());

    // Pending / InFlight 不受 cleanup 影响。
    std::vector<Delivery> pendingRows = h.store.deliveriesByMessage(pending.messageId);
    ASSERT_EQ(1u, pendingRows.size());
    EXPECT_EQ(DeliveryState::Pending, pendingRows[0].state);
    std::vector<Delivery> inflightRows = h.store.deliveriesByMessage(inflight.messageId);
    ASSERT_EQ(1u, inflightRows.size());
    EXPECT_EQ(DeliveryState::InFlight, inflightRows[0].state);
}

} // namespace
