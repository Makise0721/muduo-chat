#include "MySqlTestFixture.hpp"

#include "app/MySQLUserRepository.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

TEST(MySQLUserRepositoryTest, ErrorMappingClassifiesStableErrors)
{
    EXPECT_EQ(UserError::NameExists, mapMySqlError(1062));
    EXPECT_EQ(UserError::Disconnected, mapMySqlError(2006));
    EXPECT_EQ(UserError::Disconnected, mapMySqlError(2016));
    EXPECT_EQ(UserError::Disconnected, mapMySqlError(1053));
    EXPECT_EQ(UserError::Disconnected, mapMySqlError(2003));
    EXPECT_EQ(UserError::Timeout, mapMySqlError(1205));
    EXPECT_EQ(UserError::StorageFailure, mapMySqlError(1406));
    EXPECT_EQ(UserError::StorageFailure, mapMySqlError(1366));
    EXPECT_EQ(UserError::StorageFailure, mapMySqlError(1300));
    EXPECT_EQ(UserError::StorageFailure, mapMySqlError(9999));
}

TEST(MySQLUserRepositoryTest, EmojiRejectedByUtf8Schema)
{
    MySqlTestFixture::resetSchema();
    MySQLUserRepository users(MySqlTestFixture::pool());
    CreateUserResult r = users.create("user\U0001F600", "pw");
    // 当前 User 表为 utf8（3 字节/字符），4 字节 emoji 被拒；
    // schema 升级 utf8mb4 后本用例应改为 EXPECT_TRUE(r.ok)。
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(UserError::StorageFailure, r.error);
}

TEST(MySQLUserRepositoryTest, ConcurrentUniqueKeyRaceYieldsExactlyOneWinner)
{
    MySqlTestFixture::resetSchema();
    MySQLUserRepository users(MySqlTestFixture::pool());
    const std::string name = "race_user";
    std::atomic<int> wins{0};
    std::atomic<int> conflicts{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([&users, &wins, &conflicts, &name] {
            CreateUserResult r = users.create(name, "pw");
            if (r.ok) {
                wins.fetch_add(1);
            } else if (r.error == UserError::NameExists) {
                conflicts.fetch_add(1);
            }
        });
    }
    for (std::thread& t : threads) {
        t.join();
    }
    EXPECT_EQ(1, wins.load());
    EXPECT_EQ(7, conflicts.load());
}
