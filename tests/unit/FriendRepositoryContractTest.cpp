#include "MySqlTestFixture.hpp"

#include "app/InMemoryFriendRepository.hpp"
#include "app/InMemoryUserRepository.hpp"
#include "app/MySQLFriendRepository.hpp"
#include "app/MySQLUserRepository.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

// 好友契约：成功 / 重复 / 目标不存在 / 反向不自动成立，两个 adapter 必须一致。
// a、b 为数据库里真实存在的用户 id（InMemory 或 MySQL）。
void runFriendRepositoryContract(FriendRepository& friends, int64_t aId, int64_t bId)
{
    AddFriendResult r1 = friends.add(aId, bId);
    EXPECT_TRUE(r1.ok) << "first add must succeed";

    AddFriendResult dup = friends.add(aId, bId);
    EXPECT_FALSE(dup.ok);
    EXPECT_EQ(FriendError::Duplicate, dup.error);

    // 反向不自动成立（有向边）
    EXPECT_TRUE(friends.add(bId, aId).ok);

    AddFriendResult missing = friends.add(aId, 999999);
    EXPECT_FALSE(missing.ok);
    EXPECT_EQ(FriendError::TargetNotFound, missing.error);
}

TEST(FriendRepositoryContractTest, InMemoryAdapterSatisfiesContract)
{
    InMemoryUserRepository users;
    CreateUserResult a = users.create("alice", "pw");
    CreateUserResult b = users.create("bob", "pw");
    ASSERT_TRUE(a.ok);
    ASSERT_TRUE(b.ok);
    InMemoryFriendRepository friends(users);
    runFriendRepositoryContract(friends, a.id, b.id);
}

TEST(FriendRepositoryContractTest, MySqlAdapterSatisfiesContract)
{
    MySqlTestFixture::resetSchema();
    MySQLUserRepository users(MySqlTestFixture::pool());
    CreateUserResult a = users.create("alice", "pw");
    CreateUserResult b = users.create("bob", "pw");
    ASSERT_TRUE(a.ok);
    ASSERT_TRUE(b.ok);
    MySQLFriendRepository friends(MySqlTestFixture::pool());
    runFriendRepositoryContract(friends, a.id, b.id);
}

TEST(FriendRepositoryContractTest, MySqlConcurrentAddsYieldsExactlyOneWinner)
{
    MySqlTestFixture::resetSchema();
    MySQLUserRepository users(MySqlTestFixture::pool());
    CreateUserResult a = users.create("alice", "pw");
    CreateUserResult b = users.create("bob", "pw");
    ASSERT_TRUE(a.ok);
    ASSERT_TRUE(b.ok);
    MySQLFriendRepository friends(MySqlTestFixture::pool());

    std::atomic<int> wins{0};
    std::atomic<int> conflicts{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([&friends, &wins, &conflicts, aId = a.id, bId = b.id] {
            AddFriendResult r = friends.add(aId, bId);
            if (r.ok) {
                wins.fetch_add(1);
            } else if (r.error == FriendError::Duplicate) {
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
