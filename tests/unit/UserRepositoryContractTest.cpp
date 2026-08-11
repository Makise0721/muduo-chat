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

    // password 超长（>50，User.password VARCHAR(50) 上限）：name 合法 + password
    // 超长组合必须拒绝为 InvalidInput，双 adapter 一致（R1 在 isRepositoryInputValid
    // 补 password 长度校验前，InMemory 接受、MySQL 返回 StorageFailure，跑红属预期）。
    CreateUserResult longPwd = users.create("longpw_user", std::string(51, 'p'));
    EXPECT_FALSE(longPwd.ok);
    EXPECT_EQ(UserError::InvalidInput, longPwd.error);

    // 边界：50 字符 password 仍合法（校验必须是 >50 而非 >=50）。
    CreateUserResult edgePwd = users.create("edgepw_user", std::string(50, 'p'));
    EXPECT_TRUE(edgePwd.ok);

    // 认证与状态契约
    AuthResult bad = users.authenticate(firstId, "WRONG");
    EXPECT_FALSE(bad.ok);
    AuthResult auth = users.authenticate(firstId, "pw");
    EXPECT_TRUE(auth.ok);
    EXPECT_EQ(firstId, auth.id);
    EXPECT_EQ("alice", auth.name);
    EXPECT_EQ(UserState::Offline, auth.state);

    EXPECT_TRUE(users.updateState(firstId, UserState::Online));
    AuthResult auth2 = users.authenticate(firstId, "pw");
    EXPECT_TRUE(auth2.ok);
    EXPECT_EQ(UserState::Online, auth2.state);
    EXPECT_TRUE(users.updateState(firstId, UserState::Offline));
    AuthResult auth3 = users.authenticate(firstId, "pw");
    EXPECT_TRUE(auth3.ok);
    EXPECT_EQ(UserState::Offline, auth3.state);
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
