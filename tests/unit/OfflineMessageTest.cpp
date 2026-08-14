#include "MySqlTestFixture.hpp"

#include "app/InMemoryGroupRepository.hpp"
#include "app/InMemoryUserRepository.hpp"
#include "app/MySQLGroupRepository.hpp"
#include "app/MySQLUserRepository.hpp"

#include <gtest/gtest.h>

// P3-10 cutover：旧 OfflineMessage 存取路径（storeOffline/takeOffline）已退役，
// 登录补投只走 P3-07 ledger claim；本文件保留群成员查询契约测试。
TEST(OfflineMessageTest, GroupMembersContract)
{
    MySqlTestFixture::resetSchema();
    MySQLUserRepository users(MySqlTestFixture::pool());
    CreateUserResult owner = users.create("owner", "pw");
    CreateUserResult m1 = users.create("m1", "pw");
    CreateUserResult m2 = users.create("m2", "pw");
    ASSERT_TRUE(owner.ok);
    ASSERT_TRUE(m1.ok);
    ASSERT_TRUE(m2.ok);

    MySQLGroupRepository groups(MySqlTestFixture::pool());
    CreateGroupResult g = groups.create(owner.id, "g", "d");
    ASSERT_TRUE(g.ok);
    ASSERT_TRUE(groups.join(g.groupId, m1.id).ok);
    ASSERT_TRUE(groups.join(g.groupId, m2.id).ok);

    MembersResult all = groups.members(g.groupId);
    ASSERT_TRUE(all.ok);
    ASSERT_EQ(3u, all.userIds.size());

    MembersResult missing = groups.members(999999);
    EXPECT_TRUE(missing.ok);
    EXPECT_TRUE(missing.userIds.empty());
}

TEST(OfflineMessageTest, GroupMembersInMemoryMatchesMySqlShape)
{
    InMemoryUserRepository users;
    InMemoryGroupRepository groups(users);
    CreateUserResult owner = users.create("owner", "pw");
    ASSERT_TRUE(owner.ok);
    CreateGroupResult g = groups.create(owner.id, "g", "d");
    ASSERT_TRUE(g.ok);
    MembersResult all = groups.members(g.groupId);
    ASSERT_TRUE(all.ok);
    ASSERT_EQ(1u, all.userIds.size());
    EXPECT_EQ(owner.id, all.userIds[0]);
}
