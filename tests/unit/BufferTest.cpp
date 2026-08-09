#include "Buffer.h"

#include <gtest/gtest.h>

#include <string>

TEST(BufferTest, InitialState)
{
    const size_t initialSize = Buffer::kInitialSize;
    const size_t cheapPrepend = Buffer::kCheapPrepend;
    Buffer buf;
    EXPECT_EQ(0u, buf.readableBytes());
    EXPECT_EQ(initialSize, buf.writableBytes());
    EXPECT_EQ(cheapPrepend, buf.prependableBytes());
}

TEST(BufferTest, AppendAndRetrieveAll)
{
    const size_t initialSize = Buffer::kInitialSize;
    Buffer buf;
    buf.append("hello", 5);
    EXPECT_EQ(5u, buf.readableBytes());
    EXPECT_EQ("hello", buf.retrieveAllAsString());
    EXPECT_EQ(0u, buf.readableBytes());
    EXPECT_EQ(initialSize, buf.writableBytes());
}

TEST(BufferTest, PartialRetrieve)
{
    Buffer buf;
    buf.append("hello world", 11);
    EXPECT_EQ("hello", buf.retrieveAsString(5));
    EXPECT_EQ(6u, buf.readableBytes());
    EXPECT_EQ(" world", buf.retrieveAllAsString());
    EXPECT_EQ(0u, buf.readableBytes());
}

TEST(BufferTest, GrowBeyondInitialSize)
{
    const size_t initialSize = Buffer::kInitialSize;
    Buffer buf;
    std::string payload(initialSize * 4, 'x');
    buf.append(payload.data(), payload.size());
    EXPECT_EQ(payload.size(), buf.readableBytes());
    EXPECT_EQ(payload, buf.retrieveAllAsString());
}

TEST(BufferTest, RetrieveFromEmpty)
{
    Buffer buf;
    EXPECT_EQ("", buf.retrieveAllAsString());
    EXPECT_EQ(0u, buf.readableBytes());
    EXPECT_EQ("", buf.retrieveAsString(0));
}

TEST(BufferTest, ReuseAfterRetrieveAll)
{
    Buffer buf;
    buf.append("abc", 3);
    buf.retrieveAllAsString();
    buf.append("def", 3);
    EXPECT_EQ(3u, buf.readableBytes());
    EXPECT_EQ("def", buf.retrieveAllAsString());
}
