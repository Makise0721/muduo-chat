#pragma once

#include "app/Clock.hpp"

#include <cstdint>

// 确定性测试时钟：测试直接控制 nowMs（P3-08 引入生产系统时钟实现）。
class FakeClock : public Clock {
public:
    void set(int64_t ms) { nowMs_ = ms; }
    void advance(int64_t delta) { nowMs_ += delta; }

    int64_t nowMs() override { return nowMs_; }

private:
    int64_t nowMs_ = 0;
};
