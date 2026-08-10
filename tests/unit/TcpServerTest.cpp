#include "EventLoop.h"
#include "TcpServer.h"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>

namespace {

struct ServerThread
{
    std::promise<void> readyP;
    std::future<void> readyF;
    std::promise<void> endedP;
    std::future<void> endedF;
    EventLoop *loop = nullptr;
    TcpServer *server = nullptr;
    std::thread t;

    ServerThread(const std::function<void(TcpServer *)> &setup)
        : readyF(readyP.get_future()),
          endedF(endedP.get_future()),
          t([this, setup]
            {
                EventLoop l;
                loop = &l;
                InetAddress addr(0, "127.0.0.1");
                TcpServer srv(&l, addr, "test");
                server = &srv;
                setup(&srv);
                srv.start();
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

    ~ServerThread()
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

int connectTo(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr;
    bzero(&addr, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr) == 0)
    {
        return fd;
    }
    close(fd);
    return -1;
}

} // namespace

TEST(TcpServerTest, RejectsConnectionsBeyondMax)
{
    std::atomic<int> accepts{0};
    std::promise<void> firstAccepted;
    ServerThread st([&](TcpServer *srv)
                    {
                        srv->setMaxConnections(1);
                        srv->setConnectionCallback([&](const TcpConnectionPtr &conn)
                                                   {
                                                       if (conn->connected())
                                                       {
                                                           ++accepts;
                                                           firstAccepted.set_value();
                                                       }
                                                   });
                    });
    ASSERT_TRUE(st.waitReady());

    int port = 0;
    {
        std::promise<void> done;
        st.loop->runInLoop([&]
                           {
                               port = st.server->listenPort();
                               done.set_value();
                           });
        done.get_future().wait();
    }

    int fd1 = connectTo(port);
    ASSERT_GE(fd1, 0);
    EXPECT_EQ(std::future_status::ready,
              firstAccepted.get_future().wait_for(std::chrono::seconds(5)));
    EXPECT_EQ(1, accepts.load());

    int fd2 = connectTo(port);
    ASSERT_GE(fd2, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_EQ(1, accepts.load());

    std::promise<void> done;
    int rejected = -1;
    st.loop->runInLoop([&]
                       {
                           rejected = st.server->rejectedConnections();
                           done.set_value();
                       });
    done.get_future().wait();
    EXPECT_EQ(1, rejected);

    close(fd1);
    close(fd2);
}

TEST(TcpServerTest, AcceptErrorCountStaysZero)
{
    ServerThread st([&](TcpServer *srv)
                    {
                        srv->setConnectionCallback([](const TcpConnectionPtr &) {});
                    });
    ASSERT_TRUE(st.waitReady());

    int port = 0;
    {
        std::promise<void> done;
        st.loop->runInLoop([&]
                           {
                               port = st.server->listenPort();
                               done.set_value();
                           });
        done.get_future().wait();
    }

    std::promise<void> done;
    int errorsBefore = -1;
    st.loop->runInLoop([&]
                       {
                           errorsBefore = st.server->acceptErrorCount();
                           done.set_value();
                       });
    done.get_future().wait();
    EXPECT_EQ(0, errorsBefore);

    int fd = connectTo(port);
    ASSERT_GE(fd, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    std::promise<void> done2;
    int errorsAfter = -1;
    st.loop->runInLoop([&]
                       {
                           errorsAfter = st.server->acceptErrorCount();
                           done2.set_value();
                       });
    done2.get_future().wait();
    EXPECT_EQ(0, errorsAfter);
    close(fd);
}

TEST(TcpServerTest, ForceCloseFromOtherThreadIsIdempotent)
{
    std::promise<void> accepted;
    std::promise<void> closed;
    TcpConnectionPtr connPtr;
    std::atomic<int> closeCount{0};
    ServerThread st([&](TcpServer *srv)
                    {
                        srv->setConnectionCallback([&](const TcpConnectionPtr &conn)
                                                   {
                                                       if (conn->connected())
                                                       {
                                                           connPtr = conn;
                                                           accepted.set_value();
                                                       }
                                                       else
                                                       {
                                                           closeCount.fetch_add(1);
                                                           closed.set_value();
                                                       }
                                                   });
                    });
    ASSERT_TRUE(st.waitReady());

    int port = 0;
    {
        std::promise<void> done;
        st.loop->runInLoop([&]
                           {
                               port = st.server->listenPort();
                               done.set_value();
                           });
        done.get_future().wait();
    }

    int fd = connectTo(port);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(std::future_status::ready,
              accepted.get_future().wait_for(std::chrono::seconds(5)));

    TcpConnectionPtr conn = connPtr;
    ASSERT_TRUE(conn != nullptr);

    conn->forceClose();
    conn->forceClose();
    conn->forceClose();

    EXPECT_EQ(std::future_status::ready,
              closed.get_future().wait_for(std::chrono::seconds(5)));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_EQ(1, closeCount.load());
    close(fd);
}

TEST(TcpServerTest, ForceCloseAllWithWorkersIsLoopAffine)
{
    std::atomic<int> connected{0};
    std::atomic<int> closed{0};
    std::promise<void> allConnected;
    std::promise<void> allClosed;
    ServerThread st([&](TcpServer *srv)
                    {
                        srv->setThreadNum(2);
                        srv->setConnectionCallback([&](const TcpConnectionPtr &conn)
                                                   {
                                                       if (conn->connected())
                                                       {
                                                           if (connected.fetch_add(1) + 1 == 3)
                                                           {
                                                               allConnected.set_value();
                                                           }
                                                       }
                                                       else
                                                       {
                                                           if (closed.fetch_add(1) + 1 == 3)
                                                           {
                                                               allClosed.set_value();
                                                           }
                                                       }
                                                   });
                    });
    ASSERT_TRUE(st.waitReady());

    int port = 0;
    {
        std::promise<void> done;
        st.loop->runInLoop([&]
                           {
                               port = st.server->listenPort();
                               done.set_value();
                           });
        done.get_future().wait();
    }

    std::vector<int> fds;
    for (int i = 0; i < 3; ++i)
    {
        int fd = connectTo(port);
        ASSERT_GE(fd, 0);
        fds.push_back(fd);
    }
    ASSERT_EQ(std::future_status::ready,
              allConnected.get_future().wait_for(std::chrono::seconds(5)));

    st.server->forceCloseAllConnections();

    EXPECT_EQ(std::future_status::ready,
              allClosed.get_future().wait_for(std::chrono::seconds(5)));

    std::promise<void> done;
    int remaining = -1;
    st.loop->runInLoop([&]
                       {
                           remaining = st.server->connectionCount();
                           done.set_value();
                       });
    done.get_future().wait();
    EXPECT_EQ(0, remaining);
    EXPECT_EQ(3, closed.load());

    for (int fd : fds)
    {
        close(fd);
    }
}
