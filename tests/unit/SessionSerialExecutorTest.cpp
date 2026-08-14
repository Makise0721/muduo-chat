// P3-11 RED：keyed serial executor（SessionSerialExecutor / SessionExecutorKey
// 尚不存在——引用即编译失败，合法 RED）。公开 interface 契约（P3-11.md §Interface）：
// - submit(key, task, completion, deadlineMs) -> SubmitResult{Accepted,RejectedFull,RejectedShutdown}
// - 同 key 严格 FIFO；不同 key 并行；completion 经 runInLoop 回 origin EventLoop
// - 全局 ready 队列 + 每 key lane 均有界，任一满 -> RejectedFull fail-fast
// - deadline 沿用 BlockingExecutor：自提交起超时不执行、不回调
// - generation 失效沿用 P2-05：业务侧守卫判定，executor 不做业务判断
// - shutdown 幂等、有界 drain、拒新（RejectedShutdown）
// - 生产 adapter 与 deterministic inline adapter 共用 submit interface
// 不读私有容器、不依赖固定 sleep 断言（pacing sleep 用于制造工作负载）。

#include "app/SessionSerialExecutor.hpp"
#include "EventLoop.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

const std::chrono::seconds kWait(5);

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

void notifyOnce(std::atomic<bool>* notified, std::promise<void>* signal)
{
    if (!notified->exchange(true)) {
        signal->set_value();
    }
}

// 两 adapter 共用的公开 submit interface（P3-11 §Interface：共用 interface 的断言）。
class ExecutorIf {
public:
    virtual ~ExecutorIf() = default;
    virtual SubmitResult submit(const SessionExecutorKey& key,
                                const std::function<void()>& task,
                                const std::function<void()>& completion,
                                int64_t deadlineMs = 0) = 0;
    virtual void shutdown() = 0;
    virtual int queueDepth() const = 0;
    virtual uint64_t droppedFull() const = 0;
    virtual uint64_t droppedShutdown() const = 0;
};

// 生产 adapter：真实 EventLoop + N worker SessionSerialExecutor。
class ProductionSessionExecutor : public ExecutorIf {
public:
    ProductionSessionExecutor(int workers = 4, int globalCap = 64, int laneCap = 64)
        : loop_(), ex_(loop_.loop, workers, globalCap, laneCap)
    {
    }

    SubmitResult submit(const SessionExecutorKey& key,
                        const std::function<void()>& task,
                        const std::function<void()>& completion,
                        int64_t deadlineMs) override
    {
        return ex_.submit(key, task, completion, deadlineMs);
    }

    void shutdown() override { ex_.shutdown(); }

    int queueDepth() const override { return ex_.queueDepth(); }
    uint64_t droppedFull() const override { return ex_.droppedFull(); }
    uint64_t droppedShutdown() const override { return ex_.droppedShutdown(); }

private:
    LoopThread loop_;
    SessionSerialExecutor ex_;
};

// deterministic inline adapter：同步/单步执行，completion 同步回调。
class InlineSessionExecutor : public ExecutorIf {
public:
    InlineSessionExecutor(int = 4, int = 64, int = 64) {}

    SubmitResult submit(const SessionExecutorKey& key,
                        const std::function<void()>& task,
                        const std::function<void()>& completion,
                        int64_t deadlineMs) override
    {
        return inner_.submit(key, task, completion, deadlineMs);
    }

    void shutdown() override { inner_.shutdown(); }

    int queueDepth() const override { return inner_.queueDepth(); }
    uint64_t droppedFull() const override { return inner_.droppedFull(); }
    uint64_t droppedShutdown() const override { return inner_.droppedShutdown(); }

private:
    InlineSessionSerialExecutor inner_;
};

// 两 lane 交错提交：各自 FIFO 保持，跨 lane 不串行（确定性断言，两 adapter 均适用）。
void assertTwoLaneOrderIndependent(ExecutorIf& ex, const SessionExecutorKey& keyA,
                                   const SessionExecutorKey& keyB)
{
    std::mutex m;
    std::vector<int> orderA;
    std::vector<int> orderB;
    std::promise<void> done;
    std::atomic<bool> doneNotified{false};
    const int kEach = 10;
    // 总期望 completion 数先置满：fast async worker 可能在提交循环内就完成前几个
    // completion，若从 0 起加再减会在中途提前触发 done；且提前 done 会在还有在途
    // 任务时返回本函数，任务引用的本地 orderA/orderB/m 析构后悬垂（UB）。
    std::atomic<int> pending{2 * kEach};
    auto maybeDone = [&] {
        if (--pending == 0) {
            if (!doneNotified.exchange(true)) {
                done.set_value();
            }
        }
    };
    for (int i = 0; i < kEach; ++i) {
        ASSERT_EQ(SubmitResult::Accepted,
                  ex.submit(keyA,
                            [&, i] {
                                std::lock_guard<std::mutex> lk(m);
                                orderA.push_back(i);
                            },
                            maybeDone));
        ASSERT_EQ(SubmitResult::Accepted,
                  ex.submit(keyB,
                            [&, i] {
                                std::lock_guard<std::mutex> lk(m);
                                orderB.push_back(i);
                            },
                            maybeDone));
    }
    ASSERT_EQ(std::future_status::ready, done.get_future().wait_for(kWait));
    std::lock_guard<std::mutex> lk(m);
    ASSERT_EQ(static_cast<size_t>(kEach), orderA.size());
    for (int i = 0; i < kEach; ++i) {
        EXPECT_EQ(i, orderA[i]);
    }
    ASSERT_EQ(static_cast<size_t>(kEach), orderB.size());
    for (int i = 0; i < kEach; ++i) {
        EXPECT_EQ(i, orderB[i]);
    }
}

// 确定性种子序列：3 key × 固定交错 60 任务，返回每 key 执行序。
std::vector<std::vector<int>> runSeededSequence(ExecutorIf& ex,
                                                const std::vector<SessionExecutorKey>& keys,
                                                int kTasks)
{
    std::vector<std::mutex> laneMutexes(keys.size());
    std::vector<std::vector<int>> orders(keys.size());
    std::atomic<int> pending{kTasks};
    std::promise<void> done;
    for (int i = 0; i < kTasks; ++i) {
        size_t kidx = static_cast<size_t>(i % keys.size());
        int tag = i;
        SubmitResult r = ex.submit(keys[kidx],
                                   [&, kidx, tag] {
                                       std::lock_guard<std::mutex> lk(laneMutexes[kidx]);
                                       orders[kidx].push_back(tag);
                                   },
                                   [&] {
                                       if (--pending == 0) {
                                           done.set_value();
                                       }
                                   });
        EXPECT_EQ(SubmitResult::Accepted, r) << "tag=" << tag;
    }
    if (done.get_future().wait_for(kWait) != std::future_status::ready) {
        ADD_FAILURE() << "seeded sequence did not complete within " << kWait.count() << "s";
    }
    return orders;
}

} // namespace

// 两 adapter（生产 + inline）跑同一契约套件。
template <typename T>
class SessionSerialExecutorContractTest : public ::testing::Test {
protected:
    std::unique_ptr<ExecutorIf> make(int workers, int globalCap, int laneCap) const
    {
        return std::unique_ptr<ExecutorIf>(new T(workers, globalCap, laneCap));
    }
};

using AdapterTypes = ::testing::Types<ProductionSessionExecutor, InlineSessionExecutor>;
TYPED_TEST_SUITE(SessionSerialExecutorContractTest, AdapterTypes);

// 1. 同 key 1000 任务严格 FIFO（P2-06 根因：多 worker 无 lane 会乱序）。
TYPED_TEST(SessionSerialExecutorContractTest, SameKeyTasksRunFifo)
{
    auto ex = this->make(4, 4096, 4096);
    const SessionExecutorKey key = SessionExecutorKey::session(42, 7);
    std::mutex orderMutex;
    std::vector<int> execOrder;
    std::promise<void> allDone;
    std::atomic<int> pending{1000};
    for (int i = 0; i < 1000; ++i) {
        SubmitResult r = ex->submit(key,
                                    [&, i] {
                                        std::lock_guard<std::mutex> lk(orderMutex);
                                        execOrder.push_back(i);
                                    },
                                    [&] {
                                        if (--pending == 0) {
                                            allDone.set_value();
                                        }
                                    });
        ASSERT_EQ(SubmitResult::Accepted, r) << "task " << i;
    }
    ASSERT_EQ(std::future_status::ready, allDone.get_future().wait_for(kWait));
    std::lock_guard<std::mutex> lk(orderMutex);
    ASSERT_EQ(1000u, execOrder.size());
    for (int i = 0; i < 1000; ++i) {
        EXPECT_EQ(i, execOrder[i]);
    }
}

// 不同 key 的 lane 互不串行：每 key 各自 FIFO 保持。
TYPED_TEST(SessionSerialExecutorContractTest, PerKeyOrderIndependentOfOtherKeys)
{
    auto ex = this->make(4, 256, 256);
    const SessionExecutorKey keyA = SessionExecutorKey::session(1, 1);
    const SessionExecutorKey keyB = SessionExecutorKey::session(2, 1);
    assertTwoLaneOrderIndependent(*ex, keyA, keyB);
}

// 11. key 切换语义（P3-11 冻结值）：pre-login=ConnectionId；登录后=(UserId,generation)。
TYPED_TEST(SessionSerialExecutorContractTest, KeySwitchPreLoginToSession)
{
    const SessionExecutorKey conn = SessionExecutorKey::connection(1234);
    const SessionExecutorKey sess = SessionExecutorKey::session(42, 7);
    const SessionExecutorKey sessNextGen = SessionExecutorKey::session(42, 8);
    EXPECT_EQ(SessionExecutorKey::Kind::Connection, conn.kind);
    EXPECT_EQ(1234u, conn.connectionId);
    EXPECT_EQ(SessionExecutorKey::Kind::Session, sess.kind);
    EXPECT_EQ(42, sess.userId);
    EXPECT_EQ(7, sess.generation);
    EXPECT_NE(sess, sessNextGen);  // 代次不同 = 不同 lane
    EXPECT_NE(conn, sess);         // 连接 key 与会话 key = 不同 lane

    auto ex = this->make(2, 256, 256);
    assertTwoLaneOrderIndependent(*ex, conn, sess);
}

// 2. 不同 key 慢任务并行：A 在途阻塞时 B 独立完成（证明跨 key 并行，非单 worker 全局 FIFO）。
TEST(SessionSerialExecutorTest, DifferentKeysRunConcurrently)
{
    LoopThread lt;
    SessionSerialExecutor ex(lt.loop, 2, 32, 8);
    const SessionExecutorKey keyA = SessionExecutorKey::session(1, 1);
    const SessionExecutorKey keyB = SessionExecutorKey::session(2, 1);

    std::promise<void> aStarted;
    std::promise<void> aRelease;
    std::promise<void> bDone;
    std::atomic<bool> aStartedN(false);
    std::atomic<bool> bDoneN(false);
    ASSERT_EQ(SubmitResult::Accepted,
              ex.submit(keyA,
                        [&] {
                            notifyOnce(&aStartedN, &aStarted);
                            aRelease.get_future().wait();
                        },
                        [] {}));
    ASSERT_EQ(std::future_status::ready, aStarted.get_future().wait_for(kWait));

    // aRelease 未 set（A 仍在途）时 B 必须独立完成。
    ASSERT_EQ(SubmitResult::Accepted,
              ex.submit(keyB, [] {}, [&] { notifyOnce(&bDoneN, &bDone); }));
    ASSERT_EQ(std::future_status::ready, bDone.get_future().wait_for(kWait));

    aRelease.set_value();
    ex.shutdown();
}

// 3. 热 key 公平：热 key 持续灌任务时冷 key 任务仍被调度（不饿死）。
TEST(SessionSerialExecutorTest, HotKeyDoesNotStarveOthers)
{
    LoopThread lt;
    SessionSerialExecutor ex(lt.loop, 2, 128, 32);
    const SessionExecutorKey hotKey = SessionExecutorKey::session(1, 1);
    const SessionExecutorKey coldKey = SessionExecutorKey::session(2, 1);

    std::atomic<bool> keepChaining{true};
    std::atomic<int> hotRuns{0};
    std::promise<void> hotChainStarted;
    std::atomic<bool> hotChainStartedN(false);

    std::function<void()> hotTask;
    hotTask = [&] {
        hotRuns.fetch_add(1);
        notifyOnce(&hotChainStartedN, &hotChainStarted);
        // 有界 pacing：模拟热 key 真实工作负载（每次有界占住 worker），
        // 公平性断言经 promise 有界等待，不依赖该 sleep 生效。
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (keepChaining.load()) {
            ex.submit(hotKey, hotTask, [] {});
        }
    };
    ASSERT_EQ(SubmitResult::Accepted, ex.submit(hotKey, hotTask, [] {}));
    ASSERT_EQ(std::future_status::ready, hotChainStarted.get_future().wait_for(kWait));

    std::promise<void> coldStarted;
    std::promise<void> coldRelease;
    std::promise<void> coldDone;
    std::atomic<bool> coldStartedN(false);
    std::atomic<bool> coldDoneN(false);
    ASSERT_EQ(SubmitResult::Accepted,
              ex.submit(coldKey,
                        [&] {
                            notifyOnce(&coldStartedN, &coldStarted);
                            coldRelease.get_future().wait();
                        },
                        [&] { notifyOnce(&coldDoneN, &coldDone); }));

    // 热 key 链持续占住 worker 期间，冷 key 任务必须仍被调度（公平出队）。
    ASSERT_EQ(std::future_status::ready, coldStarted.get_future().wait_for(kWait));
    EXPECT_GE(hotRuns.load(), 1) << "hot lane must have been active while cold ran";

    coldRelease.set_value();
    ASSERT_EQ(std::future_status::ready, coldDone.get_future().wait_for(kWait));
    keepChaining.store(false);
    ex.shutdown();
}

// 4a. 全局 ready 队列满 -> RejectedFull fail-fast（含 droppedFull 计数）。
TEST(SessionSerialExecutorTest, GlobalQueueFullFailsFast)
{
    LoopThread lt;
    SessionSerialExecutor ex(lt.loop, 2, 2, 64);  // 全局容量 2，lane 容量充足
    const SessionExecutorKey key1 = SessionExecutorKey::session(1, 1);
    const SessionExecutorKey key2 = SessionExecutorKey::session(2, 1);

    std::promise<void> aStarted;
    std::promise<void> aRelease;
    std::promise<void> bStarted;
    std::promise<void> bRelease;
    std::promise<void> aDone;
    std::promise<void> bDone;
    std::atomic<bool> aStartedN(false);
    std::atomic<bool> bStartedN(false);
    std::atomic<bool> aDoneN(false);
    std::atomic<bool> bDoneN(false);
    // 两 worker 均在途阻塞：全局队列停止被 drain。
    ASSERT_EQ(SubmitResult::Accepted,
              ex.submit(key1,
                        [&] {
                            notifyOnce(&aStartedN, &aStarted);
                            aRelease.get_future().wait();
                        },
                        [&] { notifyOnce(&aDoneN, &aDone); }));
    ASSERT_EQ(SubmitResult::Accepted,
              ex.submit(key2,
                        [&] {
                            notifyOnce(&bStartedN, &bStarted);
                            bRelease.get_future().wait();
                        },
                        [&] { notifyOnce(&bDoneN, &bDone); }));
    ASSERT_EQ(std::future_status::ready, aStarted.get_future().wait_for(kWait));
    ASSERT_EQ(std::future_status::ready, bStarted.get_future().wait_for(kWait));

    const SessionExecutorKey key3 = SessionExecutorKey::session(3, 1);
    const SessionExecutorKey key4 = SessionExecutorKey::session(4, 1);
    const SessionExecutorKey key5 = SessionExecutorKey::session(5, 1);
    ASSERT_EQ(SubmitResult::Accepted, ex.submit(key3, [] {}, [] {}));
    ASSERT_EQ(SubmitResult::Accepted, ex.submit(key4, [] {}, [] {}));
    EXPECT_EQ(2, ex.queueDepth());
    EXPECT_EQ(SubmitResult::RejectedFull, ex.submit(key5, [] {}, [] {}));
    EXPECT_GE(ex.droppedFull(), 1u);

    aRelease.set_value();
    bRelease.set_value();
    ASSERT_EQ(std::future_status::ready, aDone.get_future().wait_for(kWait));
    ASSERT_EQ(std::future_status::ready, bDone.get_future().wait_for(kWait));
    ex.shutdown();
}

// 4b. 单 key lane 满 -> RejectedFull，且不影响其它 key。
TEST(SessionSerialExecutorTest, PerKeyLaneFullFailsFast)
{
    LoopThread lt;
    SessionSerialExecutor ex(lt.loop, 2, 64, 2);  // 每 key lane 容量 2
    const SessionExecutorKey hotKey = SessionExecutorKey::session(1, 1);
    const SessionExecutorKey otherKey = SessionExecutorKey::session(2, 1);

    std::promise<void> started;
    std::promise<void> release;
    std::promise<void> done;
    std::atomic<bool> startedN(false);
    std::atomic<bool> doneN(false);
    // 占住该 lane 头（在途阻塞）。
    ASSERT_EQ(SubmitResult::Accepted,
              ex.submit(hotKey,
                        [&] {
                            notifyOnce(&startedN, &started);
                            release.get_future().wait();
                        },
                        [&] { notifyOnce(&doneN, &done); }));
    ASSERT_EQ(std::future_status::ready, started.get_future().wait_for(kWait));

    // lane 容量 2：两个排队任务 Accepted，第三个同 key 必须 RejectedFull。
    ASSERT_EQ(SubmitResult::Accepted, ex.submit(hotKey, [] {}, [] {}));
    ASSERT_EQ(SubmitResult::Accepted, ex.submit(hotKey, [] {}, [] {}));
    EXPECT_EQ(SubmitResult::RejectedFull, ex.submit(hotKey, [] {}, [] {}));
    EXPECT_GE(ex.droppedFull(), 1u);
    // 其它 key 不受影响。
    ASSERT_EQ(SubmitResult::Accepted, ex.submit(otherKey, [] {}, [] {}));

    release.set_value();
    ASSERT_EQ(std::future_status::ready, done.get_future().wait_for(kWait));
    ex.shutdown();
}

// 6. deadline 超时跳过：过期任务不执行、不回调；不过期照常。
TEST(SessionSerialExecutorTest, DeadlineSkipsExpired)
{
    LoopThread lt;
    SessionSerialExecutor ex(lt.loop, 1, 16, 4);
    const SessionExecutorKey key = SessionExecutorKey::session(1, 1);

    std::promise<void> blockerStarted;
    std::promise<void> blockerRelease;
    std::promise<void> blockerDone;
    std::atomic<bool> blockerStartedN(false);
    std::atomic<bool> blockerDoneN(false);
    ASSERT_EQ(SubmitResult::Accepted,
              ex.submit(key,
                        [&] {
                            notifyOnce(&blockerStartedN, &blockerStarted);
                            blockerRelease.get_future().wait();
                        },
                        [&] { notifyOnce(&blockerDoneN, &blockerDone); }));
    ASSERT_EQ(std::future_status::ready, blockerStarted.get_future().wait_for(kWait));

    std::atomic<int> expiredRan{0};
    std::atomic<int> expiredCompleted{0};
    // 1ms deadline 的任务排在阻塞任务后，必然过期。
    ASSERT_EQ(SubmitResult::Accepted,
              ex.submit(key,
                        [&expiredRan] { expiredRan.fetch_add(1); },
                        [&expiredCompleted] { expiredCompleted.fetch_add(1); },
                        1));

    // 时钟门控等待：确定性地越过 1ms deadline（非断言型固定 sleep）。
    const auto gateStart = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - gateStart < std::chrono::milliseconds(50)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    blockerRelease.set_value();
    ASSERT_EQ(std::future_status::ready, blockerDone.get_future().wait_for(kWait));

    // 过期任务被跳过：不执行、不回调。
    EXPECT_EQ(0, expiredRan.load());
    EXPECT_EQ(0, expiredCompleted.load());

    // 不过期照常。
    std::promise<void> freshDone;
    std::atomic<bool> freshDoneN(false);
    ASSERT_EQ(SubmitResult::Accepted,
              ex.submit(key, [] {}, [&] { notifyOnce(&freshDoneN, &freshDone); }, 5000));
    ASSERT_EQ(std::future_status::ready, freshDone.get_future().wait_for(kWait));
    ex.shutdown();
}

// 7. generation 失效：P2-05 语义（业务侧守卫），旧代次 completion 不覆盖新 Session。
TEST(SessionSerialExecutorTest, GenerationInvalidationDiscardsStaleCompletion)
{
    LoopThread lt;
    SessionSerialExecutor ex(lt.loop, 2, 32, 8);
    const SessionExecutorKey oldKey = SessionExecutorKey::session(42, 1);
    const SessionExecutorKey newKey = SessionExecutorKey::session(42, 2);

    std::atomic<int64_t> currentGen{1};
    std::mutex stateMutex;
    std::string sessionState;
    auto recordIfCurrent = [&](int64_t gen, const char* value) {
        if (currentGen.load() == gen) {
            std::lock_guard<std::mutex> lk(stateMutex);
            sessionState = value;
        }
    };

    std::promise<void> oldStarted;
    std::promise<void> oldRelease;
    std::atomic<bool> oldStartedN(false);
    ASSERT_EQ(SubmitResult::Accepted,
              ex.submit(oldKey,
                        [&] {
                            notifyOnce(&oldStartedN, &oldStarted);
                            oldRelease.get_future().wait();
                        },
                        [&] { recordIfCurrent(1, "stale-gen1"); }));
    ASSERT_EQ(std::future_status::ready, oldStarted.get_future().wait_for(kWait));

    // 重连：代次前进（beginSessionAttempt 语义），旧代次 completion 失效。
    currentGen.store(2);
    std::promise<void> newDone;
    std::atomic<bool> newDoneN(false);
    ASSERT_EQ(SubmitResult::Accepted,
              ex.submit(newKey,
                        [] {},
                        [&] {
                            recordIfCurrent(2, "current-gen2");
                            notifyOnce(&newDoneN, &newDone);
                        }));
    // 旧代次在途任务占住旧 lane 时，新代次任务仍并行完成（不同 lane）。
    ASSERT_EQ(std::future_status::ready, newDone.get_future().wait_for(kWait));

    // 释放旧任务：旧 completion 跑守卫但被丢弃，不得覆盖新 Session 状态。
    oldRelease.set_value();
    std::promise<void> barrierDone;
    std::atomic<bool> barrierDoneN(false);
    ASSERT_EQ(SubmitResult::Accepted,
              ex.submit(oldKey, [] {}, [&] { notifyOnce(&barrierDoneN, &barrierDone); }));
    ASSERT_EQ(std::future_status::ready, barrierDone.get_future().wait_for(kWait));

    {
        std::lock_guard<std::mutex> lk(stateMutex);
        EXPECT_EQ("current-gen2", sessionState);
    }
    ex.shutdown();
}

// 8. completion 回 origin EventLoop：N worker 下 completion 执行线程 == 提交时 EventLoop 线程。
TEST(SessionSerialExecutorTest, CompletionRunsOnOriginEventLoop)
{
    LoopThread lt;
    SessionSerialExecutor ex(lt.loop, 4, 64, 16);
    std::atomic<int> completionOnLoop{0};
    std::atomic<int> completionOffLoop{0};
    std::promise<void> done;
    std::atomic<int> pending{8};
    for (int i = 0; i < 8; ++i) {
        const SessionExecutorKey key = SessionExecutorKey::session(i + 1, 1);
        ASSERT_EQ(SubmitResult::Accepted,
                  ex.submit(key,
                            [] {},
                            [&] {
                                if (std::this_thread::get_id() == lt.loopTid) {
                                    completionOnLoop.fetch_add(1);
                                } else {
                                    completionOffLoop.fetch_add(1);
                                }
                                if (--pending == 0) {
                                    done.set_value();
                                }
                            }));
    }
    ASSERT_EQ(std::future_status::ready, done.get_future().wait_for(kWait));
    EXPECT_EQ(8, completionOnLoop.load());
    EXPECT_EQ(0, completionOffLoop.load());
    ex.shutdown();
}

// 9. shutdown 幂等、有界 drain 已接受任务、之后 RejectedShutdown（拒新）。
TEST(SessionSerialExecutorTest, ShutdownBoundedDrain)
{
    LoopThread lt;
    SessionSerialExecutor ex(lt.loop, 2, 16, 8);
    const SessionExecutorKey key = SessionExecutorKey::session(1, 1);

    std::atomic<int> ran{0};
    std::promise<void> completionDone;
    std::atomic<bool> completionDoneN(false);
    ASSERT_EQ(SubmitResult::Accepted,
              ex.submit(key, [&ran] { ++ran; },
                        [&] { notifyOnce(&completionDoneN, &completionDone); }));
    ex.shutdown();
    ex.shutdown();  // 幂等
    EXPECT_EQ(1, ran.load());
    ASSERT_EQ(std::future_status::ready, completionDone.get_future().wait_for(kWait));
    EXPECT_EQ(SubmitResult::RejectedShutdown, ex.submit(key, [] {}, [] {}));
    EXPECT_GE(ex.droppedShutdown(), 1u);
}

// 10. 1/2/4/8 worker 行为等价：同 seed 任务序列结果与确定性 inline adapter 参照一致。
TEST(SessionSerialExecutorTest, WorkerCountEquivalence1And2And4And8)
{
    std::vector<SessionExecutorKey> keys;
    keys.push_back(SessionExecutorKey::session(1, 1));
    keys.push_back(SessionExecutorKey::session(2, 1));
    keys.push_back(SessionExecutorKey::session(3, 1));
    const int kTasks = 60;

    // 确定性 inline adapter 为参照。
    InlineSessionExecutor inlineEx;
    const std::vector<std::vector<int>> reference = runSeededSequence(inlineEx, keys, kTasks);

    const int workerCounts[] = {1, 2, 4, 8};
    for (int workers : workerCounts) {
        ProductionSessionExecutor prod(workers, 256, 256);
        const std::vector<std::vector<int>> orders = runSeededSequence(prod, keys, kTasks);
        for (size_t k = 0; k < keys.size(); ++k) {
            EXPECT_EQ(reference[k], orders[k]) << "workers=" << workers << " key=" << k;
        }
    }
}

// P3-11 M1 功能回归：lane 排空（回 Idle 并被擦除）后再 submit 多次，FIFO 语义
// 不回退。map 大小不可公开观察，测试锁功能语义：100 次 drain→submit 循环后
// 同 key 执行序仍 == 提交序（防擦除引入的 bug）。
TEST(SessionSerialExecutorTest, LaneRecreatedAfterDrainKeepsFifo)
{
    LoopThread lt;
    SessionSerialExecutor ex(lt.loop, 2, 64, 64);
    const SessionExecutorKey key = SessionExecutorKey::session(42, 7);

    std::mutex m;
    std::vector<int> order;
    const int kIters = 100;
    const int kPerIter = 4;
    for (int iter = 0; iter < kIters; ++iter) {
        std::promise<void> done;
        std::atomic<bool> doneN(false);
        std::atomic<int> pending{kPerIter};
        for (int seq = 0; seq < kPerIter; ++seq) {
            const int tag = iter * kPerIter + seq;
            ASSERT_EQ(SubmitResult::Accepted,
                      ex.submit(key,
                                [&, tag] {
                                    std::lock_guard<std::mutex> lk(m);
                                    order.push_back(tag);
                                },
                                [&] {
                                    if (--pending == 0) {
                                        notifyOnce(&doneN, &done);
                                    }
                                }));
        }
        // 排空：本迭代全部 completion 完成后再进入下一轮（lane 回 Idle →
        // 擦除 → 下次 submit 重建）。
        ASSERT_EQ(std::future_status::ready, done.get_future().wait_for(kWait));
    }
    ex.shutdown();

    std::lock_guard<std::mutex> lk(m);
    ASSERT_EQ(static_cast<size_t>(kIters * kPerIter), order.size());
    for (int iter = 0; iter < kIters; ++iter) {
        for (int seq = 0; seq < kPerIter; ++seq) {
            EXPECT_EQ(iter * kPerIter + seq, order[iter * kPerIter + seq]);
        }
    }
}
