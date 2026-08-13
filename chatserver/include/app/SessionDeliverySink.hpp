#pragma once

#include "app/DeliverySink.hpp"
#include "app/SessionRegistry.hpp"

#include <functional>
#include <mutex>
#include <unordered_map>

// P3-07 真实投递出口 adapter（计划 §3）：把 DeliveryAttempt 写到接收者当前活动
// 连接的 TcpConnection。实现（chatserver/src/SessionDeliverySink.cpp，经
// aux_source_directory 自动进 ChatServer 可执行目标）在 mymuduo 侧 TU：
// `TcpConnection::send` 的 SendOutcome → DeliverDisposition 映射（P1-04 背压，
// 冻结映射见 docs/tasks/P3-07.md 决策 3）。
class SessionDeliverySink : public DeliverySink {
public:
    explicit SessionDeliverySink(SessionRegistry* registry);

    DeliverDisposition deliver(const DeliveryAttempt& attempt) override;

private:
    SessionRegistry* registry_;
};

// Owner-loop seam for connecting a Session's low-water signal to the delivery
// executor.  This is a ChatService-owned/internal seam, not a generic
// self-lifetime-safe callback utility.  The SessionDeliveryArmer object,
// registry, and each TcpConnection owner EventLoop must remain alive until
// every queued arm/clear functor and every pressure callback has completed.
// Before any of those owners are destroyed, ChatService must clear each
// armed session on its owner loop and wait for an owner-loop barrier/drain.
// The weak TcpConnection capture only avoids extending a connection lifetime;
// it does not keep the armer, registry, or owner EventLoop alive.
class SessionDeliveryArmer {
public:
    explicit SessionDeliveryArmer(SessionRegistry* registry);

    void armSessionDelivery(const TcpConnectionPtr& conn,
                            const BoundSession& expected,
                            const std::function<bool()>& submitAvailable,
                            const std::function<bool()>& submitResume);

    void clearSessionDelivery(const TcpConnectionPtr& conn,
                              const BoundSession& expected);

private:
    void closeRejectedSubmission(const TcpConnectionPtr& conn,
                                 SessionRegistry* registry,
                                 const BoundSession& expected);

    SessionRegistry* registry_;
    // The mutex covers different owner loops using one ChatService armer;
    // generation prevents a stale close from clearing a newer login.
    std::mutex armedMutex_;
    std::unordered_map<TcpConnection*, BoundSession> armedSessions_;
};
