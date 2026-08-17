// P4-05 RED：Presence 生产接线契约（docs/tasks/P4-05.md §Interface 可观察行为/
// §RED/§冻结参数/§设计决定 D4/D5）。GREEN 目标：login claim、close/loginout
// compare-and-delete release、renew TTL/2 窗口、Redis down 降级——全部穿过
// GatewayAwareDeliverySink 的 bindUser/unbindUser/renew 公开面（D5 本地影子表
// seam）在真实 Redis 上断言（卡 §RED 第二段）。
//
// RED 依据（现状）：本文件引用尚不存在的 app/GatewayTransport.hpp 与
// app/GatewayAwareDeliverySink.hpp → 编译失败（`app/GatewayTransport.hpp: No such
// file or directory`）即合法 RED（沿 P4-02/P4-03/P4-04 先例）。
//
// 真实 Redis 集成（127.0.0.1:6379 db=1，env REDIS_TEST_HOST/PORT/DB 可覆盖），
// **不 skip**：Redis 不可用时 SetUp 连接失败即测试失败（P4-02/P4-05 完成定义）。
// 键空间隔离：db=1 + 前缀 presence:v1:，不动 db0；fixture SetUp/TearDown
// FLUSHDB db=1 保证测试键清理与跨轮隔离。
//
// 本文件冻结的实现契约（GREEN 据此实现；D4/D5 定案细化）：
//
//   // ---- chatserver/include/app/GatewayAwareDeliverySink.hpp（新建，D3/D4/D5 seam）----
//   // 单 Gateway 投递 + 本地 Presence 生命周期接线对象：持有本地影子表
//   // (userId → (connId, SessionEpoch))（D5：claim 写入、release 清除；内部 seam，
//   // 不进 Presence port）。本文件的 Presence 生产接线断言全走 bindUser/unbindUser/
//   // renew（login/close/renew 调度接线），投递面见 GatewayDeliveryContractTest。
//   class GatewayAwareDeliverySink : public DeliverySink, public GatewayTransportTarget {
//   public:
//       GatewayAwareDeliverySink(PresenceDirectory& presence, GatewayId localGateway,
//                                GatewayTransport& transport, DeliverySink& localSink);
//       ClaimResult bindUser(UserId user, ConnectionId conn);
//       ReleaseResult unbindUser(UserId user, ConnectionId conn, SessionEpoch epoch);
//       RenewResult renew(UserId user);
//       SessionEpoch shadowEpoch(UserId user) const;  // 未绑定 = SessionEpoch(0)
//       bool locallyClaimed(UserId user) const;
//       DeliverDisposition deliver(const DeliveryAttempt& attempt) override;
//       GatewayDeliverResult deliverCrossNode(const DeliveryRoute& route,
//                                             const DeliveryAttempt& attempt) override;
//   };
//
// 生产接线可观察行为（卡 Interface/§冻结参数，本文件逐测试断言）：
//   - login completion：SessionRegistry.bind 成功后 claim（生成新 epoch 原子覆盖，
//     context-map §3）→ locate 命中；重登 claim 覆盖旧 epoch（旧 epoch renew/
//     release 被 fencing）。
//   - close/loginout：对绑定 (user, gateway, conn, epoch) compare-and-delete
//     release（旧 epoch 被拒且条目不变，P4-01 契约）。
//   - renew：每 TTL/2 携带当前 epoch 执行（窗口 = TTL/2，D4）；旧 epoch renew →
//     NotEpoch（重登后旧调度 renew 被 fencing）。
//   - Redis down 降级（P4-00 冻结/ADR-0002）：新登录 claim 失败 → 登录暂停（不建
//     Presence 条目，不误投）；durable accept 继续；跨节点直投不可用（locate 失败
//     按离线处理，保留 Pending）。恢复后自动可用。
//
// 无固定 sleep：renew/到期判定由注入 FakeClock 确定性驱动（Redis value 内嵌
// expiresAtMs，locate 比较注入 Clock，不依赖 key 自动过期，P4-02 契约）。

#include "app/GatewayTransport.hpp"          // RED：尚不存在 → 编译失败即合法 RED
#include "app/GatewayAwareDeliverySink.hpp"  // RED：尚不存在 → 编译失败即合法 RED
#include "app/RecordingGatewayTransport.hpp" // RED：尚不存在 → 编译失败即合法 RED

#include "app/DeliverySink.hpp"
#include "app/DomainTypes.hpp"
#include "app/InMemoryMessageStore.hpp"
#include "app/PresenceDirectory.hpp"
#include "app/RedisConn.hpp"
#include "app/RedisPresenceDirectory.hpp"
#include "app/ReliableMessaging.hpp"

#include "FakeClock.hpp"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

const UserId kAlice{1};
const UserId kBob{2};
const UserId kCarol{3};
const GatewayId kGwLocal{7};
const GatewayId kGwOther{8};
const ConnectionId kConn1{1};
const ConnectionId kConn2{2};
const int64_t kTtlMs = 4000;    // renew 测试用短 TTL
const int64_t kWindowMs = 2000;  // TTL/2 renew 窗口（冻结参数，P4-02 L-3 归位）
const int64_t kT0 = 1000000;
const int64_t kLeaseMs = 100000;

const char* kKeyPrefix = "presence:v1:";

std::string keyFor(UserId user)
{
    return std::string(kKeyPrefix) + std::to_string(user.value);
}

std::string testHost()
{
    const char* h = getenv("REDIS_TEST_HOST");
    return h ? std::string(h) : std::string("127.0.0.1");
}

int testPort()
{
    const char* p = getenv("REDIS_TEST_PORT");
    return p ? std::atoi(p) : 6379;
}

int testDb()
{
    const char* d = getenv("REDIS_TEST_DB");
    return d ? std::atoi(d) : 1;
}

// 取一个当前无监听的端口（先 bind 到 ephemeral 再关闭 → 连接被拒 = Redis down 模拟）。
int freePort()
{
    int s = ::socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    (void)::bind(s, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    socklen_t len = sizeof(addr);
    (void)::getsockname(s, reinterpret_cast<struct sockaddr*>(&addr), &len);
    const int p = ntohs(addr.sin_port);
    ::close(s);
    return p;
}

RetryConfig harnessRetryConfig()
{
    RetryConfig c;
    c.cleanupCycleMs = 0;      // 不触发过期/清理
    c.ackTimeoutMs = 100000;
    c.jitterSeed = 1;
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

// 记录投递 attempt 的本地 sink（投递面断言用）。
class RecordingDeliverySink : public DeliverySink {
public:
    explicit RecordingDeliverySink(DeliverDisposition next = DeliverDisposition::Accepted)
        : next_(next)
    {
    }
    DeliverDisposition deliver(const DeliveryAttempt& attempt) override
    {
        attempts_.push_back(attempt);
        return next_;
    }
    size_t count() const { return attempts_.size(); }

private:
    DeliverDisposition next_;
    std::vector<DeliveryAttempt> attempts_;
};

class GatewayPresenceWiringContractTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(raw_.connect(testHost(), testPort(), testDb(), 2000))
            << "Redis unavailable (this test requires a local Redis server on "
            << testHost() << ":" << testPort() << " db=" << testDb() << ")";
        RedisConn::Reply r = raw_.command({"FLUSHDB"}, 2000);
        ASSERT_TRUE(!r.isError()) << "FLUSHDB failed: " << r.error();
    }

    void TearDown() override
    {
        if (raw_.connected()) {
            (void)raw_.command({"FLUSHDB"}, 2000);
            raw_.close();
        }
    }

    RedisConn raw_;
};

// 卡 §RED 第二段"login claim"：bind 后 claim 生成新 epoch、locate 命中（路由三要素
// 一致）；另一用户 claim 新 epoch 不冲突；未登录用户 locate 不存在、影子为空。
TEST_F(GatewayPresenceWiringContractTest, LoginClaimsPresence)
{
    FakeClock clock;
    clock.set(kT0);
    RedisPresenceDirectory dir(clock, testHost(), testPort(), testDb(), kTtlMs, 1000, 1000);
    RecordingDeliverySink sink;
    RecordingGatewayTransport transport;  // RED
    GatewayAwareDeliverySink wrapper(dir, kGwLocal, transport, sink);

    const ClaimResult c = wrapper.bindUser(kBob, kConn1);
    ASSERT_TRUE(c.ok);
    ASSERT_GT(c.epoch.value, 0u);

    const LocateResult l = dir.locate(kBob);
    ASSERT_TRUE(l.ok);
    EXPECT_EQ(kGwLocal, l.route.gatewayId);
    EXPECT_EQ(kConn1, l.route.connectionId);
    EXPECT_EQ(c.epoch, l.route.sessionEpoch);
    EXPECT_EQ(c.epoch, wrapper.shadowEpoch(kBob));
    EXPECT_TRUE(wrapper.locallyClaimed(kBob));

    // 另一用户登录：独立新 epoch（全局单调，互不冲突）。
    const ClaimResult c2 = wrapper.bindUser(kCarol, kConn2);
    ASSERT_TRUE(c2.ok);
    EXPECT_NE(c.epoch.value, c2.epoch.value);

    // 未登录用户：影子为空、locate 不存在（与"从未 claim"一致）。
    EXPECT_FALSE(wrapper.locallyClaimed(kAlice));
    EXPECT_EQ(SessionEpoch(0), wrapper.shadowEpoch(kAlice));
    const LocateResult absent = dir.locate(kAlice);
    EXPECT_FALSE(absent.ok);
    EXPECT_FALSE(absent.expired);
}

// 卡 §RED 第二段"close/loginout release（compare-and-delete）"：logout 删除条目
//（locate 不存在且 expired=false，与 TTL 到期可区分）、清影子；重复 logout 幂等
//（NotFound）。
TEST_F(GatewayPresenceWiringContractTest, CloseReleasesPresenceCompareAndDelete)
{
    FakeClock clock;
    clock.set(kT0);
    RedisPresenceDirectory dir(clock, testHost(), testPort(), testDb(), kTtlMs, 1000, 1000);
    RecordingDeliverySink sink;
    RecordingGatewayTransport transport;  // RED
    GatewayAwareDeliverySink wrapper(dir, kGwLocal, transport, sink);

    const ClaimResult c = wrapper.bindUser(kBob, kConn1);
    ASSERT_TRUE(c.ok);

    const ReleaseResult rel = wrapper.unbindUser(kBob, kConn1, c.epoch);
    ASSERT_TRUE(rel.ok);

    const LocateResult l = dir.locate(kBob);
    EXPECT_FALSE(l.ok);
    EXPECT_FALSE(l.expired) << "release 删除的条目像从未 claim（非 TTL 过期）";
    EXPECT_FALSE(wrapper.locallyClaimed(kBob));

    // 重复 logout：条目已无 → NotFound（幂等无害）。
    const ReleaseResult again = wrapper.unbindUser(kBob, kConn1, c.epoch);
    EXPECT_FALSE(again.ok);
    EXPECT_EQ(PresenceError::NotFound, again.error);
}

// 卡 §RED 第二段 + P4-01 契约（经 close 接线）：重登 claim 原子覆盖旧 epoch；旧
// 登录的迟到 logout（旧 epoch）被拒（NotEpoch）且新租约原样保留；当前租约 logout
// 成功。
TEST_F(GatewayPresenceWiringContractTest, StaleReleaseDoesNotDeleteNewLease)
{
    FakeClock clock;
    clock.set(kT0);
    RedisPresenceDirectory dir(clock, testHost(), testPort(), testDb(), kTtlMs, 1000, 1000);
    RecordingDeliverySink sink;
    RecordingGatewayTransport transport;  // RED
    GatewayAwareDeliverySink wrapper(dir, kGwLocal, transport, sink);

    const ClaimResult c1 = wrapper.bindUser(kBob, kConn1);
    ASSERT_TRUE(c1.ok);
    const ClaimResult c2 = wrapper.bindUser(kBob, kConn2);  // 重登 = 原子覆盖
    ASSERT_TRUE(c2.ok);
    EXPECT_GT(c2.epoch.value, c1.epoch.value);

    // 旧登录的迟到 logout：epoch 不匹配 → 拒绝，新租约原样保留。
    const ReleaseResult stale = wrapper.unbindUser(kBob, kConn1, c1.epoch);
    EXPECT_FALSE(stale.ok);
    EXPECT_EQ(PresenceError::NotEpoch, stale.error);
    EXPECT_TRUE(wrapper.locallyClaimed(kBob)) << "新租约不被旧 logout 破坏";

    const LocateResult l = dir.locate(kBob);
    ASSERT_TRUE(l.ok);
    EXPECT_EQ(kGwLocal, l.route.gatewayId);
    EXPECT_EQ(kConn2, l.route.connectionId);
    EXPECT_EQ(c2.epoch, l.route.sessionEpoch);

    // 当前租约 logout 成功。
    const ReleaseResult cur = wrapper.unbindUser(kBob, kConn2, c2.epoch);
    EXPECT_TRUE(cur.ok);
}

// 卡 §RED 第二段"renew TTL/2 窗口内成功/旧 epoch 被拒"：renew 在 TTL/2 窗口内成功
// 并向后推移到期点（越过原到期点仍活）；旧 epoch renew → NotEpoch（重登后旧调度
// renew 被 fencing，条目不变）。
TEST_F(GatewayPresenceWiringContractTest, RenewKeepsEntryAlive)
{
    FakeClock clock;
    clock.set(kT0);
    RedisPresenceDirectory dir(clock, testHost(), testPort(), testDb(), kTtlMs, 1000, 1000);
    RecordingDeliverySink sink;
    RecordingGatewayTransport transport;  // RED
    GatewayAwareDeliverySink wrapper(dir, kGwLocal, transport, sink);

    const ClaimResult c = wrapper.bindUser(kBob, kConn1);
    ASSERT_TRUE(c.ok);

    // TTL/2 窗口内 renew：连续 3 个窗口条目始终存活，到期点随每次 renew 后移。
    for (int i = 0; i < 3; ++i) {
        clock.advance(kWindowMs);
        const RenewResult r = wrapper.renew(kBob);
        ASSERT_TRUE(r.ok) << "window " << i;
        EXPECT_EQ(clock.nowMs() + kTtlMs, r.expiresAtMs) << "window " << i;
    }
    // now = T0 + 3*窗口 = T0+6000（已越过原到期 T0+4000）：条目仍活、epoch 不变。
    const LocateResult alive = dir.locate(kBob);
    ASSERT_TRUE(alive.ok);
    EXPECT_EQ(c.epoch, alive.route.sessionEpoch);

    // 不再 renew：越过续期后的到期点 → 条目消失（expired=true）。
    clock.advance(kTtlMs - 1);  // now = T0+9999（续期到期 T0+10000 前）：仍活
    const LocateResult nearExpiry = dir.locate(kBob);
    ASSERT_TRUE(nearExpiry.ok);
    clock.advance(1);  // now = T0+10000 == expiresAtMs（>= 语义）→ 不存在
    const LocateResult gone = dir.locate(kBob);
    EXPECT_FALSE(gone.ok);
    EXPECT_TRUE(gone.expired);
    EXPECT_EQ(PresenceError::NotFound, gone.error);

    // 旧 epoch renew 被 fencing：重登（新 epoch E2）后，携带旧 epoch 的 renew
    //（旧调度携带 E1）→ NotEpoch 且条目不变（adapter 层证据，P4-02 契约原样）。
    const ClaimResult c2 = wrapper.bindUser(kBob, kConn2);
    ASSERT_TRUE(c2.ok);
    const RenewResult stale = dir.renew(kBob, kGwLocal, kConn2, c.epoch);
    EXPECT_FALSE(stale.ok);
    EXPECT_EQ(PresenceError::NotEpoch, stale.error);
    const LocateResult intact = dir.locate(kBob);
    ASSERT_TRUE(intact.ok);
    EXPECT_EQ(c2.epoch, intact.route.sessionEpoch);
}

// 卡 §RED 第二段"Redis down 时 claim 失败→登录暂停而 durable accept 继续、恢复后
// 自动可用"（可观察行为降级段 / P4-00 冻结 / ADR-0002）：Redis 不可达 → login
// claim 失败（DependencyUnavailable）、不建 Presence 条目、不误投（locate 失败按
// 离线处理：Closed、保留 Pending、零写入）；durable accept 继续（accept 不依赖
// Redis）；恢复后登录自动可用、投递成功。
TEST_F(GatewayPresenceWiringContractTest, RedisDownDegradesToLocalOnly)
{
    FakeClock clock;
    clock.set(kT0);
    const int deadPort = freePort();

    // ---- 降级：Redis 不可达（端口关闭）----
    RedisPresenceDirectory dirDown(clock, "127.0.0.1", deadPort, testDb(), kTtlMs, 300, 300);
    RecordingDeliverySink sink;
    RecordingGatewayTransport transport;  // RED
    GatewayAwareDeliverySink wrapperDown(dirDown, kGwLocal, transport, sink);

    // 新登录 claim 失败 → 登录暂停（不建 Presence 条目、影子为空）。
    const ClaimResult c = wrapperDown.bindUser(kBob, kConn1);
    EXPECT_FALSE(c.ok);
    EXPECT_EQ(PresenceError::DependencyUnavailable, c.error);
    EXPECT_FALSE(wrapperDown.locallyClaimed(kBob));

    // 不误投：真实 Redis 中无 bob 条目（登录暂停未建任何路由）。
    RedisConn check;
    ASSERT_TRUE(check.connect(testHost(), testPort(), testDb(), 2000));
    const RedisConn::Reply got = check.command({"GET", keyFor(kBob)}, 2000);
    EXPECT_EQ(RedisConn::Reply::Type::Nil, got.type) << "登录暂停：不建 Presence 条目";
    check.close();

    // 投递面降级：locate 失败（Redis 不可达）→ 按离线处理（Closed、保留 Pending、
    // 零写入）——跨节点直投不可用不误投。
    DeliveryAttempt attempt;
    attempt.messageId = MessageId(9001);
    attempt.conversationId = ConversationId(1);
    attempt.sequence = ConversationSequence(1);
    attempt.senderId = kAlice;
    attempt.recipient = kBob;
    attempt.kind = AttemptKind::Direct;
    attempt.directRecipient = kBob;
    attempt.content = "held-during-outage";
    attempt.attemptNumber = 1;
    EXPECT_EQ(DeliverDisposition::Closed, wrapperDown.deliver(attempt));
    EXPECT_EQ(0u, sink.count());

    // ---- durable accept 继续（accept 不依赖 Redis；投递挂起保留 Pending）----
    InMemoryMessageStore store;
    ReliableMessaging rm(store, sink, clock, kLeaseMs, harnessRetryConfig());
    rm.sessionAvailable(SessionIdentity(kAlice, 1));
    const AcceptOutcome a = rm.accept(SessionIdentity(kAlice, 1), directTo(kBob, "cm1", "m1"));
    ASSERT_TRUE(a.ok) << "Redis down 时 durable accept 继续";
    const std::vector<Delivery> dlv = store.deliveriesByMessage(a.messageId);
    ASSERT_EQ(1u, dlv.size());
    EXPECT_EQ(DeliveryState::Pending, dlv[0].state) << "降级期投递挂起（不误投）";
    EXPECT_EQ(0u, sink.count());

    // ---- 恢复：Redis 可用后登录自动恢复、locate 命中、投递成功 ----
    RedisPresenceDirectory dirUp(clock, testHost(), testPort(), testDb(), kTtlMs, 1000, 1000);
    GatewayAwareDeliverySink wrapperUp(dirUp, kGwLocal, transport, sink);
    const ClaimResult rc = wrapperUp.bindUser(kBob, kConn1);
    ASSERT_TRUE(rc.ok);
    const LocateResult l = dirUp.locate(kBob);
    ASSERT_TRUE(l.ok);
    EXPECT_EQ(rc.epoch, l.route.sessionEpoch);

    const DeliverDisposition d2 = wrapperUp.deliver(attempt);
    EXPECT_EQ(DeliverDisposition::Accepted, d2);
    EXPECT_EQ(1u, sink.count()) << "恢复后投递成功";
}

} // namespace
