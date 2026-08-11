// P3-05 SessionRegistry 单元契约（无 MySQL、无网络）：
// 双向一致性（connection→session 与 user→connection）、B-21 收紧（同一连接
// 二次登录被拒）、B-08 单会话（同一 User 异地登录被拒）、unbind/unbindUser
// 幂等恰好一次、旧 generation 不覆盖新会话、活跃连接集合（addConnection/
// removeConnection；未登记/已移除连接 bind 被拒，close 与 bind 并发无泄漏）、
// 并发不变量（TSan 聚焦载体）。
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
// （socketpair fd），但从不 connectEstablished，EventLoop 不运行。除被测场景
// 外，bind 前一律 addConnection（活跃连接集合契约：连接须先登记才能绑定）。
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
    reg.addConnection(conn);

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
    reg.addConnection(conn);
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
    reg.addConnection(conn);
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
    reg.addConnection(conn);
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
    reg.addConnection(connA);
    reg.addConnection(connB);
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
    reg.addConnection(conn);
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
    reg.addConnection(connA);
    reg.addConnection(connB);
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
    reg.addConnection(conn);
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
    reg.addConnection(connA);
    reg.addConnection(connB);
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
        reg.addConnection(conns[i]);
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
        reg.addConnection(conns[i]);
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

TEST_F(RegistryFixture, BindRejectsUnregisteredConnection)
{
    // 对抗审查：从未 addConnection 登记（或 close 已移除）的连接直接 bind
    // → 拒绝 ConnectionInactive，不建会话（防 bind 死连接 → 会话泄漏）。
    SessionRegistry reg;
    TcpConnectionPtr conn = makeConn();
    ASSERT_TRUE(conn);

    EXPECT_EQ(SessionRegistry::BindResult::ConnectionInactive, reg.bind(conn, 1, 1));
    EXPECT_EQ(0u, reg.size());
    BoundSession s;
    EXPECT_FALSE(reg.lookupByConnection(conn, &s));
    EXPECT_EQ(TcpConnectionPtr(), reg.lookupByUser(1));
}

TEST_F(RegistryFixture, BindRejectsAfterRemoveConnection)
{
    // 模拟 close 回调先于登录 completion：addConnection → removeConnection 后
    // bind 被拒；用户未被占用，可被其他活跃连接正常登录（B-08 不锁死）。
    SessionRegistry reg;
    TcpConnectionPtr connA = makeConn();
    reg.addConnection(connA);
    reg.removeConnection(connA);
    EXPECT_EQ(SessionRegistry::BindResult::ConnectionInactive, reg.bind(connA, 1, 1));

    TcpConnectionPtr connB = makeConn();
    reg.addConnection(connB);
    EXPECT_EQ(SessionRegistry::BindResult::Ok, reg.bind(connB, 1, 2));
    EXPECT_EQ(connB, reg.lookupByUser(1));
    EXPECT_EQ(1u, reg.size());
}

TEST_F(RegistryFixture, RemoveConnectionAndUnbindAfterBindIsIdempotent)
{
    // 模拟 bind 先于 close 的正常交错：close 回调 removeConnection+unbind
    // 恰好释放一次；重复 remove/unbind 幂等，注册表无残留。
    SessionRegistry reg;
    TcpConnectionPtr conn = makeConn();
    reg.addConnection(conn);
    ASSERT_EQ(SessionRegistry::BindResult::Ok, reg.bind(conn, 1, 1));

    reg.removeConnection(conn);
    EXPECT_EQ(1, reg.unbind(conn));
    EXPECT_EQ(0, reg.unbind(conn));  // 幂等
    reg.removeConnection(conn);      // 幂等

    BoundSession s;
    EXPECT_FALSE(reg.lookupByConnection(conn, &s));
    EXPECT_EQ(TcpConnectionPtr(), reg.lookupByUser(1));
    EXPECT_EQ(0u, reg.size());
}

TEST_F(RegistryFixture, AddBindUnbindRebindFullLifecycle)
{
    // 正常生命周期：addConnection → bind → unbind（登出不杀连接）→ 同连接
    // 重登 → close（removeConnection+unbind）→ 再 bind 被拒，全程无残留。
    SessionRegistry reg;
    TcpConnectionPtr conn = makeConn();
    reg.addConnection(conn);
    ASSERT_EQ(SessionRegistry::BindResult::Ok, reg.bind(conn, 1, 1));
    EXPECT_EQ(1, reg.unbind(conn));
    EXPECT_EQ(0u, reg.size());

    // 登出后连接仍活跃：可再次登录（B-10 语义）。
    ASSERT_EQ(SessionRegistry::BindResult::Ok, reg.bind(conn, 1, 2));
    // close：移出活跃集合并释放。
    reg.removeConnection(conn);
    EXPECT_EQ(1, reg.unbind(conn));
    // 连接已死：再 bind 被拒。
    EXPECT_EQ(SessionRegistry::BindResult::ConnectionInactive, reg.bind(conn, 1, 3));
    EXPECT_EQ(0u, reg.size());
}

TEST_F(RegistryFixture, ConcurrentBindVsCloseNoSessionLeak)
{
    // 对抗审查竞态载体（TSan 聚焦）：同一连接上"登录 completion"(bind) 与
    // "close 回调"(removeConnection+unbind) 并发竞争。锁内串行化保证：
    // bind 成功 ⟺ 后续 unbind 恰好释放（返回 userId）；close 先到则 bind
    // 拒绝 ConnectionInactive 且 unbind 返回 0。不允许 bind 成功而 close
    // 未释放（会话泄漏、B-08 锁死用户）。
    SessionRegistry reg;
    const int kIters = 500;
    std::vector<TcpConnectionPtr> conns;
    for (int i = 0; i < kIters; ++i) {
        conns.push_back(makeConn());
        reg.addConnection(conns[i]);
    }
    std::vector<std::atomic<int>> bindOk(kIters);    // 1=Ok, 0=拒绝
    std::vector<std::atomic<int>> unbindRet(kIters); // 释放的 userId 或 0
    for (int i = 0; i < kIters; ++i) {
        bindOk[i] = -1;
        unbindRet[i] = -1;
    }
    std::vector<std::thread> threads;
    threads.emplace_back([&] {
        for (int i = 0; i < kIters; ++i) {
            SessionRegistry::BindResult r = reg.bind(conns[i], 100 + i, 1);
            bindOk[i] = (r == SessionRegistry::BindResult::Ok) ? 1 : 0;
        }
    });
    threads.emplace_back([&] {
        for (int i = 0; i < kIters; ++i) {
            reg.removeConnection(conns[i]);
            unbindRet[i] = static_cast<int>(reg.unbind(conns[i]));
        }
    });
    for (std::size_t i = 0; i < threads.size(); ++i) {
        threads[i].join();
    }

    for (int i = 0; i < kIters; ++i) {
        ASSERT_NE(-1, bindOk[i].load());
        if (bindOk[i].load() == 1) {
            // bind 先到：close 必须恰好释放该会话。
            EXPECT_EQ(100 + i, unbindRet[i].load()) << "i=" << i;
        } else {
            // close 先到：unbind 无残留可释。
            EXPECT_EQ(0, unbindRet[i].load()) << "i=" << i;
        }
    }
    EXPECT_EQ(0u, reg.size());
    EXPECT_EQ(TcpConnectionPtr(), reg.lookupByUser(100));
}
