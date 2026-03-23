#include "EventLoopThreadPool.h"
#include "EventLoopThread.h"

EventLoopThreadPool::EventLoopThreadPool(EventLoop *baseLoop, const std::string &nameArg)
    : baseLoop_(baseLoop),
      name_(nameArg),
      started_(false),
      numThreads_(0),
      next_(0)
{
}

EventLoopThreadPool::~EventLoopThreadPool() = default;

void EventLoopThreadPool::start(const ThreadInitCallback &cb)
{
    started_ = true;

    for (int i = 0; i < numThreads_; ++i)
    {
        std::string threadName = name_ + std::to_string(i);
        EventLoopThread *thread = new EventLoopThread(cb, threadName);
        threads_.push_back(std::unique_ptr<EventLoopThread>(thread));
        loops_.push_back(thread->startLoop());
    }

    if (numThreads_ == 0 && cb)
    {
        cb(baseLoop_);
    }
}

EventLoop *EventLoopThreadPool::getNextLoop()
{
    EventLoop *loop = baseLoop_;

    if (!loops_.empty())
    {
        loop = loops_[next_++ % loops_.size()];
    }

    return loop;
}

std::vector<EventLoop *> EventLoopThreadPool::getAllLoops()
{
    if (loops_.empty())
    {
        return {baseLoop_};
    }
    else
    {
        return loops_;
    }
}