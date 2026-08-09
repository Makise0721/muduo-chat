#include "Buffer.h"
#include "LegacyJsonLineCodec.h"

#include <gtest/gtest.h>

#include <string>

TEST(LegacyJsonLineCodecTest, SingleLine)
{
    LegacyJsonLineCodec codec;
    Buffer buf;
    buf.append("{\"msgid\":1}\n", 12);
    std::string msg;
    EXPECT_TRUE(codec.decode(&buf, &msg));
    EXPECT_EQ("{\"msgid\":1}", msg);
    EXPECT_EQ(0u, buf.readableBytes());
}

TEST(LegacyJsonLineCodecTest, MultipleLinesConsumedOneByOne)
{
    LegacyJsonLineCodec codec;
    Buffer buf;
    buf.append("{\"a\":1}\n{\"b\":2}\n", 16);
    std::string msg;
    EXPECT_TRUE(codec.decode(&buf, &msg));
    EXPECT_EQ("{\"a\":1}", msg);
    EXPECT_TRUE(codec.decode(&buf, &msg));
    EXPECT_EQ("{\"b\":2}", msg);
    EXPECT_EQ(0u, buf.readableBytes());
    EXPECT_FALSE(codec.decode(&buf, &msg));
}

TEST(LegacyJsonLineCodecTest, IncompleteLineWaitsWithoutConsuming)
{
    LegacyJsonLineCodec codec;
    Buffer buf;
    buf.append("{\"msgid\":1}", 11);
    std::string msg;
    EXPECT_FALSE(codec.decode(&buf, &msg));
    EXPECT_EQ(11u, buf.readableBytes());
    buf.append("\n", 1);
    EXPECT_TRUE(codec.decode(&buf, &msg));
    EXPECT_EQ("{\"msgid\":1}", msg);
    EXPECT_EQ(0u, buf.readableBytes());
}

TEST(LegacyJsonLineCodecTest, CarriageReturnSuffixStripped)
{
    LegacyJsonLineCodec codec;
    Buffer buf;
    buf.append("{\"msgid\":1}\r\n", 13);
    std::string msg;
    EXPECT_TRUE(codec.decode(&buf, &msg));
    EXPECT_EQ("{\"msgid\":1}", msg);
}

TEST(LegacyJsonLineCodecTest, EmptyLineSkipped)
{
    LegacyJsonLineCodec codec;
    Buffer buf;
    buf.append("\n{\"a\":1}\n", 9);
    std::string msg;
    EXPECT_TRUE(codec.decode(&buf, &msg));
    EXPECT_EQ("{\"a\":1}", msg);
}

TEST(LegacyJsonLineCodecTest, EncodeAppendsNewline)
{
    LegacyJsonLineCodec codec;
    Buffer out;
    codec.encode("{\"msgid\":1}", &out);
    EXPECT_EQ("{\"msgid\":1}\n", out.retrieveAllAsString());
}
