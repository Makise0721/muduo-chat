#include "MySqlTestFixture.hpp"

#include "db/ConnectionPool.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

namespace {

using PoolError = ConnectionPool::PoolError;

TEST(ConnectionPoolTest, AcquireReturnsLeaseAndLeaseReturnsConnectionOnDestruction)
{
    MySqlTestFixture::resetSchema();
    ConnectionPool& pool = MySqlTestFixture::pool();
    ConnectionPool::Metrics m0 = pool.metrics();
    ASSERT_EQ(2, m0.idle);
    {
        ConnectionPool::AcquireResult r = pool.acquire(1000);
        ASSERT_TRUE(r.lease);
        ASSERT_EQ(PoolError::None, r.error);
        ConnectionPool::Metrics m1 = pool.metrics();
        EXPECT_EQ(1, m1.active);
        EXPECT_EQ(1, m1.idle);
    }  // lease 析构自动归还
    ConnectionPool::Metrics m2 = pool.metrics();
    EXPECT_EQ(0, m2.active);
    EXPECT_EQ(2, m2.idle);
}

TEST(ConnectionPoolTest, AcquireTimesOutWhenPoolExhausted)
{
    MySqlTestFixture::resetSchema();
    ConnectionPool& pool = MySqlTestFixture::pool();
    ConnectionPool::AcquireResult held1 = pool.acquire(1000);
    ConnectionPool::AcquireResult held2 = pool.acquire(1000);
    ASSERT_TRUE(held1.lease);
    ASSERT_TRUE(held2.lease);

    ConnectionPool::AcquireResult r = pool.acquire(200);
    EXPECT_FALSE(r.lease);
    EXPECT_EQ(PoolError::Timeout, r.error);
}

TEST(ConnectionPoolTest, LeaseIsMoveOnlyAndTransferable)
{
    MySqlTestFixture::resetSchema();
    ConnectionPool& pool = MySqlTestFixture::pool();
    ConnectionPool::AcquireResult r1 = pool.acquire(1000);
    ASSERT_TRUE(r1.lease);
    ConnectionLease moved = std::move(r1.lease);
    EXPECT_FALSE(r1.lease);
    EXPECT_TRUE(moved);
    // 显式移动后归还计数正确
    {
        ConnectionLease other = std::move(moved);
        EXPECT_FALSE(moved);
    }
    ConnectionPool::Metrics m = pool.metrics();
    EXPECT_EQ(2, m.idle);
    EXPECT_EQ(0, m.active);
}

TEST(ConnectionPoolTest, ConcurrentAcquireReleaseCyclesAreSafe)
{
    MySqlTestFixture::resetSchema();
    ConnectionPool& pool = MySqlTestFixture::pool();
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&pool, &failures] {
            for (int i = 0; i < 20; ++i) {
                ConnectionPool::AcquireResult r = pool.acquire(2000);
                if (!r.lease) {
                    failures.fetch_add(1);
                    continue;
                }
                // 使用后随作用域归还
            }
        });
    }
    for (std::thread& t : threads) {
        t.join();
    }
    EXPECT_EQ(0, failures.load());
    ConnectionPool::Metrics m = pool.metrics();
    EXPECT_EQ(0, m.active);
    EXPECT_EQ(2, m.idle);
}

// 以下两个测试把池置于 shutdown 终态，必须放在最后。
TEST(ConnectionPoolTest, AcquireFailsFastAfterShutdown)
{
    MySqlTestFixture::resetSchema();
    ConnectionPool& pool = MySqlTestFixture::pool();
    pool.shutdown();
    ConnectionPool::AcquireResult r = pool.acquire(1000);
    EXPECT_FALSE(r.lease);
    EXPECT_EQ(PoolError::Shutdown, r.error);
}

TEST(ConnectionPoolTest, ShutdownWakesWaitingAcquirers)
{
    MySqlTestFixture::resetSchema();
    ConnectionPool& pool = MySqlTestFixture::pool();
    ConnectionPool::AcquireResult held1 = pool.acquire(1000);
    ConnectionPool::AcquireResult held2 = pool.acquire(1000);
    ASSERT_TRUE(held1.lease);
    ASSERT_TRUE(held2.lease);

    std::atomic<PoolError> waiterError{PoolError::None};
    std::atomic<bool> woke{false};
    std::thread waiter([&pool, &waiterError, &woke] {
        ConnectionPool::AcquireResult r = pool.acquire(5000);
        waiterError = r.error;
        woke = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    pool.shutdown();
    waiter.join();
    EXPECT_TRUE(woke.load());
    EXPECT_EQ(PoolError::Shutdown, waiterError.load());
}

} // namespace
