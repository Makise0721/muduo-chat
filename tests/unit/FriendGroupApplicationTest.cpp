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

TEST(FriendApplicationTest, AddFriendPropagatesSuccess)
{
    AppHarness h;
    CreateUserResult a = h.users.create("alice", "pw");
    CreateUserResult b = h.users.create("bob", "pw");
    ASSERT_TRUE(a.ok);
    ASSERT_TRUE(b.ok);

    AddFriendResult r = h.app.addFriend(a.id, b.id);
    EXPECT_TRUE(r.ok);
}

TEST(FriendApplicationTest, AddFriendPropagatesDuplicate)
{
    AppHarness h;
    CreateUserResult a = h.users.create("alice", "pw");
    CreateUserResult b = h.users.create("bob", "pw");
    ASSERT_TRUE(a.ok);
    ASSERT_TRUE(b.ok);
    ASSERT_TRUE(h.app.addFriend(a.id, b.id).ok);

    AddFriendResult r = h.app.addFriend(a.id, b.id);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(FriendError::Duplicate, r.error);
}

TEST(FriendApplicationTest, AddFriendPropagatesTargetNotFound)
{
    AppHarness h;
    CreateUserResult a = h.users.create("alice", "pw");
    ASSERT_TRUE(a.ok);

    AddFriendResult r = h.app.addFriend(a.id, 999999);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(FriendError::TargetNotFound, r.error);
}

TEST(GroupApplicationTest, CreateGroupPropagatesGroupId)
{
    AppHarness h;
    CreateUserResult owner = h.users.create("owner", "pw");
    ASSERT_TRUE(owner.ok);

    CreateGroupResult r = h.app.createGroup(owner.id, "g", "d");
    EXPECT_TRUE(r.ok);
    EXPECT_GT(r.groupId, 0);
}

TEST(GroupApplicationTest, CreateGroupPropagatesTargetNotFound)
{
    AppHarness h;
    CreateGroupResult r = h.app.createGroup(999999, "g", "d");
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(GroupError::TargetNotFound, r.error);
}

TEST(GroupApplicationTest, JoinGroupPropagatesDuplicate)
{
    AppHarness h;
    CreateUserResult owner = h.users.create("owner", "pw");
    CreateUserResult member = h.users.create("member", "pw");
    ASSERT_TRUE(owner.ok);
    ASSERT_TRUE(member.ok);
    CreateGroupResult g = h.app.createGroup(owner.id, "g", "d");
    ASSERT_TRUE(g.ok);
    ASSERT_TRUE(h.app.joinGroup(g.groupId, member.id).ok);

    JoinGroupResult r = h.app.joinGroup(g.groupId, member.id);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(GroupError::Duplicate, r.error);
}
