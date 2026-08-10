#include "MySqlTestFixture.hpp"

#include "app/InMemoryGroupRepository.hpp"
#include "app/InMemoryUserRepository.hpp"
#include "app/MySQLGroupRepository.hpp"
#include "app/MySQLUserRepository.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

// 群组契约：建群（owner=creator）/owner 不存在/join 成功/join 重复/join 群不存在。
// ownerId、memberId 为数据库里真实存在的用户 id。
void runGroupRepositoryContract(GroupRepository& groups, int64_t ownerId, int64_t memberId)
{
    CreateGroupResult created = groups.create(ownerId, "chat group", "desc");
    EXPECT_TRUE(created.ok);
    EXPECT_GT(created.groupId, 0);
    const int64_t groupId = created.groupId;

    CreateGroupResult missingOwner = groups.create(999999, "orphan", "d");
    EXPECT_FALSE(missingOwner.ok);
    EXPECT_EQ(GroupError::TargetNotFound, missingOwner.error);

    JoinGroupResult joined = groups.join(groupId, memberId);
    EXPECT_TRUE(joined.ok);

    JoinGroupResult dup = groups.join(groupId, memberId);
    EXPECT_FALSE(dup.ok);
    EXPECT_EQ(GroupError::Duplicate, dup.error);

    JoinGroupResult missingGroup = groups.join(999999, memberId);
    EXPECT_FALSE(missingGroup.ok);
    EXPECT_EQ(GroupError::TargetNotFound, missingGroup.error);
}

TEST(GroupRepositoryContractTest, InMemoryAdapterSatisfiesContract)
{
    InMemoryUserRepository users;
    CreateUserResult owner = users.create("owner", "pw");
    CreateUserResult member = users.create("member", "pw");
    ASSERT_TRUE(owner.ok);
    ASSERT_TRUE(member.ok);
    InMemoryGroupRepository groups(users);
    runGroupRepositoryContract(groups, owner.id, member.id);
}

TEST(GroupRepositoryContractTest, MySqlAdapterSatisfiesContract)
{
    MySqlTestFixture::resetSchema();
    MySQLUserRepository users(MySqlTestFixture::pool());
    CreateUserResult owner = users.create("owner", "pw");
    CreateUserResult member = users.create("member", "pw");
    ASSERT_TRUE(owner.ok);
    ASSERT_TRUE(member.ok);
    MySQLGroupRepository groups(MySqlTestFixture::pool());
    runGroupRepositoryContract(groups, owner.id, member.id);
}

// 部分事务失败：owner 不存在 → creator 成员插入失败 → 整组回滚，不留孤儿群。
TEST(GroupRepositoryContractTest, MySqlCreateRollsBackPartialFailure)
{
    MySqlTestFixture::resetSchema();
    MySQLGroupRepository groups(MySqlTestFixture::pool());
    CreateGroupResult r = groups.create(999999, "orphan", "d");
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(GroupError::TargetNotFound, r.error);

    // 群表无残留：孤儿群名不可见（用不存在的 owner 建群，成功后也无意义——
    // 断言 AllGroup 没有该名字）。
    MySQL admin;
    ASSERT_TRUE(admin.connect("127.0.0.1", "root", MySqlTestFixture::password(), "chat_test", 3306));
    EXPECT_EQ(0, mysql_query(admin.getConnection(),
                             "SELECT COUNT(*) FROM AllGroup WHERE groupname = 'orphan'"));
    MYSQL_RES* res = mysql_store_result(admin.getConnection());
    ASSERT_TRUE(res != nullptr);
    MYSQL_ROW row = mysql_fetch_row(res);
    ASSERT_TRUE(row != nullptr);
    EXPECT_STREQ("0", row[0]);
    mysql_free_result(res);
}

TEST(GroupRepositoryContractTest, MySqlConcurrentJoinsYieldExactlyOneWinner)
{
    MySqlTestFixture::resetSchema();
    MySQLUserRepository users(MySqlTestFixture::pool());
    CreateUserResult owner = users.create("owner", "pw");
    ASSERT_TRUE(owner.ok);
    MySQLGroupRepository groups(MySqlTestFixture::pool());
    CreateGroupResult g = groups.create(owner.id, "race group", "d");
    ASSERT_TRUE(g.ok);

    std::atomic<int> wins{0};
    std::atomic<int> conflicts{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([&groups, &wins, &conflicts, gid = g.groupId, uid = owner.id] {
            JoinGroupResult r = groups.join(gid, uid);
            if (r.ok) {
                wins.fetch_add(1);
            } else if (r.error == GroupError::Duplicate) {
                conflicts.fetch_add(1);
            }
        });
    }
    for (std::thread& t : threads) {
        t.join();
    }
    // owner 已在群（creator）：8 个并发 join 全部重复。
    EXPECT_EQ(0, wins.load());
    EXPECT_EQ(8, conflicts.load());
}
