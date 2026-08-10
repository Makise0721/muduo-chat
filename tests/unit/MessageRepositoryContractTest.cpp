#include "MySqlTestFixture.hpp"

#include "app/InMemoryMessageRepository.hpp"
#include "app/MySQLMessageRepository.hpp"
#include "app/MySQLUserRepository.hpp"

#include <gtest/gtest.h>

#include <string>

// 消息契约：存取往返（payload 原样）/多消息顺序/取走清空/重复请求两条。
// userId、otherUserId 为数据库里真实存在的用户 id（InMemory 无 FK 约束可任意）。
void runMessageRepositoryContract(MessageRepository& messages, int64_t userId, int64_t otherUserId)
{
    StoreResult s1 = messages.storeOffline(userId, "{\"msgid\":6,\"msg\":\"first\"}");
    StoreResult s2 = messages.storeOffline(userId, "{\"msgid\":6,\"msg\":\"second\"}");
    EXPECT_TRUE(s1.ok);
    EXPECT_TRUE(s2.ok);

    // 重复请求（同 payload 两次）产生两条记录（去重属 P3）。
    EXPECT_TRUE(messages.storeOffline(userId, "{\"msgid\":6,\"msg\":\"dup\"}").ok);
    EXPECT_TRUE(messages.storeOffline(userId, "{\"msgid\":6,\"msg\":\"dup\"}").ok);

    std::vector<OfflineMessage> taken = messages.takeOffline(userId);
    ASSERT_EQ(4u, taken.size());
    EXPECT_EQ("{\"msgid\":6,\"msg\":\"first\"}", taken[0].payload);
    EXPECT_EQ("{\"msgid\":6,\"msg\":\"second\"}", taken[1].payload);
    EXPECT_EQ("{\"msgid\":6,\"msg\":\"dup\"}", taken[2].payload);
    EXPECT_EQ("{\"msgid\":6,\"msg\":\"dup\"}", taken[3].payload);
    EXPECT_EQ(userId, taken[0].userId);

    // 取走后队列清空（补投后不重投）。
    std::vector<OfflineMessage> again = messages.takeOffline(userId);
    EXPECT_TRUE(again.empty());

    // 不同用户互不干扰。
    StoreResult other = messages.storeOffline(otherUserId, "x");
    EXPECT_TRUE(other.ok);
    EXPECT_TRUE(messages.takeOffline(userId).empty());
    ASSERT_EQ(1u, messages.takeOffline(otherUserId).size());
}

TEST(MessageRepositoryContractTest, InMemoryAdapterSatisfiesContract)
{
    InMemoryMessageRepository messages;
    runMessageRepositoryContract(messages, 42, 43);
}

TEST(MessageRepositoryContractTest, MySqlAdapterSatisfiesContract)
{
    MySqlTestFixture::resetSchema();
    MySQLUserRepository users(MySqlTestFixture::pool());
    CreateUserResult u = users.create("alice", "pw");
    CreateUserResult v = users.create("bob", "pw");
    ASSERT_TRUE(u.ok);
    ASSERT_TRUE(v.ok);
    MySQLMessageRepository messages(MySqlTestFixture::pool());
    runMessageRepositoryContract(messages, u.id, v.id);
}
