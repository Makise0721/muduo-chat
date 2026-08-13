#include "app/SessionDeliverySink.hpp"

#include "ChatService.hpp"  // EnMsgType：投递复用原命令 msgid（ONE_CHAT_MSG=6/GROUP_CHAT_MSG=10）
#include "EventLoop.h"
#include "TcpConnection.h"
#include "json.hpp"

SessionDeliverySink::SessionDeliverySink(SessionRegistry* registry) : registry_(registry) {}

namespace {

bool matchesExpected(const SessionRegistry* registry,
                     const TcpConnectionPtr& conn,
                     const BoundSession& expected)
{
    if (registry == nullptr || !conn) {
        return false;
    }
    BoundSession actual;
    return registry->lookupByConnection(conn, &actual) &&
           actual.userId == expected.userId &&
           actual.generation == expected.generation;
}

} // namespace

SessionDeliveryArmer::SessionDeliveryArmer(SessionRegistry* registry) : registry_(registry) {}

void SessionDeliveryArmer::closeRejectedSubmission(const TcpConnectionPtr& conn,
                                                   SessionRegistry* registry,
                                                   const BoundSession& expected)
{
    if (!conn || registry == nullptr) {
        return;
    }
    bool reserved = false;
    {
        std::lock_guard<std::mutex> lock(armedMutex_);
        std::unordered_map<TcpConnection*, BoundSession>::iterator it =
            armedSessions_.find(conn.get());
        if (it == armedSessions_.end() ||
            it->second.userId != expected.userId ||
            it->second.generation != expected.generation) {
            return;
        }
        // Keep the lock order armedMutex_ -> SessionRegistry::mutex_.  The
        // registry reservation is the sole generation-conditional close
        // decision; remove the stale arm regardless of its result.
        reserved = registry->reserveCloseIfBound(conn, expected);
        armedSessions_.erase(it);
    }
    if (!reserved) {
        return;
    }
    conn->setPressureCallback(TcpConnection::PressureCallback());
    conn->forceClose();
}

void SessionDeliveryArmer::armSessionDelivery(
    const TcpConnectionPtr& conn, const BoundSession& expected,
    const std::function<bool()>& submitAvailable,
    const std::function<bool()>& submitResume)
{
    if (!conn || conn->getLoop() == nullptr || registry_ == nullptr) {
        return;
    }

    const std::weak_ptr<TcpConnection> weakConn(conn);
    SessionRegistry* registry = registry_;
    conn->getLoop()->runInLoop(
        [this, weakConn, registry, expected, submitAvailable, submitResume] {
            const TcpConnectionPtr current = weakConn.lock();
            if (!current || !current->connected() ||
                !matchesExpected(registry, current, expected)) {
                return;
            }

            const std::weak_ptr<TcpConnection> callbackConn(current);
            current->setPressureCallback(
                [this, callbackConn, registry, expected, submitResume] {
                    const TcpConnectionPtr pressured = callbackConn.lock();
                    if (!pressured || !pressured->connected() ||
                        !matchesExpected(registry, pressured, expected)) {
                        return;
                    }
                    if (!submitResume()) {
                        this->closeRejectedSubmission(pressured, registry, expected);
                    }
                });
            {
                std::lock_guard<std::mutex> lock(this->armedMutex_);
                this->armedSessions_[current.get()] = expected;
            }
            if (!submitAvailable()) {
                this->closeRejectedSubmission(current, registry, expected);
            }
        });
}

void SessionDeliveryArmer::clearSessionDelivery(const TcpConnectionPtr& conn,
                                                const BoundSession& expected)
{
    if (!conn || conn->getLoop() == nullptr || registry_ == nullptr) {
        return;
    }

    const std::weak_ptr<TcpConnection> weakConn(conn);
    conn->getLoop()->runInLoop(
        [this, weakConn, expected] {
            const TcpConnectionPtr current = weakConn.lock();
            if (!current) {
                return;
            }
            {
                std::lock_guard<std::mutex> lock(this->armedMutex_);
                std::unordered_map<TcpConnection*, BoundSession>::iterator it =
                    this->armedSessions_.find(current.get());
                if (it == this->armedSessions_.end() ||
                    it->second.userId != expected.userId ||
                    it->second.generation != expected.generation) {
                    return;
                }
                this->armedSessions_.erase(it);
            }
            current->setPressureCallback(TcpConnection::PressureCallback());
        });
}

DeliverDisposition SessionDeliverySink::deliver(const DeliveryAttempt& attempt)
{
    if (registry_ == nullptr) {
        return DeliverDisposition::Closed;
    }
    const TcpConnectionPtr conn = registry_->lookupByUser(attempt.recipient.value);
    if (!conn || !conn->connected()) {
        return DeliverDisposition::Closed;
    }

    // 投递格式（docs/tasks/P3-07.md 冻结决策 1）：旧格式扩展——原命令 msgid
    // （direct=6/group=10）+ 原字段（id=发送者、toid/groupid）+ 服务器字段
    // {message_id, conversation_id, sequence} + content。接收端按 message_id
    // 去重并 ACK；legacy 客户端忽略附加字段照常展示。
    nlohmann::json payload;
    payload["msgid"] = (attempt.kind == AttemptKind::Direct) ? static_cast<int>(ONE_CHAT_MSG)
                                                             : static_cast<int>(GROUP_CHAT_MSG);
    payload["id"] = attempt.senderId.value;
    if (attempt.kind == AttemptKind::Direct) {
        payload["toid"] = attempt.directRecipient.value;
    } else {
        payload["groupid"] = attempt.groupId.value;
    }
    payload["content"] = attempt.content;
    payload["message_id"] = attempt.messageId.value;
    payload["conversation_id"] = attempt.conversationId.value;
    payload["sequence"] = attempt.sequence.value;

    const TcpConnection::SendOutcome outcome = conn->send(payload.dump() + "\n");
    switch (outcome.disposition) {
        case TcpConnection::SendOutcome::Disposition::Accepted:
            // AcceptedPauseProducer：消息已入输出缓冲但接近上限——不记 attempt，
            // 暂停该 Session 等 low-water 后重投（客户端按 message_id 去重）。
            return outcome.pressure == TcpConnection::SendOutcome::Pressure::PauseProducer
                       ? DeliverDisposition::Paused
                       : DeliverDisposition::Accepted;
        case TcpConnection::SendOutcome::Disposition::WouldBlock:
            return DeliverDisposition::Paused;
        case TcpConnection::SendOutcome::Disposition::Closed:
        case TcpConnection::SendOutcome::Disposition::TooLarge:
            // TooLarge 防御（content 16KB 远低于 64MB hardLimit，正常不出现）。
            return DeliverDisposition::Closed;
    }
    return DeliverDisposition::Closed;
}
