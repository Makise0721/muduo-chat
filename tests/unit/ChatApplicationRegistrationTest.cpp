#include "app/ChatApplication.hpp"
#include "app/InMemoryFriendRepository.hpp"
#include "app/InMemoryGroupRepository.hpp"
#include "app/InMemoryUserRepository.hpp"
#include "app/UserRepository.hpp"

#include <gtest/gtest.h>

#include <string>

namespace {

Command registerCommand(const std::string& name, const std::string& password)
{
    Command cmd;
    cmd.type = Command::Type::Register;
    cmd.name = name;
    cmd.password = password;
    cmd.raw = "{\"msgid\":4}";
    return cmd;
}

TEST(ChatApplicationRegistrationTest, RegistersUserWithFreshId)
{
    InMemoryUserRepository users;
    InMemoryFriendRepository friends(users);
    InMemoryGroupRepository groups(users);
    ChatApplication app(&users, &friends, &groups);
    SessionContext ctx;
    Reply reply;
    app.handle(ctx, registerCommand("alice", "pw"), &reply);
    EXPECT_EQ(0, reply.errnoCode);
    const int64_t firstId = reply.userId;
    EXPECT_GT(firstId, 0);

    app.handle(ctx, registerCommand("bob", "pw"), &reply);
    EXPECT_EQ(0, reply.errnoCode);
    EXPECT_GT(reply.userId, firstId);
}

TEST(ChatApplicationRegistrationTest, DuplicateNameRejected)
{
    InMemoryUserRepository users;
    InMemoryFriendRepository friends(users);
    InMemoryGroupRepository groups(users);
    ChatApplication app(&users, &friends, &groups);
    SessionContext ctx;
    Reply reply;
    app.handle(ctx, registerCommand("alice", "pw"), &reply);
    ASSERT_EQ(0, reply.errnoCode);

    app.handle(ctx, registerCommand("alice", "other"), &reply);
    EXPECT_EQ(1, reply.errnoCode);
    EXPECT_EQ("this name is already exist!", reply.errmsg);
}

TEST(ChatApplicationRegistrationTest, EmptyNameRejected)
{
    InMemoryUserRepository users;
    InMemoryFriendRepository friends(users);
    InMemoryGroupRepository groups(users);
    ChatApplication app(&users, &friends, &groups);
    SessionContext ctx;
    Reply reply;
    app.handle(ctx, registerCommand("", "pw"), &reply);
    EXPECT_EQ(1, reply.errnoCode);
}

TEST(ChatApplicationRegistrationTest, EmptyPasswordRejected)
{
    InMemoryUserRepository users;
    InMemoryFriendRepository friends(users);
    InMemoryGroupRepository groups(users);
    ChatApplication app(&users, &friends, &groups);
    SessionContext ctx;
    Reply reply;
    app.handle(ctx, registerCommand("alice", ""), &reply);
    EXPECT_EQ(1, reply.errnoCode);
}

TEST(ChatApplicationRegistrationTest, OversizedNameRejected)
{
    InMemoryUserRepository users;
    InMemoryFriendRepository friends(users);
    InMemoryGroupRepository groups(users);
    ChatApplication app(&users, &friends, &groups);
    SessionContext ctx;
    Reply reply;
    app.handle(ctx, registerCommand(std::string(51, 'n'), "pw"), &reply);
    EXPECT_EQ(1, reply.errnoCode);
}

class FailingUserRepository : public UserRepository
{
public:
    CreateUserResult create(const std::string&, const std::string&) override
    {
        CreateUserResult r;
        r.ok = false;
        r.error = UserError::StorageFailure;
        return r;
    }
    AuthResult authenticate(int64_t, const std::string&) override
    {
        return AuthResult();
    }
    bool updateState(int64_t, UserState) override
    {
        return false;
    }
};

TEST(ChatApplicationRegistrationTest, RepositoryFailureMapsToRegisterFailed)
{
    FailingUserRepository users;
    InMemoryUserRepository dummyUsers;
    InMemoryFriendRepository dummyFriends(dummyUsers);
    InMemoryGroupRepository dummyGroups(dummyUsers);
    ChatApplication app(&users, &dummyFriends, &dummyGroups);
    SessionContext ctx;
    Reply reply;
    app.handle(ctx, registerCommand("alice", "pw"), &reply);
    EXPECT_EQ(1, reply.errnoCode);
    EXPECT_EQ("register failed!", reply.errmsg);
}

TEST(ChatApplicationRegistrationTest, UnsupportedCommandRejected)
{
    InMemoryUserRepository users;
    InMemoryFriendRepository friends(users);
    InMemoryGroupRepository groups(users);
    ChatApplication app(&users, &friends, &groups);
    SessionContext ctx;
    Command cmd;
    cmd.type = Command::Type::Login;
    Reply reply;
    app.handle(ctx, cmd, &reply);
    EXPECT_EQ(1, reply.errnoCode);
}

} // namespace
