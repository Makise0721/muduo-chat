#include "MySqlTestFixture.hpp"

#include "db/ConnectionPool.hpp"
#include "db/MySQL.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>

// 真实 MySQL 集成：acquire 可用性与坏连接替换。
TEST(MySQLPoolIntegrationTest, AcquiredConnectionServesQueries)
{
    MySqlTestFixture::resetSchema();
    ConnectionPool& pool = MySqlTestFixture::pool();
    ConnectionPool::AcquireResult r = pool.acquire(1000);
    ASSERT_TRUE(r.lease);
    MySQL* mysql = r.lease.get();
    EXPECT_EQ(0, mysql_query(mysql->getConnection(), "SELECT 1"));
    MYSQL_RES* res = mysql_store_result(mysql->getConnection());
    ASSERT_TRUE(res != nullptr);
    mysql_free_result(res);
}

// 数据库断线：KILL 掉池内连接后，再 acquire 必须触发 ping 校验并替换为可用连接。
TEST(MySQLPoolIntegrationTest, BrokenConnectionIsReplacedOnNextAcquire)
{
    MySqlTestFixture::resetSchema();
    ConnectionPool& pool = MySqlTestFixture::pool();

    // 拿到一条池连接并记录其服务端 connection id。
    ConnectionPool::AcquireResult r = pool.acquire(1000);
    ASSERT_TRUE(r.lease);
    MySQL* mysql = r.lease.get();
    EXPECT_EQ(0, mysql_query(mysql->getConnection(), "SELECT CONNECTION_ID()"));
    MYSQL_RES* res = mysql_store_result(mysql->getConnection());
    ASSERT_TRUE(res != nullptr);
    MYSQL_ROW row = mysql_fetch_row(res);
    ASSERT_TRUE(row != nullptr && row[0] != nullptr);
    std::string connId = row[0];
    mysql_free_result(res);
    // 归还（坏连接回到池中）。
    r.lease = ConnectionLease();

    // 从管理连接 KILL 它。
    MySQL admin;
    ASSERT_TRUE(admin.connect("127.0.0.1", "root", MySqlTestFixture::password(), "", 3306));
    std::string killSql = "KILL " + connId;
    EXPECT_TRUE(admin.update(killSql));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 再 acquire：ping 失败 → 替换 → 新连接必须可用。
    ConnectionPool::AcquireResult r2 = pool.acquire(1000);
    ASSERT_TRUE(r2.lease) << "broken connection must be replaced, not handed out";
    EXPECT_EQ(0, mysql_query(r2.lease.get()->getConnection(), "SELECT 1"));
    MYSQL_RES* res2 = mysql_store_result(r2.lease.get()->getConnection());
    ASSERT_TRUE(res2 != nullptr);
    mysql_free_result(res2);
}
