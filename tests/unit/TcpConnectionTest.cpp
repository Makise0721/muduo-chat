#include "EventLoop.h"
#include "TcpConnection.h"

#include <gtest/gtest.h>

#include <sys/socket.h>
#include <sys/types.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <future>
#include <string>
#include <thread>

namespace {

struct ConnHarness
{
    std::promise<void> readyP;
    std::future<void> readyF;
    std::promise<void> endedP;
    std::future<void> endedF;
    EventLoop *loop = nullptr;
    TcpConnectionPtr conn;
    std::thread t;

    ConnHarness(int connFd, const std::function<void(TcpConnection *)> &setup)
        : readyF(readyP.get_future()),
          endedF(endedP.get_future()),
          t([this, connFd, setup]
            {
                EventLoop l;
                loop = &l;
                int flags = fcntl(connFd, F_GETFL, 0);
                fcntl(connFd, F_SETFL, flags | O_NONBLOCK);
                conn.reset(new TcpConnection(&l, "test", connFd, InetAddress(0), InetAddress(0)));
                conn->setConnectionCallback([](const TcpConnectionPtr &) {});
                setup(conn.get());
                conn->connectEstablished();
                readyP.set_value();
                l.loop();
                endedP.set_value();
            })
    {
    }

    bool waitReady()
    {
        return readyF.wait_for(std::chrono::seconds(10)) == std::future_status::ready;
    }

    ~ConnHarness()
    {
        if (loop != nullptr &&
            readyF.wait_for(std::chrono::seconds(0)) == std::future_status::ready &&
            endedF.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        {
            EventLoop *l = loop;
            l->queueInLoop([l] { l->quit(); });
        }
        t.join();
    }
};

} // namespace

TEST(TcpConnectionTest, ConnectionAndCloseCallbacks)
{
    int fds[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds));
    std::atomic<int> connectCalls{0};
    std::promise<void> closed;
    ConnHarness h(fds[1], [&](TcpConnection *conn)
                  {
                      conn->setConnectionCallback(
                          [&connectCalls](const TcpConnectionPtr &) { ++connectCalls; });
                      conn->setCloseCallback(
                          [&closed](const TcpConnectionPtr &) { closed.set_value(); });
                  });
    ASSERT_TRUE(h.waitReady());
    EXPECT_EQ(1, connectCalls.load());
    close(fds[0]);
    EXPECT_EQ(std::future_status::ready,
              closed.get_future().wait_for(std::chrono::seconds(10)));
    EXPECT_EQ(2, connectCalls.load());
}

TEST(TcpConnectionTest, SendFromLoopThreadCompletesDirectly)
{
    int fds[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds));
    std::promise<void> writeComplete;
    ConnHarness h(fds[1], [&](TcpConnection *conn)
                  {
                      conn->setWriteCompleteCallback(
                          [&writeComplete](const TcpConnectionPtr &) { writeComplete.set_value(); });
                  });
    ASSERT_TRUE(h.waitReady());
    h.loop->runInLoop([&] { h.conn->send("hello"); });
    EXPECT_EQ(std::future_status::ready,
              writeComplete.get_future().wait_for(std::chrono::seconds(10)));
    struct pollfd pfd = {fds[0], POLLIN, 0};
    ASSERT_EQ(1, poll(&pfd, 1, 10000));
    char buf[16] = {0};
    ssize_t n = read(fds[0], buf, sizeof buf);
    ASSERT_EQ(5, n);
    EXPECT_EQ("hello", std::string(buf, 5));
}

TEST(TcpConnectionTest, WriteCompleteAfterOutputBufferDrained)
{
    const int kBufSize = 64 * 1024;
    int fds[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds));
    setsockopt(fds[1], SOL_SOCKET, SO_SNDBUF, &kBufSize, sizeof kBufSize);
    setsockopt(fds[0], SOL_SOCKET, SO_RCVBUF, &kBufSize, sizeof kBufSize);
    std::promise<void> highWater;
    std::promise<void> writeComplete;
    ConnHarness h(fds[1], [&](TcpConnection *conn)
                  {
                      conn->setHighWaterMarkCallback(
                          [&highWater](const TcpConnectionPtr &, size_t)
                          { highWater.set_value(); },
                          256 * 1024);
                      conn->setWriteCompleteCallback(
                          [&writeComplete](const TcpConnectionPtr &)
                          { writeComplete.set_value(); });
                  });
    ASSERT_TRUE(h.waitReady());

    const size_t payloadSize = 8 * 1024 * 1024;
    std::string payload(payloadSize, 'x');
    std::string received;
    std::atomic<bool> readFailed{false};
    std::thread reader([&]
                       {
                           received.reserve(payloadSize);
                           while (received.size() < payloadSize)
                           {
                               char chunk[65536];
                               ssize_t n = read(fds[0], chunk, sizeof chunk);
                               if (n <= 0)
                               {
                                   readFailed = true;
                                   return;
                               }
                               received.append(chunk, static_cast<size_t>(n));
                           }
                       });

    h.loop->runInLoop([&] { h.conn->send(payload); });
    EXPECT_EQ(std::future_status::ready,
              highWater.get_future().wait_for(std::chrono::seconds(10)));
    EXPECT_EQ(std::future_status::ready,
              writeComplete.get_future().wait_for(std::chrono::seconds(10)));
    reader.join();
    EXPECT_FALSE(readFailed.load());
    EXPECT_EQ(payloadSize, received.size());
    EXPECT_EQ(payload, received);
}

TEST(TcpConnectionTest, OutputEncoderTransformsSentData)
{
    int fds[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds));
    ConnHarness h(fds[1], [](TcpConnection *conn)
                  {
                      conn->setOutputEncoder([](const std::string &message, Buffer *output)
                                             {
                                                 output->append("ENC[", 4);
                                                 output->append(message.data(), message.size());
                                                 output->append("]", 1);
                                             });
                  });
    ASSERT_TRUE(h.waitReady());
    h.loop->runInLoop([&] { h.conn->send("hello"); });
    struct pollfd pfd = {fds[0], POLLIN, 0};
    ASSERT_EQ(1, poll(&pfd, 1, 10000));
    char buf[64] = {0};
    ssize_t n = read(fds[0], buf, sizeof buf);
    ASSERT_GT(n, 0);
    EXPECT_EQ("ENC[hello]", std::string(buf, static_cast<size_t>(n)));
}

TEST(TcpConnectionTest, CrossThreadSendOwnsPayloadAndConnection)
{
    int fds[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds));
    ConnHarness h(fds[1], [](TcpConnection *) {});
    ASSERT_TRUE(h.waitReady());

    const size_t payloadSize = 8 * 1024;
    std::string received;
    std::atomic<bool> readFailed{false};
    std::thread reader([&]
                       {
                           for (;;)
                           {
                               char chunk[65536];
                               ssize_t n = read(fds[0], chunk, sizeof chunk);
                               if (n == 0)
                               {
                                   break;
                               }
                               if (n < 0)
                               {
                                   readFailed = true;
                                   break;
                               }
                               received.append(chunk, static_cast<size_t>(n));
                           }
                       });

    std::promise<void> entered;
    std::promise<void> release;
    h.loop->queueInLoop([&entered, &release]
                        {
                            entered.set_value();
                            release.get_future().wait();
                        });
    ASSERT_EQ(std::future_status::ready,
              entered.get_future().wait_for(std::chrono::seconds(10)));

    std::string *payload = new std::string(payloadSize, 'y');
    h.conn->send(*payload);
    delete payload;
    release.set_value();

    std::promise<void> sentinel;
    h.loop->queueInLoop([&sentinel] { sentinel.set_value(); });
    EXPECT_EQ(std::future_status::ready,
              sentinel.get_future().wait_for(std::chrono::seconds(10)));

    std::promise<void> entered2;
    std::promise<void> release2;
    h.loop->queueInLoop([&entered2, &release2]
                        {
                            entered2.set_value();
                            release2.get_future().wait();
                        });
    ASSERT_EQ(std::future_status::ready,
              entered2.get_future().wait_for(std::chrono::seconds(10)));

    h.conn->send(std::string(payloadSize, 'z'));
    h.conn.reset();
    release2.set_value();

    std::promise<void> sentinel2;
    h.loop->queueInLoop([&sentinel2] { sentinel2.set_value(); });
    EXPECT_EQ(std::future_status::ready,
              sentinel2.get_future().wait_for(std::chrono::seconds(10)));

    reader.join();
    EXPECT_FALSE(readFailed.load());
    EXPECT_EQ(payloadSize * 2, received.size());
    EXPECT_EQ(std::string(payloadSize, 'y') + std::string(payloadSize, 'z'), received);
}

TEST(TcpConnectionTest, CrossThreadShutdownOwnsConnection)
{
    int fds[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds));
    ConnHarness h(fds[1], [](TcpConnection *) {});
    ASSERT_TRUE(h.waitReady());

    std::promise<void> entered;
    std::promise<void> release;
    h.loop->queueInLoop([&entered, &release]
                        {
                            entered.set_value();
                            release.get_future().wait();
                        });
    ASSERT_EQ(std::future_status::ready,
              entered.get_future().wait_for(std::chrono::seconds(10)));

    h.conn->shutdown();
    h.conn.reset();
    release.set_value();

    std::promise<void> sentinel;
    h.loop->queueInLoop([&sentinel] { sentinel.set_value(); });
    EXPECT_EQ(std::future_status::ready,
              sentinel.get_future().wait_for(std::chrono::seconds(10)));

    struct pollfd pfd = {fds[0], POLLIN, 0};
    ASSERT_EQ(1, poll(&pfd, 1, 10000));
    char buf[16];
    EXPECT_EQ(0, read(fds[0], buf, sizeof buf));
}

TEST(TcpConnectionTest, CrossThreadSendShutdownInterleave)
{
    int fds[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds));
    ConnHarness h(fds[1], [](TcpConnection *) {});
    ASSERT_TRUE(h.waitReady());

    const std::string payload(16, 'x');
    const int kTotal = 10000;
    const int kDelivered = 100;
    std::string received;
    std::promise<void> readerDone;
    std::atomic<bool> readFailed{false};
    std::thread reader([&]
                       {
                           for (;;)
                           {
                               char chunk[4096];
                               ssize_t n = read(fds[0], chunk, sizeof chunk);
                               if (n == 0)
                               {
                                   break;
                               }
                               if (n < 0)
                               {
                                   readFailed = true;
                                   break;
                               }
                               received.append(chunk, static_cast<size_t>(n));
                           }
                           readerDone.set_value();
                       });

    for (int i = 1; i <= kTotal; ++i)
    {
        h.conn->send(payload);
        if (i % (kTotal / kDelivered) == 0)
        {
            h.conn->shutdown();
        }
    }
    EXPECT_EQ(std::future_status::ready,
              readerDone.get_future().wait_for(std::chrono::seconds(30)));
    reader.join();
    EXPECT_FALSE(readFailed.load());
    std::string expected;
    for (int i = 0; i < kDelivered; ++i)
    {
        expected += payload;
    }
    EXPECT_EQ(expected, received);
}
