#include "app/BlockingExecutor.hpp"
#include "app/ChatApplication.hpp"
#include "app/InMemoryFriendRepository.hpp"
#include "app/InMemoryGroupRepository.hpp"
#include "app/InMemoryUserRepository.hpp"
#include "app/SessionRegistry.hpp"
#include "app/SessionSerialExecutor.hpp"
#include "EventLoop.h"
#include "TcpConnection.h"

#include <gtest/gtest.h>

#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <thread>

namespace {

struct LoopThread
{
    std::mutex m;
    std::condition_variable cv;
    bool started = false;
    EventLoop* loop = nullptr;
    std::thread::id loopTid;
    std::promise<void> ended;
    std::future<void> endedF;
    std::thread t;

    LoopThread()
        : endedF(ended.get_future()),
          t([this]
            {
                EventLoop l;
                {
                    std::lock_guard<std::mutex> lk(m);
                    loop = &l;
                    loopTid = std::this_thread::get_id();
                    started = true;
                }
                cv.notify_one();
                l.loop();
                ended.set_value();
            })
    {
        std::unique_lock<std::mutex> lk(m);
        cv.wait(lk, [this] { return started; });
    }

    ~LoopThread()
    {
        if (started && loop != nullptr &&
            endedF.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        {
            EventLoop* l = loop;
            l->queueInLoop([l] { l->quit(); });
        }
        t.join();
    }
};

int64_t registerUser(InMemoryUserRepository& users, const std::string& name)
{
    CreateUserResult r = users.create(name, "pw");
    EXPECT_TRUE(r.ok);
    return r.id;
}

// 占满唯一 worker，把后续任务堵在队列里（制造慢认证窗口）。
void blockExecutor(BlockingExecutor& ex, std::promise<void>* started, int ms)
{
    ex.submit([started, ms] {
                  started->set_value();
                  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
              },
              [] {});
}

// 屏障：等待 loop 处理完所有先前提交的 completion，之后才允许析构
// app/executor（否则 loop 线程仍可能访问已析构的 ChatApplication）。
void drainLoopCompletions(EventLoop* loop)
{
    std::promise<void> barrier;
    loop->queueInLoop([&barrier] { barrier.set_value(); });
    EXPECT_EQ(std::future_status::ready,
              barrier.get_future().wait_for(std::chrono::seconds(5)));
}

} // namespace

TEST(SessionApplicationTest, LoginFlowAuthenticatesAndEstablishesSession)
{
    LoopThread lt;
    InMemoryUserRepository users;
    InMemoryFriendRepository friends(users);
    InMemoryGroupRepository groups(users);
    ChatApplication app(&users, &friends, &groups);
    int64_t uid = registerUser(users, "alice");
    BlockingExecutor ex(lt.loop, 1, 8);

    int64_t gen = app.beginSessionAttempt(uid);
    std::promise<void> done;
    std::atomic<bool> completionRan{false};
    auto result = std::make_shared<AuthResult>();
    ASSERT_EQ(SubmitResult::Accepted,
              ex.submit([&app, uid, result] { *result = app.authenticate(uid, "pw"); },
                        [&app, uid, gen, result, &completionRan, &done] {
                            if (!app.isSessionCurrent(uid, gen)) {
                                return;
                            }
                            completionRan = result->ok && result->id == uid &&
                                            result->name == "alice" &&
                                            result->state == UserState::Offline;
                            done.set_value();
                        }));
    EXPECT_EQ(std::future_status::ready,
              done.get_future().wait_for(std::chrono::seconds(5)));
    EXPECT_TRUE(completionRan.load());
    drainLoopCompletions(lt.loop);
}

TEST(SessionApplicationTest, LoginWrongPasswordRejected)
{
    LoopThread lt;
    InMemoryUserRepository users;
    InMemoryFriendRepository friends(users);
    InMemoryGroupRepository groups(users);
    ChatApplication app(&users, &friends, &groups);
    int64_t uid = registerUser(users, "alice");
    BlockingExecutor ex(lt.loop, 1, 8);

    int64_t gen = app.beginSessionAttempt(uid);
    std::promise<void> done;
    std::atomic<bool> ok{true};
    auto result = std::make_shared<AuthResult>();
    ex.submit([&app, uid, result] { *result = app.authenticate(uid, "WRONG"); },
              [&app, uid, gen, result, &ok, &done] {
                  if (!app.isSessionCurrent(uid, gen)) {
                      return;
                  }
                  ok = result->ok;
                  done.set_value();
              });
    EXPECT_EQ(std::future_status::ready,
              done.get_future().wait_for(std::chrono::seconds(5)));
    EXPECT_FALSE(ok.load());
    drainLoopCompletions(lt.loop);
}

TEST(SessionApplicationTest, DuplicateLoginSeesOnlineState)
{
    LoopThread lt;
    InMemoryUserRepository users;
    InMemoryFriendRepository friends(users);
    InMemoryGroupRepository groups(users);
    ChatApplication app(&users, &friends, &groups);
    int64_t uid = registerUser(users, "alice");
    BlockingExecutor ex(lt.loop, 1, 8);

    // 第一次登录成功并置 online
    app.updateUserState(uid, UserState::Online);

    int64_t gen = app.beginSessionAttempt(uid);
    std::promise<void> done;
    std::atomic<UserState> seen{UserState::Offline};
    auto result = std::make_shared<AuthResult>();
    ex.submit([&app, uid, result] { *result = app.authenticate(uid, "pw"); },
              [&app, uid, gen, result, &seen, &done] {
                  if (!app.isSessionCurrent(uid, gen)) {
                      return;
                  }
                  seen = result->state;
                  done.set_value();
              });
    EXPECT_EQ(std::future_status::ready,
              done.get_future().wait_for(std::chrono::seconds(5)));
    EXPECT_EQ(UserState::Online, seen.load());
    drainLoopCompletions(lt.loop);
}

TEST(SessionApplicationTest, LogoutMarksUserOffline)
{
    InMemoryUserRepository users;
    InMemoryFriendRepository friends(users);
    InMemoryGroupRepository groups(users);
    ChatApplication app(&users, &friends, &groups);
    int64_t uid = registerUser(users, "alice");
    app.updateUserState(uid, UserState::Online);
    app.beginSessionAttempt(uid);  // 会话建立
    EXPECT_TRUE(app.updateUserState(uid, UserState::Offline));
    EXPECT_EQ(UserState::Offline, users.authenticate(uid, "pw").state);
    EXPECT_FALSE(app.isSessionCurrent(uid, 0));
    EXPECT_TRUE(app.isSessionCurrent(uid, 1));
}

TEST(SessionApplicationTest, DisconnectMarksUserOffline)
{
    InMemoryUserRepository users;
    InMemoryFriendRepository friends(users);
    InMemoryGroupRepository groups(users);
    ChatApplication app(&users, &friends, &groups);
    int64_t uid = registerUser(users, "alice");
    app.updateUserState(uid, UserState::Online);
    app.beginSessionAttempt(uid);
    EXPECT_TRUE(app.updateUserState(uid, UserState::Offline));
    EXPECT_EQ(UserState::Offline, users.authenticate(uid, "pw").state);
}

TEST(SessionApplicationTest, StaleCompletionAfterLogoutIsDiscarded)
{
    LoopThread lt;
    InMemoryUserRepository users;
    InMemoryFriendRepository friends(users);
    InMemoryGroupRepository groups(users);
    ChatApplication app(&users, &friends, &groups);
    int64_t uid = registerUser(users, "alice");
    BlockingExecutor ex(lt.loop, 1, 8);

    std::promise<void> blockerStarted;
    blockExecutor(ex, &blockerStarted, 150);
    EXPECT_EQ(std::future_status::ready,
              blockerStarted.get_future().wait_for(std::chrono::seconds(5)));

    // 慢认证任务被堵在队列；任务执行前登出（代次递增）→ completion 必须被丢弃
    int64_t gen = app.beginSessionAttempt(uid);
    std::atomic<int> completions{0};
    auto result = std::make_shared<AuthResult>();
    ex.submit([&app, uid, result] { *result = app.authenticate(uid, "pw"); },
              [&app, uid, gen, &completions] {
                  if (app.isSessionCurrent(uid, gen)) {
                      completions.fetch_add(1);
                  }
              });
    app.beginSessionAttempt(uid);  // 登出
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_EQ(0, completions.load());
    drainLoopCompletions(lt.loop);
}

TEST(SessionApplicationTest, FastDisconnectReconnectDiscardsStaleCompletion)
{
    LoopThread lt;
    InMemoryUserRepository users;
    InMemoryFriendRepository friends(users);
    InMemoryGroupRepository groups(users);
    ChatApplication app(&users, &friends, &groups);
    int64_t uid = registerUser(users, "alice");
    BlockingExecutor ex(lt.loop, 1, 8);

    std::promise<void> blockerStarted;
    blockExecutor(ex, &blockerStarted, 150);
    EXPECT_EQ(std::future_status::ready,
              blockerStarted.get_future().wait_for(std::chrono::seconds(5)));

    // 旧会话的慢登录
    int64_t genOld = app.beginSessionAttempt(uid);
    std::atomic<int> oldCompletions{0};
    auto oldResult = std::make_shared<AuthResult>();
    ex.submit([&app, uid, oldResult] { *oldResult = app.authenticate(uid, "pw"); },
              [&app, uid, genOld, &oldCompletions] {
                  if (app.isSessionCurrent(uid, genOld)) {
                      oldCompletions.fetch_add(1);
                  }
              });
    // 断开（代次递增）后立即重连发起新登录
    app.beginSessionAttempt(uid);
    int64_t genNew = app.beginSessionAttempt(uid);
    std::atomic<int> newCompletions{0};
    std::promise<void> newDone;
    auto newResult = std::make_shared<AuthResult>();
    ex.submit([&app, uid, newResult] { *newResult = app.authenticate(uid, "pw"); },
              [&app, uid, genNew, newResult, &newCompletions, &newDone] {
                  if (app.isSessionCurrent(uid, genNew)) {
                      newCompletions.fetch_add(newResult->ok ? 1 : 0);
                      newDone.set_value();
                  }
              });
    EXPECT_EQ(std::future_status::ready,
              newDone.get_future().wait_for(std::chrono::seconds(5)));
    EXPECT_EQ(0, oldCompletions.load());
    EXPECT_EQ(1, newCompletions.load());
    drainLoopCompletions(lt.loop);
}

TEST(SessionApplicationTest, CloseOfOldGenerationDoesNotInvalidateNewLogin)
{
    InMemoryUserRepository users;
    InMemoryFriendRepository friends(users);
    InMemoryGroupRepository groups(users);
    ChatApplication app(&users, &friends, &groups);
    int64_t uid = registerUser(users, "alice");

    const int64_t oldGeneration = app.beginSessionAttempt(uid);
    const int64_t newGeneration = app.beginSessionAttempt(uid);

    // A delayed close callback for the old session must not advance the
    // generation that already belongs to the new login attempt.
    EXPECT_FALSE(app.invalidateSessionAttempt(uid, oldGeneration));
    EXPECT_TRUE(app.isSessionCurrent(uid, newGeneration));
}

TEST(SessionApplicationTest, CloseOfCurrentGenerationInvalidatesBeforeReconnect)
{
    InMemoryUserRepository users;
    InMemoryFriendRepository friends(users);
    InMemoryGroupRepository groups(users);
    ChatApplication app(&users, &friends, &groups);
    int64_t uid = registerUser(users, "alice");

    const int64_t oldGeneration = app.beginSessionAttempt(uid);
    EXPECT_TRUE(app.invalidateSessionAttempt(uid, oldGeneration));
    EXPECT_FALSE(app.isSessionCurrent(uid, oldGeneration));

    const int64_t newGeneration = app.beginSessionAttempt(uid);
    EXPECT_TRUE(app.isSessionCurrent(uid, newGeneration));
}

// P3-11 M2 RED：跨代影子状态竞态。旧代次断线/登出的 Offline 与新登录的 Online
// 分属两个 lane 可并行，旧 Offline 若在 Online 完成之后执行会覆盖 DB 状态。
// 确定性交错（InlineSessionSerialExecutor 同步执行）：新登录 Online 先完成、
// 旧代 Offline 任务后执行，断言 User 状态不被旧 Offline 覆盖。修复前
// updateUserStateOfflineUnlessActive 不存在 → 编译失败（RED）；修复后守卫跳过。
TEST(SessionApplicationTest, OldGenCloseOfflineDoesNotOverwriteNewLoginOnline)
{
    InMemoryUserRepository users;
    InMemoryFriendRepository friends(users);
    InMemoryGroupRepository groups(users);
    ChatApplication app(&users, &friends, &groups);
    int64_t uid = registerUser(users, "alice");

    // 旧会话（genOld）断开；用户随后在新连接重连登录（genNew）并绑定活动会话，
    // DB 影子状态置 online。
    SessionRegistry registry;
    EventLoop loop;
    int fds[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds));
    close(fds[0]);
    TcpConnectionPtr connNew(
        new TcpConnection(&loop, "newconn", fds[1], InetAddress(0), InetAddress(0)));
    registry.addConnection(connNew);
    const int64_t genOld = app.beginSessionAttempt(uid);
    const int64_t genNew = app.beginSessionAttempt(uid);
    ASSERT_EQ(SessionRegistry::BindResult::Ok, registry.bind(connNew, uid, genNew));
    app.updateUserState(uid, UserState::Online);

    // 确定性交错：Online（新代 lane）先完成，旧代断线的 Offline 任务随后执行。
    InlineSessionSerialExecutor ex;
    const SessionExecutorKey newLane = SessionExecutorKey::session(uid, genNew);
    const SessionExecutorKey oldLane = SessionExecutorKey::session(uid, genOld);
    ASSERT_EQ(SubmitResult::Accepted,
              ex.submit(newLane,
                        [&] { app.updateUserState(uid, UserState::Online); },
                        [] {}));
    ASSERT_EQ(SubmitResult::Accepted,
              ex.submit(oldLane,
                        [&] { app.updateUserStateOfflineUnlessActive(registry, uid); },
                        [] {}));

    // 用户实际在线：旧 Offline 不得覆盖 Online。
    EXPECT_EQ(UserState::Online, users.authenticate(uid, "pw").state);
}
