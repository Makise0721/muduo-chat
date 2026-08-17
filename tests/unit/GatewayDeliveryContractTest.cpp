// P4-05 RED：Gateway 定向投递与 epoch 校验契约（docs/tasks/P4-05.md §Interface/
// §RED/§冻结参数/§设计决定 D1-D3/D5）。
//
// RED 依据（现状）：本文件引用尚不存在的 app/GatewayTransport.hpp（跨 Gateway
// 投递 port）、app/InProcessGatewayTransport.hpp（in-process 共享路由表 adapter）、
// app/RecordingGatewayTransport.hpp（记录/脚本化断言替身）与 Gateway 感知 sink
// wrapper（app/GatewayAwareDeliverySink.hpp，D3 的 deliver 前 locate + epoch 校验
// seam，兼 D4/D5 的本地 Presence 生命周期接线）→ 编译失败
//（`app/GatewayTransport.hpp: No such file or directory`）即合法 RED（卡 §RED：
// 预期失败 = 类型不存在 → 编译失败，沿 P4-02/P4-03/P4-04 先例）。
//
// 本文件冻结的实现契约（GREEN 据此实现；卡 §Interface 签名定案细化）：
//
//   // ---- chatserver/include/app/GatewayTransport.hpp（新建，卡 §Interface）----
//   struct GatewayDeliverResult {
//       bool ok = false;
//       bool staleEpoch = false;    // 目标侧核验发现 epoch 不匹配：已丢弃并触发目标侧重路由
//       bool unreachable = false;   // 目标 Gateway 不可达（crash/分区/未注册）
//       std::string error;          // 失败摘要（日志/指标，不含敏感 payload）
//   };
//   class GatewayTransport {
//   public:
//       virtual ~GatewayTransport() = default;
//       // route = Presence locate 返回的 DeliveryRoute；attempt = P3-07 冻结投递
//       // 负载。不抛：全部失败收敛为结果字段。
//       virtual GatewayDeliverResult deliver(const DeliveryRoute& route,
//                                            const DeliveryAttempt& attempt) = 0;
//   };
//   // 目标侧入口（InProcessGatewayTransport 注册/回调对象；wrapper 实现此面）。
//   class GatewayTransportTarget {
//   public:
//       virtual ~GatewayTransportTarget() = default;
//       // 目标侧核验 route.sessionEpoch vs 本地会话（影子表）并落目标侧 sink；
//       // 不匹配 → staleEpoch=true（丢弃，目标侧重路由）；匹配 → 本地 sink 投递。
//       virtual GatewayDeliverResult deliverCrossNode(const DeliveryRoute& route,
//                                                     const DeliveryAttempt& attempt) = 0;
//   };
//
//   // ---- chatserver/include/app/InProcessGatewayTransport.hpp（新建）----
//   // 共享路由表把投递交给注册在进程内的目标实例（目标侧校验 epoch 并落到目标侧
//   // sink）；目标未注册 = crash/分区 → unreachable=true。
//   class InProcessGatewayTransport : public GatewayTransport {
//   public:
//       bool registerTarget(GatewayId gateway, GatewayTransportTarget* target);
//       void unregisterTarget(GatewayId gateway);
//       GatewayDeliverResult deliver(const DeliveryRoute& route,
//                                    const DeliveryAttempt& attempt) override;
//   };
//
//   // ---- chatserver/include/app/RecordingGatewayTransport.hpp（新建）----
//   // 记录 deliver 调用与结果；可脚本化注入 staleEpoch/unreachable 失败（测试替身）。
//   class RecordingGatewayTransport : public GatewayTransport {
//   public:
//       struct Record {
//           DeliveryRoute route;
//           DeliveryAttempt attempt;
//           GatewayDeliverResult result;
//       };
//       void scriptStaleEpoch();    // 下一条 deliver 返回 staleEpoch=true（丢弃）
//       void scriptUnreachable();   // 下一条 deliver 返回 unreachable=true
//       void clearScript();         // 恢复 ok=true
//       GatewayDeliverResult deliver(const DeliveryRoute& route,
//                                    const DeliveryAttempt& attempt) override;
//       const std::vector<Record>& records() const;
//   };
//
//   // ---- chatserver/include/app/GatewayAwareDeliverySink.hpp（新建，D3/D4/D5 seam）----
//   // 单 Gateway 投递 + 本地 Presence 生命周期接线对象：持有本地影子表
//   // (userId → (connId, SessionEpoch))（D5：claim 写入、release 清除；内部 seam，
//   // 不进 Presence port）；deliver 前经 Presence locate + 影子 epoch 校验：
//   // 无条目=离线（Closed、保留 Pending）；本地匹配=影子校验后本地 sink 直投；
//   // 跨节点=transport 路由；epoch 不匹配=丢弃+重路由（Closed、保留 Pending）。
//   class GatewayAwareDeliverySink : public DeliverySink, public GatewayTransportTarget {
//   public:
//       GatewayAwareDeliverySink(PresenceDirectory& presence, GatewayId localGateway,
//                                GatewayTransport& transport, DeliverySink& localSink);
//       // login wiring（bind 成功后、sessionAvailableDelivery 提交前）：claim 生成
//       // 新 epoch 原子覆盖 + 写影子表；Redis down → 失败（登录暂停：不建条目、
//       // 影子不变）。调用方只在 ok 时提交 sessionAvailableDelivery。
//       ClaimResult bindUser(UserId user, ConnectionId conn);
//       // close/loginout wiring：对绑定 (user, gateway, conn, epoch) compare-and-delete
//       // release；旧 epoch 被拒（NotEpoch）且条目不变；成功后清影子。
//       ReleaseResult unbindUser(UserId user, ConnectionId conn, SessionEpoch epoch);
//       // renew 调度（调用方每 TTL/2 驱动，D4）：携带当前影子 epoch；旧 epoch 由
//       // adapter fencing（NotEpoch）——重登后旧调度 renew 被拒。
//       RenewResult renew(UserId user);
//       // 影子表投影（可观测内部 seam）：未绑定 = SessionEpoch(0)。
//       SessionEpoch shadowEpoch(UserId user) const;
//       bool locallyClaimed(UserId user) const;
//       DeliverDisposition deliver(const DeliveryAttempt& attempt) override;
//       GatewayDeliverResult deliverCrossNode(const DeliveryRoute& route,
//                                             const DeliveryAttempt& attempt) override;
//   };
//
// 投递语义（卡 Interface 可观察行为 ①②③ 与完成定义，本文件逐测试断言）：
//   - deliver 前经 Presence 校验；本地匹配=直投（本地短路，绝不经过 transport）；
//   - 旧 epoch 包不投递并触发重路由：返回 Closed 保留 Pending，由下次
//     sessionAvailable/claim 重新投递，不产生第二套状态机（同一 RM 状态机；
//     store 中 (messageId, recipient) 恒一行、Message 唯一）；
//   - 跨节点投递经 transport 路由，目标侧核验 epoch；ACK 与 MessageId 语义与单机
//     完全一致（同一 ledger 与 Delivery 状态机，attempt 字段原样透传）。
//
// Pending/InFlight 观测用真实 InMemoryMessageStore + ReliableMessaging 驱动
//（P4-04 harness 同构：FakeClock 冻结 → lease 永不过期）；跨节点投递用 in-process
// 双实例（两个 GatewayAwareDeliverySink 共享同一 InMemory presence，卡 §RED 场景 3）。
// 无固定 sleep：全部为确定性状态/记录断言。

#include "app/GatewayTransport.hpp"           // RED：尚不存在 → 编译失败即合法 RED
#include "app/InProcessGatewayTransport.hpp"  // RED：尚不存在 → 编译失败即合法 RED
#include "app/RecordingGatewayTransport.hpp"  // RED：尚不存在 → 编译失败即合法 RED
#include "app/GatewayAwareDeliverySink.hpp"   // RED：尚不存在 → 编译失败即合法 RED

#include "app/DeliverySink.hpp"
#include "app/DomainTypes.hpp"
#include "app/InMemoryMessageStore.hpp"
#include "app/InMemoryPresenceDirectory.hpp"
#include "app/PresenceDirectory.hpp"
#include "app/RedisConn.hpp"
#include "app/RedisPresenceDirectory.hpp"
#include "app/ReliableMessaging.hpp"

#include "FakeClock.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

const UserId kAlice{1};
const UserId kBob{2};
const UserId kCarol{3};
const GatewayId kGwA{10};
const GatewayId kGwB{20};
const ConnectionId kConn1{1};
const ConnectionId kConn2{2};
const int64_t kTtlMs = 100000;  // FakeClock 冻结：TTL 测试期间永不过期
const int64_t kLeaseMs = 100000;  // RM lease：测试期间永不过期（重放 fencing 确定）
const int64_t kT0 = 1000000;

RetryConfig harnessRetryConfig()
{
    RetryConfig c;
    c.cleanupCycleMs = 0;      // 不触发过期/清理，隔离投递关注点
    c.ackTimeoutMs = 100000;   // ACK 超时远大于测试时长
    c.jitterSeed = 1;          // 确定性
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

// 手工构造一条投递负载（P3-07 冻结字段；跨节点/注入场景不经 RM accept 路径）。
DeliveryAttempt makeAttempt(uint64_t messageId, uint64_t conversationId, uint64_t sequence,
                            UserId sender, UserId recipient, const std::string& content)
{
    DeliveryAttempt a;
    a.messageId = MessageId(messageId);
    a.conversationId = ConversationId(conversationId);
    a.sequence = ConversationSequence(sequence);
    a.senderId = sender;
    a.recipient = recipient;
    a.kind = AttemptKind::Direct;
    a.directRecipient = recipient;
    a.content = content;
    a.attemptNumber = 1;
    return a;
}

// ---- H1 修复（2026-08-17 编排者裁决，P4-05.md §H1）真实 Redis 夹具 ----
// Redis down 注入方式（卡内记录理由）：真实 RedisPresenceDirectory（真实 Redis）
// 外裹测试本地 TogglePresence 装饰器——up 转发真实 Redis（claim/epoch 真实），
// down 对所有操作返回 DependencyUnavailable（与真实 Redis 故障时 adapter 的可观察
// 症状一致）。选择该 seam 的理由：RedisPresenceDirectory 无公开故障注入 seam；
// 停共享测试 Redis 实例不可行（同 CTest 轮内其它用例共用）；TCP 黑洞转发器引入
// 时序不确定性而无额外契约覆盖。up→down→up 单实例转换使 H1 两用例可确定性断言。

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

// 依赖不可用开关装饰器：down 期间四操作全部 DependencyUnavailable（保留计数供
// "本地短路零 Redis RTT"断言）；up 期间原样转发内层 presence（真实 Redis）。
class TogglePresence : public PresenceDirectory {
public:
    explicit TogglePresence(PresenceDirectory& inner) : inner_(inner) {}

    void setDown(bool down)
    {
        std::lock_guard<std::mutex> lk(mutex_);
        down_ = down;
    }

    // down 期间对内层（真实 Redis）的调用次数：本地短路断言 = 0。
    size_t callsWhileDown() const
    {
        std::lock_guard<std::mutex> lk(mutex_);
        return downCalls_;
    }

    ClaimResult claim(UserId user, GatewayId gateway, ConnectionId conn) override
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (down_) {
            ++downCalls_;
            ClaimResult r;
            r.error = PresenceError::DependencyUnavailable;
            return r;
        }
        return inner_.claim(user, gateway, conn);
    }
    RenewResult renew(UserId user, GatewayId gateway, ConnectionId conn,
                      SessionEpoch epoch) override
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (down_) {
            ++downCalls_;
            RenewResult r;
            r.error = PresenceError::DependencyUnavailable;
            return r;
        }
        return inner_.renew(user, gateway, conn, epoch);
    }
    ReleaseResult release(UserId user, GatewayId gateway, ConnectionId conn,
                          SessionEpoch epoch) override
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (down_) {
            ++downCalls_;
            ReleaseResult r;
            r.error = PresenceError::DependencyUnavailable;
            return r;
        }
        return inner_.release(user, gateway, conn, epoch);
    }
    LocateResult locate(UserId user) override
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (down_) {
            ++downCalls_;
            LocateResult r;
            r.error = PresenceError::DependencyUnavailable;
            return r;
        }
        return inner_.locate(user);
    }

private:
    PresenceDirectory& inner_;
    bool down_ = false;
    size_t downCalls_ = 0;
    mutable std::mutex mutex_;
};

// 真实 Redis 隔离夹具（与 GatewayPresenceWiringContractTest 同构：db=1 FLUSHDB，
// 不 skip；Redis 不可用即测试失败）。
class RedisDownContractTest : public ::testing::Test {
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

// 记录全部投递 attempt 的本地 sink（返回可配置 disposition，默认 Accepted）。
class RecordingDeliverySink : public DeliverySink {
public:
    explicit RecordingDeliverySink(DeliverDisposition next = DeliverDisposition::Accepted)
        : next_(next)
    {
    }
    void setNext(DeliverDisposition d) { next_ = d; }
    DeliverDisposition deliver(const DeliveryAttempt& attempt) override
    {
        attempts_.push_back(attempt);
        return next_;
    }
    size_t count() const { return attempts_.size(); }
    const std::vector<DeliveryAttempt>& attempts() const { return attempts_; }

private:
    DeliverDisposition next_;
    std::vector<DeliveryAttempt> attempts_;
};

// 记录 transport 递交给目标侧的 (route, attempt)，再转发给真实目标（断言 transport
// 使用了哪条路由；用已冻结的 GatewayTransportTarget 面，不改 InProcess adapter）。
class RouteRecordingTarget : public GatewayTransportTarget {
public:
    struct Rec {
        DeliveryRoute route;
        DeliveryAttempt attempt;
    };
    explicit RouteRecordingTarget(GatewayTransportTarget* inner) : inner_(inner) {}
    GatewayDeliverResult deliverCrossNode(const DeliveryRoute& route,
                                          const DeliveryAttempt& attempt) override
    {
        records_.push_back({route, attempt});
        return inner_->deliverCrossNode(route, attempt);
    }
    const std::vector<Rec>& records() const { return records_; }

private:
    GatewayTransportTarget* inner_;
    std::vector<Rec> records_;
};

// 单 Gateway 真实状态机 harness：RM 的 sink = Gateway 感知 wrapper（本地 gwA）。
struct SingleGatewayHarness {
    FakeClock clock;
    InMemoryPresenceDirectory presence;
    RecordingDeliverySink sink;
    RecordingGatewayTransport transport;  // RED
    GatewayAwareDeliverySink wrapper;     // RED
    InMemoryMessageStore store;
    ReliableMessaging rm;

    SingleGatewayHarness()
        : presence(clock, kTtlMs),
          wrapper(presence, kGwA, transport, sink),
          rm(store, wrapper, clock, kLeaseMs, harnessRetryConfig())
    {
        clock.set(kT0);
    }
};

// 双 Gateway in-process harness：两 wrapper 共享同一 InMemory presence；transport
// 目标经 RouteRecordingTarget 记录实际路由（卡 §RED 场景 3"共享 InMemory Presence"）。
struct TwoGatewayHarness {
    FakeClock clock;
    InMemoryPresenceDirectory presence;
    RecordingDeliverySink sinkA;
    RecordingDeliverySink sinkB;
    InProcessGatewayTransport transport;  // RED
    GatewayAwareDeliverySink wrapperA;    // RED
    GatewayAwareDeliverySink wrapperB;    // RED
    RouteRecordingTarget routeA;
    RouteRecordingTarget routeB;

    TwoGatewayHarness()
        : presence(clock, kTtlMs),
          wrapperA(presence, kGwA, transport, sinkA),
          wrapperB(presence, kGwB, transport, sinkB),
          routeA(&wrapperA),
          routeB(&wrapperB)
    {
        clock.set(kT0);
        transport.registerTarget(kGwA, &routeA);
        transport.registerTarget(kGwB, &routeB);
    }
};

// 卡 §RED 场景 1：无 Presence 条目时投递行为（离线不变）——sink Closed、零 socket
// 写入、Delivery 保留 Pending；登录 claim 后 sessionAvailable 恢复投递（与单机离线
// 语义逐位一致）。
TEST(GatewayDeliveryContractTest, DeliverWithoutPresenceEntryKeepsPending)
{
    SingleGatewayHarness h;
    h.rm.sessionAvailable(SessionIdentity(kAlice, 1));

    AcceptOutcome a = h.rm.accept(SessionIdentity(kAlice, 1),
                                  directTo(kBob, "cm1", "hello-bob"));
    ASSERT_TRUE(a.ok);
    const std::vector<Delivery> pending = h.store.deliveriesByMessage(a.messageId);
    ASSERT_EQ(1u, pending.size());
    EXPECT_EQ(DeliveryState::Pending, pending[0].state) << "无 Presence 条目 = 离线：保留 Pending";
    EXPECT_EQ(0u, h.sink.count()) << "离线：零 socket 写入";
    EXPECT_EQ(0u, h.transport.records().size());

    // 登录 claim（bind 成功后 claim 生成新 epoch）→ sessionAvailable 恢复投递。
    ClaimResult c = h.wrapper.bindUser(kBob, kConn1);
    ASSERT_TRUE(c.ok);
    EXPECT_EQ(c.epoch, h.wrapper.shadowEpoch(kBob));
    h.rm.sessionAvailable(SessionIdentity(kBob, 1));

    const std::vector<Delivery> done = h.store.deliveriesByMessage(a.messageId);
    ASSERT_EQ(1u, done.size());
    EXPECT_EQ(DeliveryState::InFlight, done[0].state) << "登录后恰好投递一次";
    ASSERT_EQ(1u, h.sink.count());
    EXPECT_EQ(a.messageId, h.sink.attempts()[0].messageId);
    EXPECT_EQ(kBob, h.sink.attempts()[0].recipient);
    EXPECT_EQ(0u, h.transport.records().size()) << "本地匹配 = 直投，绝不经过 transport";
}

// 卡 §RED 场景 2：旧 epoch 收包丢弃+重路由——路由携带旧 epoch（vs 本地影子表）→
// 包被丢弃（零写入）、状态不变；重连（新 claim 新 epoch）后 sessionAvailable 重
// claim 恰好投递一次（重路由=本地重 claim，无第二套状态机）。
TEST(GatewayDeliveryContractTest, StaleEpochDeliveryDroppedAndRerouted)
{
    SingleGatewayHarness h;
    h.rm.sessionAvailable(SessionIdentity(kAlice, 1));

    // 离线 accept → Pending（起点）。
    AcceptOutcome a = h.rm.accept(SessionIdentity(kAlice, 1), directTo(kBob, "cm1", "m1"));
    ASSERT_TRUE(a.ok);
    ASSERT_EQ(DeliveryState::Pending, h.store.deliveriesByMessage(a.messageId)[0].state);

    // 登录 E1 → 投递一次（InFlight）。
    ClaimResult c1 = h.wrapper.bindUser(kBob, kConn1);
    ASSERT_TRUE(c1.ok);
    h.rm.sessionAvailable(SessionIdentity(kBob, 1));
    EXPECT_EQ(1u, h.sink.count());

    // 重连：新 claim 新 epoch E2 → 重路由（旧 InFlight fencing → Pending → 重新投递
    // 一次；同一 Delivery 行，无第二套状态机）。
    ClaimResult c2 = h.wrapper.bindUser(kBob, kConn2);
    ASSERT_TRUE(c2.ok);
    EXPECT_GT(c2.epoch.value, c1.epoch.value);
    h.rm.sessionAvailable(SessionIdentity(kBob, 2));  // 重连 = 新 generation
    EXPECT_EQ(2u, h.sink.count());

    // 旧 epoch 收包：路由携带 E1、本地影子表 = E2 → 丢弃（零写入）、staleEpoch=true、
    // Delivery 状态与 attemptCount 不变（不重复投递）。
    DeliveryRoute stale;
    stale.user = kBob;
    stale.gatewayId = kGwA;
    stale.connectionId = kConn1;
    stale.sessionEpoch = c1.epoch;
    const DeliveryAttempt attempt =
        makeAttempt(a.messageId.value, a.conversationId.value, a.sequence.value, kAlice, kBob,
                    "m1");
    const GatewayDeliverResult r = h.wrapper.deliverCrossNode(stale, attempt);
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.staleEpoch) << "旧 epoch 包：目标侧核验发现 epoch 不匹配 → 丢弃";
    EXPECT_EQ(2u, h.sink.count()) << "丢弃 = 零写入";
    const std::vector<Delivery> dlv = h.store.deliveriesByMessage(a.messageId);
    ASSERT_EQ(1u, dlv.size());
    EXPECT_EQ(DeliveryState::InFlight, dlv[0].state);
    EXPECT_EQ(2u, dlv[0].attemptCount) << "旧 epoch 包不记 attempt（不重复投递）";
}

// 卡 §RED 场景 3：Alice(gwA) accept 的消息目标 Bob(gwB)——gwA 的 sink locate 到
// gwB → transport 路由 → gwB 侧核验 epoch → 落到 Bob 本地 sink；恰好一次投递、
// ACK/MessageId 语义与单机一致（attempt 字段原样透传）。
TEST(GatewayDeliveryContractTest, CrossGatewayDeliveryViaRoute)
{
    TwoGatewayHarness h;
    ClaimResult c = h.wrapperB.bindUser(kBob, kConn1);
    ASSERT_TRUE(c.ok);

    const DeliveryAttempt attempt = makeAttempt(1001, 7, 3, kAlice, kBob, "cross-node");
    const DeliverDisposition d = h.wrapperA.deliver(attempt);
    ASSERT_EQ(DeliverDisposition::Accepted, d);

    EXPECT_EQ(0u, h.sinkA.count()) << "目标在 gwB：gwA 本地 sink 不写入";
    EXPECT_EQ(1u, h.sinkB.count()) << "目标在 gwB：恰好一次投递";

    // transport 记录：route 指向 gwB、epoch 与 claim 一致；attempt 原样透传
    //（ACK/MessageId 语义一致）。
    const std::vector<RouteRecordingTarget::Rec>& recs = h.routeB.records();
    ASSERT_EQ(1u, recs.size());
    EXPECT_EQ(kGwB, recs[0].route.gatewayId);
    EXPECT_EQ(c.epoch, recs[0].route.sessionEpoch);
    EXPECT_EQ(kConn1, recs[0].route.connectionId);
    EXPECT_EQ(attempt.messageId, recs[0].attempt.messageId);
    EXPECT_EQ(attempt.sequence, recs[0].attempt.sequence);
    EXPECT_EQ(attempt.content, recs[0].attempt.content);

    const DeliveryAttempt& got = h.sinkB.attempts()[0];
    EXPECT_EQ(attempt.messageId, got.messageId);
    EXPECT_EQ(attempt.conversationId, got.conversationId);
    EXPECT_EQ(attempt.sequence, got.sequence);
    EXPECT_EQ(attempt.recipient, got.recipient);
    EXPECT_EQ(attempt.senderId, got.senderId);
    EXPECT_EQ(attempt.content, got.content);
}

// 卡 §RED 场景 4：投递中重连（locate 变化）——attempt 在途时 Bob 重连（新 claim
// 新 epoch）→ 重投使用最新路由（目标侧 epoch 匹配），新会话恰好投递一次（不重复
// 投递）；重连后绝无旧路由投递。
TEST(GatewayDeliveryContractTest, ReconnectDuringDeliveryUsesLatestRoute)
{
    TwoGatewayHarness h;
    ClaimResult c1 = h.wrapperB.bindUser(kBob, kConn1);
    ASSERT_TRUE(c1.ok);

    const DeliveryAttempt attempt = makeAttempt(2001, 8, 1, kAlice, kBob, "reconnect");
    // 第一次投递（E1 路由）。
    ASSERT_EQ(DeliverDisposition::Accepted, h.wrapperA.deliver(attempt));
    ASSERT_EQ(1u, h.sinkB.count());
    ASSERT_EQ(1u, h.routeB.records().size());
    EXPECT_EQ(c1.epoch, h.routeB.records()[0].route.sessionEpoch);

    // attempt 在途期间 Bob 重连：新 claim 新 epoch E2。
    ClaimResult c2 = h.wrapperB.bindUser(kBob, kConn2);
    ASSERT_TRUE(c2.ok);
    EXPECT_GT(c2.epoch.value, c1.epoch.value);

    // 重投（ACK 超时/调度重试）：locate 现指向最新路由 E2 → 目标侧核验 epoch 匹配
    // → 新会话投递一次。
    ASSERT_EQ(DeliverDisposition::Accepted, h.wrapperA.deliver(attempt));
    EXPECT_EQ(2u, h.sinkB.count()) << "重连后恰好再投一次（不重复投递）";
    ASSERT_EQ(2u, h.routeB.records().size());
    EXPECT_EQ(c2.epoch, h.routeB.records()[1].route.sessionEpoch) << "重连后使用最新路由";
    EXPECT_NE(c1.epoch, h.routeB.records()[1].route.sessionEpoch) << "重连后绝无旧路由";
}

// 卡 §RED 场景 5：目标 Gateway crash（transport 不可达→重路由）——目标注销 →
// unreachable=true → wrapper 返回 Closed（attempt 保留 Pending，零写入）；transport
// 恢复后投递成功。
TEST(GatewayDeliveryContractTest, TargetGatewayCrashReroutes)
{
    TwoGatewayHarness h;
    ClaimResult c = h.wrapperB.bindUser(kBob, kConn1);
    ASSERT_TRUE(c.ok);

    const DeliveryAttempt attempt = makeAttempt(3001, 9, 1, kAlice, kBob, "crash");

    // 目标 Gateway crash：从 transport 注销 gwB。
    h.transport.unregisterTarget(kGwB);

    // transport 直接语义：未注册目标 → unreachable=true。
    DeliveryRoute route;
    route.user = kBob;
    route.gatewayId = kGwB;
    route.connectionId = kConn1;
    route.sessionEpoch = c.epoch;
    const GatewayDeliverResult r = h.transport.deliver(route, attempt);
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.unreachable);

    // wrapper 把 unreachable 映射为 Closed：attempt 不记（保留 Pending）、零写入。
    EXPECT_EQ(DeliverDisposition::Closed, h.wrapperA.deliver(attempt));
    EXPECT_EQ(0u, h.sinkB.count()) << "崩溃目标：零写入";

    // transport 恢复（gwB 重新注册）后投递成功。
    h.transport.registerTarget(kGwB, &h.routeB);
    EXPECT_EQ(DeliverDisposition::Accepted, h.wrapperA.deliver(attempt));
    EXPECT_EQ(1u, h.sinkB.count()) << "transport 恢复后恰好一次";
}

// 卡 §RED 场景 6：epoch 校验失败不破坏单机回归——单 Gateway、epoch 全匹配时投递与
// P3 完全一致（回归锚，配合全量 P3 process 测试）：HOL、ACK 放行、状态转移、本地
// 短路零 transport。
TEST(GatewayDeliveryContractTest, EpochMismatchDoesNotBreakSingleNodeRegression)
{
    SingleGatewayHarness h;
    h.rm.sessionAvailable(SessionIdentity(kAlice, 1));
    ClaimResult c = h.wrapper.bindUser(kBob, kConn1);
    ASSERT_TRUE(c.ok);
    h.rm.sessionAvailable(SessionIdentity(kBob, 1));

    // 两条消息：HOL 只投 m1（InFlight），m2 保持 Pending（P3 语义原样）。
    const AcceptOutcome a1 = h.rm.accept(SessionIdentity(kAlice, 1), directTo(kBob, "cm1", "m1"));
    const AcceptOutcome a2 = h.rm.accept(SessionIdentity(kAlice, 1), directTo(kBob, "cm2", "m2"));
    ASSERT_TRUE(a1.ok && a2.ok);
    EXPECT_EQ(DeliveryState::InFlight, h.store.deliveriesByMessage(a1.messageId)[0].state);
    EXPECT_EQ(DeliveryState::Pending, h.store.deliveriesByMessage(a2.messageId)[0].state);
    EXPECT_EQ(1u, h.sink.count());

    // ACK m1 → 放行 m2（同一 coordinator 状态机，ACK/MessageId 语义不变）。
    const AckOutcome ack = h.rm.acknowledge(SessionIdentity(kBob, 1), a1.messageId);
    ASSERT_EQ(AckResult::Acknowledged, ack.result);
    EXPECT_EQ(DeliveryState::InFlight, h.store.deliveriesByMessage(a2.messageId)[0].state);
    EXPECT_EQ(2u, h.sink.count());

    // 回归锚：epoch 全匹配的本地投递绝不经过 transport、绝不触发丢弃。
    EXPECT_EQ(0u, h.transport.records().size());
    EXPECT_EQ(c.epoch, h.wrapper.shadowEpoch(kBob));
    const LocateResult l = h.presence.locate(kBob);
    ASSERT_TRUE(l.ok);
    EXPECT_EQ(c.epoch, l.route.sessionEpoch);
}

// 卡 §RED 场景 2 不变式 / 完成定义"不产生第二套状态机"：重连 + 旧 epoch 包 + 恢复
// 投递全流程后，(messageId, recipient) 恒一行、Message 唯一、状态只在 Pending/
// InFlight 之间推进——路由绝不创建第二套 Message/Delivery 状态机。
TEST(GatewayDeliveryContractTest, RoutingNeverCreatesSecondStateMachine)
{
    SingleGatewayHarness h;
    h.rm.sessionAvailable(SessionIdentity(kAlice, 1));
    ClaimResult c1 = h.wrapper.bindUser(kBob, kConn1);
    ASSERT_TRUE(c1.ok);
    h.rm.sessionAvailable(SessionIdentity(kBob, 1));

    const AcceptOutcome a = h.rm.accept(SessionIdentity(kAlice, 1), directTo(kBob, "cm1", "m1"));
    ASSERT_TRUE(a.ok);

    // 路由事件风暴：重连（E1→E2）+ 旧 epoch 包注入。
    const ClaimResult c2 = h.wrapper.bindUser(kBob, kConn2);
    ASSERT_TRUE(c2.ok);
    h.rm.sessionAvailable(SessionIdentity(kBob, 2));
    DeliveryRoute stale;
    stale.user = kBob;
    stale.gatewayId = kGwA;
    stale.connectionId = kConn1;
    stale.sessionEpoch = c1.epoch;
    const GatewayDeliverResult r = h.wrapper.deliverCrossNode(
        stale, makeAttempt(a.messageId.value, a.conversationId.value, a.sequence.value, kAlice,
                           kBob, "m1"));
    EXPECT_TRUE(r.staleEpoch);

    // 同一 (messageId, recipient) 恒一行；Message 唯一；状态合法。
    const std::vector<Delivery> dlv = h.store.deliveriesByMessage(a.messageId);
    ASSERT_EQ(1u, dlv.size());
    EXPECT_TRUE(dlv[0].state == DeliveryState::Pending || dlv[0].state == DeliveryState::InFlight);
    EXPECT_EQ(2u, dlv[0].attemptCount);
    ASSERT_TRUE(h.store.findMessage(a.messageId));
    EXPECT_EQ(a.messageId.value, h.store.findMessage(a.messageId)->id.value) << "Message 唯一";
    EXPECT_EQ(2u, h.sink.count()) << "投递仍由同一 RM 状态机推进（两次真实投递）";
}

// 允许写入清单 Recording adapter 契约：记录 deliver 调用与结果；脚本化注入
// staleEpoch/unreachable 使 wrapper 返回 Closed（丢弃+重路由/保留 Pending）；清脚本
// 后 ok → wrapper 返回 Accepted（recording transport 不落真实目标 sink）。
TEST(GatewayDeliveryContractTest, RecordingTransportRecordsForAssertion)
{
    FakeClock clock;
    clock.set(kT0);
    InMemoryPresenceDirectory presence(clock, kTtlMs);
    RecordingDeliverySink sink;
    RecordingGatewayTransport recorder;  // RED
    GatewayAwareDeliverySink wrapper(presence, kGwA, recorder, sink);

    // bob 在远端 gwB（共享 presence 直接 claim，模拟远端 Gateway 登录）。
    const ClaimResult c = presence.claim(kBob, kGwB, kConn1);
    ASSERT_TRUE(c.ok);
    const DeliveryAttempt attempt = makeAttempt(4001, 10, 1, kAlice, kBob, "record");

    // 脚本化 staleEpoch：跨节点投递被目标侧丢弃 → wrapper 返回 Closed（重路由）。
    recorder.scriptStaleEpoch();
    EXPECT_EQ(DeliverDisposition::Closed, wrapper.deliver(attempt));
    ASSERT_EQ(1u, recorder.records().size());
    EXPECT_EQ(kGwB, recorder.records()[0].route.gatewayId);
    EXPECT_EQ(c.epoch, recorder.records()[0].route.sessionEpoch);
    EXPECT_TRUE(recorder.records()[0].result.staleEpoch);
    EXPECT_FALSE(recorder.records()[0].result.ok);
    EXPECT_EQ(0u, sink.count());

    // 脚本化 unreachable：目标不可达 → wrapper 返回 Closed（保留 Pending）。
    recorder.scriptUnreachable();
    EXPECT_EQ(DeliverDisposition::Closed, wrapper.deliver(attempt));
    ASSERT_EQ(2u, recorder.records().size());
    EXPECT_TRUE(recorder.records()[1].result.unreachable);
    EXPECT_EQ(0u, sink.count());

    // 清脚本：transport ok → wrapper 返回 Accepted（断言替身不落真实目标 sink）。
    recorder.clearScript();
    EXPECT_EQ(DeliverDisposition::Accepted, wrapper.deliver(attempt));
    ASSERT_EQ(3u, recorder.records().size());
    EXPECT_TRUE(recorder.records()[2].result.ok);
    EXPECT_EQ(0u, sink.count()) << "recording transport 是断言替身，不接触真实目标";
}

// H1（Spec，2026-08-17 编排者裁决）：Redis down 时已 claim 的本地用户间投递继续
//（本地短路，绝不进 Redis locate），非 claim 用户行为不变（locate 失败按离线 →
// Closed、零写入）。对应 cluster-failure-contract §2 Redis down 行"本地持有连接上
// 的会话与本地投递不受影响"。当前实现（deliver 前一律 presence locate）下 Redis
// down → locate DependencyUnavailable → Closed，已 claim 用户本地投递被打断 →
// 本用例 RED。
TEST_F(RedisDownContractTest, RedisDownKeepsLocalDeliveryWorking)
{
    FakeClock clock;
    clock.set(kT0);
    const int64_t ttl = 100000;  // FakeClock 冻结：测试期间条目不过期
    RedisPresenceDirectory realDir(clock, testHost(), testPort(), testDb(), ttl, 1000, 1000);
    TogglePresence presence(realDir);
    RecordingDeliverySink sink;
    RecordingGatewayTransport transport;
    GatewayAwareDeliverySink wrapper(presence, kGwA, transport, sink);

    // 阶段 1：Redis up → claim（真实 Redis 条目 + 本地影子）。
    const ClaimResult c = wrapper.bindUser(kBob, kConn1);
    ASSERT_TRUE(c.ok);
    const LocateResult live = realDir.locate(kBob);
    ASSERT_TRUE(live.ok);
    EXPECT_EQ(c.epoch, live.route.sessionEpoch);

    // 阶段 2：Redis down → 已 claim 本地用户投递继续（影子短路：Accepted、零
    // Redis RTT、零 transport）。
    presence.setDown(true);
    const DeliveryAttempt attempt =
        makeAttempt(5001, 11, 1, kAlice, kBob, "local-survives-down");
    EXPECT_EQ(DeliverDisposition::Accepted, wrapper.deliver(attempt));
    ASSERT_EQ(1u, sink.count());
    EXPECT_EQ(attempt.messageId, sink.attempts()[0].messageId);
    EXPECT_EQ(0u, presence.callsWhileDown()) << "本地短路：Redis down 期间零 Redis 调用";
    EXPECT_EQ(0u, transport.records().size()) << "本地短路：绝不经过 transport";

    // 非 claim 用户行为不变：locate 失败（Redis down）按离线 → Closed、零写入。
    const DeliveryAttempt unclaimed =
        makeAttempt(5002, 11, 2, kAlice, kCarol, "unclaimed-user");
    EXPECT_EQ(DeliverDisposition::Closed, wrapper.deliver(unclaimed));
    EXPECT_EQ(1u, sink.count());
    EXPECT_EQ(0u, transport.records().size());
}

// H1（Spec，2026-08-17 编排者裁决）：Redis 恢复后 renew 触发自动重 claim（无需
// 重登）——Redis down 期间 renew 失败但影子保留；恢复后条目已逻辑过期（clock 越过
// TTL）→ renew 遇 NotFound → 自动原子重 claim（新 epoch 覆盖）；自愈后本地投递
// 继续工作。当前实现（renew 遇 NotFound 只返回失败）下 r2.ok == false → 本用例 RED。
TEST_F(RedisDownContractTest, RedisRecoveryReclaimsPresenceAutomatically)
{
    FakeClock clock;
    clock.set(kT0);
    const int64_t ttl = 4000;
    RedisPresenceDirectory realDir(clock, testHost(), testPort(), testDb(), ttl, 1000, 1000);
    TogglePresence presence(realDir);
    RecordingDeliverySink sink;
    RecordingGatewayTransport transport;
    GatewayAwareDeliverySink wrapper(presence, kGwA, transport, sink);

    // 阶段 1：Redis up → claim E1（影子 + 真实 Redis 条目）。
    const ClaimResult c1 = wrapper.bindUser(kBob, kConn1);
    ASSERT_TRUE(c1.ok);
    const SessionEpoch e1 = c1.epoch;

    // 阶段 2：Redis down → renew 失败（DependencyUnavailable），影子保留（登录
    // 会话不被剥夺；恢复后继续）。
    presence.setDown(true);
    const RenewResult r1 = wrapper.renew(kBob);
    EXPECT_FALSE(r1.ok);
    EXPECT_EQ(PresenceError::DependencyUnavailable, r1.error);
    EXPECT_TRUE(wrapper.locallyClaimed(kBob)) << "Redis down 期间影子保留";
    EXPECT_EQ(e1, wrapper.shadowEpoch(kBob));

    // 阶段 3：Redis 恢复 + 原条目已逻辑过期（now 越过原 expiresAt）→ renew 遇
    // NotFound → 自动重 claim（新 epoch 原子覆盖，无需重登）。
    clock.advance(ttl);  // 原条目到期（renew 脚本 now >= expiresAtMs → NotFound）
    presence.setDown(false);
    const RenewResult r2 = wrapper.renew(kBob);
    ASSERT_TRUE(r2.ok) << "renew 自愈：Redis 恢复后自动重 claim（无需重登）";
    const SessionEpoch e2 = wrapper.shadowEpoch(kBob);
    EXPECT_GT(e2.value, e1.value) << "重 claim = 新 epoch";
    EXPECT_TRUE(wrapper.locallyClaimed(kBob));

    // 真实 Redis 条目已按新 epoch 重建（路由三要素一致）。
    const LocateResult l = realDir.locate(kBob);
    ASSERT_TRUE(l.ok);
    EXPECT_EQ(kGwA, l.route.gatewayId);
    EXPECT_EQ(kConn1, l.route.connectionId);
    EXPECT_EQ(e2, l.route.sessionEpoch);

    // 自愈后本地投递继续（影子短路，无需重登即恢复服务）。
    const DeliveryAttempt attempt =
        makeAttempt(6001, 12, 1, kAlice, kBob, "recovered-delivery");
    EXPECT_EQ(DeliverDisposition::Accepted, wrapper.deliver(attempt));
    EXPECT_EQ(1u, sink.count());
}

} // namespace
