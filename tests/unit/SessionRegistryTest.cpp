// P3-05 SessionRegistry 单元契约（无 MySQL、无网络）：
// 双向一致性（connection→session 与 user→connection）、B-21 收紧（同一连接
// 二次登录被拒）、B-08 单会话（同一 User 异地登录被拒）、unbind/unbindUser
// 幂等恰好一次、旧 generation 不覆盖新会话、并发不变量（TSan 聚焦载体）。
#include "app/SessionRegistry.hpp"
#include "EventLoop.h"
#include "TcpConnection.h"

#include <gtest/gtest.h>

#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <thread>
#include <vector>

namespace {

// Registry 只使用 TcpConnection 的指针身份：连接用真实 TcpConnection 构造
// （socketpair fd），但从不 connectEstablished，EventLoop 不运行。
class RegistryFixture : public ::testing::Test {
protected:
    EventLoop loop;

    TcpConnectionPtr makeConn()
    {
        int fds[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
            return TcpConnectionPtr();
        }
        close(fds[0]);
        return TcpConnectionPtr(
            new TcpConnection(&loop, "c", fds[1], InetAddress(0), InetAddress(0)));
    }
};

} // namespace

TEST_F(RegistryFixture, BindAndLookupBothDirections)
{
    SessionRegistry reg;
    TcpConnectionPtr conn = makeConn();
    ASSERT_TRUE(conn);

    EXPECT_EQ(SessionRegistry::BindResult::Ok, reg.bind(conn, 1, 5));
    EXPECT_EQ(1u, reg.size());

    BoundSession s;
    ASSERT_TRUE(reg.lookupByConnection(conn, &s));
    EXPECT_EQ(1, s.userId);
    EXPECT_EQ(5, s.generation);
    EXPECT_EQ(conn, reg.lookupByUser(1));

    BoundSession none;
    EXPECT_FALSE(reg.lookupByConnection(makeConn(), &none));
    EXPECT_EQ(TcpConnectionPtr(), reg.lookupByUser(2));
}

TEST_F(RegistryFixture, BindRejectsUserSwitchOnSameConnection)
{
    // B-21 收紧：同一连接已登录后切换另一 User 被拒。
    SessionRegistry reg;
    TcpConnectionPtr conn = makeConn();
    ASSERT_EQ(SessionRegistry::BindResult::Ok, reg.bind(conn, 1, 1));

    EXPECT_EQ(SessionRegistry::BindResult::ConnectionBusy, reg.bind(conn, 2, 1));

    BoundSession s;
    ASSERT_TRUE(reg.lookupByConnection(conn, &s));
    EXPECT_EQ(1, s.userId);
    EXPECT_EQ(TcpConnectionPtr(), reg.lookupByUser(2));
    EXPECT_EQ(1u, reg.size());
}

TEST_F(RegistryFixture, BindRejectsSameUserTwiceOnSameConnection)
{
    // 已登录连接同 User 二次登录同样被拒，原会话不被覆盖。
    SessionRegistry reg;
    TcpConnectionPtr conn = makeConn();
    ASSERT_EQ(SessionRegistry::BindResult::Ok, reg.bind(conn, 1, 3));

    EXPECT_EQ(SessionRegistry::BindResult::ConnectionBusy, reg.bind(conn, 1, 4));

    BoundSession s;
    ASSERT_TRUE(reg.lookupByConnection(conn, &s));
    EXPECT_EQ(1, s.userId);
    EXPECT_EQ(3, s.generation);
}

TEST_F(RegistryFixture, BindRejectsStaleGenerationAttempt)
{
    // 旧 generation 登录 completion 不得覆盖已建立的新会话。
    SessionRegistry reg;
    TcpConnectionPtr conn = makeConn();
    ASSERT_EQ(SessionRegistry::BindResult::Ok, reg.bind(conn, 1, 6));

    EXPECT_EQ(SessionRegistry::BindResult::ConnectionBusy, reg.bind(conn, 1, 5));

    BoundSession s;
    ASSERT_TRUE(reg.lookupByConnection(conn, &s));
    EXPECT_EQ(6, s.generation);
}

TEST_F(RegistryFixture, BindRejectsUserAlreadyActiveElsewhere)
{
    // B-08 单会话约束：同一 User 已在另一连接活动 → 拒绝。
    SessionRegistry reg;
    TcpConnectionPtr connA = makeConn();
    TcpConnectionPtr connB = makeConn();
    ASSERT_EQ(SessionRegistry::BindResult::Ok, reg.bind(connA, 1, 1));

    EXPECT_EQ(SessionRegistry::BindResult::UserBusy, reg.bind(connB, 1, 2));

    EXPECT_EQ(connA, reg.lookupByUser(1));
    BoundSession s;
    EXPECT_FALSE(reg.lookupByConnection(connB, &s));
    EXPECT_EQ(1u, reg.size());
}

TEST_F(RegistryFixture, UnbindReleasesExactlyOnce)
{
    SessionRegistry reg;
    TcpConnectionPtr conn = makeConn();
    ASSERT_EQ(SessionRegistry::BindResult::Ok, reg.bind(conn, 1, 1));

    EXPECT_EQ(1, reg.unbind(conn));
    EXPECT_EQ(0, reg.unbind(conn));  // 幂等：重复释放 no-op

    BoundSession s;
    EXPECT_FALSE(reg.lookupByConnection(conn, &s));
    EXPECT_EQ(TcpConnectionPtr(), reg.lookupByUser(1));
    EXPECT_EQ(0u, reg.size());
}

TEST_F(RegistryFixture, UnbindUserClearsBothDirections)
{
    SessionRegistry reg;
    TcpConnectionPtr connA = makeConn();
    TcpConnectionPtr connB = makeConn();
    ASSERT_EQ(SessionRegistry::BindResult::Ok, reg.bind(connA, 1, 1));
    ASSERT_EQ(SessionRegistry::BindResult::Ok, reg.bind(connB, 2, 1));

    EXPECT_EQ(1, reg.unbindUser(1));
    EXPECT_EQ(0, reg.unbindUser(1));  // 幂等

    BoundSession s;
    EXPECT_FALSE(reg.lookupByConnection(connA, &s));
    EXPECT_EQ(TcpConnectionPtr(), reg.lookupByUser(1));
    ASSERT_TRUE(reg.lookupByConnection(connB, &s));
    EXPECT_EQ(2, s.userId);
    EXPECT_EQ(1u, reg.size());
}

TEST_F(RegistryFixture, UnbindUserThenRebindOnSameConnection)
{
    // 登出后同一连接可再次登录（B-10 登出幂等语义下的正常循环）。
    SessionRegistry reg;
    TcpConnectionPtr conn = makeConn();
    ASSERT_EQ(SessionRegistry::BindResult::Ok, reg.bind(conn, 1, 1));
    EXPECT_EQ(1, reg.unbindUser(1));

    EXPECT_EQ(SessionRegistry::BindResult::Ok, reg.bind(conn, 1, 2));
    BoundSession s;
    ASSERT_TRUE(reg.lookupByConnection(conn, &s));
    EXPECT_EQ(1, s.userId);
    EXPECT_EQ(2, s.generation);
}

TEST_F(RegistryFixture, SnapshotConnectionsExcludesSenderAndUnbound)
{
    SessionRegistry reg;
    TcpConnectionPtr connA = makeConn();
    TcpConnectionPtr connB = makeConn();
    ASSERT_EQ(SessionRegistry::BindResult::Ok, reg.bind(connA, 1, 1));
    ASSERT_EQ(SessionRegistry::BindResult::Ok, reg.bind(connB, 2, 1));

    std::vector<int64_t> userIds;
    userIds.push_back(1);
    userIds.push_back(2);
    userIds.push_back(3);  // 未登录
    std::unordered_map<int64_t, TcpConnectionPtr> online =
        reg.snapshotConnections(userIds, 1);  // 排除发送者 1
    EXPECT_EQ(1u, online.size());
    ASSERT_NE(online.end(), online.find(2));
    EXPECT_EQ(connB, online[2]);

    std::unordered_map<int64_t, TcpConnectionPtr> all =
        reg.snapshotConnections(userIds, 0);
    EXPECT_EQ(2u, all.size());
}

TEST_F(RegistryFixture, ConcurrentSameUserSingleWinner)
{
    // 多 Reactor 竞争：同一 User 并发登录多个连接，恰好一个赢家。
    SessionRegistry reg;
    const int kThreads = 8;
    std::vector<TcpConnectionPtr> conns;
    for (int i = 0; i < kThreads; ++i) {
        conns.push_back(makeConn());
    }
    std::atomic<int> ok{0};
    std::atomic<int> busy{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&, i] {
            SessionRegistry::BindResult r = reg.bind(conns[i], 1000, 1);
            if (r == SessionRegistry::BindResult::Ok) {
                ok.fetch_add(1);
            } else if (r == SessionRegistry::BindResult::UserBusy) {
                busy.fetch_add(1);
            }
        });
    }
    for (std::size_t i = 0; i < threads.size(); ++i) {
        threads[i].join();
    }

    EXPECT_EQ(1, ok.load());
    EXPECT_EQ(kThreads - 1, busy.load());
    EXPECT_EQ(1u, reg.size());
    TcpConnectionPtr winner = reg.lookupByUser(1000);
    ASSERT_TRUE(winner);
    BoundSession s;
    ASSERT_TRUE(reg.lookupByConnection(winner, &s));
    EXPECT_EQ(1000, s.userId);
    EXPECT_EQ(1000, reg.unbind(winner));
    EXPECT_EQ(0u, reg.size());
}

TEST_F(RegistryFixture, ConcurrentBindUnbindKeepsBidirectionalInvariant)
{
    // 并发 bind/unbind/lookup：双向一致性在任何时刻保持（TSan 聚焦载体）。
    SessionRegistry reg;
    const int kThreads = 4;
    const int kIters = 500;
    std::vector<TcpConnectionPtr> conns;
    for (int i = 0; i < kThreads; ++i) {
        conns.push_back(makeConn());
    }
    std::atomic<int> violations{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&, i] {
            int64_t uid = 100 + i;
            for (int it = 0; it < kIters; ++it) {
                if (reg.bind(conns[i], uid, 1) != SessionRegistry::BindResult::Ok) {
                    violations.fetch_add(1);
                }
                BoundSession s;
                if (!reg.lookupByConnection(conns[i], &s) || s.userId != uid) {
                    violations.fetch_add(1);
                }
                if (reg.lookupByUser(uid) != conns[i]) {
                    violations.fetch_add(1);
                }
                if (reg.unbind(conns[i]) != uid) {
                    violations.fetch_add(1);
                }
                if (reg.lookupByConnection(conns[i], &s) || reg.lookupByUser(uid)) {
                    violations.fetch_add(1);
                }
            }
        });
    }
    for (std::size_t i = 0; i < threads.size(); ++i) {
        threads[i].join();
    }

    EXPECT_EQ(0, violations.load());
    EXPECT_EQ(0u, reg.size());
}
