#include "app/BlockingExecutor.hpp"

#include "EventLoop.h"

#include <chrono>

namespace {

int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace

BlockingExecutor::BlockingExecutor(EventLoop* loop, int workerCount, int queueCapacity)
    : loop_(loop), workerCount_(workerCount), queueCapacity_(queueCapacity)
{
    for (int i = 0; i < workerCount_; ++i) {
        workers_.emplace_back([this] { workerLoop(); });
    }
}

BlockingExecutor::~BlockingExecutor()
{
    shutdown();
}

SubmitResult BlockingExecutor::submit(const std::function<void()>& task,
                                      const std::function<void()>& completion,
                                      int64_t deadlineMs)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shuttingDown_) {
            ++droppedShutdown_;
            return SubmitResult::RejectedShutdown;
        }
        if (static_cast<int>(queue_.size()) >= queueCapacity_) {
            ++droppedFull_;
            return SubmitResult::RejectedFull;
        }
        Pending p;
        p.task = task;
        p.completion = completion;
        p.submitAtMs = nowMs();
        p.deadlineMs = deadlineMs;
        queue_.push_back(std::move(p));
    }
    cond_.notify_one();
    return SubmitResult::Accepted;
}

void BlockingExecutor::shutdown()
{
    bool notify = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!shuttingDown_) {
            shuttingDown_ = true;
            notify = true;
        }
    }
    if (notify) {
        cond_.notify_all();
    }
    for (std::thread& w : workers_) {
        if (w.joinable()) {
            w.join();
        }
    }
}

int BlockingExecutor::queueDepth() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<int>(queue_.size());
}

uint64_t BlockingExecutor::droppedFull() const
{
    return droppedFull_.load();
}

uint64_t BlockingExecutor::droppedShutdown() const
{
    return droppedShutdown_.load();
}

void BlockingExecutor::workerLoop()
{
    for (;;) {
        Pending p;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cond_.wait(lock, [this] { return shuttingDown_ || !queue_.empty(); });
            if (queue_.empty()) {
                if (shuttingDown_) {
                    return;
                }
                continue;
            }
            p = std::move(queue_.front());
            queue_.pop_front();
        }
        if (p.deadlineMs > 0 && nowMs() - p.submitAtMs > p.deadlineMs) {
            continue;  // 过期：不执行、不回调
        }
        if (p.task) {
            p.task();
        }
        if (p.completion) {
            loop_->runInLoop(std::move(p.completion));
        }
    }
}

SubmitResult InlineBlockingExecutor::submit(const std::function<void()>& task,
                                            const std::function<void()>& completion,
                                            int64_t)
{
    if (task) {
        task();
    }
    if (completion) {
        completion();
    }
    return SubmitResult::Accepted;
}

void InlineBlockingExecutor::shutdown()
{
}
