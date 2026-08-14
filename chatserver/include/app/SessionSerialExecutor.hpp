#pragma once

#include "app/BlockingExecutor.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

class EventLoop;

// P3-11 keyed serial executor 的 key。冻结映射（docs/tasks/P3-11.md §冻结参数）：
// pre-login=ConnectionId（同一连接的任务串行）；登录后=(UserId,generation)
// （按用户+代次隔离，不同代次 = 不同 lane）。值与 ChatService 现有
// BoundSession/beginSessionAttempt 对齐。
struct SessionExecutorKey {
    enum class Kind {
        Connection,  // 未登录：以连接 id 为 lane
        Session,     // 登录后：以 (userId, generation) 为 lane
    };

    static SessionExecutorKey connection(uint32_t id)
    {
        SessionExecutorKey k;
        k.kind = Kind::Connection;
        k.connectionId = id;
        return k;
    }

    static SessionExecutorKey session(int64_t userId, int64_t generation)
    {
        SessionExecutorKey k;
        k.kind = Kind::Session;
        k.userId = userId;
        k.generation = generation;
        return k;
    }

    Kind kind = Kind::Connection;
    uint32_t connectionId = 0;
    int64_t userId = 0;
    int64_t generation = 0;
};

inline bool operator==(const SessionExecutorKey& a, const SessionExecutorKey& b)
{
    if (a.kind != b.kind) {
        return false;
    }
    if (a.kind == SessionExecutorKey::Kind::Connection) {
        return a.connectionId == b.connectionId;
    }
    return a.userId == b.userId && a.generation == b.generation;
}

inline bool operator!=(const SessionExecutorKey& a, const SessionExecutorKey& b)
{
    return !(a == b);
}

inline bool operator<(const SessionExecutorKey& a, const SessionExecutorKey& b)
{
    if (a.kind != b.kind) {
        return static_cast<int>(a.kind) < static_cast<int>(b.kind);
    }
    if (a.kind == SessionExecutorKey::Kind::Connection) {
        return a.connectionId < b.connectionId;
    }
    if (a.userId != b.userId) {
        return a.userId < b.userId;
    }
    return a.generation < b.generation;
}

// P3-11：keyed serial executor（per-key FIFO lane + N worker 共享公平 ready 队列）。
// - 同 key 任务严格 FIFO（lane 串行，同一 Session key 单线程）；不同 key 可并行
//   （N worker 取 ready 队头）。不按 Conversation 分片。
// - 全局 ready 队列（每 lane 至多一个在 ready）+ 每 key lane 均有界，任一满 ->
//   RejectedFull fail-fast（含 droppedFull 计数）。
// - 公平：lane 出队后若仍非空再入队尾（round-robin），热 key 不饿死其它 key。
// - deadline 沿用 BlockingExecutor：deadlineMs>0 且自提交起超时：不执行、不回调。
// - completion 经 runInLoop 调度回构造关联的 EventLoop（提交时 origin loop）。
// - shutdown 幂等、有界 drain（任务应为有限执行）、拒新（RejectedShutdown）。
// 生命周期：executor 必须在 EventLoop 存活期内析构。
class SessionSerialExecutor {
public:
    // P3-11 冻结：生产默认每 key lane 上限（docs/tasks/P3-11.md §冻结参数；
    // 与全局 queue_capacity 默认 64 一致；测试注入小值只经构造参数）。
    static constexpr int kDefaultLaneCapacity = 64;

    SessionSerialExecutor(EventLoop* loop, int workerCount,
                          int globalQueueCapacity, int laneCapacity);
    ~SessionSerialExecutor();

    SubmitResult submit(const SessionExecutorKey& key,
                        const std::function<void()>& task,
                        const std::function<void()>& completion,
                        int64_t deadlineMs = 0);

    void shutdown();

    // P2-10 运行期观测（快照语义）：全局 ready 队列深度与累计拒绝计数。
    int queueDepth() const;
    uint64_t droppedFull() const;
    uint64_t droppedShutdown() const;

private:
    struct Pending {
        std::function<void()> task;
        std::function<void()> completion;
        int64_t submitAtMs;
        int64_t deadlineMs;
    };

    struct Lane {
        // 同 key 至多一个任务在途（严格 FIFO）：Idle → Queued（持有 ready 槽位）
        // → Running（worker 正在执行队头）→ Queued/Idle（队头完成后按队列是否
        // 非空决定）。Running 期间新 submit 只追加，不占新 ready 槽位。
        enum class State {
            Idle,
            Queued,
            Running,
        };
        std::deque<Pending> queue;
        State state = State::Idle;
    };

    void workerLoop();

    EventLoop* loop_;
    int workerCount_;
    int globalQueueCapacity_;
    int laneCapacity_;
    std::vector<std::thread> workers_;
    mutable std::mutex mutex_;
    std::condition_variable cond_;
    std::map<SessionExecutorKey, Lane> lanes_;
    std::deque<SessionExecutorKey> ready_;  // FIFO（队尾入队，公平轮转）
    bool shuttingDown_ = false;
    std::atomic<uint64_t> droppedFull_{0};
    std::atomic<uint64_t> droppedShutdown_{0};
};

// 测试/开发用 inline adapter：同一提交接口，同步执行（completion 同步回调）。
class InlineSessionSerialExecutor {
public:
    SubmitResult submit(const SessionExecutorKey& key,
                        const std::function<void()>& task,
                        const std::function<void()>& completion,
                        int64_t deadlineMs = 0);
    void shutdown();
    int queueDepth() const { return 0; }
    uint64_t droppedFull() const { return 0; }
    uint64_t droppedShutdown() const { return 0; }
};
