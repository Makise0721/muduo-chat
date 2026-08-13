// P3-07 DeliveryCoordinator 状态机（通过 ReliableMessaging facade 驱动）：
// 可控 Clock + ScriptedDeliverySink（脚本化 DeliverDisposition）+ InMemory store。
// 覆盖计划 §5 P3-07 RED 场景：socket WouldBlock/Closed、发送后断线、ACK 丢失、
// 他人 ACK、同 ACK 重复、lease owner 崩溃、背压不自旋、legacy implicit-ack、
// 在线 accept 立即投递、HOL 单在途与 ACK 放行。

#include "app/DeliverySink.hpp"
#include "app/Clock.hpp"
#include "app/InMemoryMessageStore.hpp"
#include "app/MessageStore.hpp"
#include "app/ReliableMessaging.hpp"

#include "FakeClock.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <chrono>
#include <string>
#include <vector>

namespace {

TEST(DeliveryCoordinatorTest, UnixEpochClockUsesSystemEpochMilliseconds)
{
    UnixEpochClock clock;
    const int64_t before = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
    const int64_t observed = clock.nowMs();
    const int64_t after = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count();

    EXPECT_GE(observed, before);
    EXPECT_LE(observed, after);
}

const UserId kAlice{1};
const UserId kBob{2};
const UserId kCarol{3};
const uint64_t kLeaseMs = 1000;
const int64_t kNow = 1000000;

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

    Harness() : rm(store, sink, clock, kLeaseMs) { clock.set(kNow); }
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

TEST(DeliveryCoordinatorTest, ClosedSocketKeepsPendingWithoutAttempt)
{
    Harness h;
    AcceptOutcome a = h.rm.accept(SessionIdentity{kAlice, 1}, directTo(kBob, "m1", "hello"));
    ASSERT_TRUE(a.ok);

    h.sink.enqueue(DeliverDisposition::Closed);
    h.rm.sessionAvailable(SessionIdentity{kBob, 1});
    ASSERT_EQ(1u, h.sink.attempts().size());  // 尝试了一次，被拒
    EXPECT_EQ(1u, h.sink.attempts()[0].attemptNumber);

    // Closed 不记 attempt：重领时 attemptNumber 仍为 1，Delivery 保持可投。
    h.rm.sessionAvailable(SessionIdentity{kBob, 1});
    ASSERT_EQ(2u, h.sink.attempts().size());
    EXPECT_EQ(1u, h.sink.attempts()[1].attemptNumber);
    EXPECT_EQ(a.messageId.value, h.sink.attempts()[1].messageId.value);
}

TEST(DeliveryCoordinatorTest, WouldBlockMapsToPauseNotSpin)
{
    Harness h;
    AcceptOutcome a1 = h.rm.accept(SessionIdentity{kAlice, 1}, directTo(kBob, "m1", "hello"));
    ASSERT_TRUE(a1.ok);
    // 第二个 conversation（Carol→Bob）：验证暂停是会话级（不同 conversation 也不投）。
    AcceptOutcome a2 =
        h.rm.accept(SessionIdentity{kCarol, 1}, directTo(kBob, "m2", "other conv"));
    ASSERT_TRUE(a2.ok);
    ASSERT_NE(a1.conversationId.value, a2.conversationId.value);

    h.sink.enqueue(DeliverDisposition::Paused);
    h.rm.sessionAvailable(SessionIdentity{kBob, 1});
    ASSERT_EQ(1u, h.sink.attempts().size());  // 首个投递被暂停

    // 暂停中：sessionAvailable / 在线 accept 都不得再尝试（不自旋）。
    h.rm.sessionAvailable(SessionIdentity{kBob, 1});
    ASSERT_EQ(1u, h.sink.attempts().size());
    AcceptOutcome a3 =
        h.rm.accept(SessionIdentity{kAlice, 1}, directTo(kBob, "m3", "during pause"));
    ASSERT_TRUE(a3.ok);
    ASSERT_EQ(1u, h.sink.attempts().size());

    // low-water resume 后恢复；Paused 不记 attempt（attemptNumber 仍为 1）。
    h.rm.resume(SessionIdentity{kBob, 1});
    ASSERT_EQ(3u, h.sink.attempts().size());
    EXPECT_EQ(1u, h.sink.attempts()[1].attemptNumber);
    EXPECT_EQ(1u, h.sink.attempts()[2].attemptNumber);
    EXPECT_EQ(a1.messageId.value, h.sink.attempts()[1].messageId.value);
    EXPECT_EQ(a2.messageId.value, h.sink.attempts()[2].messageId.value);
    // 暂停期间接受的消息（a3）仍在 HOL（a1 未确认）：不越过，ACK 后才放行。
    EXPECT_EQ(AckResult::Acknowledged, h.rm.acknowledge(SessionIdentity{kBob, 1}, a1.messageId).result);
    ASSERT_EQ(4u, h.sink.attempts().size());
    EXPECT_EQ(a3.messageId.value, h.sink.attempts()[3].messageId.value);
    EXPECT_EQ(1u, h.sink.attempts()[3].attemptNumber);
}

TEST(DeliveryCoordinatorTest, SendThenDisconnectReclaimsOnReconnect)
{
    Harness h;
    AcceptOutcome a = h.rm.accept(SessionIdentity{kAlice, 1}, directTo(kBob, "m1", "hello"));
    ASSERT_TRUE(a.ok);

    h.rm.sessionAvailable(SessionIdentity{kBob, 1});
    ASSERT_EQ(1u, h.sink.attempts().size());
    EXPECT_EQ(1u, h.sink.attempts()[0].attemptNumber);

    // 发送后断线（未 ACK）：InFlight 回 Pending。
    h.rm.sessionClosed(SessionIdentity{kBob, 1});
    // 重连：同 messageId 重投（attempt 2，客户端按 MessageId 去重）。
    h.rm.sessionAvailable(SessionIdentity{kBob, 1});
    ASSERT_EQ(2u, h.sink.attempts().size());
    EXPECT_EQ(2u, h.sink.attempts()[1].attemptNumber);
    EXPECT_EQ(a.messageId.value, h.sink.attempts()[1].messageId.value);
}

TEST(DeliveryCoordinatorTest, AckLossHoldsUntilLeaseExpiry)
{
    Harness h;
    AcceptOutcome a = h.rm.accept(SessionIdentity{kAlice, 1}, directTo(kBob, "m1", "hello"));
    ASSERT_TRUE(a.ok);

    h.rm.sessionAvailable(SessionIdentity{kBob, 1});
    ASSERT_EQ(1u, h.sink.attempts().size());

    // ACK 丢失：lease 内不重投（不产生新 attempt）。
    h.clock.advance(static_cast<int64_t>(kLeaseMs) / 2);
    h.rm.sessionAvailable(SessionIdentity{kBob, 1});
    ASSERT_EQ(1u, h.sink.attempts().size());

    // lease 到期后重领：同 messageId 重投（attempt 2）。
    h.clock.advance(static_cast<int64_t>(kLeaseMs) / 2 + 1);
    h.rm.sessionAvailable(SessionIdentity{kBob, 1});
    ASSERT_EQ(2u, h.sink.attempts().size());
    EXPECT_EQ(2u, h.sink.attempts()[1].attemptNumber);
    EXPECT_EQ(a.messageId.value, h.sink.attempts()[1].messageId.value);
}

TEST(DeliveryCoordinatorTest, NotRecipientAckIsIgnored)
{
    Harness h;
    AcceptOutcome a = h.rm.accept(SessionIdentity{kAlice, 1}, directTo(kBob, "m1", "hello"));
    ASSERT_TRUE(a.ok);

    // Bob ACK Carol 的消息：NotRecipient，状态不变（Carol 的 Delivery 仍可投）。
    AckOutcome wrong = h.rm.acknowledge(SessionIdentity{kCarol, 1}, a.messageId);
    EXPECT_EQ(AckResult::NotRecipient, wrong.result);

    h.rm.sessionAvailable(SessionIdentity{kBob, 1});
    ASSERT_EQ(1u, h.sink.attempts().size());
    EXPECT_EQ(a.messageId.value, h.sink.attempts()[0].messageId.value);
    EXPECT_EQ(1u, h.sink.attempts()[0].attemptNumber);

    AckOutcome right = h.rm.acknowledge(SessionIdentity{kBob, 1}, a.messageId);
    EXPECT_EQ(AckResult::Acknowledged, right.result);
}

TEST(DeliveryCoordinatorTest, DuplicateAckIdempotentAndNeverReclaims)
{
    Harness h;
    AcceptOutcome a = h.rm.accept(SessionIdentity{kAlice, 1}, directTo(kBob, "m1", "hello"));
    ASSERT_TRUE(a.ok);

    h.rm.sessionAvailable(SessionIdentity{kBob, 1});
    ASSERT_EQ(1u, h.sink.attempts().size());

    EXPECT_EQ(AckResult::Acknowledged, h.rm.acknowledge(SessionIdentity{kBob, 1}, a.messageId).result);
    EXPECT_EQ(AckResult::Idempotent, h.rm.acknowledge(SessionIdentity{kBob, 1}, a.messageId).result);

    // ACK 后不再 claim：sessionAvailable 无新 attempt。
    h.rm.sessionAvailable(SessionIdentity{kBob, 1});
    ASSERT_EQ(1u, h.sink.attempts().size());
}

TEST(DeliveryCoordinatorTest, HeadOfLineBlocksUntilAck)
{
    Harness h;
    h.rm.sessionAvailable(SessionIdentity{kBob, 1});

    AcceptOutcome m1 = h.rm.accept(SessionIdentity{kAlice, 1}, directTo(kBob, "m1", "first"));
    ASSERT_TRUE(m1.ok);
    ASSERT_EQ(1u, h.sink.attempts().size());  // 在线 accept 立即投递

    // 同 conversation 前序未确认：后续 sequence 不越过（HOL）。
    AcceptOutcome m2 = h.rm.accept(SessionIdentity{kAlice, 1}, directTo(kBob, "m2", "second"));
    ASSERT_TRUE(m2.ok);
    EXPECT_EQ(2u, m2.sequence.value);
    ASSERT_EQ(1u, h.sink.attempts().size());

    // ACK 放行下一 sequence。
    EXPECT_EQ(AckResult::Acknowledged, h.rm.acknowledge(SessionIdentity{kBob, 1}, m1.messageId).result);
    ASSERT_EQ(2u, h.sink.attempts().size());
    EXPECT_EQ(m2.messageId.value, h.sink.attempts()[1].messageId.value);
    EXPECT_EQ(1u, h.sink.attempts()[1].attemptNumber);
}

TEST(DeliveryCoordinatorTest, LegacyImplicitAckOnDelivery)
{
    Harness h;
    // legacy identity（spec §5.1："legacy:" 前缀）。
    AcceptOutcome a = h.rm.accept(SessionIdentity{kAlice, 1}, directTo(kBob, "legacy:1:1", "hi"));
    ASSERT_TRUE(a.ok);

    h.rm.sessionAvailable(SessionIdentity{kBob, 1});
    ASSERT_EQ(1u, h.sink.attempts().size());

    // 投递即隐式确认：随后的 ACK 是幂等（Idempotent），不再 claim/重投。
    EXPECT_EQ(AckResult::Idempotent, h.rm.acknowledge(SessionIdentity{kBob, 1}, a.messageId).result);
    h.rm.sessionAvailable(SessionIdentity{kBob, 1});
    ASSERT_EQ(1u, h.sink.attempts().size());
    // lease 到期后也不重投（已 Acknowledged）。
    h.clock.advance(static_cast<int64_t>(kLeaseMs) + 1);
    h.rm.sessionAvailable(SessionIdentity{kBob, 1});
    ASSERT_EQ(1u, h.sink.attempts().size());
}

TEST(DeliveryCoordinatorTest, LegacyChainDeliversAllPendingInConversation)
{
    Harness h;
    // 同 conversation 两条 legacy Pending：一次 sessionAvailable 链式全部补投
    // （implicit-ack 连续放行；等价旧离线补投语义）。
    AcceptOutcome m1 = h.rm.accept(SessionIdentity{kAlice, 1}, directTo(kBob, "legacy:1:1", "a"));
    ASSERT_TRUE(m1.ok);
    AcceptOutcome m2 = h.rm.accept(SessionIdentity{kAlice, 1}, directTo(kBob, "legacy:1:2", "b"));
    ASSERT_TRUE(m2.ok);
    EXPECT_EQ(1u, m1.sequence.value);
    EXPECT_EQ(2u, m2.sequence.value);

    h.rm.sessionAvailable(SessionIdentity{kBob, 1});
    ASSERT_EQ(2u, h.sink.attempts().size());
    EXPECT_EQ(m1.messageId.value, h.sink.attempts()[0].messageId.value);
    EXPECT_EQ(m2.messageId.value, h.sink.attempts()[1].messageId.value);
    EXPECT_EQ(1u, h.sink.attempts()[0].attemptNumber);
    EXPECT_EQ(1u, h.sink.attempts()[1].attemptNumber);
}

TEST(DeliveryCoordinatorTest, OnlineAcceptTriggersImmediateClaim)
{
    Harness h;
    h.rm.sessionAvailable(SessionIdentity{kBob, 1});

    // 在线接收者：accept 提交后立即 claim 投递（不需要再 sessionAvailable）。
    AcceptOutcome a = h.rm.accept(SessionIdentity{kAlice, 1}, directTo(kBob, "m1", "hello"));
    ASSERT_TRUE(a.ok);
    ASSERT_EQ(1u, h.sink.attempts().size());
    EXPECT_EQ(a.messageId.value, h.sink.attempts()[0].messageId.value);
    EXPECT_EQ(kBob.value, h.sink.attempts()[0].recipient.value);
}

TEST(DeliveryCoordinatorTest, OfflineAcceptStaysPendingUntilLogin)
{
    Harness h;
    AcceptOutcome a = h.rm.accept(SessionIdentity{kAlice, 1}, directTo(kBob, "m1", "hello"));
    ASSERT_TRUE(a.ok);
    ASSERT_TRUE(h.sink.attempts().empty());  // 离线：不投递

    h.rm.sessionAvailable(SessionIdentity{kBob, 1});
    ASSERT_EQ(1u, h.sink.attempts().size());
    EXPECT_EQ(a.messageId.value, h.sink.attempts()[0].messageId.value);
}

TEST(DeliveryCoordinatorTest, AckBeforeDeliveryNeverClaims)
{
    Harness h;
    // 迟到 ACK（接收端未收到投递就确认）：确认后不再 claim。
    AcceptOutcome a = h.rm.accept(SessionIdentity{kAlice, 1}, directTo(kBob, "m1", "hello"));
    ASSERT_TRUE(a.ok);

    EXPECT_EQ(AckResult::Acknowledged, h.rm.acknowledge(SessionIdentity{kBob, 1}, a.messageId).result);
    h.rm.sessionAvailable(SessionIdentity{kBob, 1});
    ASSERT_TRUE(h.sink.attempts().empty());
}

TEST(DeliveryCoordinatorTest, ExpiredLeaseReclaimedByNewSession)
{
    Harness h;
    AcceptOutcome a = h.rm.accept(SessionIdentity{kAlice, 1}, directTo(kBob, "m1", "hello"));
    ASSERT_TRUE(a.ok);

    h.rm.sessionAvailable(SessionIdentity{kBob, 1});
    ASSERT_EQ(1u, h.sink.attempts().size());

    // lease owner 崩溃（无 sessionClosed 清理）：lease 到期后新 Session 可重领。
    h.clock.advance(static_cast<int64_t>(kLeaseMs) + 1);
    h.rm.sessionAvailable(SessionIdentity{kBob, 2});
    ASSERT_EQ(2u, h.sink.attempts().size());
    EXPECT_EQ(2u, h.sink.attempts()[1].attemptNumber);
    EXPECT_EQ(a.messageId.value, h.sink.attempts()[1].messageId.value);

    // 新 owner 的 ACK 正常终结。
    EXPECT_EQ(AckResult::Acknowledged, h.rm.acknowledge(SessionIdentity{kBob, 2}, a.messageId).result);
    h.rm.sessionAvailable(SessionIdentity{kBob, 2});
    ASSERT_EQ(2u, h.sink.attempts().size());
}

TEST(DeliveryCoordinatorTest, ClosedSessionReleaseDoesNotAffectNewOwner)
{
    Harness h;
    AcceptOutcome a = h.rm.accept(SessionIdentity{kAlice, 1}, directTo(kBob, "m1", "hello"));
    ASSERT_TRUE(a.ok);

    h.rm.sessionAvailable(SessionIdentity{kBob, 1});
    ASSERT_EQ(1u, h.sink.attempts().size());

    // lease 到期后新 Session（gen2）重领（owner=gen2）。
    h.clock.advance(static_cast<int64_t>(kLeaseMs) + 1);
    h.rm.sessionAvailable(SessionIdentity{kBob, 2});
    ASSERT_EQ(2u, h.sink.attempts().size());
    EXPECT_EQ(2u, h.sink.attempts()[1].attemptNumber);

    // 旧 Session 断开：只回滚自己名下 InFlight（leaseOwner==gen2 不被 gen1 的
    // close 触碰），新 Session 的在途保持，不重投。
    h.rm.sessionClosed(SessionIdentity{kBob, 1});
    h.rm.sessionAvailable(SessionIdentity{kBob, 2});
    ASSERT_EQ(2u, h.sink.attempts().size());

    // 新 owner 的 ACK 正常终结，之后不再 claim。
    EXPECT_EQ(AckResult::Acknowledged, h.rm.acknowledge(SessionIdentity{kBob, 2}, a.messageId).result);
    h.rm.sessionAvailable(SessionIdentity{kBob, 2});
    ASSERT_EQ(2u, h.sink.attempts().size());
}

TEST(DeliveryCoordinatorTest, CloseBeforeNewSessionClaimRaceRecovers)
{
    Harness h;
    AcceptOutcome a = h.rm.accept(SessionIdentity{kAlice, 1}, directTo(kBob, "m1", "hello"));
    ASSERT_TRUE(a.ok);

    h.rm.sessionAvailable(SessionIdentity{kBob, 1});
    ASSERT_EQ(1u, h.sink.attempts().size());

    // 重连竞态：新 Session 上线时旧 lease 仍有效。generation fencing 必须
    // 立即回收旧 claim 并交给当前在线会话，不依赖 lease timeout。
    h.rm.sessionAvailable(SessionIdentity{kBob, 2});
    ASSERT_EQ(2u, h.sink.attempts().size());
    h.rm.sessionClosed(SessionIdentity{kBob, 1});
    ASSERT_EQ(2u, h.sink.attempts().size());
    EXPECT_EQ(2u, h.sink.attempts()[1].attemptNumber);
    EXPECT_EQ(a.messageId.value, h.sink.attempts()[1].messageId.value);

    EXPECT_EQ(AckResult::Acknowledged,
              h.rm.acknowledge(SessionIdentity{kBob, 2}, a.messageId).result);
    h.rm.sessionAvailable(SessionIdentity{kBob, 2});
    ASSERT_EQ(2u, h.sink.attempts().size());
}

TEST(DeliveryCoordinatorTest, StaleGenerationAckCannotAcknowledgeOrReleaseHol)
{
    Harness h;
    AcceptOutcome first =
        h.rm.accept(SessionIdentity{kAlice, 1}, directTo(kBob, "m1", "first"));
    ASSERT_TRUE(first.ok);
    AcceptOutcome second =
        h.rm.accept(SessionIdentity{kAlice, 1}, directTo(kBob, "m2", "second"));
    ASSERT_TRUE(second.ok);

    h.rm.sessionAvailable(SessionIdentity{kBob, 1});
    ASSERT_EQ(1u, h.sink.attempts().size());

    // Generation 2 has reclaimed the same head after the old lease expired.
    h.clock.advance(static_cast<int64_t>(kLeaseMs) + 1);
    h.rm.sessionAvailable(SessionIdentity{kBob, 2});
    ASSERT_EQ(2u, h.sink.attempts().size());
    EXPECT_EQ(first.messageId.value, h.sink.attempts()[1].messageId.value);

    // A delayed ACK from generation 1 must not acknowledge generation 2's
    // lease or release the next sequence past HOL.
    AckOutcome stale = h.rm.acknowledge(SessionIdentity{kBob, 1}, first.messageId);
    EXPECT_NE(AckResult::Acknowledged, stale.result);
    EXPECT_EQ(2u, h.sink.attempts().size());

    EXPECT_EQ(AckResult::Acknowledged,
              h.rm.acknowledge(SessionIdentity{kBob, 2}, first.messageId).result);
    ASSERT_EQ(3u, h.sink.attempts().size());
    EXPECT_EQ(second.messageId.value, h.sink.attempts()[2].messageId.value);
}

TEST(DeliveryCoordinatorTest, NewGenerationImmediatelyReclaimsOldLease)
{
    Harness h;
    AcceptOutcome first =
        h.rm.accept(SessionIdentity{kAlice, 1}, directTo(kBob, "m1", "first"));
    ASSERT_TRUE(first.ok);

    h.rm.sessionAvailable(SessionIdentity{kBob, 1});
    ASSERT_EQ(1u, h.sink.attempts().size());

    // Reconnect before the old lease expires: generation change itself must
    // fence the old claim and make the message immediately claimable.
    h.rm.sessionAvailable(SessionIdentity{kBob, 2});
    ASSERT_EQ(2u, h.sink.attempts().size());
    EXPECT_EQ(first.messageId.value, h.sink.attempts()[1].messageId.value);
    EXPECT_EQ(2u, h.sink.attempts()[1].attemptNumber);
}

TEST(DeliveryCoordinatorTest, LateOlderSessionAvailableCannotRewindActiveGeneration)
{
    Harness h;
    AcceptOutcome first =
        h.rm.accept(SessionIdentity{kAlice, 1}, directTo(kBob, "m1", "first"));
    ASSERT_TRUE(first.ok);

    h.rm.sessionAvailable(SessionIdentity{kBob, 1});
    ASSERT_EQ(1u, h.sink.attempts().size());

    // A newer generation fences the old lease and claims the head.
    h.rm.sessionAvailable(SessionIdentity{kBob, 2});
    ASSERT_EQ(2u, h.sink.attempts().size());
    EXPECT_EQ(2u, h.sink.attempts()[1].attemptNumber);

    // A delayed old sessionAvailable must be ignored: it must not roll back
    // generation 2's lease or make generation 1 the active claimant again.
    h.rm.sessionAvailable(SessionIdentity{kBob, 1});
    ASSERT_EQ(2u, h.sink.attempts().size());

    EXPECT_EQ(AckResult::Acknowledged,
              h.rm.acknowledge(SessionIdentity{kBob, 2}, first.messageId).result);
    h.rm.sessionAvailable(SessionIdentity{kBob, 2});
    ASSERT_EQ(2u, h.sink.attempts().size());
}

TEST(DeliveryCoordinatorTest, ClosedHigherGenerationStillFencesOlderAck)
{
    Harness h;
    AcceptOutcome first =
        h.rm.accept(SessionIdentity{kAlice, 1}, directTo(kBob, "m1", "first"));
    ASSERT_TRUE(first.ok);

    // Generation 2 has observed and claimed the delivery, then disconnects.
    h.rm.sessionAvailable(SessionIdentity{kBob, 2});
    ASSERT_EQ(1u, h.sink.attempts().size());
    h.rm.sessionClosed(SessionIdentity{kBob, 2});

    // Even with no active session entry, a delayed generation 1 ACK must be
    // rejected after generation 2 was observed.
    AckOutcome stale = h.rm.acknowledge(SessionIdentity{kBob, 1}, first.messageId);
    EXPECT_EQ(AckResult::NotRecipient, stale.result);

    // The pending delivery remains claimable by generation 2, proving that the
    // rejected stale ACK did not acknowledge it.
    h.rm.sessionAvailable(SessionIdentity{kBob, 2});
    ASSERT_EQ(2u, h.sink.attempts().size());
    EXPECT_EQ(AckResult::Acknowledged,
              h.rm.acknowledge(SessionIdentity{kBob, 2}, first.messageId).result);
}

TEST(DeliveryCoordinatorTest, StaleResumeCannotReclaimOrRebindNewGeneration)
{
    Harness h;
    AcceptOutcome first =
        h.rm.accept(SessionIdentity{kAlice, 1}, directTo(kBob, "m1", "first"));
    ASSERT_TRUE(first.ok);
    AcceptOutcome second =
        h.rm.accept(SessionIdentity{kCarol, 1}, directTo(kBob, "m2", "second"));
    ASSERT_TRUE(second.ok);

    // Generation 1 owns the first conversation, then pauses on the second.
    h.sink.enqueue(DeliverDisposition::Accepted);
    h.sink.enqueue(DeliverDisposition::Paused);
    h.rm.sessionAvailable(SessionIdentity{kBob, 1});
    ASSERT_EQ(2u, h.sink.attempts().size());

    // A low-water resume intent from generation 1 is queued while generation
    // 2 logs in. Generation 2 immediately fences/reclaims the old lease, then
    // pauses on the second conversation as well.
    h.sink.enqueue(DeliverDisposition::Accepted);
    h.sink.enqueue(DeliverDisposition::Paused);
    h.rm.sessionAvailable(SessionIdentity{kBob, 2});
    ASSERT_EQ(4u, h.sink.attempts().size());
    EXPECT_EQ(first.messageId.value, h.sink.attempts()[2].messageId.value);
    EXPECT_EQ(second.messageId.value, h.sink.attempts()[3].messageId.value);

    // The stale resume must not clear/fence generation 2's state or claim the
    // still-pending second delivery under generation 1.
    h.rm.resume(SessionIdentity{kBob, 1});
    ASSERT_EQ(4u, h.sink.attempts().size());
    std::vector<Delivery> firstRows = h.store.deliveriesByMessage(first.messageId);
    ASSERT_EQ(1u, firstRows.size());
    const SessionIdentity newOwner{kBob, 2};
    EXPECT_EQ(newOwner, firstRows[0].leaseOwner);

    // Generation 2 remains the valid ACK owner after the stale resume.
    EXPECT_EQ(AckResult::Acknowledged,
              h.rm.acknowledge(SessionIdentity{kBob, 2}, first.messageId).result);
}

// P3-08 跨进程重启 owner 碰撞 RED：模拟服务 kill 后重启（无 sessionClosed 清理），
// 新 coordinator 实例看到与自身完全相同的 (uid, gen) 的未到期 InFlight lease。
// 现状（MySQL 侧 lease_owner 仅编 "uid:gen"，跨进程生成代重置后字符串相同）：
// sessionAvailable 的 generation-fencing 判定 `d.leaseOwner != session` 为假，
// 不会立即回收，HOL 被卡到 lease 到期。期望：进程实例标识使跨进程 owner 必不同，
// 重启后立即重领（不等 lease 到期、不等 ack-timeout）。时钟只前进 50ms
// （< ack_timeout、<< lease），排除 runRetryScan / lease 到期路径（RED 依据）。
TEST(DeliveryCoordinatorTest, RestartWithSameGenerationReclaimsStaleLease)
{
    FakeClock clock;
    InMemoryMessageStore store;
    ScriptedDeliverySink sink;
    clock.set(kNow);

    // Process A: {bob,1} claims the delivery, then crashes (no sessionClosed).
    AcceptOutcome a;
    {
        ReliableMessaging rmA(store, sink, clock, kLeaseMs);
        a = rmA.accept(SessionIdentity{kAlice, 1}, directTo(kBob, "m1", "hello"));
        ASSERT_TRUE(a.ok);
        rmA.sessionAvailable(SessionIdentity{kBob, 1});
        ASSERT_EQ(1u, sink.attempts().size());
        EXPECT_EQ(1u, sink.attempts()[0].attemptNumber);
    }

    // Restart: a fresh coordinator instance observes the same uid:gen {bob,1}.
    // Lease not expired and ack-timeout not elapsed; only the stale owner
    // (identical "uid:gen" across processes) would block immediate reclaim.
    clock.advance(50);
    ReliableMessaging rmB(store, sink, clock, kLeaseMs);
    rmB.sessionAvailable(SessionIdentity{kBob, 1});
    ASSERT_EQ(2u, sink.attempts().size());  // RED: stuck at 1 until lease expiry
    EXPECT_EQ(a.messageId.value, sink.attempts()[1].messageId.value);
    EXPECT_EQ(2u, sink.attempts()[1].attemptNumber);
    rmB.stop(0);
}

TEST(DeliveryCoordinatorTest, ResumeAfterClosedSessionCannotClaimWithoutActiveSession)
{
    Harness h;
    AcceptOutcome first =
        h.rm.accept(SessionIdentity{kAlice, 1}, directTo(kBob, "m1", "first"));
    ASSERT_TRUE(first.ok);

    const SessionIdentity generationOne{kBob, 1};
    h.rm.sessionAvailable(generationOne);
    ASSERT_EQ(1u, h.sink.attempts().size());

    // The old connection closed after claiming. Its delivery is Pending again,
    // and the domain active-session entry has been removed. A delayed resume
    // from that connection must not claim until the replacement is available.
    h.rm.sessionClosed(generationOne);
    h.rm.resume(generationOne);
    EXPECT_EQ(1u, h.sink.attempts().size());

    std::vector<Delivery> rows = h.store.deliveriesByMessage(first.messageId);
    ASSERT_EQ(1u, rows.size());
    EXPECT_EQ(DeliveryState::Pending, rows[0].state);
    EXPECT_EQ(1u, rows[0].attemptCount);
}

} // namespace
