#include "MySqlTestFixture.hpp"

#include "app/InMemoryGroupRepository.hpp"
#include "app/InMemoryMessageRepository.hpp"
#include "app/InMemoryUserRepository.hpp"
#include "app/MySQLGroupRepository.hpp"
#include "app/MySQLMessageRepository.hpp"
#include "app/MySQLUserRepository.hpp"

#include <gtest/gtest.h>

// 离线消息集成：真实 MySQL 存取 + 群成员查询契约。
TEST(OfflineMessageTest, MySqlStoreTakeRoundTripPreservesPayload)
{
    MySqlTestFixture::resetSchema();
    MySQLUserRepository users(MySqlTestFixture::pool());
    CreateUserResult u = users.create("alice", "pw");
    ASSERT_TRUE(u.ok);
    MySQLMessageRepository messages(MySqlTestFixture::pool());
    ASSERT_TRUE(messages.storeOffline(u.id, "{\"msgid\":6,\"msg\":\"hello\"}").ok);
    ASSERT_TRUE(messages.storeOffline(u.id, "{\"msgid\":10,\"groupid\":1}").ok);

    std::vector<OfflineMessage> taken = messages.takeOffline(u.id);
    ASSERT_EQ(2u, taken.size());
    EXPECT_EQ("{\"msgid\":6,\"msg\":\"hello\"}", taken[0].payload);
    EXPECT_EQ("{\"msgid\":10,\"groupid\":1}", taken[1].payload);
    EXPECT_TRUE(messages.takeOffline(u.id).empty());
}

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
