// 非构建目标（P5-04 进入条件失效收口；保留为候选规格）。
// P5-04 RED：Buffer 内存池化契约（flag = CMake option ENABLE_BUFFER_POOL，编译期
// 注入 MUDUO_ENABLE_BUFFER_POOL；观察面 = 尚不存在的 Buffer::BufferPoolStats 与
// Buffer::poolStats() → 当前编译失败即合法 RED，沿 P3-12/P4-02/P5-03A 先例）。
// 同一测试在 flag OFF / ON 双构建下都必须编译并通过：
//   OFF：行为与主线 std::vector<char> 存储完全等价，池化观察面报 enabled=false 零计数；
//   ON ：公开语义（append/retrieve/peek/retrieveAll/readableBytes/readFd）不变，
//        且重复分配释放后池化复用可观察（hits 增长）。
#include "Buffer.h"

#include <gtest/gtest.h>

#include <errno.h>
#include <unistd.h>

#include <string>

namespace {

const size_t kCheapPrepend = Buffer::kCheapPrepend;
const size_t kInitialSize = Buffer::kInitialSize;

}  // namespace

// flag OFF 时与现 baseline 完全等价（既有 BufferTest 11 用例语义不回退）。
TEST(BufferPoolContractTest, PoolOffIdenticalToBaseline)
{
    Buffer buf;
    buf.append("hello", 5);
    EXPECT_EQ(5u, buf.readableBytes());
    EXPECT_EQ("hello", buf.retrieveAllAsString());
    EXPECT_EQ(0u, buf.readableBytes());
    EXPECT_EQ(kInitialSize, buf.writableBytes());
    EXPECT_EQ(kCheapPrepend, buf.prependableBytes());
#if !defined(MUDUO_ENABLE_BUFFER_POOL)
    const Buffer::BufferPoolStats stats = Buffer::poolStats();
    EXPECT_FALSE(stats.enabled);
    EXPECT_EQ(0ull, stats.acquires);
    EXPECT_EQ(0ull, stats.hits);
    EXPECT_EQ(0ull, stats.releases);
#endif
}

// 公开语义不变（两构建同跑）：边界读写、partial retrieve、retrieveAll 复位、
// 超初始容量增长、空缓冲、readFd 内部区路径。
TEST(BufferPoolContractTest, PoolOnPreservesSemantics)
{
    Buffer buf;
    EXPECT_EQ(0u, buf.readableBytes());
    EXPECT_EQ("", buf.retrieveAllAsString());
    EXPECT_EQ("", buf.retrieveAsString(0));

    std::string payload(kInitialSize * 4, 'x');
    buf.append(payload.data(), payload.size());
    EXPECT_EQ(payload.size(), buf.readableBytes());
    EXPECT_EQ(payload, std::string(buf.peek(), payload.size()));
    EXPECT_EQ(payload.substr(0, 7), buf.retrieveAsString(7));
    EXPECT_EQ(payload.size() - 7, buf.readableBytes());
    EXPECT_EQ(payload.substr(7), buf.retrieveAllAsString());
    EXPECT_EQ(0u, buf.readableBytes());
    EXPECT_EQ(kInitialSize, buf.writableBytes());
    EXPECT_EQ(kCheapPrepend, buf.prependableBytes());

    buf.append("abc", 3);
    buf.retrieveAll();
    buf.append("def", 3);
    EXPECT_EQ(3u, buf.readableBytes());
    EXPECT_EQ("def", buf.retrieveAllAsString());

    buf.ensureWritableBytes(kInitialSize * 2);
    EXPECT_LE(kInitialSize * 2, buf.writableBytes());

    int fds[2];
    ASSERT_EQ(0, pipe(fds));
    const char fdPayload[] = "hello";
    ASSERT_EQ(5, static_cast<ssize_t>(write(fds[1], fdPayload, 5)));
    int savedErrno = 0;
    EXPECT_EQ(5, buf.readFd(fds[0], &savedErrno));
    EXPECT_EQ(5u, buf.readableBytes());
    EXPECT_EQ(kCheapPrepend, buf.prependableBytes());
    EXPECT_EQ("hello", buf.retrieveAllAsString());
    close(fds[0]);
    close(fds[1]);

#if defined(MUDUO_ENABLE_BUFFER_POOL)
    EXPECT_TRUE(Buffer::poolStats().enabled);
#endif
}

// flag ON 时池化复用可观察：重复构造/析构后 free-list 命中计数增长；
// flag OFF 时不得虚报池化（enabled=false 且命中/归还恒零）。
TEST(BufferPoolContractTest, PoolReuseObservation)
{
#if defined(MUDUO_ENABLE_BUFFER_POOL)
    const std::string payload(kInitialSize * 4, 'y');
    const int kCycles = 32;
    for (int i = 0; i < kCycles; ++i)
    {
        Buffer buf;
        buf.append(payload.data(), payload.size());
        EXPECT_EQ(payload.size(), buf.readableBytes());
        EXPECT_EQ(payload, std::string(buf.peek(), payload.size()));
    }
    const Buffer::BufferPoolStats warm = Buffer::poolStats();
    EXPECT_TRUE(warm.enabled);
    for (int i = 0; i < kCycles; ++i)
    {
        Buffer buf;
        buf.append(payload.data(), payload.size());
        EXPECT_EQ(payload.size(), buf.readableBytes());
    }
    const Buffer::BufferPoolStats reused = Buffer::poolStats();
    EXPECT_GT(reused.hits, warm.hits);
#else
    const Buffer::BufferPoolStats stats = Buffer::poolStats();
    EXPECT_FALSE(stats.enabled);
    EXPECT_EQ(0ull, stats.hits);
    EXPECT_EQ(0ull, stats.releases);
#endif
}
