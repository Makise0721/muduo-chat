#include "EventLoop.h"
#include "TcpServer.h"

#include <gtest/gtest.h>

#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>

namespace {

class MultiReactorTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        serverLoop = new EventLoop();
        server = new TcpServer(serverLoop, InetAddress(0), "MultiReactor");
        server->setThreadNum(4);
        server->setConnectionCallback(
            [this](const TcpConnectionPtr& c) {
                if (c->connected()) {
                    ++connections;
                } else {
                    --connections;
                }
            });
        server->setMessageCallback(
            [this](const TcpConnectionPtr& c, Buffer* buf, Timestamp) {
                std::string msg = buf->retrieveAllAsString();
                std::thread::id tid = std::this_thread::get_id();
                {
                    std::lock_guard<std::mutex> lk(msgMutex);
                    msgCount.fetch_add(1);
                    msgByLoop[tid] += msg.size();
                    auto it = connAffinity.find(c.get());
                    if (it == connAffinity.end()) {
                        connAffinity[c.get()] = tid;
                    } else if (it->second != tid) {
                        affinityViolated = true;
                    }
                }
            });
        server->start();
        std::thread t([this] { serverLoop->loop(); });
        loopThread = std::move(t);
        uint16_t port = server->listenPort();
        for (int i = 0; i < 8; ++i) {
            int fd = ::socket(AF_INET, SOCK_STREAM, 0);
            ASSERT_GE(fd, 0);
            struct sockaddr_in addr;
            memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = htons(port);
            ASSERT_EQ(0, ::connect(fd, (struct sockaddr*)&addr, sizeof(addr)));
            fds.push_back(fd);
        }
        for (int i = 0; i < 200; ++i) {
            if (connections.load() == 8) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ASSERT_EQ(8, connections.load());
    }

    void TearDown() override
    {
        for (int fd : fds) {
            ::close(fd);
        }
        serverLoop->quit();
        loopThread.join();
        delete server;
        delete serverLoop;
    }

    void sendAll(const char* payload)
    {
        size_t n = strlen(payload);
        for (int fd : fds) {
            ASSERT_EQ(static_cast<ssize_t>(n), ::send(fd, payload, n, 0));
        }
    }

    void waitMsgCount(int target)
    {
        for (int i = 0; i < 200; ++i) {
            if (msgCount.load() >= target) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    EventLoop* serverLoop = nullptr;
    TcpServer* server = nullptr;
    std::thread loopThread;
    std::vector<int> fds;
    std::atomic<int> connections{0};
    std::atomic<int> msgCount{0};
    std::atomic<bool> affinityViolated{false};
    std::mutex msgMutex;
    std::map<std::thread::id, size_t> msgByLoop;
    std::map<const void*, std::thread::id> connAffinity;
};

} // namespace

// 4 个 I/O loop + 8 连接并发收发：全部消息被处理且回显，
// 且消息分布到多个 loop 线程（多 Reactor 生效）。
TEST_F(MultiReactorTest, ConcurrentTrafficSpreadsAcrossLoops)
{
    for (int round = 0; round < 10; ++round) {
        sendAll("hello multi-reactor\n");
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    waitMsgCount(8 * 10);
    ASSERT_EQ(8 * 10, msgCount.load());

    {
        std::lock_guard<std::mutex> lk(msgMutex);
        ASSERT_GT(msgByLoop.size(), 1u) << "messages must spread across worker loops";
    }
}

// 同一连接的全部消息回调必须稳定归属同一个 loop 线程（连接不迁移）。
TEST_F(MultiReactorTest, PerConnectionCallbacksAreLoopAffine)
{
    const char* payload = "affine\n";
    for (int i = 0; i < 20; ++i) {
        ASSERT_EQ(static_cast<ssize_t>(strlen(payload)),
                  ::send(fds[0], payload, strlen(payload), 0));
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    waitMsgCount(20);
    ASSERT_EQ(20, msgCount.load());
    EXPECT_FALSE(affinityViolated.load()) << "connection callbacks must not migrate loops";
}
