#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

class EventLoop;

enum class SubmitResult {
    Accepted,
    RejectedFull,
    RejectedShutdown,
};

// 有界阻塞工作执行器：慢任务（DB 等）在 worker 线程执行，不阻塞 EventLoop；
// completion 保证被调度回 EventLoop 线程；队列容量有界，满则 fail-fast 拒绝。
// 生命周期：executor 必须在 EventLoop 存活期内析构；shutdown 幂等且等待在途
// 任务完成（任务应为有限执行）。
class BlockingExecutor {
public:
    BlockingExecutor(EventLoop* loop, int workerCount, int queueCapacity);
    ~BlockingExecutor();

    // deadlineMs > 0 时任务自提交起超过该时长即视为过期：不执行、不回调。
    SubmitResult submit(const std::function<void()>& task,
                        const std::function<void()>& completion,
                        int64_t deadlineMs = 0);

    void shutdown();

    // P2-10 运行期观测（快照语义）：队列深度与累计拒绝计数。
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

    void workerLoop();

    EventLoop* loop_;
    int workerCount_;
    int queueCapacity_;
    std::vector<std::thread> workers_;
    mutable std::mutex mutex_;
    std::condition_variable cond_;
    std::deque<Pending> queue_;
    bool shuttingDown_ = false;
    std::atomic<uint64_t> droppedFull_{0};
    std::atomic<uint64_t> droppedShutdown_{0};
};

// 测试/开发用 inline adapter：同一提交接口，同步执行（completion 同步回调）。
class InlineBlockingExecutor {
public:
    SubmitResult submit(const std::function<void()>& task,
                        const std::function<void()>& completion,
                        int64_t deadlineMs = 0);
    void shutdown();
    int queueDepth() const { return 0; }
    uint64_t droppedFull() const { return 0; }
    uint64_t droppedShutdown() const { return 0; }
};
