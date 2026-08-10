#include "app/ChatApplication.hpp"
#include "app/InMemoryFriendRepository.hpp"
#include "app/InMemoryGroupRepository.hpp"
#include "app/InMemoryMessageRepository.hpp"
#include "app/InMemoryUserRepository.hpp"

#include <gtest/gtest.h>

namespace {

struct AppHarness
{
    InMemoryUserRepository users;
    InMemoryFriendRepository friends;
    InMemoryGroupRepository groups;
    InMemoryMessageRepository messages;
    ChatApplication app;

    AppHarness()
        : friends(users), groups(users),
          app(&users, &friends, &groups, &messages)
    {
    }
};

} // namespace

TEST(DirectMessageApplicationTest, StoreOfflineForOfflineTarget)
{
    AppHarness h;
    StoreResult r = h.app.storeOfflineMessage(99, "{\"msgid\":6}");
    EXPECT_TRUE(r.ok);
    std::vector<OfflineMessage> taken = h.app.takeOfflineMessages(99);
    ASSERT_EQ(1u, taken.size());
    EXPECT_EQ("{\"msgid\":6}", taken[0].payload);
}

TEST(DirectMessageApplicationTest, TakeOfflineIsIdempotentAfterDelivery)
{
    AppHarness h;
    ASSERT_TRUE(h.app.storeOfflineMessage(99, "a").ok);
    ASSERT_EQ(1u, h.app.takeOfflineMessages(99).size());
    EXPECT_TRUE(h.app.takeOfflineMessages(99).empty());
}

TEST(GroupMessageApplicationTest, GroupMembersIncludesAllMembers)
{
    AppHarness h;
    CreateUserResult owner = h.users.create("owner", "pw");
    CreateUserResult m = h.users.create("m", "pw");
    ASSERT_TRUE(owner.ok);
    ASSERT_TRUE(m.ok);
    CreateGroupResult g = h.app.createGroup(owner.id, "g", "d");
    ASSERT_TRUE(g.ok);
    ASSERT_TRUE(h.app.joinGroup(g.groupId, m.id).ok);

    MembersResult members = h.app.groupMembers(g.groupId);
    ASSERT_TRUE(members.ok);
    ASSERT_EQ(2u, members.userIds.size());
}

TEST(GroupMessageApplicationTest, StoreOfflineForOfflineGroupMembers)
{
    AppHarness h;
    CreateUserResult m1 = h.users.create("m1", "pw");
    CreateUserResult m2 = h.users.create("m2", "pw");
    ASSERT_TRUE(m1.ok);
    ASSERT_TRUE(m2.ok);
    CreateGroupResult g = h.app.createGroup(m1.id, "g", "d");
    ASSERT_TRUE(g.ok);
    ASSERT_TRUE(h.app.joinGroup(g.groupId, m2.id).ok);

    // 群聊离线存储：为每个离线成员入队（m1 是发送者不存）。
    ASSERT_TRUE(h.app.storeOfflineMessage(m2.id, "{\"msgid\":10}").ok);
    ASSERT_EQ(1u, h.app.takeOfflineMessages(m2.id).size());
    EXPECT_TRUE(h.app.takeOfflineMessages(m1.id).empty());
}
