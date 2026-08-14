#include "app/ChatApplication.hpp"
#include "app/InMemoryFriendRepository.hpp"
#include "app/InMemoryGroupRepository.hpp"
#include "app/InMemoryUserRepository.hpp"

#include <gtest/gtest.h>

namespace {

struct AppHarness
{
    InMemoryUserRepository users;
    InMemoryFriendRepository friends;
    InMemoryGroupRepository groups;
    ChatApplication app;

    AppHarness()
        : friends(users), groups(users),
          app(&users, &friends, &groups)
    {
    }
};

} // namespace

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
