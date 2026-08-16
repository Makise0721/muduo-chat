// P4-01 RED：PresenceDirectory 领域 interface 的 in-memory contract
// （docs/tasks/P4-01.md、docs/adr/0002-cluster-ownership-and-failure-contract.md、
// docs/architecture/cluster-context-map.md §1/§3）。
//
// P4-02 起 PresenceDirectory 抽象为 port（docs/tasks/P4-02.md"Port 重构理由"节），
// 原 in-memory 具体实现迁为 InMemoryPresenceDirectory（语义零改动，injectFailure
// 保留为具体 adapter seam）。本文件 11 处实例化改 InMemoryPresenceDirectory。
//
// 全部断言穿过公开 interface（claim/renew/locate/release + 注入 seam），不读模块
// 私有容器；TTL 由可控 FakeClock 驱动，无固定 sleep。
//
// 语义约束（卡 Interface/完成定义，本文件逐测试断言）：
//   - claim 不带 epoch 参数：epoch 由 claim 生成（cluster-context-map §1 "SessionEpoch
//     由 claim 生成，Gateways 只读"），claim 无条件原子覆盖既有条目、无需旧 lease
//     已过期（§3 不变式）；renew/release 携带 epoch 做 compare-and-delete。
//   - 错误可区分：NotEpoch / NotFound / DependencyUnavailable 各有唯一可断言结果。
//   - 状态转换原子：单次调用二值结果，无部分状态；并发 claim 终态单一、无双路由残留。
//   - 无 epoch 回退：clock 前跳/回退不使可观测 epoch 低于当前 claim 的 epoch。

#include "app/InMemoryPresenceDirectory.hpp"
#include "FakeClock.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace {

const UserId kAlice{1};
const UserId kBob{2};
const GatewayId kGwA{10};
const GatewayId kGwB{20};
const ConnectionId kConn1{1};
const ConnectionId kConn2{2};
const int64_t kTtlMs = 1000;
const int64_t kT0 = 1000000;

// 卡场景 1：Interface "claim 总是生成新 epoch"；context-map §1 "SessionEpoch 单调递增"。
// 首次 claim 成功且 epoch>0；后续 claim（同/异 Gateway、同/异 User）epoch 严格单调递增。
TEST(PresenceContractTest, ClaimReturnsMonotonicEpoch)
{
    FakeClock clock;
    clock.set(kT0);
    InMemoryPresenceDirectory dir(clock, kTtlMs);

    ClaimResult c1 = dir.claim(kAlice, kGwA, kConn1);
    ASSERT_TRUE(c1.ok);
    ASSERT_GT(c1.epoch.value, 0u);

    // 同 Gateway 重 claim 生成新 epoch（重 claim 不幂等、必出新 epoch）。
    ClaimResult c2 = dir.claim(kAlice, kGwA, kConn1);
    ASSERT_TRUE(c2.ok);
    EXPECT_GT(c2.epoch.value, c1.epoch.value);

    // 不同 User / 不同 Gateway 的 claim 仍推进同一单调序列（全局单调，无回退）。
    ClaimResult c3 = dir.claim(kBob, kGwB, kConn2);
    ASSERT_TRUE(c3.ok);
    EXPECT_GT(c3.epoch.value, c2.epoch.value);

    // locate 反映最后一次 claim 的 epoch。
    LocateResult l = dir.locate(kAlice);
    ASSERT_TRUE(l.ok);
    EXPECT_EQ(c2.epoch, l.route.sessionEpoch);
}

// 卡场景 2：P4-00 关键不变式（context-map §3 "claim 总是生成新 epoch 并原子覆盖
// 既有条目，无需旧 lease 已过期"）。两次 claim 间不做任何时钟推进：旧 lease 未
// 过期也必须被覆盖。
TEST(PresenceContractTest, ClaimAlwaysGeneratesNewEpochAndOverwrites)
{
    FakeClock clock;
    clock.set(kT0);
    InMemoryPresenceDirectory dir(clock, kTtlMs);

    ClaimResult c1 = dir.claim(kAlice, kGwA, kConn1);
    ASSERT_TRUE(c1.ok);
    // 立即（TTL 内、无任何 advance）再次 claim 另一 Gateway：原子覆盖。
    ClaimResult c2 = dir.claim(kAlice, kGwB, kConn2);
    ASSERT_TRUE(c2.ok);

    // 覆盖是原子的：locate 只看到 c2 的完整路由（gateway/conn/epoch 同属一次写入），
    // 无 gateway 与 epoch 撕裂混合（无部分状态）。
    LocateResult l = dir.locate(kAlice);
    ASSERT_TRUE(l.ok);
    EXPECT_EQ(kGwB, l.route.gatewayId);
    EXPECT_EQ(kConn2, l.route.connectionId);
    EXPECT_EQ(c2.epoch, l.route.sessionEpoch);
    EXPECT_NE(c1.epoch, l.route.sessionEpoch);

    // 被覆盖的 epoch 立即失效（fencing 收敛到新 epoch）。
    RenewResult r = dir.renew(kAlice, kGwA, kConn1, c1.epoch);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(PresenceError::NotEpoch, r.error);
}

// 卡场景 3：Interface "renew 携带当前 epoch 生效"（TTL 续期）。续期可观测：越过原
// 到期时刻、仍在续期后 TTL 内时 locate 仍命中。
TEST(PresenceContractTest, RenewWithCurrentEpochSucceeds)
{
    FakeClock clock;
    clock.set(kT0);
    InMemoryPresenceDirectory dir(clock, kTtlMs);

    ClaimResult c1 = dir.claim(kAlice, kGwA, kConn1);
    ASSERT_TRUE(c1.ok);

    clock.advance(kTtlMs - 10);  // 原到期前 10ms
    RenewResult r = dir.renew(kAlice, kGwA, kConn1, c1.epoch);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(clock.nowMs() + kTtlMs, r.expiresAtMs);

    // 越过原到期点（T0+TTL+5）、仍在续期 TTL 内 → locate 仍活。
    clock.advance(15);
    LocateResult l = dir.locate(kAlice);
    ASSERT_TRUE(l.ok);
    EXPECT_EQ(c1.epoch, l.route.sessionEpoch);
}

// 卡场景 4：RED "旧 epoch renew 被拒"。重 claim 后持旧 epoch renew 被拒（NotEpoch），
// 且条目不变（locate 仍是新 epoch 的路由）。
TEST(PresenceContractTest, RenewWithStaleEpochRejected)
{
    FakeClock clock;
    clock.set(kT0);
    InMemoryPresenceDirectory dir(clock, kTtlMs);

    ClaimResult c1 = dir.claim(kAlice, kGwA, kConn1);
    ASSERT_TRUE(c1.ok);
    ClaimResult c2 = dir.claim(kAlice, kGwB, kConn2);
    ASSERT_TRUE(c2.ok);

    RenewResult stale = dir.renew(kAlice, kGwA, kConn1, c1.epoch);
    EXPECT_FALSE(stale.ok);
    EXPECT_EQ(PresenceError::NotEpoch, stale.error);

    // 错误可区分于 NotFound：条目仍在（被拒原因是 epoch 不匹配，而非不存在）。
    LocateResult l = dir.locate(kAlice);
    ASSERT_TRUE(l.ok);
    EXPECT_EQ(c2.epoch, l.route.sessionEpoch);

    // 持当前 epoch 仍可 renew（stale renew 不破坏条目）。
    RenewResult cur = dir.renew(kAlice, kGwB, kConn2, c2.epoch);
    EXPECT_TRUE(cur.ok);
}

// 卡场景 5：RED "release compare-and-delete 精确语义：epoch 匹配删除成功；条目消失"。
// 再次 release（条目已无）幂等/不存在（NotFound），且不破坏后续 claim。
TEST(PresenceContractTest, ReleaseIsCompareAndDelete)
{
    FakeClock clock;
    clock.set(kT0);
    InMemoryPresenceDirectory dir(clock, kTtlMs);

    ClaimResult c1 = dir.claim(kAlice, kGwA, kConn1);
    ASSERT_TRUE(c1.ok);

    ReleaseResult rel = dir.release(kAlice, kGwA, kConn1, c1.epoch);
    ASSERT_TRUE(rel.ok);

    // 条目消失：locate 报不存在，expired=false（与 TTL 到期可区分）。
    LocateResult l = dir.locate(kAlice);
    EXPECT_FALSE(l.ok);
    EXPECT_FALSE(l.expired) << "release 删除的条目应像从未 claim 一样（非 TTL 过期）";
    EXPECT_EQ(PresenceError::NotFound, l.error);

    // 再次 release：条目已不存在 → NotFound（幂等无害，无双重删除、无状态破坏）。
    ReleaseResult again = dir.release(kAlice, kGwA, kConn1, c1.epoch);
    EXPECT_FALSE(again.ok);
    EXPECT_EQ(PresenceError::NotFound, again.error);

    // 释放后可重新 claim，新 epoch 单调。
    ClaimResult c2 = dir.claim(kAlice, kGwA, kConn1);
    ASSERT_TRUE(c2.ok);
    EXPECT_GT(c2.epoch.value, c1.epoch.value);
}

// 卡场景 6：RED "旧 epoch release（compare-and-delete）被拒且条目不变"——防旧
// Gateway 延迟 release 误删新租约（ADR-0002 后果：延迟 release 经 epoch fencing）。
TEST(PresenceContractTest, ReleaseWithStaleEpochRejected)
{
    FakeClock clock;
    clock.set(kT0);
    InMemoryPresenceDirectory dir(clock, kTtlMs);

    ClaimResult c1 = dir.claim(kAlice, kGwA, kConn1);
    ASSERT_TRUE(c1.ok);
    ClaimResult c2 = dir.claim(kAlice, kGwB, kConn2);
    ASSERT_TRUE(c2.ok);

    // 旧 Gateway（c1）迟到 release：epoch 不匹配 → 拒绝，新租约（c2）原样保留。
    ReleaseResult stale = dir.release(kAlice, kGwA, kConn1, c1.epoch);
    EXPECT_FALSE(stale.ok);
    EXPECT_EQ(PresenceError::NotEpoch, stale.error);

    LocateResult l = dir.locate(kAlice);
    ASSERT_TRUE(l.ok);
    EXPECT_EQ(kGwB, l.route.gatewayId);
    EXPECT_EQ(kConn2, l.route.connectionId);
    EXPECT_EQ(c2.epoch, l.route.sessionEpoch);

    // 新 owner 持当前 epoch release 仍成功（旧迟到 release 未破坏新租约）。
    ReleaseResult cur = dir.release(kAlice, kGwB, kConn2, c2.epoch);
    EXPECT_TRUE(cur.ok);
}

// 卡场景 7：Interface "locate 返回当前路由 (gateway_id, connection_id, session_epoch)；
// 不命中返回不存在（与 TTL 到期可区分）"。从未 claim 的 User → NotFound 且
// expired=false；已 claim 的 User → 完整路由。
TEST(PresenceContractTest, LocateReturnsRouteForLiveEntry)
{
    FakeClock clock;
    clock.set(kT0);
    InMemoryPresenceDirectory dir(clock, kTtlMs);

    // 从未 claim → 不存在（expired=false 区分"从未 claim"与"TTL 到期"）。
    LocateResult absent = dir.locate(kBob);
    EXPECT_FALSE(absent.ok);
    EXPECT_FALSE(absent.expired);
    EXPECT_EQ(PresenceError::NotFound, absent.error);

    ClaimResult c1 = dir.claim(kAlice, kGwA, kConn1);
    ASSERT_TRUE(c1.ok);

    // 命中 → 完整路由三要素与 claim 一致。
    LocateResult hit = dir.locate(kAlice);
    ASSERT_TRUE(hit.ok);
    EXPECT_EQ(kAlice, hit.route.user);
    EXPECT_EQ(kGwA, hit.route.gatewayId);
    EXPECT_EQ(kConn1, hit.route.connectionId);
    EXPECT_EQ(c1.epoch, hit.route.sessionEpoch);
}

// 卡场景 8：RED "TTL 到期后 locate 不存在（与从未 claim 的结果可区分）"。FakeClock
// 驱动，无固定 sleep：TTL 内活；到期边界（now == expiresAtMs，M3 冻结 >= 语义）与
// 越过 TTL 后 locate 均不存在且 expired=true、renew 报 NotFound。
TEST(PresenceContractTest, TtlExpiryMakesEntryAbsent)
{
    FakeClock clock;
    clock.set(kT0);
    InMemoryPresenceDirectory dir(clock, kTtlMs);

    ClaimResult c1 = dir.claim(kAlice, kGwA, kConn1);
    ASSERT_TRUE(c1.ok);

    clock.advance(kTtlMs - 1);  // TTL 内最后 1ms：仍活
    LocateResult live = dir.locate(kAlice);
    ASSERT_TRUE(live.ok);

    clock.advance(1);  // now == expiresAtMs（到期边界）：实现 >= 语义 → 视为不存在（M3 冻结）
    LocateResult boundary = dir.locate(kAlice);
    EXPECT_FALSE(boundary.ok);
    EXPECT_TRUE(boundary.expired) << "now == expiresAtMs 恰在到期时刻视为不存在（>= 语义）";
    EXPECT_EQ(PresenceError::NotFound, boundary.error);

    clock.advance(1);  // 越过到期：仍不存在，且 expired=true 与"从未 claim"可区分
    LocateResult gone = dir.locate(kAlice);
    EXPECT_FALSE(gone.ok);
    EXPECT_TRUE(gone.expired);
    EXPECT_EQ(PresenceError::NotFound, gone.error);

    // renew 对已到期条目报不存在（NotFound，而非 NotEpoch——条目已无 epoch 可校验）。
    RenewResult r = dir.renew(kAlice, kGwA, kConn1, c1.epoch);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(PresenceError::NotFound, r.error);

    // 过期后 claim 正常生成新 epoch（TTL 到期不阻塞新 claim）。
    ClaimResult c2 = dir.claim(kAlice, kGwB, kConn2);
    ASSERT_TRUE(c2.ok);
    EXPECT_GT(c2.epoch.value, c1.epoch.value);
}

// 卡场景 9：RED "两实例并发 claim 同一 user 恰一赢（恰一个成功持新 epoch，另一个被
// 原子覆盖，无双路由残留）"。真实两线程经 barrier 并发 claim 同一 User；断言终态
// 唯一且一致（最后一次写入者持最大 epoch，无撕裂、无双路由），败者 epoch 被 fencing
// （renew 报 NotEpoch）。终态断言与交错顺序无关（确定性）。
TEST(PresenceContractTest, ConcurrentClaimsOneWins)
{
    FakeClock clock;
    clock.set(kT0);
    InMemoryPresenceDirectory dir(clock, kTtlMs);

    struct ClaimAttempt {
        ClaimResult result;
        GatewayId gateway;
        ConnectionId conn;
    };

    std::atomic<int> go{0};
    std::mutex mu;
    std::vector<ClaimAttempt> attempts;
    std::vector<std::thread> threads;
    for (int i = 0; i < 2; ++i) {
        threads.emplace_back([&, i] {
            while (go.load() == 0) {
                std::this_thread::yield();
            }
            ClaimAttempt a;
            a.gateway = (i == 0) ? kGwA : kGwB;
            a.conn = (i == 0) ? kConn1 : kConn2;
            a.result = dir.claim(kAlice, a.gateway, a.conn);
            std::lock_guard<std::mutex> lock(mu);
            attempts.push_back(a);
        });
    }
    go.store(1);
    for (size_t i = 0; i < threads.size(); ++i) {
        threads[i].join();
    }

    ASSERT_EQ(2u, attempts.size());
    for (size_t i = 0; i < attempts.size(); ++i) {
        ASSERT_TRUE(attempts[i].result.ok) << "thread " << i;
    }
    // 两次 claim 各生成不同新 epoch（无幂等重claim）。
    EXPECT_NE(attempts[0].result.epoch, attempts[1].result.epoch);

    // 恰一赢：终态 = 最后一次写入（epoch 最大者），无双路由残留，路由与赢家无撕裂。
    const size_t winner = (attempts[0].result.epoch.value > attempts[1].result.epoch.value) ? 0 : 1;
    LocateResult l = dir.locate(kAlice);
    ASSERT_TRUE(l.ok);
    EXPECT_EQ(attempts[winner].result.epoch, l.route.sessionEpoch);
    EXPECT_EQ(attempts[winner].gateway, l.route.gatewayId);
    EXPECT_EQ(attempts[winner].conn, l.route.connectionId);

    // 败者 epoch 被 fencing（并发 claim 冲突的可断言结果）：持败者 epoch renew 报 NotEpoch。
    const size_t loser = 1 - winner;
    RenewResult r = dir.renew(kAlice, attempts[loser].gateway, attempts[loser].conn,
                              attempts[loser].result.epoch);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(PresenceError::NotEpoch, r.error);
}

// 卡场景 10：RED "dependency unavailable：adapter 注入失败（模拟依赖不可用）返回
// 可区分错误"。注入失败时四操作统一返回 DependencyUnavailable，区别于
// NotEpoch/NotFound；关闭注入后恢复，且注入期间不破坏既有状态。
TEST(PresenceContractTest, DependencyUnavailableIsDistinctError)
{
    FakeClock clock;
    clock.set(kT0);
    InMemoryPresenceDirectory dir(clock, kTtlMs);

    ClaimResult c1 = dir.claim(kAlice, kGwA, kConn1);
    ASSERT_TRUE(c1.ok);

    dir.injectFailure(true);

    // 四操作统一 DependencyUnavailable（与业务错误区分：不是 NotEpoch/NotFound）。
    ClaimResult c = dir.claim(kBob, kGwB, kConn2);
    EXPECT_FALSE(c.ok);
    EXPECT_EQ(PresenceError::DependencyUnavailable, c.error);

    RenewResult r = dir.renew(kAlice, kGwA, kConn1, c1.epoch);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(PresenceError::DependencyUnavailable, r.error);

    ReleaseResult rel = dir.release(kAlice, kGwA, kConn1, c1.epoch);
    EXPECT_FALSE(rel.ok);
    EXPECT_EQ(PresenceError::DependencyUnavailable, rel.error);

    LocateResult l = dir.locate(kAlice);
    EXPECT_FALSE(l.ok);
    EXPECT_EQ(PresenceError::DependencyUnavailable, l.error);

    // 注入失败是 per-op 故障注入：不销毁既有条目。恢复后条目原样可读。
    dir.injectFailure(false);
    LocateResult back = dir.locate(kAlice);
    ASSERT_TRUE(back.ok);
    EXPECT_EQ(c1.epoch, back.route.sessionEpoch);
}

// 卡场景 11：RED "clock jump 行为：可控 Clock 前跳/回退时 renew/locate 的 TTL 判定
// 不产生错误状态（不出现 epoch 回退）"。卡未冻结具体跳变语义，本测试登记并断言卡内
// 语义：
//   - 回退（set 到更早时刻）：条目从未过期，以注入 Clock 重新判定 TTL 仍活；epoch
//     保持当前 claim 的值（不出现 epoch 回退），renew 持当前 epoch 成功；
//   - 前跳（set 大幅越过 TTL）：与逐步推进同果——locate 不存在且 expired=true、renew
//     报 NotFound（二值结果，无错误状态）；
//   - 跳变后 claim 的 epoch 仍严格单调（时钟跳变不产生 epoch 回退）。
TEST(PresenceContractTest, ClockJumpBehaviorIsDefined)
{
    FakeClock clock;
    clock.set(kT0);
    InMemoryPresenceDirectory dir(clock, kTtlMs);

    ClaimResult c1 = dir.claim(kAlice, kGwA, kConn1);
    ASSERT_TRUE(c1.ok);
    clock.advance(kTtlMs / 2);  // TTL 内

    // 回退：set 回更早时刻 → 条目仍活、epoch 保持 c1（无 epoch 回退），renew 成功。
    clock.set(kT0 - 100);
    LocateResult back = dir.locate(kAlice);
    ASSERT_TRUE(back.ok);
    EXPECT_EQ(c1.epoch, back.route.sessionEpoch);
    RenewResult rBack = dir.renew(kAlice, kGwA, kConn1, c1.epoch);
    ASSERT_TRUE(rBack.ok);

    // 回退后再 claim：epoch 仍严格大于前值（时钟跳变不产生 epoch 回退）。
    ClaimResult c2 = dir.claim(kAlice, kGwB, kConn2);
    ASSERT_TRUE(c2.ok);
    EXPECT_GT(c2.epoch.value, c1.epoch.value);

    // 前跳：set 大幅越过 TTL → 与逐步推进同果（不存在、expired=true、renew NotFound）。
    clock.set(kT0 + 10 * kTtlMs);
    LocateResult jumped = dir.locate(kAlice);
    EXPECT_FALSE(jumped.ok);
    EXPECT_TRUE(jumped.expired);
    RenewResult rJump = dir.renew(kAlice, kGwA, kConn1, c2.epoch);
    EXPECT_FALSE(rJump.ok);
    EXPECT_EQ(PresenceError::NotFound, rJump.error);
}

} // namespace
