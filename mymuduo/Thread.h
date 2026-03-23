#pragma once

#include <functional>
#include <thread>
#include <memory>
#include <string>
#include <unistd.h>
#include <atomic>

#include "noncopyable.h"
#include "CurrentThread.h"

class Thread : noncopyable
{
public:
    using ThreadFunc = std::function<void()>;
    explicit Thread(ThreadFunc, const std::string &name = std::string());
    ~Thread();

    void start();
    void join();

    bool started() const { return started_; }
    pid_t tid() const { return tid_; }
    const std::string &name() const { return name_; }

    static int32_t numCreated() { return numCreated_; }

private:
    void setDefaultName();

    bool started_;
    bool joined_;
    std::shared_ptr<std::thread> thread_;
    pid_t tid_;
    ThreadFunc func_;
    std::string name_;
    static std::atomic_int numCreated_;
};