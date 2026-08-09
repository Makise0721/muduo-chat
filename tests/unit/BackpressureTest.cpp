#include "EventLoop.h"
#include "TcpConnection.h"

#include <gtest/gtest.h>

#include <sys/socket.h>
#include <sys/types.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <future>
#include <string>
#include <thread>

namespace {

struct ConnHarness
{    std::promise<void> readyP;
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

TEST(BackpressureTest, SmallSendStaysAccepted)
{
    signal(SIGPIPE, SIG_IGN);
    const int kBufSize = 64 * 1024;
    int fds[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds));
    setsockopt(fds[1], SOL_SOCKET, SO_SNDBUF, &kBufSize, sizeof kBufSize);
    setsockopt(fds[0], SOL_SOCKET, SO_RCVBUF, &kBufSize, sizeof kBufSize);
    TcpConnection::WriteBufferLimits limits;
    limits.pauseReadBytes = 64 * 1024;
    limits.resumeReadBytes = 32 * 1024;
    limits.hardLimitBytes = 256 * 1024;
    limits.stallTimeout = std::chrono::milliseconds(100);
    ConnHarness h(fds[1], [limits](TcpConnection *conn)
                  {
                      conn->setWriteBufferLimits(limits);
                  });
    ASSERT_TRUE(h.waitReady());
    TcpConnection::SendResult r = TcpConnection::TcpConnection::SendResult::Closed;
    std::promise<void> done;
    h.loop->runInLoop([&]
                      {
                          r = h.conn->send(std::string(1024, 'a'));
                          done.set_value();
                      });
    EXPECT_EQ(std::future_status::ready,
              done.get_future().wait_for(std::chrono::seconds(5)));
    EXPECT_EQ(TcpConnection::SendResult::Accepted, r);
}

TEST(BackpressureTest, OversizedMessageIsTooLarge)
{
    const int kBufSize = 64 * 1024;
    int fds[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds));
    setsockopt(fds[1], SOL_SOCKET, SO_SNDBUF, &kBufSize, sizeof kBufSize);
    setsockopt(fds[0], SOL_SOCKET, SO_RCVBUF, &kBufSize, sizeof kBufSize);
    TcpConnection::WriteBufferLimits limits;
    limits.pauseReadBytes = 64 * 1024;
    limits.resumeReadBytes = 32 * 1024;
    limits.hardLimitBytes = 256 * 1024;
    limits.stallTimeout = std::chrono::milliseconds(100);
    ConnHarness h(fds[1], [limits](TcpConnection *conn)
                  {
                      conn->setWriteBufferLimits(limits);
                  });
    ASSERT_TRUE(h.waitReady());
    TcpConnection::SendResult r = TcpConnection::TcpConnection::SendResult::Accepted;
    std::promise<void> done;
    h.loop->runInLoop([&]
                      {
                          r = h.conn->send(std::string(1024 * 1024, 'x'));
                          done.set_value();
                      });
    EXPECT_EQ(std::future_status::ready,
              done.get_future().wait_for(std::chrono::seconds(5)));
    EXPECT_EQ(TcpConnection::SendResult::TooLarge, r);
}

TEST(BackpressureTest, UnreadPeerTriggersBackpressureThenClose)
{
    const int kBufSize = 64 * 1024;
    int fds[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds));
    setsockopt(fds[1], SOL_SOCKET, SO_SNDBUF, &kBufSize, sizeof kBufSize);
    setsockopt(fds[0], SOL_SOCKET, SO_RCVBUF, &kBufSize, sizeof kBufSize);
    TcpConnection::WriteBufferLimits limits;
    limits.pauseReadBytes = 64 * 1024;
    limits.resumeReadBytes = 32 * 1024;
    limits.hardLimitBytes = 256 * 1024;
    limits.stallTimeout = std::chrono::milliseconds(80);
    std::promise<void> closed;
    ConnHarness h(fds[1], [limits, &closed](TcpConnection *conn)
                  {
                      conn->setWriteBufferLimits(limits);
                      conn->setCloseCallback(
                          [&closed](const TcpConnectionPtr &) { closed.set_value(); });
                  });
    ASSERT_TRUE(h.waitReady());

    std::vector<TcpConnection::SendResult> results;
    std::promise<void> sawBackpressured;
    h.loop->runInLoop([&]
                      {
                          for (int i = 0; i < 20; ++i)
                          {
                              results.push_back(h.conn->send(std::string(128 * 1024, 'b')));
                          }
                           for (TcpConnection::SendResult r : results)
                          {
                              if (r == TcpConnection::SendResult::Backpressured)
                              {
                                  sawBackpressured.set_value();
                                  break;
                              }
                          }
                      });
    EXPECT_EQ(std::future_status::ready,
              sawBackpressured.get_future().wait_for(std::chrono::seconds(5)));
    EXPECT_EQ(std::future_status::ready,
              closed.get_future().wait_for(std::chrono::seconds(5)));
}

TEST(BackpressureTest, PeerResumesReadingRecoversToAccepted)
{
    signal(SIGPIPE, SIG_IGN);
    const int kBufSize = 64 * 1024;
    int fds[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds));
    setsockopt(fds[1], SOL_SOCKET, SO_SNDBUF, &kBufSize, sizeof kBufSize);
    setsockopt(fds[0], SOL_SOCKET, SO_RCVBUF, &kBufSize, sizeof kBufSize);
    TcpConnection::    WriteBufferLimits limits;
    limits.pauseReadBytes = 64 * 1024;
    limits.resumeReadBytes = 32 * 1024;
    limits.hardLimitBytes = 512 * 1024;
    limits.stallTimeout = std::chrono::milliseconds(2000);
    ConnHarness h(fds[1], [limits](TcpConnection *conn)
                  {
                      conn->setWriteBufferLimits(limits);
                      conn->setCloseCallback([](const TcpConnectionPtr &) {});
                  });
    ASSERT_TRUE(h.waitReady());

    std::atomic<bool> sawBackpressured{false};
    std::promise<void> backpressured;
    h.loop->runInLoop([&]
                      {
                          for (int i = 0; i < 10; ++i)
                          {
                              TcpConnection::SendResult r = h.conn->send(std::string(128 * 1024, 'c'));
                              if (r == TcpConnection::SendResult::Backpressured)
                              {
                                  sawBackpressured = true;
                                  backpressured.set_value();
                                  break;
                              }
                          }
                      });
    EXPECT_EQ(std::future_status::ready,
              backpressured.get_future().wait_for(std::chrono::seconds(5)));

    std::string received;
    std::promise<void> drained;
    std::thread reader([&]
                       {
                           char chunk[65536];
                           for (;;)
                           {
                               ssize_t n = read(fds[0], chunk, sizeof chunk);
                               if (n <= 0)
                               {
                                   break;
                               }
                               received.append(chunk, static_cast<size_t>(n));
                           }
                           drained.set_value();
                       });

    std::promise<void> recovered;
    std::atomic<bool> acceptedAfter{false};
    std::function<void()> trySend;
    trySend = [&]
    {
        TcpConnection::SendResult r = h.conn->send(std::string(1024, 'd'));
        if (r == TcpConnection::SendResult::Accepted)
        {
            acceptedAfter = true;
            recovered.set_value();
        }
        else if (r == TcpConnection::SendResult::Backpressured)
        {
            h.loop->queueInLoop(trySend);
        }
        else
        {
            recovered.set_value();
        }
    };
    h.loop->queueInLoop(trySend);
    EXPECT_EQ(std::future_status::ready,
              recovered.get_future().wait_for(std::chrono::seconds(5)));
    EXPECT_TRUE(acceptedAfter.load());
    EXPECT_TRUE(sawBackpressured.load());

    close(fds[0]);
    drained.get_future().wait_for(std::chrono::seconds(5));
    reader.join();
    EXPECT_GT(received.size(), 0u);
}
