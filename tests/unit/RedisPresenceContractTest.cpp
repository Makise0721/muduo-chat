// P4-02 RED：RedisPresenceDirectory 真实 Redis contract + 故障测试
// （docs/tasks/P4-02.md、docs/adr/0002-cluster-ownership-and-failure-contract.md、
// docs/architecture/cluster-context-map.md §1/§3）。
//
// 本文件 RED 引用尚不存在的 app/RedisPresenceDirectory.hpp 与 app/RedisConn.hpp
// → 编译失败（missing header）即合法 RED；GREEN 时按本文件用法精确实现两接口
// （命名/语义冻结，参照 P4-01 先例）。
//
// 真实 Redis 集成（127.0.0.1:6379 db=1，env REDIS_TEST_HOST/PORT/DB 可覆盖），
// **不 skip**：Redis 不可用时 SetUp 连接失败即测试失败（P4-02 完成定义）。
// 键空间隔离：db=1 + 前缀 presence:v1:，不动 db0；fixture SetUp/TearDown
// FLUSHDB db=1 保证测试键清理与跨轮隔离。
//
// 预期接口（GREEN 目标，本文件按此用法驱动；值/结果类型同 P4-01 抽象 port）：
//   class RedisPresenceDirectory : public PresenceDirectory {
//   public:
//       RedisPresenceDirectory(Clock& clock, const std::string& host, int port,
//                              int db, int64_t ttlMs,
//                              int64_t connectTimeoutMs, int64_t commandTimeoutMs);
//       ClaimResult claim(UserId user, GatewayId gateway, ConnectionId conn) override;
//       RenewResult renew(UserId, GatewayId, ConnectionId, SessionEpoch) override;
//       ReleaseResult release(UserId, GatewayId, ConnectionId, SessionEpoch) override;
//       LocateResult locate(UserId) override;
//   };
//   class RedisConn {   // 最小 RESP 客户端（adapter 与测试 fixture 共用）
//       struct Reply { enum class Type { Simple, Integer, Bulk, Error, Nil, Array }; ... };
//       bool connect(const std::string& host, int port, int db, int64_t timeoutMs);
//       Reply command(const std::vector<std::string>& argv, int64_t timeoutMs);
//       bool connected() const; void close();
//   };
//
// 语义约束（卡 Interface/完成定义，本文件逐测试断言）：
//   - Lua 原子 claim（新 epoch = 当前条目 epoch+1，原子覆盖）；并发 claim 恰一赢，
//     终态=最大 epoch 唯一路由，败者 epoch 被 fencing（renew→NotEpoch）。
//   - renew/release compare-and-delete：旧 epoch → NotEpoch 且条目不变；条目缺失/
//     TTL 到期 → NotFound（可区分）。
//   - value 内嵌 expiresAtMs，locate 由注入 Clock 比较判定 expired（不依赖 key
//     自动过期删除）；TTL 到期 → NotFound 且 expired=true。
//   - Redis down/超时/端口关闭 → DependencyUnavailable（区别于业务错误），恢复后可用。
//   - restart（--save ""）后条目消失 == TTL 语义一致（presence 为易失路由投影）。

#include "app/PresenceDirectory.hpp"  // P4-01 抽象 port（值/结果类型）
#include "app/RedisPresenceDirectory.hpp"  // RED：尚不存在 → 编译失败即合法 RED
#include "app/RedisConn.hpp"               // RED：尚不存在 → 编译失败即合法 RED
#include "FakeClock.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

const UserId kAlice{1};
const UserId kBob{2};
const GatewayId kGwA{10};
const GatewayId kGwB{20};
const ConnectionId kConn1{1};
const ConnectionId kConn2{2};
const int64_t kTtlMs = 1000;
const int64_t kT0 = 1000000;

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

std::string cliBin()
{
    const char* c = getenv("REDIS_CLI_BIN");
    return c ? std::string(c) : std::string("redis-cli");
}

std::string serverBin()
{
    const char* s = getenv("REDIS_SERVER_BIN");
    return s ? std::string(s) : std::string("redis-server");
}

// 轮询等待 6379 可连（服务在起）/不可连（服务已停）。
void waitUntilClosed()
{
    for (int i = 0; i < 100; ++i) {
        RedisConn c;
        if (!c.connect(testHost(), testPort(), testDb(), 300)) {
            return;
        }
        c.close();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

void waitUntilOpen()
{
    for (int i = 0; i < 200; ++i) {
        RedisConn c;
        if (c.connect(testHost(), testPort(), testDb(), 300)) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

void shutdownRedis()
{
    std::string cmd = cliBin() + " -h " + testHost() + " -p " + std::to_string(testPort())
        + " shutdown nosave >/dev/null 2>&1";
    (void)std::system(cmd.c_str());
    waitUntilClosed();
}

void startRedis()
{
    std::string cfg = "/tmp/redis-p42-test.conf";
    {
        std::ofstream out(cfg);
        out << "port " << testPort() << "\n"
            << "bind 127.0.0.1\n"  // M-2：受控测试实例只监听回环，不暴露 0.0.0.0
            << "daemonize yes\n"
            << "save \"\"\n"
            << "appendonly no\n"
            << "dir /tmp\n"
            << "logfile \"\"\n";
    }
    std::string cmd = serverBin() + " " + cfg + " >/dev/null 2>&1";
    (void)std::system(cmd.c_str());
    waitUntilOpen();
}

// 假慢 Redis：accept 后不回包 → 命令读超时（timeout 故障注入）。
class SlowRedisServer {
public:
    SlowRedisServer()
    {
        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listener_ < 0) {
            throw std::runtime_error("SlowRedisServer: socket failed");
        }
        struct sockaddr_in addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(listener_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
            throw std::runtime_error("SlowRedisServer: bind failed");
        }
        if (::listen(listener_, 4) != 0) {
            throw std::runtime_error("SlowRedisServer: listen failed");
        }
        socklen_t len = sizeof(addr);
        if (::getsockname(listener_, reinterpret_cast<struct sockaddr*>(&addr), &len) != 0) {
            throw std::runtime_error("SlowRedisServer: getsockname failed");
        }
        port_ = ntohs(addr.sin_port);
        thread_ = std::thread([this] {
            int c = ::accept(listener_, nullptr, nullptr);
            if (c >= 0) {
                // 接收连接后保持打开、永不回复；直到对端（adapter 超时）关闭。
                char buf[64];
                while (::recv(c, buf, sizeof(buf), 0) > 0) {
                }
                ::close(c);
            }
        });
    }

    ~SlowRedisServer()
    {
        ::close(listener_);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    int port() const { return port_; }

private:
    int listener_ = -1;
    int port_ = 0;
    std::thread thread_;
};

// 取一个当前无监听的端口（先 bind 到 ephemeral 再关闭 → 连接被拒）。
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
    int p = ntohs(addr.sin_port);
    ::close(s);
    return p;
}

class RedisPresenceContractTest : public ::testing::Test {
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

// 卡场景 1：claim→locate round-trip——真实 Redis 下路由三要素完整、expiresAtMs 内嵌。
TEST_F(RedisPresenceContractTest, ClaimThenLocateRoundTrip)
{
    FakeClock clock;
    clock.set(kT0);
    RedisPresenceDirectory dir(clock, testHost(), testPort(), testDb(), kTtlMs, 1000, 1000);

    ClaimResult c = dir.claim(kAlice, kGwA, kConn1);
    ASSERT_TRUE(c.ok);
    ASSERT_GT(c.epoch.value, 0u);

    LocateResult l = dir.locate(kAlice);
    ASSERT_TRUE(l.ok);
    EXPECT_EQ(kAlice, l.route.user);
    EXPECT_EQ(kGwA, l.route.gatewayId);
    EXPECT_EQ(kConn1, l.route.connectionId);
    EXPECT_EQ(c.epoch, l.route.sessionEpoch);

    // 未 claim 的 User：NotFound 且 expired=false（与 TTL 到期可区分）。
    LocateResult absent = dir.locate(kBob);
    EXPECT_FALSE(absent.ok);
    EXPECT_FALSE(absent.expired);
    EXPECT_EQ(PresenceError::NotFound, absent.error);
}

// 卡场景 2a：真实多线程并发 claim 恰一赢——每线程独立 adapter 实例（独立连接），
// 8 线程 barrier 同时 claim 同 user；终态=最大 epoch 唯一路由，败者被 fencing。
// 20 轮重复（关键竞态重复运行证据）。
TEST_F(RedisPresenceContractTest, ConcurrentClaimsExactlyOneWinsThreads)
{
    const int kThreads = 8;
    for (int round = 0; round < 20; ++round) {
        FakeClock clock;
        clock.set(kT0);

        struct Attempt {
            int id = 0;
            GatewayId gateway;
            ConnectionId conn;
            ClaimResult result;
        };

        std::atomic<int> go{0};
        std::mutex mu;
        std::vector<Attempt> attempts;
        std::vector<std::thread> threads;
        for (int i = 0; i < kThreads; ++i) {
            threads.emplace_back([&, i] {
                while (go.load() == 0) {
                    std::this_thread::yield();
                }
                RedisPresenceDirectory dir(clock, testHost(), testPort(), testDb(),
                                           kTtlMs, 1000, 1000);
                Attempt a;
                a.id = i;
                a.gateway = GatewayId(1000 + i);
                a.conn = ConnectionId(i + 1);
                a.result = dir.claim(kAlice, a.gateway, a.conn);
                std::lock_guard<std::mutex> lk(mu);
                attempts.push_back(a);
            });
        }
        go.store(1);
        for (size_t i = 0; i < threads.size(); ++i) {
            threads[i].join();
        }

        ASSERT_EQ(static_cast<size_t>(kThreads), attempts.size());
        uint64_t maxEpoch = 0;
        size_t winner = 0;
        for (size_t i = 0; i < attempts.size(); ++i) {
            ASSERT_TRUE(attempts[i].result.ok) << "round " << round << " thread "
                                               << attempts[i].id;
            for (size_t j = i + 1; j < attempts.size(); ++j) {
                EXPECT_NE(attempts[i].result.epoch.value, attempts[j].result.epoch.value)
                    << "round " << round << ": 并发 claim 的 epoch 必须两两不同";
            }
            if (attempts[i].result.epoch.value > maxEpoch) {
                maxEpoch = attempts[i].result.epoch.value;
                winner = i;
            }
        }

        // 终态唯一且与赢家无撕裂（最后一次写入者持最大 epoch，无双路由残留）。
        RedisPresenceDirectory dir(clock, testHost(), testPort(), testDb(), kTtlMs, 1000, 1000);
        LocateResult l = dir.locate(kAlice);
        ASSERT_TRUE(l.ok) << "round " << round;
        EXPECT_EQ(attempts[winner].gateway, l.route.gatewayId);
        EXPECT_EQ(attempts[winner].conn, l.route.connectionId);
        EXPECT_EQ(attempts[winner].result.epoch, l.route.sessionEpoch);

        // 败者 epoch 被 fencing（并发冲突的可断言结果）。
        const size_t loser = (winner == 0) ? 1 : 0;
        RenewResult r = dir.renew(kAlice, attempts[loser].gateway, attempts[loser].conn,
                                  attempts[loser].result.epoch);
        EXPECT_FALSE(r.ok) << "round " << round;
        EXPECT_EQ(PresenceError::NotEpoch, r.error);

        (void)raw_.command({"DEL", keyFor(kAlice)}, 2000);
    }
}

// 卡场景 2b：真实多进程并发 claim 恰一赢——fork 4 子进程，每进程独立连接 claim，
// 终态=最大 epoch 唯一路由（进程级隔离下的 Redis Lua 原子性证据）。
TEST_F(RedisPresenceContractTest, ConcurrentClaimsExactlyOneWinsMultiProcess)
{
    FakeClock clock;
    clock.set(kT0);

    const int kChildren = 4;
    int pipes[kChildren][2];
    pid_t pids[kChildren];
    for (int i = 0; i < kChildren; ++i) {
        ASSERT_EQ(0, ::pipe(pipes[i]));
    }
    for (int i = 0; i < kChildren; ++i) {
        pids[i] = ::fork();
        ASSERT_GE(pids[i], 0);
        if (pids[i] == 0) {
            // 子进程：独立连接 claim，把 epoch 写入管道。
            ::close(pipes[i][0]);
            RedisConn conn;
            if (!conn.connect(testHost(), testPort(), testDb(), 2000)) {
                _exit(2);
            }
            RedisPresenceDirectory dir(clock, testHost(), testPort(), testDb(),
                                       kTtlMs, 1000, 1000);
            ClaimResult c = dir.claim(kAlice, GatewayId(100 + i), ConnectionId(i + 1));
            std::string out = c.ok ? std::to_string(c.epoch.value) : "FAIL";
            (void)::write(pipes[i][1], out.c_str(), out.size());
            ::close(pipes[i][1]);
            _exit(c.ok ? 0 : 3);
        }
        ::close(pipes[i][1]);
    }

    std::vector<uint64_t> epochs(kChildren, 0);
    bool allOk = true;
    for (int i = 0; i < kChildren; ++i) {
        char buf[64];
        ssize_t n = ::read(pipes[i][0], buf, sizeof(buf) - 1);
        buf[n < 0 ? 0 : n] = '\0';
        ::close(pipes[i][0]);
        if (std::string(buf) == "FAIL") {
            allOk = false;
        } else {
            epochs[i] = std::strtoull(buf, nullptr, 10);
        }
        int status = 0;
        ASSERT_EQ(pids[i], ::waitpid(pids[i], &status, 0));
        ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0) << "child " << i;
    }
    ASSERT_TRUE(allOk);
    for (int i = 0; i < kChildren; ++i) {
        EXPECT_GT(epochs[i], 0u);
        for (int j = i + 1; j < kChildren; ++j) {
            EXPECT_NE(epochs[i], epochs[j]) << "并发进程 claim 的 epoch 必须两两不同";
        }
    }

    uint64_t maxEpoch = 0;
    int winner = 0;
    for (int i = 0; i < kChildren; ++i) {
        if (epochs[i] > maxEpoch) {
            maxEpoch = epochs[i];
            winner = i;
        }
    }
    RedisPresenceDirectory dir(clock, testHost(), testPort(), testDb(), kTtlMs, 1000, 1000);
    LocateResult l = dir.locate(kAlice);
    ASSERT_TRUE(l.ok);
    EXPECT_EQ(GatewayId(100 + winner), l.route.gatewayId);
    EXPECT_EQ(maxEpoch, l.route.sessionEpoch.value);

    // 败者 epoch 被 fencing（进程级并发的可断言结果，与线程用例一致）。
    const int loser = (winner == 0) ? 1 : 0;
    RenewResult r = dir.renew(kAlice, GatewayId(100 + loser), ConnectionId(loser + 1),
                              SessionEpoch(epochs[loser]));
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(PresenceError::NotEpoch, r.error);
}

// 卡场景 3：旧节点延迟 release 不删新租约（compare-and-delete，epoch 不匹配拒绝且
// 新租约原样保留）。
TEST_F(RedisPresenceContractTest, StaleEpochReleaseRejectedEntryIntact)
{
    FakeClock clock;
    clock.set(kT0);
    RedisPresenceDirectory dir(clock, testHost(), testPort(), testDb(), kTtlMs, 1000, 1000);

    ClaimResult c1 = dir.claim(kAlice, kGwA, kConn1);
    ASSERT_TRUE(c1.ok);
    ClaimResult c2 = dir.claim(kAlice, kGwB, kConn2);
    ASSERT_TRUE(c2.ok);
    EXPECT_GT(c2.epoch.value, c1.epoch.value);

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

// 卡场景 4：旧 epoch renew 被拒且条目不变；过期条目 renew/release 报 NotFound
// （与 NotEpoch 可区分）。
TEST_F(RedisPresenceContractTest, StaleEpochRenewRejectedAndExpiredIsNotFound)
{
    FakeClock clock;
    clock.set(kT0);
    RedisPresenceDirectory dir(clock, testHost(), testPort(), testDb(), kTtlMs, 1000, 1000);

    ClaimResult c1 = dir.claim(kAlice, kGwA, kConn1);
    ASSERT_TRUE(c1.ok);
    ClaimResult c2 = dir.claim(kAlice, kGwB, kConn2);
    ASSERT_TRUE(c2.ok);

    RenewResult stale = dir.renew(kAlice, kGwA, kConn1, c1.epoch);
    EXPECT_FALSE(stale.ok);
    EXPECT_EQ(PresenceError::NotEpoch, stale.error);

    LocateResult l = dir.locate(kAlice);
    ASSERT_TRUE(l.ok);
    EXPECT_EQ(c2.epoch, l.route.sessionEpoch);

    // 过期后 renew/release 报 NotFound（条目已无 epoch 可校验），非 NotEpoch。
    clock.advance(10 * kTtlMs);
    RenewResult rExp = dir.renew(kAlice, kGwB, kConn2, c2.epoch);
    EXPECT_FALSE(rExp.ok);
    EXPECT_EQ(PresenceError::NotFound, rExp.error);
    ReleaseResult relExp = dir.release(kAlice, kGwB, kConn2, c2.epoch);
    EXPECT_FALSE(relExp.ok);
    EXPECT_EQ(PresenceError::NotFound, relExp.error);
}

// 卡场景 5：TTL 到期——注入 FakeClock 确定性判定（now >= expiresAtMs 冻结语义，
// 无 sleep）；value 内嵌 expiresAtMs 由客户端比较，不依赖 key 自动过期。
TEST_F(RedisPresenceContractTest, TtlExpiryDeterministic)
{
    FakeClock clock;
    clock.set(kT0);
    RedisPresenceDirectory dir(clock, testHost(), testPort(), testDb(), kTtlMs, 1000, 1000);

    ClaimResult c = dir.claim(kAlice, kGwA, kConn1);
    ASSERT_TRUE(c.ok);

    clock.advance(kTtlMs - 1);
    LocateResult live = dir.locate(kAlice);
    ASSERT_TRUE(live.ok);

    clock.advance(1);  // now == expiresAtMs（到期边界，>= 语义）→ 不存在且 expired=true
    LocateResult boundary = dir.locate(kAlice);
    EXPECT_FALSE(boundary.ok);
    EXPECT_TRUE(boundary.expired);
    EXPECT_EQ(PresenceError::NotFound, boundary.error);

    clock.advance(1);
    LocateResult gone = dir.locate(kAlice);
    EXPECT_FALSE(gone.ok);
    EXPECT_TRUE(gone.expired);
    EXPECT_EQ(PresenceError::NotFound, gone.error);

    // 过期后 claim 正常生成新 epoch（>= 旧 epoch+1）。
    ClaimResult c2 = dir.claim(kAlice, kGwB, kConn2);
    ASSERT_TRUE(c2.ok);
    EXPECT_GT(c2.epoch.value, c.epoch.value);
}

// 卡场景 5b：TTL 到期——真实墙钟对照（短 TTL + 真实等待），证明与 Redis 实际
// 时间语义一致。
TEST_F(RedisPresenceContractTest, TtlExpiryAgainstWallClock)
{
    UnixEpochClock wall;
    const int64_t kShortTtl = 300;
    RedisPresenceDirectory dir(wall, testHost(), testPort(), testDb(), kShortTtl, 1000, 1000);

    ClaimResult c = dir.claim(kAlice, kGwA, kConn1);
    ASSERT_TRUE(c.ok);

    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    LocateResult gone = dir.locate(kAlice);
    EXPECT_FALSE(gone.ok);
    EXPECT_TRUE(gone.expired);
    EXPECT_EQ(PresenceError::NotFound, gone.error);
}

// 卡场景 6：renew 延长到期（可观测：越过原到期点仍在续期 TTL 内时 locate 仍活）。
TEST_F(RedisPresenceContractTest, RenewExtendsExpiry)
{
    FakeClock clock;
    clock.set(kT0);
    RedisPresenceDirectory dir(clock, testHost(), testPort(), testDb(), kTtlMs, 1000, 1000);

    ClaimResult c = dir.claim(kAlice, kGwA, kConn1);
    ASSERT_TRUE(c.ok);

    clock.advance(kTtlMs - 10);
    RenewResult r = dir.renew(kAlice, kGwA, kConn1, c.epoch);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(clock.nowMs() + kTtlMs, r.expiresAtMs);

    clock.advance(15);  // 越过原到期点、仍在续期 TTL 内 → 仍活。
    LocateResult l = dir.locate(kAlice);
    ASSERT_TRUE(l.ok);
    EXPECT_EQ(c.epoch, l.route.sessionEpoch);
}

// 卡场景 7：release compare-and-delete 精确语义 + release 后重新 claim 新 epoch。
TEST_F(RedisPresenceContractTest, ReleaseCompareAndDeleteThenReclaim)
{
    FakeClock clock;
    clock.set(kT0);
    RedisPresenceDirectory dir(clock, testHost(), testPort(), testDb(), kTtlMs, 1000, 1000);

    ClaimResult c1 = dir.claim(kAlice, kGwA, kConn1);
    ASSERT_TRUE(c1.ok);

    ReleaseResult rel = dir.release(kAlice, kGwA, kConn1, c1.epoch);
    ASSERT_TRUE(rel.ok);

    LocateResult l = dir.locate(kAlice);
    EXPECT_FALSE(l.ok);
    EXPECT_FALSE(l.expired);  // release 删除的条目像从未 claim（非 TTL 过期）。
    EXPECT_EQ(PresenceError::NotFound, l.error);

    ReleaseResult again = dir.release(kAlice, kGwA, kConn1, c1.epoch);
    EXPECT_FALSE(again.ok);
    EXPECT_EQ(PresenceError::NotFound, again.error);

    ClaimResult c2 = dir.claim(kAlice, kGwB, kConn2);
    ASSERT_TRUE(c2.ok);
    EXPECT_GT(c2.epoch.value, c1.epoch.value);
}

// 卡场景 8：timeout——假慢 Redis（accept 后不回包）→ DependencyUnavailable。
TEST_F(RedisPresenceContractTest, SlowRedisTimesOutToDependencyUnavailable)
{
    SlowRedisServer slow;
    FakeClock clock;
    clock.set(kT0);
    RedisPresenceDirectory dir(clock, "127.0.0.1", slow.port(), testDb(), kTtlMs, 300, 300);

    ClaimResult c = dir.claim(kAlice, kGwA, kConn1);
    EXPECT_FALSE(c.ok);
    EXPECT_EQ(PresenceError::DependencyUnavailable, c.error);
}

// 卡场景 8b：连接已关端口（ECONNREFUSED）→ DependencyUnavailable。
TEST_F(RedisPresenceContractTest, ClosedPortYieldsDependencyUnavailable)
{
    const int deadPort = freePort();
    FakeClock clock;
    clock.set(kT0);
    RedisPresenceDirectory dir(clock, "127.0.0.1", deadPort, testDb(), kTtlMs, 300, 300);

    ClaimResult c = dir.claim(kAlice, kGwA, kConn1);
    EXPECT_FALSE(c.ok);
    EXPECT_EQ(PresenceError::DependencyUnavailable, c.error);
}

// 卡场景 9：Redis down → 四操作统一 DependencyUnavailable → 恢复后 claim/locate 正常。
TEST_F(RedisPresenceContractTest, RedisDownYieldsDependencyUnavailableThenRecovers)
{
    FakeClock clock;
    clock.set(kT0);
    RedisPresenceDirectory dir(clock, testHost(), testPort(), testDb(), kTtlMs, 1000, 1000);
    ASSERT_TRUE(dir.claim(kAlice, kGwA, kConn1).ok);

    shutdownRedis();

    ClaimResult c = dir.claim(kBob, kGwB, kConn2);
    EXPECT_FALSE(c.ok);
    EXPECT_EQ(PresenceError::DependencyUnavailable, c.error);
    LocateResult l = dir.locate(kAlice);
    EXPECT_FALSE(l.ok);
    EXPECT_EQ(PresenceError::DependencyUnavailable, l.error);
    RenewResult r = dir.renew(kAlice, kGwA, kConn1, c.epoch);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(PresenceError::DependencyUnavailable, r.error);
    ReleaseResult rel = dir.release(kAlice, kGwA, kConn1, c.epoch);
    EXPECT_FALSE(rel.ok);
    EXPECT_EQ(PresenceError::DependencyUnavailable, rel.error);

    startRedis();

    ClaimResult c2 = dir.claim(kAlice, kGwA, kConn1);
    ASSERT_TRUE(c2.ok);
    LocateResult l2 = dir.locate(kAlice);
    ASSERT_TRUE(l2.ok);
    EXPECT_EQ(kGwA, l2.route.gatewayId);
    EXPECT_EQ(c2.epoch, l2.route.sessionEpoch);
}

// 卡场景 10：restart（--save ""）——条目随重启消失 == TTL 语义一致（presence 为
// 易失路由投影，不持久化）；重启后重新 claim 可用。
TEST_F(RedisPresenceContractTest, RestartLosesPresenceEntryConsistentWithTtl)
{
    FakeClock clock;
    clock.set(kT0);
    RedisPresenceDirectory dir(clock, testHost(), testPort(), testDb(), kTtlMs, 1000, 1000);

    ClaimResult c = dir.claim(kAlice, kGwA, kConn1);
    ASSERT_TRUE(c.ok);
    LocateResult before = dir.locate(kAlice);
    ASSERT_TRUE(before.ok);

    shutdownRedis();
    startRedis();

    // --save ""：Redis 重启不恢复键 → 条目消失（locate NotFound，非 TTL 过期）。
    // 重启杀死旧连接：首查可能报 DependencyUnavailable（陈旧连接），重连后即为
    // 条目消失的确定性语义。
    LocateResult after;
    for (int attempt = 0; attempt < 5; ++attempt) {
        after = dir.locate(kAlice);
        if (after.error != PresenceError::DependencyUnavailable) {
            break;
        }
    }
    EXPECT_FALSE(after.ok);
    EXPECT_FALSE(after.expired);
    EXPECT_EQ(PresenceError::NotFound, after.error);

    ClaimResult c2 = dir.claim(kAlice, kGwA, kConn1);
    ASSERT_TRUE(c2.ok);
    LocateResult l2 = dir.locate(kAlice);
    ASSERT_TRUE(l2.ok);
    EXPECT_EQ(c2.epoch, l2.route.sessionEpoch);
}

// 卡场景 11：键空间隔离——键在 db=1 以 presence:v1:<id> 存在，db0 无该键。
TEST_F(RedisPresenceContractTest, KeyPrefixAndDbIsolation)
{
    FakeClock clock;
    clock.set(kT0);
    RedisPresenceDirectory dir(clock, testHost(), testPort(), testDb(), kTtlMs, 1000, 1000);
    ASSERT_TRUE(dir.claim(kAlice, kGwA, kConn1).ok);

    // db=1：键存在（Bulk，非 nil）。
    RedisConn::Reply inDb = raw_.command({"GET", keyFor(kAlice)}, 2000);
    ASSERT_EQ(RedisConn::Reply::Type::Bulk, inDb.type);

    // db0：键不存在（隔离）。
    RedisConn::Reply sel0 = raw_.command({"SELECT", "0"}, 2000);
    ASSERT_TRUE(!sel0.isError());
    RedisConn::Reply db0 = raw_.command({"GET", keyFor(kAlice)}, 2000);
    EXPECT_EQ(RedisConn::Reply::Type::Nil, db0.type);

    // 切回 db=1（TearDown FLUSHDB 需要）。
    RedisConn::Reply sel1 = raw_.command({"SELECT", std::to_string(testDb())}, 2000);
    ASSERT_TRUE(!sel1.isError());
}

} // namespace
