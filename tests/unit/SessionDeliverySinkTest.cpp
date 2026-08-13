#include "app/SessionDeliverySink.hpp"
#include "app/SessionRegistry.hpp"
#include "EventLoop.h"
#include "TcpConnection.h"

#include <gtest/gtest.h>

#include <sys/socket.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cerrno>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

const std::chrono::seconds kWait(5);
const size_t kBurstBytes = 2 * 128 * 1024;

class RealConnection {
public:
    RealConnection() : endedFuture_(ended_.get_future())
    {
        int fds[2] = {-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
            std::abort();
        }
        peerFd_ = fds[0];
        int flags = ::fcntl(peerFd_, F_GETFL, 0);
        if (flags == -1 || ::fcntl(peerFd_, F_SETFL, flags | O_NONBLOCK) != 0) {
            std::abort();
        }
        flags = ::fcntl(fds[1], F_GETFL, 0);
        if (flags == -1 || ::fcntl(fds[1], F_SETFL, flags | O_NONBLOCK) != 0) {
            std::abort();
        }

        thread_ = std::thread([this, serverFd = fds[1]] {
            EventLoop loop;
            TcpConnectionPtr local(new TcpConnection(
                &loop, "session-delivery-test", serverFd, InetAddress(0), InetAddress(0)));
            local->setConnectionCallback([](const TcpConnectionPtr &) {});
            local->setCloseCallback([](const TcpConnectionPtr &) {});
            local->connectEstablished();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                loop_ = &loop;
                conn_ = local;
                ready_ = true;
            }
            readyCv_.notify_one();
            loop.loop();
            local->connectDestroyed();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                conn_.reset();
                loop_ = nullptr;
            }
            ended_.set_value();
        });

        std::unique_lock<std::mutex> lock(mutex_);
        if (!readyCv_.wait_for(lock, kWait, [this] { return ready_; })) {
            std::abort();
        }
    }

    ~RealConnection()
    {
        EventLoop *loop = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            loop = loop_;
        }
        if (loop != nullptr &&
            endedFuture_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            loop->queueInLoop([loop] { loop->quit(); });
        }
        thread_.join();
        ::close(peerFd_);
    }

    TcpConnectionPtr connection() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return conn_;
    }

    bool run(const std::function<void(const TcpConnectionPtr &)> &fn)
    {
        EventLoop *loop = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            loop = loop_;
        }
        if (loop == nullptr) {
            return false;
        }
        std::shared_ptr<std::promise<void>> done(new std::promise<void>());
        std::future<void> future = done->get_future();
        loop->queueInLoop([this, fn, done] { fn(connection()); done->set_value(); });
        return future.wait_for(kWait) == std::future_status::ready;
    }

    bool waitForReadiness() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return ready_ && loop_ != nullptr && conn_ != nullptr;
    }

    int peerFd() const { return peerFd_; }

private:
    mutable std::mutex mutex_;
    std::condition_variable readyCv_;
    bool ready_ = false;
    EventLoop *loop_ = nullptr;
    TcpConnectionPtr conn_;
    int peerFd_ = -1;
    std::promise<void> ended_;
    std::future<void> endedFuture_;
    std::thread thread_;
};

TcpConnection::WriteBufferLimits pressureLimits()
{
    TcpConnection::WriteBufferLimits limits;
    limits.pauseReadBytes = 64 * 1024;
    limits.resumeReadBytes = 32 * 1024;
    limits.hardLimitBytes = 256 * 1024;
    limits.stallTimeout = std::chrono::milliseconds(5000);
    return limits;
}

void sendBurst(const TcpConnectionPtr &conn)
{
    conn->setWriteBufferLimits(pressureLimits());
    const std::string chunk(128 * 1024, 'p');
    (void)conn->send(chunk);
    (void)conn->send(chunk);
}

void drainPeer(int fd, size_t expectedBytes)
{
    size_t total = 0;
    char buf[64 * 1024];
    while (total < expectedBytes) {
        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n > 0) {
            total += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pfd;
            pfd.fd = fd;
            pfd.events = POLLIN;
            pfd.revents = 0;
            if (::poll(&pfd, 1, 5000) <= 0) {
                FAIL() << "timed out draining accepted burst";
            }
            continue;
        }
        FAIL() << "peer closed before draining the accepted burst";
    }
    EXPECT_EQ(expectedBytes, total);
}

void bindExpected(SessionRegistry *registry, const TcpConnectionPtr &conn,
                  const BoundSession &expected)
{
    registry->addConnection(conn);
    ASSERT_EQ(SessionRegistry::BindResult::Ok,
              registry->bind(conn, expected.userId, expected.generation));
}

void notifyOnce(std::atomic<bool> *notified, std::promise<void> *signal)
{
    if (!notified->exchange(true)) {
        signal->set_value();
    }
}

} // namespace

TEST(SessionDeliverySinkTest, ArmInstallsCallbackBeforeAvailableSubmit)
{
    RealConnection real;
    SessionRegistry registry;
    const BoundSession expected(42, 7);
    bindExpected(&registry, real.connection(), expected);

    SessionDeliveryArmer armer(&registry);
    std::promise<void> available;
    std::promise<void> resumed;
    std::atomic<bool> availableNotified(false);
    std::atomic<bool> resumedNotified(false);
    std::vector<std::string> events;

    ASSERT_TRUE(real.run([&](const TcpConnectionPtr &conn) {
        armer.armSessionDelivery(
            conn, expected,
            [&] {
                events.push_back("available");
                sendBurst(conn);
                if (!availableNotified.exchange(true)) {
                    available.set_value();
                }
                return true;
            },
            [&] {
                events.push_back("resume");
                if (!resumedNotified.exchange(true)) {
                    resumed.set_value();
                }
                return true;
            });
    }));

    ASSERT_EQ(std::future_status::ready, available.get_future().wait_for(kWait));
    std::thread reader(drainPeer, real.peerFd(), kBurstBytes);
    ASSERT_EQ(std::future_status::ready, resumed.get_future().wait_for(kWait));
    reader.join();

    ASSERT_GE(events.size(), 2u);
    EXPECT_EQ("available", events[0]);
    for (size_t i = 1; i < events.size(); ++i) {
        EXPECT_EQ("resume", events[i]);
    }

    armer.clearSessionDelivery(real.connection(), expected);
    ASSERT_TRUE(real.run([](const TcpConnectionPtr &) {}));
}

TEST(SessionDeliverySinkTest, StaleExpectedSessionCannotSubmitResume)
{
    RealConnection real;
    SessionRegistry registry;
    const BoundSession oldSession(42, 7);
    bindExpected(&registry, real.connection(), oldSession);

    SessionDeliveryArmer armer(&registry);
    std::promise<void> armed;
    std::atomic<bool> armedNotified(false);
    int resumeCount = 0;
    ASSERT_TRUE(real.run([&](const TcpConnectionPtr &conn) {
        armer.armSessionDelivery(conn, oldSession,
                                 [&] {
                                     notifyOnce(&armedNotified, &armed);
                                     return true;
                                 },
                                 [&] {
                                     ++resumeCount;
                                     return true;
                                 });
    }));
    ASSERT_EQ(std::future_status::ready, armed.get_future().wait_for(kWait));

    ASSERT_EQ(oldSession.userId, registry.unbind(real.connection()));
    const BoundSession newSession(42, 8);
    bindExpected(&registry, real.connection(), newSession);
    ASSERT_TRUE(real.run([](const TcpConnectionPtr &conn) { sendBurst(conn); }));
    std::thread reader(drainPeer, real.peerFd(), kBurstBytes);
    reader.join();
    ASSERT_TRUE(real.run([](const TcpConnectionPtr &) {}));

    EXPECT_EQ(0, resumeCount);

    // The old generation was armed before the re-login.  Clear that exact
    // generation and drain its owner loop before the test locals disappear.
    armer.clearSessionDelivery(real.connection(), oldSession);
    ASSERT_TRUE(real.run([](const TcpConnectionPtr &) {}));
}

TEST(SessionDeliverySinkTest, ReLoginReplacesOldPressureCallback)
{
    RealConnection real;
    SessionRegistry registry;
    const BoundSession oldSession(42, 7);
    bindExpected(&registry, real.connection(), oldSession);

    SessionDeliveryArmer armer(&registry);
    std::promise<void> oldArmed;
    std::promise<void> newArmed;
    std::atomic<bool> oldArmedNotified(false);
    std::atomic<bool> newArmedNotified(false);
    int oldResumeCount = 0;
    int newResumeCount = 0;
    ASSERT_TRUE(real.run([&](const TcpConnectionPtr &conn) {
        armer.armSessionDelivery(conn, oldSession,
                                 [&] {
                                     notifyOnce(&oldArmedNotified, &oldArmed);
                                     return true;
                                 },
                                 [&] {
                                     ++oldResumeCount;
                                     return true;
                                 });
    }));
    ASSERT_EQ(std::future_status::ready, oldArmed.get_future().wait_for(kWait));

    ASSERT_EQ(oldSession.userId, registry.unbind(real.connection()));
    const BoundSession newSession(42, 8);
    bindExpected(&registry, real.connection(), newSession);
    ASSERT_TRUE(real.run([&](const TcpConnectionPtr &conn) {
        armer.armSessionDelivery(
            conn, newSession,
            [&] {
                sendBurst(conn);
                notifyOnce(&newArmedNotified, &newArmed);
                return true;
            },
            [&] {
                ++newResumeCount;
                return true;
            });
    }));
    ASSERT_TRUE(real.run([&](const TcpConnectionPtr &) {}));
    ASSERT_EQ(std::future_status::ready, newArmed.get_future().wait_for(kWait));
    std::thread reader(drainPeer, real.peerFd(), kBurstBytes);
    reader.join();
    ASSERT_TRUE(real.run([](const TcpConnectionPtr &) {}));

    EXPECT_EQ(0, oldResumeCount);
    EXPECT_GT(newResumeCount, 0);

    armer.clearSessionDelivery(real.connection(), newSession);
    ASSERT_TRUE(real.run([](const TcpConnectionPtr &) {}));
}

TEST(SessionDeliverySinkTest, ClearPreventsResumeForExpectedSession)
{
    RealConnection real;
    SessionRegistry registry;
    const BoundSession expected(42, 7);
    bindExpected(&registry, real.connection(), expected);

    SessionDeliveryArmer armer(&registry);
    std::promise<void> armed;
    std::atomic<bool> armedNotified(false);
    int resumeCount = 0;
    ASSERT_TRUE(real.run([&](const TcpConnectionPtr &conn) {
        armer.armSessionDelivery(conn, expected,
                                 [&] {
                                     notifyOnce(&armedNotified, &armed);
                                     return true;
                                 },
                                 [&] {
                                     ++resumeCount;
                                     return true;
                                 });
    }));
    ASSERT_EQ(std::future_status::ready, armed.get_future().wait_for(kWait));
    armer.clearSessionDelivery(real.connection(), expected);
    ASSERT_TRUE(real.run([](const TcpConnectionPtr &) {}));
    ASSERT_TRUE(real.run([](const TcpConnectionPtr &conn) { sendBurst(conn); }));
    std::thread reader(drainPeer, real.peerFd(), kBurstBytes);
    reader.join();
    ASSERT_TRUE(real.run([](const TcpConnectionPtr &) {}));

    EXPECT_EQ(0, resumeCount);
}

TEST(SessionDeliverySinkTest, RejectedAvailableClosesCurrentExpectedOnce)
{
    RealConnection real;
    SessionRegistry registry;
    const BoundSession expected(42, 7);
    bindExpected(&registry, real.connection(), expected);

    SessionDeliveryArmer armer(&registry);
    std::promise<void> submitted;
    std::future<void> submittedFuture = submitted.get_future();
    std::atomic<bool> submitNotified(false);
    std::atomic<int> closeCount(0);

    ASSERT_TRUE(real.run([&](const TcpConnectionPtr &conn) {
        conn->setCloseCallback([&](const TcpConnectionPtr &) {
            closeCount.fetch_add(1);
        });
        armer.armSessionDelivery(
            conn, expected,
            [&] {
                notifyOnce(&submitNotified, &submitted);
                return false;
            },
            [] {
                return true;
            });
    }));

    ASSERT_EQ(std::future_status::ready, submittedFuture.wait_for(kWait));
    ASSERT_TRUE(real.run([](const TcpConnectionPtr &) {}));
    EXPECT_EQ(1, closeCount.load());

    // Rejection removes the armed entry and force-closes on the owner loop;
    // keep an explicit clear/barrier so no queued teardown functor can retain
    // this test's callback captures.
    armer.clearSessionDelivery(real.connection(), expected);
    ASSERT_TRUE(real.run([](const TcpConnectionPtr &) {}));
}

TEST(SessionDeliverySinkTest, RejectedResumeAfterReloginDoesNotCloseNewSession)
{
    RealConnection real;
    SessionRegistry registry;
    const BoundSession oldSession(42, 7);
    const BoundSession newSession(42, 8);
    bindExpected(&registry, real.connection(), oldSession);

    SessionDeliveryArmer armer(&registry);
    std::promise<void> armed;
    std::future<void> armedFuture = armed.get_future();
    std::atomic<bool> armedNotified(false);
    std::promise<void> resumeRejected;
    std::future<void> resumeRejectedFuture = resumeRejected.get_future();
    std::atomic<bool> rejectionNotified(false);
    std::atomic<int> closeCount(0);

    ASSERT_TRUE(real.run([&](const TcpConnectionPtr &conn) {
        const TcpConnectionPtr stableConn = conn;
        stableConn->setCloseCallback([&](const TcpConnectionPtr &) {
            closeCount.fetch_add(1);
        });
        armer.armSessionDelivery(
            stableConn, oldSession,
            [&] {
                notifyOnce(&armedNotified, &armed);
                return true;
            },
            [&, stableConn] {
                // Simulate a re-login racing with the rejection result: the
                // expected old generation is replaced before returning false.
                registry.unbind(stableConn);
                registry.bind(stableConn, newSession.userId, newSession.generation);
                notifyOnce(&rejectionNotified, &resumeRejected);
                return false;
            });
    }));
    ASSERT_EQ(std::future_status::ready, armedFuture.wait_for(kWait));

    ASSERT_TRUE(real.run([](const TcpConnectionPtr &conn) { sendBurst(conn); }));
    std::thread reader(drainPeer, real.peerFd(), kBurstBytes);
    ASSERT_EQ(std::future_status::ready, resumeRejectedFuture.wait_for(kWait));
    reader.join();
    ASSERT_TRUE(real.run([](const TcpConnectionPtr &) {}));

    EXPECT_EQ(0, closeCount.load());
    EXPECT_TRUE(real.connection()->connected());
    BoundSession actual;
    ASSERT_TRUE(registry.lookupByConnection(real.connection(), &actual));
    EXPECT_EQ(newSession.userId, actual.userId);
    EXPECT_EQ(newSession.generation, actual.generation);

    // The rejected old-generation submission must have removed its armed
    // entry even though it did not close the re-logged-in connection.  A new
    // arm on the same connection must therefore install a working callback.
    std::promise<void> newArmed;
    std::future<void> newArmedFuture = newArmed.get_future();
    std::atomic<bool> newArmNotified(false);
    std::atomic<int> newResumeCount(0);
    ASSERT_TRUE(real.run([&](const TcpConnectionPtr &conn) {
        armer.armSessionDelivery(
            conn, newSession,
            [&] {
                notifyOnce(&newArmNotified, &newArmed);
                return true;
            },
            [&] {
                newResumeCount.fetch_add(1);
                return true;
            });
    }));
    ASSERT_EQ(std::future_status::ready, newArmedFuture.wait_for(kWait));
    ASSERT_TRUE(real.run([](const TcpConnectionPtr &conn) { sendBurst(conn); }));
    std::thread newReader(drainPeer, real.peerFd(), kBurstBytes);
    newReader.join();
    ASSERT_TRUE(real.run([](const TcpConnectionPtr &) {}));
    EXPECT_GT(newResumeCount.load(), 0);

    armer.clearSessionDelivery(real.connection(), newSession);
    ASSERT_TRUE(real.run([](const TcpConnectionPtr &) {}));
}
