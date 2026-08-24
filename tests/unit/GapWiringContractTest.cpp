// P5-00 阶段 B RED：缺口接线 getter 存在性 + 基础行为（docs/tasks/P5-00.md 设计
// 决定 D9、「阶段 B」RED 冻结清单）。
//
// 本文件 RED 引用尚不存在的 getter（EventLoop::loopLagProbeMs、
// TcpConnection::outstandingBytes、TcpServer::acceptReasonCounts、
// RedisPresenceDirectory::fencingConflicts、KafkaEventConsumer::consumerLag/
// rebalanceCount）→ 编译失败（member not declared）即合法 RED（沿阶段 A 先例）。
// GREEN 时按本文件用法精确实现（命名/语义冻结）。
//
// 冻结契约（本文件逐测试断言，GREEN 必须满足）：
//   - EventLoop::loopLagProbeMs()：loop-affine 探针 gauge，返回最近 poll 耗时差
//     值（ms，自记录最近 poll 返回时刻与耗时差值；从未测量 = 0），>=0。
//   - TcpConnection::outstandingBytes()：只读 getter，返回既有 outstandingBytes_
//     原子量，起始 0，发送后 >=0（不动 CAS 预算 / pause/resume / stall 语义）。
//   - TcpServer::acceptReasonCounts()：返回 AcceptReasonCounts
//     {accept, rateReject, maxReject, emfileRecover} 全零起始（只读桥接既有
//     connectionCount/rateRejectedCount/acceptErrorCount/emfile 恢复路径计数）。
//   - RedisPresenceDirectory::fencingConflicts()：NotEpoch 拒绝路径原子计数，
//     零起始（renew/release 携带旧 epoch 被 compare-and-delete 拒；不改 error
//     语义仍返回 NotEpoch，P4-01 契约零改动）。
//   - KafkaEventConsumer::consumerLag()：highWatermark 与 cursor 差值 gauge，
//     零起始（未 poll 前 = 0）；rebalanceCount()：M-2 earliest 回退代理计数，
//     零起始（本实现无 consumer group 协议，rebalance 事件源登记 N/A）。
//
// 真实依赖最小化：只测 getter 存在 + 零起始（构造对象不触发 Redis/Kafka 连接，
// 无 live 依赖、无重负载）。loop/connection/server 用 mymuduo 单线程最小 harness。

#include "app/RedisPresenceDirectory.hpp"  // 既有头（getter 尚不存在 → member not declared）
#include "app/KafkaEventConsumer.hpp"      // 既有头（getter 尚不存在 → member not declared）
#include "app/OutboxEventConsumer.hpp"     // DeliveryProgressHandler / ConsumedOutboxRecord
#include "app/InMemoryMessageStore.hpp"    // dead-letter port（构造 KafkaEventConsumer）
#include "FakeClock.hpp"
#include "EventLoop.h"
#include "TcpConnection.h"
#include "TcpServer.h"

#include <gtest/gtest.h>

#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdint>
#include <string>

namespace {

// 最小 DeliveryProgressHandler：只返回 Advanced（构造 KafkaEventConsumer 用，
// 本测试不触发 poll，仅验证 getter 零起始）。
class NoopProgressHandler : public DeliveryProgressHandler {
public:
    ConsumeDisposition handle(const ConsumedOutboxRecord&) override
    {
        return ConsumeDisposition::Advanced;
    }
};

} // namespace

TEST(GapWiringContractTest, EventLoopLagProbeExposesGauge)
{
    // loop-affine 探针：从未测量 → 0；getter 存在且 >=0（gauge 语义，不为负）。
    EventLoop loop;
    EXPECT_GE(loop.loopLagProbeMs(), 0);
    // 首次测量即为 0 起始（构造后未跑 loop）；此处只断言非负 + 类型（int64_t）。
    EXPECT_EQ(0, loop.loopLagProbeMs());
    // P5-00 M-1：探针时钟源为稳态时钟（mymuduo 内部 TimerQueue 同源）——两次
    // nowMs 采样差值语义合理：单调非负、亚秒分辨率有界（墙钟回拨不产生负值）。
    const int64_t t0 = EventLoop::steadyNowMs();
    usleep(2000);
    const int64_t t1 = EventLoop::steadyNowMs();
    EXPECT_GE(t1 - t0, 0);
    EXPECT_LT(t1 - t0, 10000);
}

TEST(GapWiringContractTest, TcpConnectionExposesOutstandingBytes)
{
    // 构造后 outstandingBytes() == 0 起始（原子量零初始化）。
    int fds[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds));
    EventLoop loop;
    TcpConnection conn(&loop, "test", fds[0], InetAddress(0), InetAddress(0));
    EXPECT_EQ(0u, conn.outstandingBytes());
    // getter 存在且基础行为：发送后 outstanding >= 0（不触发重负载，仅类型与
    // 非负不变量；实际发送路径由 TcpConnectionTest 覆盖，此处只证 getter 可读）。
    EXPECT_GE(conn.outstandingBytes(), 0u);
    close(fds[0]);
    close(fds[1]);
}

TEST(GapWiringContractTest, TcpServerExposesAcceptReasons)
{
    // acceptReason 计数 getter：accept/rate_reject/max_reject/emfile_recover
    // 全零起始（构造未启动，无 accept 事件）。
    EventLoop loop;
    TcpServer srv(&loop, InetAddress(0), "test");
    const TcpServer::AcceptReasonCounts r = srv.acceptReasonCounts();
    EXPECT_EQ(0u, r.accept);
    EXPECT_EQ(0u, r.rateReject);
    EXPECT_EQ(0u, r.maxReject);
    EXPECT_EQ(0u, r.emfileRecover);
}

TEST(GapWiringContractTest, RedisPresenceExposesFencingConflicts)
{
    // 构造不触发 Redis 连接；fencingConflicts() 零起始（无 renew/release 事件）。
    FakeClock clock;
    clock.set(1000000);
    RedisPresenceDirectory dir(clock, "127.0.0.1", 6379, 1, 1000, 1000, 1000);
    EXPECT_EQ(0u, dir.fencingConflicts());
}

TEST(GapWiringContractTest, KafkaConsumerExposesLagAndRebalance)
{
    // 构造不触发 Kafka 连接；consumerLag()/rebalanceCount() 零起始。
    InMemoryMessageStore store;
    NoopProgressHandler handler;
    KafkaEventConsumer consumer("127.0.0.1", 9092, "muduo-outbox", "g", store, handler,
                                100, 5000);
    EXPECT_EQ(0u, consumer.consumerLag());
    EXPECT_EQ(0u, consumer.rebalanceCount());
}
