#include "MySqlTestFixture.hpp"

#include "app/InMemoryUserRepository.hpp"
#include "app/MySQLUserRepository.hpp"
#include "app/UserRepository.hpp"

#include <gtest/gtest.h>

#include <string>

// 两个 adapter 必须共同满足的契约：成功/重名/引号/NUL/超长/多字节名。
void runCommonUserRepositoryContract(UserRepository& users)
{
    CreateUserResult r1 = users.create("alice", "pw");
    EXPECT_TRUE(r1.ok);
    EXPECT_GT(r1.id, 0);
    const int64_t firstId = r1.id;

    CreateUserResult r2 = users.create("bob", "pw");
    EXPECT_TRUE(r2.ok);
    EXPECT_GT(r2.id, firstId);

    CreateUserResult dup = users.create("alice", "other");
    EXPECT_FALSE(dup.ok);
    EXPECT_EQ(UserError::NameExists, dup.error);

    CreateUserResult quote = users.create("O'Brien", "p'w");
    EXPECT_TRUE(quote.ok) << "single-quoted name must survive without injection";

    CreateUserResult dquote = users.create("say \"hi\"", "pw");
    EXPECT_TRUE(dquote.ok) << "double-quoted name must survive";

    CreateUserResult utf8 = users.create("中文名", "pw");
    EXPECT_TRUE(utf8.ok) << "multibyte name must survive";

    CreateUserResult nul = users.create(std::string("a\0b", 3), "pw");
    EXPECT_FALSE(nul.ok);
    EXPECT_EQ(UserError::InvalidInput, nul.error) << "NUL must be rejected defensively";

    CreateUserResult tooLong = users.create(std::string(51, 'n'), "pw");
    EXPECT_FALSE(tooLong.ok);
    EXPECT_EQ(UserError::InvalidInput, tooLong.error);
}

TEST(UserRepositoryContractTest, InMemoryAdapterSatisfiesContract)
{
    InMemoryUserRepository users;
    runCommonUserRepositoryContract(users);
}

TEST(UserRepositoryContractTest, MySqlAdapterSatisfiesContract)
{
    MySqlTestFixture::resetSchema();
    MySQLUserRepository users(MySqlTestFixture::pool());
    runCommonUserRepositoryContract(users);
}
