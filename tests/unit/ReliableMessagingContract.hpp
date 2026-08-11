#pragma once

#include "app/ReliableMessaging.hpp"

class FakeClock;
class RecordingDeliverySink;

// P3-02 contract（tests/unit/ReliableMessagingContractTest.cpp 定义）：
// InMemory 与 MySQL（P3-04）双 adapter 共同满足的契约函数。
// 测试只穿过 ReliableMessaging interface，不读取模块容器。
void runReliableMessagingContract(ReliableMessaging& rm, FakeClock& clock,
                                  RecordingDeliverySink& sink);

// 固定种子随机 retry/ACK/clock 序列：不变量由 interface + recording sink 可观测。
void runReliableMessagingRandomOps(ReliableMessaging& rm, FakeClock& clock,
                                   RecordingDeliverySink& sink);
