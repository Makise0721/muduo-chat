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
#include <vector>

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

using Disposition = TcpConnection::SendOutcome::Disposition;
using Pressure = TcpConnection::SendOutcome::Pressure;

bool isAccepted(const TcpConnection::SendOutcome &o)
{
    return o.disposition == Disposition::Accepted;
}

bool isWouldBlock(const TcpConnection::SendOutcome &o)
{
    return o.disposition == Disposition::WouldBlock;
}

void socketPairNonblocking(int fds[2])
{
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds));
    const int kBufSize = 64 * 1024;
    setsockopt(fds[1], SOL_SOCKET, SO_SNDBUF, &kBufSize, sizeof kBufSize);
    setsockopt(fds[0], SOL_SOCKET, SO_RCVBUF, &kBufSize, sizeof kBufSize);
    int flags = fcntl(fds[1], F_GETFL, 0);
    fcntl(fds[1], F_SETFL, flags | O_NONBLOCK);
}

} // namespace

TEST(BackpressureTest, SmallSendStaysAccepted)
{
    signal(SIGPIPE, SIG_IGN);
    int fds[2];
    socketPairNonblocking(fds);
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
    TcpConnection::SendOutcome r;
    std::promise<void> done;
    h.loop->runInLoop([&]
                      {
                          r = h.conn->send(std::string(1024, 'a'));
                          done.set_value();
                      });
    EXPECT_EQ(std::future_status::ready,
              done.get_future().wait_for(std::chrono::seconds(5)));
    EXPECT_EQ(Disposition::Accepted, r.disposition);
    EXPECT_EQ(Pressure::Normal, r.pressure);
}

TEST(BackpressureTest, OversizedMessageIsTooLarge)
{
    signal(SIGPIPE, SIG_IGN);
    int fds[2];
    socketPairNonblocking(fds);
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
    TcpConnection::SendOutcome r;
    std::promise<void> done;
    h.loop->runInLoop([&]
                      {
                          r = h.conn->send(std::string(1024 * 1024, 'x'));
                          done.set_value();
                      });
    EXPECT_EQ(std::future_status::ready,
              done.get_future().wait_for(std::chrono::seconds(5)));
    EXPECT_EQ(Disposition::TooLarge, r.disposition);
}

TEST(BackpressureTest, UnreadPeerBlocksThenStallCloses)
{
    signal(SIGPIPE, SIG_IGN);
    int fds[2];
    socketPairNonblocking(fds);
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

    std::vector<TcpConnection::SendOutcome> results;
    std::promise<void> sawBlocked;
    h.loop->runInLoop([&]
                      {
                          for (int i = 0; i < 20; ++i)
                          {
                              results.push_back(h.conn->send(std::string(128 * 1024, 'b')));
                          }
                          for (const TcpConnection::SendOutcome &r : results)
                          {
                              if (isWouldBlock(r))
                              {
                                  sawBlocked.set_value();
                                  break;
                              }
                          }
                      });
    EXPECT_EQ(std::future_status::ready,
              sawBlocked.get_future().wait_for(std::chrono::seconds(5)));
    EXPECT_EQ(std::future_status::ready,
              closed.get_future().wait_for(std::chrono::seconds(5)));
}

TEST(BackpressureTest, MessageCrossingPauseIsAcceptedWithPauseProducer)
{
    signal(SIGPIPE, SIG_IGN);
    int fds[2];
    socketPairNonblocking(fds);
    TcpConnection::WriteBufferLimits limits;
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

    std::vector<TcpConnection::SendOutcome> results;
    std::promise<void> done;
    h.loop->runInLoop([&]
                      {
                          for (int i = 0; i < 4; ++i)
                          {
                              results.push_back(h.conn->send(std::string(128 * 1024, 'f')));
                          }
                          done.set_value();
                      });
    ASSERT_EQ(std::future_status::ready,
              done.get_future().wait_for(std::chrono::seconds(5)));

    bool sawAcceptedPaused = false;
    for (const TcpConnection::SendOutcome &r : results)
    {
        if (isAccepted(r) && r.pressure == Pressure::PauseProducer)
        {
            sawAcceptedPaused = true;
            break;
        }
    }
    EXPECT_TRUE(sawAcceptedPaused);
}

TEST(BackpressureTest, CrossThreadSendStopsAtBudget)
{
    signal(SIGPIPE, SIG_IGN);
    int fds[2];
    socketPairNonblocking(fds);
    TcpConnection::WriteBufferLimits limits;
    limits.pauseReadBytes = 4 * 1024 * 1024;
    limits.resumeReadBytes = 2 * 1024 * 1024;
    limits.hardLimitBytes = 4 * 1024 * 1024;
    limits.stallTimeout = std::chrono::milliseconds(2000);
    ConnHarness h(fds[1], [limits](TcpConnection *conn)
                  {
                      conn->setWriteBufferLimits(limits);
                      conn->setCloseCallback([](const TcpConnectionPtr &) {});
                  });
    ASSERT_TRUE(h.waitReady());

    std::atomic<bool> gotWouldBlock{false};
    std::promise<void> blocked;
    std::atomic<bool> stop{false};
    std::thread producer([&]
                         {
                             const std::string chunk(1024 * 1024, 'g');
                             while (!stop.load())
                             {
                                 TcpConnection::SendOutcome r = h.conn->send(chunk);
                                 if (isWouldBlock(r))
                                 {
                                     gotWouldBlock = true;
                                     blocked.set_value();
                                     return;
                                 }
                             }
                         });

    EXPECT_EQ(std::future_status::ready,
              blocked.get_future().wait_for(std::chrono::seconds(10)));
    EXPECT_TRUE(gotWouldBlock.load());

    stop = true;
    producer.join();
}

TEST(BackpressureTest, ResumeCallbackFiresOnce)
{
    signal(SIGPIPE, SIG_IGN);
    int fds[2];
    socketPairNonblocking(fds);
    int flags = fcntl(fds[0], F_GETFL, 0);
    fcntl(fds[0], F_SETFL, flags | O_NONBLOCK);
    TcpConnection::WriteBufferLimits limits;
    limits.pauseReadBytes = 64 * 1024;
    limits.resumeReadBytes = 32 * 1024;
    limits.hardLimitBytes = 256 * 1024;
    limits.stallTimeout = std::chrono::milliseconds(5000);
    std::atomic<int> resumeCount{0};
    std::atomic<int> before{0};
    std::atomic<bool> snapshotDone{false};
    std::promise<void> resumed;
    ConnHarness h(fds[1], [limits, &resumeCount, &before, &snapshotDone, &resumed](TcpConnection *conn)
                  {
                      conn->setWriteBufferLimits(limits);
                      conn->setCloseCallback([](const TcpConnectionPtr &) {});
                      conn->setPressureCallback([&resumeCount, &before, &snapshotDone, &resumed]
                                                {
                                                    if (snapshotDone.load() &&
                                                        resumeCount.fetch_add(1) + 1 ==
                                                            before.load() + 1)
                                                    {
                                                        resumed.set_value();
                                                    }
                                                });
                  });
    ASSERT_TRUE(h.waitReady());

    std::promise<void> blocked;
    h.loop->runInLoop([&]
                      {
                          for (int i = 0; i < 20 && !isWouldBlock(h.conn->send(std::string(128 * 1024, 'h'))); ++i)
                          {
                          }
                          blocked.set_value();
                      });
    ASSERT_EQ(std::future_status::ready,
              blocked.get_future().wait_for(std::chrono::seconds(5)));

    before = resumeCount.load();
    snapshotDone = true;

    std::atomic<bool> stopReader{false};
    std::thread reader([&]
                       {
                           char chunk[65536];
                           for (;;)
                           {
                               ssize_t n = read(fds[0], chunk, sizeof chunk);
                               if (n > 0)
                               {
                                   continue;
                               }
                               if (n < 0 && errno == EAGAIN && !stopReader.load())
                               {
                                   usleep(500);
                                   continue;
                               }
                               break;
                           }
                       });

    EXPECT_EQ(std::future_status::ready,
              resumed.get_future().wait_for(std::chrono::seconds(10)));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_EQ(before + 1, resumeCount.load());

    stopReader = true;
    reader.join();
    close(fds[0]);
}

TEST(BackpressureTest, SendAfterForceCloseReturnsClosed)
{
    signal(SIGPIPE, SIG_IGN);
    int fds[2];
    socketPairNonblocking(fds);
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
    TcpConnection::SendOutcome r;
    std::promise<void> done;
    h.loop->runInLoop([&]
                      {
                          h.conn->forceClose();
                          r = h.conn->send(std::string(16, 'a'));
                          done.set_value();
                      });
    EXPECT_EQ(std::future_status::ready,
              done.get_future().wait_for(std::chrono::seconds(5)));
    EXPECT_EQ(Disposition::Closed, r.disposition);
}

TEST(BackpressureTest, EmptyMessagesStillCountTowardBudget)
{
    signal(SIGPIPE, SIG_IGN);
    int fds[2];
    socketPairNonblocking(fds);
    TcpConnection::WriteBufferLimits limits;
    limits.pauseReadBytes = 512;
    limits.resumeReadBytes = 256;
    limits.hardLimitBytes = 1024;
    limits.stallTimeout = std::chrono::milliseconds(2000);
    ConnHarness h(fds[1], [limits](TcpConnection *conn)
                  {
                      conn->setWriteBufferLimits(limits);
                      conn->setCloseCallback([](const TcpConnectionPtr &) {});
                  });
    ASSERT_TRUE(h.waitReady());

    std::promise<void> done;
    bool blocked = false;
    h.loop->runInLoop([&]
                      {
                          for (int i = 0; i < 4096; ++i)
                          {
                              if (isWouldBlock(h.conn->send(std::string())))
                              {
                                  blocked = true;
                                  break;
                              }
                          }
                          done.set_value();
                      });
    EXPECT_EQ(std::future_status::ready,
              done.get_future().wait_for(std::chrono::seconds(5)));
    EXPECT_TRUE(blocked);
}

TEST(BackpressureTest, HardLimitCapOnlyModeStallsThenCloses)
{
    signal(SIGPIPE, SIG_IGN);
    int fds[2];
    socketPairNonblocking(fds);
    TcpConnection::WriteBufferLimits limits;
    limits.pauseReadBytes = 8 * 1024;
    limits.resumeReadBytes = 2 * 1024;
    limits.hardLimitBytes = 4 * 1024;
    limits.stallTimeout = std::chrono::milliseconds(100);
    std::promise<void> closed;
    ConnHarness h(fds[1], [limits, &closed](TcpConnection *conn)
                  {
                      conn->setWriteBufferLimits(limits);
                      conn->setCloseCallback(
                          [&closed](const TcpConnectionPtr &) { closed.set_value(); });
                  });
    ASSERT_TRUE(h.waitReady());

    std::vector<TcpConnection::SendOutcome> results;
    std::promise<void> sawBlocked;
    h.loop->runInLoop([&]
                      {
                          for (int i = 0; i < 64; ++i)
                          {
                              results.push_back(h.conn->send(std::string(4 * 1024, 'e')));
                          }
                          for (const TcpConnection::SendOutcome &r : results)
                          {
                              if (isWouldBlock(r))
                              {
                                  sawBlocked.set_value();
                                  break;
                              }
                          }
                      });
    EXPECT_EQ(std::future_status::ready,
              sawBlocked.get_future().wait_for(std::chrono::seconds(5)));
    EXPECT_EQ(std::future_status::ready,
              closed.get_future().wait_for(std::chrono::seconds(5)));
}
