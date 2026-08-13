// P3-08 DeliveryRetry RED→GREEN：穿过 ReliableMessaging 公开 interface，注入
// FakeClock + TimestampedDeliverySink（记录每次 attempt 的时钟时刻）+
// InMemoryMessageStore，无固定 sleep。
// 冻结参数（docs/tasks/P3-08.md）以小值经 RetryConfig 注入；jitter=0 使 backoff
// 精确为 base*乘子^(n-1)，gap 断言可复现（生产默认 jitter=±20% 不受影响）。
// R1/R4 依赖 ack-timeout 扫描：lease(10000) 显著大于 ack_timeout(100)，时钟
// 越过 ack_timeout 后 lease 仍有效——现状只有 lease 到期被动重领、无独立
// ack-timeout 扫描，attempt 2 不发生（RED 依据）。

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
const int64_t kNow = 1000000;
// P3-08 冻结参数（测试注入小值，不得成为生产默认）。
const int64_t kAckTimeoutMs = 100;
const int64_t kBackoffBaseMs = 100;
const int64_t kBackoffCapMs = 500;
// lease 显著大于 ack_timeout：越过 ack_timeout 时 lease 仍有效，排除现状的
// lease 到期重领路径，保证 attempt 2 只可能由 ack-timeout 扫描产生（RED 依据）。
const uint64_t kLeaseMs = 10000;

// 测试注入的 RetryConfig：重试参数用小值 + jitter=0（确定性 backoff）；
// 保留期限保持生产默认（R1-R5 不触发 Expired/cleanup）。
RetryConfig testRetryConfig()
{
    RetryConfig c;
    c.ackTimeoutMs = kAckTimeoutMs;
    c.backoffBaseMs = kBackoffBaseMs;
    c.backoffCapMs = kBackoffCapMs;
    c.jitterFraction = 0.0;
    c.cleanupCycleMs = 0;
    return c;
}

// 记录 attempt 与当时时钟时刻的 sink（Accepted）；用于 backoff 间隔断言。
class TimestampedDeliverySink : public DeliverySink {
public:
    explicit TimestampedDeliverySink(Clock& clock) : clock_(clock) {}

    DeliverDisposition deliver(const DeliveryAttempt& attempt) override
    {
        attempts_.push_back(attempt);
        attemptTimesMs_.push_back(clock_.nowMs());
        return DeliverDisposition::Accepted;
    }

    const std::vector<DeliveryAttempt>& attempts() const { return attempts_; }
    const std::vector<int64_t>& attemptTimesMs() const { return attemptTimesMs_; }

private:
    Clock& clock_;
    std::vector<DeliveryAttempt> attempts_;
    std::vector<int64_t> attemptTimesMs_;
};

struct Harness {
    FakeClock clock;
    InMemoryMessageStore store;
    TimestampedDeliverySink sink;
    ReliableMessaging rm;

    Harness() : sink(clock), rm(store, sink, clock, kLeaseMs, testRetryConfig())
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

TEST(DeliveryRetryTest, AckTimeoutRetriesSameMessageId)
{
    Harness h;
    AcceptOutcome a = h.rm.accept(SessionIdentity{kAlice, 1}, directTo(kBob, "m1", "hello"));
    ASSERT_TRUE(a.ok);

    h.rm.sessionAvailable(SessionIdentity{kBob, 1});
    ASSERT_EQ(1u, h.sink.attempts().size());
    EXPECT_EQ(1u, h.sink.attempts()[0].attemptNumber);
    EXPECT_EQ(a.messageId.value, h.sink.attempts()[0].messageId.value);

    // ACK 丢失：越过 ack_timeout 后 start() 并推进扫描，同一 messageId 重投
    // （attempt 2）。现状无 ack-timeout 扫描、lease(10000) 尚有效 → attempt 2
    // 不发生（预期 RED）。
    h.clock.advance(kAckTimeoutMs + 1);
    h.rm.start();
    h.rm.sessionAvailable(SessionIdentity{kBob, 1});
    ASSERT_EQ(2u, h.sink.attempts().size());
    EXPECT_EQ(a.messageId.value, h.sink.attempts()[1].messageId.value);
    EXPECT_EQ(2u, h.sink.attempts()[1].attemptNumber);

    h.rm.stop(h.clock.nowMs() + 1000);
}

TEST(DeliveryRetryTest, NewGenerationClaimsImmediatelyWithoutBackoff)
{
    Harness h;
    AcceptOutcome a = h.rm.accept(SessionIdentity{kAlice, 1}, directTo(kBob, "m1", "hello"));
    ASSERT_TRUE(a.ok);

    h.rm.sessionAvailable(SessionIdentity{kBob, 1});
    ASSERT_EQ(1u, h.sink.attempts().size());
    EXPECT_EQ(1u, h.sink.attempts()[0].attemptNumber);

    h.rm.sessionClosed(SessionIdentity{kBob, 1});
    // 离线期间（越过 ack_timeout）无扫描触发，不产生新 attempt。
    h.clock.advance(kAckTimeoutMs + 1);
    ASSERT_EQ(1u, h.sink.attempts().size());

    // gen2 上线立即 claim 同 messageId：不等 backoff，attempt 数延续。
    h.rm.sessionAvailable(SessionIdentity{kBob, 2});
    ASSERT_EQ(2u, h.sink.attempts().size());
    EXPECT_EQ(a.messageId.value, h.sink.attempts()[1].messageId.value);
    EXPECT_EQ(2u, h.sink.attempts()[1].attemptNumber);
}

TEST(DeliveryRetryTest, BackoffIncreasesAcrossRetries)
{
    Harness h;
    AcceptOutcome a = h.rm.accept(SessionIdentity{kAlice, 1}, directTo(kBob, "m1", "hello"));
    ASSERT_TRUE(a.ok);

    h.rm.start();
    h.rm.sessionAvailable(SessionIdentity{kBob, 1});
    ASSERT_EQ(1u, h.sink.attempts().size());
    ASSERT_EQ(1u, h.sink.attemptTimesMs().size());

    // 第 1 次超时：越过 ack_timeout + base 后 attempt 2，间隔 ≥ base。
    h.clock.advance(kAckTimeoutMs + kBackoffBaseMs + 1);
    h.rm.sessionAvailable(SessionIdentity{kBob, 1});
    ASSERT_EQ(2u, h.sink.attempts().size());
    const int64_t gap1 = h.sink.attemptTimesMs()[1] - h.sink.attemptTimesMs()[0];
    EXPECT_GE(gap1, kBackoffBaseMs);
    EXPECT_LE(gap1, kBackoffCapMs);

    // 第 2 次超时：再越过 2*base 后 attempt 3，间隔 ≥ 2*base。
    h.clock.advance(2 * kBackoffBaseMs);
    h.rm.sessionAvailable(SessionIdentity{kBob, 1});
    ASSERT_EQ(3u, h.sink.attempts().size());
    const int64_t gap2 = h.sink.attemptTimesMs()[2] - h.sink.attemptTimesMs()[1];
    EXPECT_GE(gap2, 2 * kBackoffBaseMs);
    EXPECT_LE(gap2, kBackoffCapMs);

    // 同一 messageId 复用，attempt 数逐次递增。
    EXPECT_EQ(a.messageId.value, h.sink.attempts()[1].messageId.value);
    EXPECT_EQ(a.messageId.value, h.sink.attempts()[2].messageId.value);
    EXPECT_EQ(2u, h.sink.attempts()[1].attemptNumber);
    EXPECT_EQ(3u, h.sink.attempts()[2].attemptNumber);

    h.rm.stop(h.clock.nowMs() + 1000);
}

TEST(DeliveryRetryTest, OfflineHourDoesNotConsumeRetryBudget)
{
    Harness h;
    AcceptOutcome a = h.rm.accept(SessionIdentity{kAlice, 1}, directTo(kBob, "m1", "hello"));
    ASSERT_TRUE(a.ok);

    h.rm.sessionAvailable(SessionIdentity{kBob, 1});
    ASSERT_EQ(1u, h.sink.attempts().size());
    EXPECT_EQ(1u, h.sink.attempts()[0].attemptNumber);

    h.rm.sessionClosed(SessionIdentity{kBob, 1});
    // 离线一小时，无扫描触发：attempt_count 不变，sink 无新 attempt。
    h.clock.advance(3600 * 1000);
    ASSERT_EQ(1u, h.sink.attempts().size());
    std::vector<Delivery> rows = h.store.deliveriesByMessage(a.messageId);
    ASSERT_EQ(1u, rows.size());
    EXPECT_EQ(1u, rows[0].attemptCount);

    // 重连立即 claim：不等待 backoff，attempt 2 沿用同 messageId。
    h.rm.sessionAvailable(SessionIdentity{kBob, 2});
    ASSERT_EQ(2u, h.sink.attempts().size());
    EXPECT_EQ(a.messageId.value, h.sink.attempts()[1].messageId.value);
    EXPECT_EQ(2u, h.sink.attempts()[1].attemptNumber);
}

TEST(DeliveryRetryTest, StopIsBoundedAndIdempotent)
{
    Harness h;
    h.rm.start();
    // stop(deadline) 必须有限返回（有界 drain/cancel），可重复调用。
    const int64_t deadline = h.clock.nowMs() + 1000;
    h.rm.stop(deadline);
    h.rm.stop(deadline);

    // stop 后接口仍可用：accept / sessionAvailable 不崩溃。
    AcceptOutcome a = h.rm.accept(SessionIdentity{kAlice, 1}, directTo(kBob, "m1", "hello"));
    ASSERT_TRUE(a.ok);
    h.rm.sessionAvailable(SessionIdentity{kBob, 1});
    ASSERT_EQ(1u, h.sink.attempts().size());
    EXPECT_EQ(a.messageId.value, h.sink.attempts()[0].messageId.value);
}

// P3-08 M1（补缺轮）：scheduler 下一次唤醒间隔的纯函数。固定 ack_timeout(30s)
// 轮询使 attempt<6 的 backoff（base*2^n，均 < 30s）因轮询粒度不可观测；改为取
// 本 tick 触碰行中最早的 nextAttemptAtMs 与 ackTimeoutMs 的 min。RED=引用尚不
// 存在的函数（编译失败），GREEN=实现 min 语义并在 runTick 接线。
TEST(DeliveryRetryTest, ComputeNextWakeMsHonorsEarliestNextAttempt)
{
    const std::vector<int64_t> empty;
    // 无触碰行：按 ackTimeoutMs 轮询。
    EXPECT_EQ(30000, computeNextWakeMs(1000000, 30000, empty));
    // 触碰行早于 ack_timeout（attempt<6 的 backoff）：按最早行提前唤醒。
    {
        std::vector<int64_t> nexts;
        nexts.push_back(1001000);
        nexts.push_back(1005000);
        EXPECT_EQ(1000, computeNextWakeMs(1000000, 30000, nexts));
    }
    // 触碰行晚于 ack_timeout：ackTimeoutMs 封顶（不把唤醒推迟到 backoff 之外）。
    {
        std::vector<int64_t> nexts;
        nexts.push_back(1035000);  // 35s 后（> 30s ack_timeout）
        nexts.push_back(1060000);  // 60s 后
        EXPECT_EQ(30000, computeNextWakeMs(1000000, 30000, nexts));
    }
    // 已到期行：不小于 1ms（避免零等待自旋）。
    {
        std::vector<int64_t> nexts;
        nexts.push_back(999999);
        EXPECT_EQ(1, computeNextWakeMs(1000000, 30000, nexts));
    }
    // ackTimeoutMs 本身受 min-1 保护。
    EXPECT_EQ(1, computeNextWakeMs(1000000, 0, empty));
}

// F1（Medium，方向安全）：scheduler 按 ms 级 nextAttemptAtMs 唤醒，但 MySQL 持久化
// next_attempt_at 为秒（ceil 写），due 判定 floor(now_sec)>=ceil——唤醒点落在 ceil
// 秒边界前 1..999ms 时该行未到期 → runRetryScan 空 → 回退 ackTimeoutMs 轮询（有效
// ack_timeout 翻倍）。最小修复：MessageStore 暴露 timeGranularityMs()（InMemory=1、
// MySQL=1000），runTick 把 runRetryScan 返回的 nextAttemptAtMs 向上对齐到该粒度；
// 因 computeNextWakeMs 的 ackTimeoutMs 封顶会抵消对齐（now+ack_timeout 恰在 ceil
// 边界前 1..999ms），触碰行的对齐到期边界在 ack_timeout+granularity 内时改按该边界
// 唤醒。RED=断言 runTick 返回对齐后的唤醒间隔（20999），现状返回封顶错位点间隔
// （20000）。因式：不读私有容器、无 sleep；经公开 runTick seam 观察。
// store 替身：包装 InMemory 并覆写 granularity=1000（模拟 MySQL 秒持久化）。
class SecondGranularityStore : public InMemoryMessageStore {
public:
    uint32_t timeGranularityMs() override { return 1000; }
};

TEST(DeliveryRetryTest, SchedulerWakeAlignsToStoreTimeGranularity)
{
    FakeClock clock;
    SecondGranularityStore store;
    TimestampedDeliverySink sink(clock);
    RetryConfig cfg = testRetryConfig();
    cfg.ackTimeoutMs = 20000;   // retryDelay(1)=max(20000, backoff 1000)=20000
    cfg.backoffBaseMs = 1000;
    cfg.backoffCapMs = 60000;
    ReliableMessaging rm(store, sink, clock, kLeaseMs, cfg);
    clock.set(kNow);            // 1000000 = 秒边界

    // 首个 attempt 在 T=1000000（秒边界）落行：nextAttemptAtMs = 1020000（对齐）。
    AcceptOutcome a = rm.accept(SessionIdentity{kAlice, 1}, directTo(kBob, "m1", "hello"));
    ASSERT_TRUE(a.ok);
    rm.sessionAvailable(SessionIdentity{kBob, 1});
    ASSERT_EQ(1u, sink.attempts().size());
    ASSERT_EQ(1000000, sink.attemptTimesMs()[0]);

    // 越过 ack_timeout 后 1ms 触发扫描：scheduler 在错位点 1000000+20001=1020001
    // 唤醒。重投行 attempt2 落 nextAttemptAtMs = 1020001+20000 = 1040001（T+20001，
    // 距 ceil 秒边界 1041000 提前 999ms）。
    clock.advance(20001);
    ASSERT_EQ(1020001, clock.nowMs());
    const int64_t wake = rm.runTick();
    ASSERT_EQ(2u, sink.attempts().size());
    ASSERT_EQ(1020001, sink.attemptTimesMs()[1]);

    // runTick 返回 scheduler 下一次唤醒间隔：应对齐到 ceil 秒边界 1041000
    // （= now 1020001 + 20999），而非封顶错位点 20000（scheduler 醒于 1040001，
    // 该行在 MySQL 上 1041000 才到期，扫描空 → 回退整轮 ack_timeout）。
    EXPECT_EQ(20999, wake);
    rm.stop(clock.nowMs() + 1000);
}

} // namespace
