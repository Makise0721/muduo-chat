#include "app/SessionSerialExecutor.hpp"

#include "EventLoop.h"

#include <chrono>
#include <exception>
#include <iostream>

namespace {

int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace

SessionSerialExecutor::SessionSerialExecutor(EventLoop* loop, int workerCount,
                                             int globalQueueCapacity, int laneCapacity)
    : loop_(loop), workerCount_(workerCount),
      globalQueueCapacity_(globalQueueCapacity), laneCapacity_(laneCapacity)
{
    for (int i = 0; i < workerCount_; ++i) {
        workers_.emplace_back([this] { workerLoop(); });
    }
}

SessionSerialExecutor::~SessionSerialExecutor()
{
    shutdown();
}

SubmitResult SessionSerialExecutor::submit(const SessionExecutorKey& key,
                                           const std::function<void()>& task,
                                           const std::function<void()>& completion,
                                           int64_t deadlineMs)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shuttingDown_) {
            ++droppedShutdown_;
            return SubmitResult::RejectedShutdown;
        }
        Lane& lane = lanes_[key];
        // 每 key lane 有界：满则 fail-fast（不含运行中任务的排队任务计数）。
        if (static_cast<int>(lane.queue.size()) >= laneCapacity_) {
            ++droppedFull_;
            return SubmitResult::RejectedFull;
        }
        // 全局 ready 队列有界：仅当该 lane 尚未在 ready 中（Idle，新占一个槽位）
        // 时检查；Queued/Running 已有槽位（或已派发），不新增。
        if (lane.state == Lane::State::Idle &&
            static_cast<int>(ready_.size()) >= globalQueueCapacity_) {
            ++droppedFull_;
            return SubmitResult::RejectedFull;
        }
        Pending p;
        p.task = task;
        p.completion = completion;
        p.submitAtMs = nowMs();
        p.deadlineMs = deadlineMs;
        lane.queue.push_back(std::move(p));
        if (lane.state == Lane::State::Idle) {
            lane.state = Lane::State::Queued;
            ready_.push_back(key);
        }
    }
    cond_.notify_one();
    return SubmitResult::Accepted;
}

void SessionSerialExecutor::shutdown()
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

int SessionSerialExecutor::queueDepth() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<int>(ready_.size());
}

uint64_t SessionSerialExecutor::droppedFull() const
{
    return droppedFull_.load();
}

uint64_t SessionSerialExecutor::droppedShutdown() const
{
    return droppedShutdown_.load();
}

void SessionSerialExecutor::workerLoop()
{
    for (;;) {
        SessionExecutorKey key;
        Pending p;
        bool hasWork = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cond_.wait(lock, [this] { return shuttingDown_ || !ready_.empty(); });
            if (ready_.empty()) {
                if (shuttingDown_) {
                    return;
                }
                continue;
            }
            // 公平出队：取队头 lane（round-robin 保证热 key 不饿死冷 key）。
            key = ready_.front();
            ready_.pop_front();
            std::map<SessionExecutorKey, Lane>::iterator it = lanes_.find(key);
            if (it == lanes_.end()) {
                continue;  // 防御：提交与出队同锁，理论不可达
            }
            Lane& lane = it->second;
            lane.state = Lane::State::Running;
            // deadline 过期：跳过（不执行、不回调），直到找到未过期队头。
            while (!lane.queue.empty()) {
                Pending& head = lane.queue.front();
                if (head.deadlineMs > 0 && nowMs() - head.submitAtMs > head.deadlineMs) {
                    lane.queue.pop_front();
                    continue;
                }
                break;
            }
            if (lane.queue.empty()) {
                // P3-11 L1：全过期分支与 M1 擦除对称——lane 排空后同锁擦除，
                // 防 (userId,generation) 代次严格递增导致 lanes_ 无界增长
                // （M1 遗留登记的全过期置 Idle 路径，此修正消除该保留点）。
                lanes_.erase(it);
                continue;  // 该 lane 已无可派发任务（全部过期）
            }
            p = std::move(lane.queue.front());
            lane.queue.pop_front();
            hasWork = true;
        }
        if (!hasWork) {
            continue;
        }
        if (p.task) {
            // 任务异常仍调度 completion（completion 内默认 errno 为非成功值，
            // 客户端得到失败响应而非悬挂）。
            try {
                p.task();
            } catch (const std::exception& e) {
                std::cerr << "executor task exception: " << e.what() << std::endl;
            } catch (...) {
                std::cerr << "executor task exception: unknown" << std::endl;
            }
        }
        if (p.completion) {
            loop_->runInLoop(std::move(p.completion));
        }
        // 队头完成后才把该 lane 的下一个任务重新入队（同 key 至多一个在途 →
        // 严格 FIFO；Running 期间 submit 只追加不占新槽位，此处才转 Queued）。
        // 队列已空则回 Idle——否则后续 submit 见 Running 只追加不入队，lane 悬挂。
        {
            std::lock_guard<std::mutex> lock(mutex_);
            std::map<SessionExecutorKey, Lane>::iterator it = lanes_.find(key);
            if (it != lanes_.end()) {
                if (!it->second.queue.empty()) {
                    it->second.state = Lane::State::Queued;
                    ready_.push_back(key);
                    cond_.notify_one();
                } else {
                    // P3-11 M1：lane 排空后同锁擦除，防 (userId,generation) 代次
                    // 严格递增导致 lanes_ 无界增长（长跑泄漏）。下次 submit 重建，
                    // 语义不变；map 大小不可公开观察。
                    lanes_.erase(it);
                }
            }
        }
    }
}

SubmitResult InlineSessionSerialExecutor::submit(const SessionExecutorKey&,
                                                 const std::function<void()>& task,
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

void InlineSessionSerialExecutor::shutdown()
{
}
