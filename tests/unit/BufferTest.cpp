#include "Buffer.h"

#include <gtest/gtest.h>

#include <errno.h>
#include <unistd.h>

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

TEST(BufferTest, CompactMovesDataToPrependArea)
{
    const size_t cheapPrepend = Buffer::kCheapPrepend;
    const size_t initialSize = Buffer::kInitialSize;
    Buffer buf;
    std::string payload(initialSize - cheapPrepend, 'a');
    buf.append(payload.data(), payload.size());
    buf.retrieveAsString(payload.size() - 10);
    const size_t leftover = 10;
    const size_t appended = 10;
    buf.append("bcdefghijk", appended);
    EXPECT_EQ(cheapPrepend, buf.prependableBytes());
    EXPECT_EQ(leftover + appended, buf.readableBytes());
    std::string expect(leftover, 'a');
    expect += "bcdefghijk";
    EXPECT_EQ(expect, buf.retrieveAllAsString());
}

TEST(BufferTest, ReadFdSmallWriteUsesInternalArea)
{
    const size_t cheapPrepend = Buffer::kCheapPrepend;
    int fds[2];
    ASSERT_EQ(0, pipe(fds));
    Buffer buf;
    const char payload[] = "hello";
    ASSERT_EQ(5, write(fds[1], payload, 5));
    int savedErrno = 0;
    ssize_t n = buf.readFd(fds[0], &savedErrno);
    EXPECT_EQ(5, n);
    EXPECT_EQ(5u, buf.readableBytes());
    EXPECT_EQ(cheapPrepend, buf.prependableBytes());
    EXPECT_EQ("hello", buf.retrieveAllAsString());
    close(fds[0]);
    close(fds[1]);
}

TEST(BufferTest, ReadFdLargeWriteUsesExtraBuffer)
{
    const size_t initialSize = Buffer::kInitialSize;
    Buffer buf;
    std::string payload(initialSize * 3 + 1234, 'x');
    int fds[2];
    ASSERT_EQ(0, pipe(fds));
    size_t written = 0;
    while (written < payload.size())
    {
        ssize_t w = write(fds[1], payload.data() + written, payload.size() - written);
        ASSERT_GT(w, 0);
        written += static_cast<size_t>(w);
    }
    close(fds[1]);
    std::string total;
    int savedErrno = 0;
    for (;;)
    {
        ssize_t n = buf.readFd(fds[0], &savedErrno);
        if (n <= 0)
        {
            break;
        }
        total += buf.retrieveAllAsString();
    }
    EXPECT_EQ(payload.size(), total.size());
    EXPECT_EQ(payload, total);
    close(fds[0]);
}

TEST(BufferTest, ReadFdEofReturnsZero)
{
    int fds[2];
    ASSERT_EQ(0, pipe(fds));
    close(fds[1]);
    Buffer buf;
    int savedErrno = 0;
    EXPECT_EQ(0, buf.readFd(fds[0], &savedErrno));
    EXPECT_EQ(0u, buf.readableBytes());
    close(fds[0]);
}

TEST(BufferTest, ReadFdErrorSetsSavedErrno)
{
    int fds[2];
    ASSERT_EQ(0, pipe(fds));
    close(fds[0]);
    close(fds[1]);
    Buffer buf;
    int savedErrno = 0;
    EXPECT_EQ(-1, buf.readFd(fds[0], &savedErrno));
    EXPECT_EQ(EBADF, savedErrno);
}
