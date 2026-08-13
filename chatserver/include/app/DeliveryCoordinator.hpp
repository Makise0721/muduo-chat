#pragma once

#include "app/Clock.hpp"
#include "app/DeliverySink.hpp"
#include "app/MessageStore.hpp"

#include <map>
#include <memory>
#include <random>
#include <set>
#include <vector>

// P3-07 投递状态机（计划 §3 模块形状的 DeliveryCoordinator）：claim/lease/ACK/
// 背压暂停/活动会话/legacy implicit-ack。ReliableMessaging（P3-02 冻结的对外
// interface）持有本类并委托 acknowledge/sessionAvailable/sessionClosed/resume，
// accept 提交后经 onAccepted 通知在线接收者立即 claim。
// P3-08：ACK timeout 重投扫描（runRetryScan）与 backoff+jitter 排程（spec §3/§4）；
// 参数来自注入 RetryConfig（生产默认 = 卡冻结值，测试注入小值）。
//
// 内联实现说明（docs/tasks/P3-07.md Diff 审查）：chatserver_core 源列表冻结
// （任务不碰 CMakeLists，注册由编排者合并），独立 .cpp 会使本任务构建树的
// ChatServer 与既有测试链接失败（undefined ref）——内联使任何引入本头的 TU
// 都自带实现。
//
// 线程约束：与 ReliableMessaging 一致，接口由单一调用者串行驱动，P3-08 起
// 内部 scheduler tick 与接口调用经 ReliableMessaging::mutex_ 串行化（本类无锁）。

class DeliveryCoordinator {
public:
    // P3-08：bootId 为该 ReliableMessaging 实例（进程）的启动标识，由
    // ReliableMessaging 构造时生成；claim 落 owner 时与 leaseOwner 一并持久化，
    // 使跨进程重启后 owner 必不同（见 DomainTypes.hpp leaseBootId）。
    DeliveryCoordinator(MessageStore& store, DeliverySink& sink, Clock& clock, uint64_t leaseMs,
                        const RetryConfig& config, uint64_t bootId)
        : store_(store), sink_(sink), clock_(clock), leaseMs_(leaseMs), config_(config),
          bootId_(bootId), rng_(config.jitterSeed)
    {
    }

    // 接收端按 MessageId 显式确认；ACK 主体只来自 Session，他人 ACK 不越权
    // （spec §2.3/§4 故障点 4/5）。成功后放行同 Conversation 的下一 sequence。
    AckOutcome acknowledge(const SessionIdentity& acker, MessageId messageId)
    {
        AckOutcome outcome;
        // A user may have only one active generation. Once a newer Session is
        // online, an ACK from an older generation is stale and must not
        // acknowledge the newer lease or release its HOL successor.
        std::map<UserId, SessionIdentity>::const_iterator active =
            activeByUser_.find(acker.userId);
        if (active != activeByUser_.end() && active->second != acker) {
            outcome.result = AckResult::NotRecipient;
            return outcome;
        }
        // Keep fencing effective after the newer Session has closed and been
        // removed from activeByUser_. A user with no observed Session remains
        // eligible for the existing ACK-before-delivery behavior.
        std::map<UserId, uint64_t>::const_iterator observed =
            observedGenerationByUser_.find(acker.userId);
        if (observed != observedGenerationByUser_.end() &&
            acker.generation < observed->second) {
            outcome.result = AckResult::NotRecipient;
            return outcome;
        }
        std::vector<Delivery> deliveries = store_.deliveriesByMessage(messageId);
        if (deliveries.empty()) {
            return outcome;  // NotFound
        }
        Delivery* mine = nullptr;
        for (size_t i = 0; i < deliveries.size(); ++i) {
            if (deliveries[i].recipient == acker.userId) {
                mine = &deliveries[i];
                break;
            }
        }
        if (mine == nullptr) {
            outcome.result = AckResult::NotRecipient;
            return outcome;
        }
        if (mine->state == DeliveryState::Acknowledged) {
            outcome.result = AckResult::Idempotent;
            return outcome;
        }
        mine->state = DeliveryState::Acknowledged;
        mine->acknowledgedAtMs = clock_.nowMs();
        store_.updateDelivery(*mine);
        claimFor(acker);  // ACK 放行同 Conversation 的下一 sequence（暂停中短路）
        outcome.result = AckResult::Acknowledged;
        return outcome;
    }

    // Session 上线：登记活动会话并 claim 自己名下 Pending Delivery（含租约
    // 到期重领）；离线/暂停时无操作。
    void sessionAvailable(const SessionIdentity& session)
    {
        std::map<UserId, uint64_t>::const_iterator observed =
            observedGenerationByUser_.find(session.userId);
        if (observed != observedGenerationByUser_.end() &&
            session.generation < observed->second) {
            return;  // 迟到旧上线事件不能回滚当前 generation。
        }
        if (observed == observedGenerationByUser_.end() ||
            session.generation > observed->second) {
            observedGenerationByUser_[session.userId] = session.generation;
        }
        // A generation change fences any InFlight claim owned by an older
        // session immediately; waiting for lease expiry would leave the new
        // session unable to claim its Pending work.  Cross-process the boot id
        // breaks owner-equality even when the generation counter was reset to
        // the same value after a restart ("uid:gen" owner string collision).
        std::vector<Delivery> deliveries = store_.deliveriesByRecipient(session.userId);
        for (size_t i = 0; i < deliveries.size(); ++i) {
            Delivery& d = deliveries[i];
            if (d.state == DeliveryState::InFlight && d.leaseOwner.userId == session.userId &&
                !(d.leaseOwner == session && d.leaseBootId == bootId_)) {
                d.state = DeliveryState::Pending;
                d.leaseOwner = SessionIdentity();
                d.leaseBootId = 0;
                d.leaseUntilMs = 0;
                store_.updateDelivery(d);
            }
        }
        activeByUser_[session.userId] = session;
        claimFor(session);
    }

    // Session 下线：解暂停；名下 InFlight 立即回 Pending（lease 释放，
    // message-reliability.md §3）。只回滚 leaseOwner==session 的行；若该用户
    // 已有更新的 Session 在线（重连竞态：新 claim 先于旧 close 处理、旧 lease
    // 仍有效而漏投），回滚后立即为当前活动会话重新 claim。
    void sessionClosed(const SessionIdentity& session)
    {
        paused_.erase(session);
        std::map<UserId, SessionIdentity>::iterator cur = activeByUser_.find(session.userId);
        if (cur != activeByUser_.end() && cur->second == session) {
            activeByUser_.erase(cur);
            cur = activeByUser_.end();
        }
        std::vector<Delivery> deliveries = store_.deliveriesByRecipient(session.userId);
        for (size_t i = 0; i < deliveries.size(); ++i) {
            Delivery& d = deliveries[i];
            if (d.state == DeliveryState::InFlight && d.leaseOwner == session) {
                d.state = DeliveryState::Pending;
                d.leaseOwner = SessionIdentity();
                d.leaseBootId = 0;
                d.leaseUntilMs = 0;
                store_.updateDelivery(d);
            }
        }
        if (cur != activeByUser_.end()) {
            claimFor(cur->second);
        }
    }

    // 背压 low-water 恢复（P1-04 机制）：解除暂停并重新 claim；由
    // TcpConnection pressure 回调经 executor 提交调用。
    void resume(const SessionIdentity& session)
    {
        // A queued low-water callback can outlive the Session that armed it.
        // Fence such stale resume intents exactly like stale ACK/sessionAvailable
        // events before touching paused state or claiming any delivery.
        std::map<UserId, SessionIdentity>::const_iterator active =
            activeByUser_.find(session.userId);
        if (active == activeByUser_.end() || active->second != session) {
            return;
        }
        std::map<UserId, uint64_t>::const_iterator observed =
            observedGenerationByUser_.find(session.userId);
        if (observed != observedGenerationByUser_.end() &&
            session.generation < observed->second) {
            return;
        }
        paused_.erase(session);
        claimFor(session);
    }

    // accept 提交后调用：对在线接收者立即 claim（在线投递，P3-07 冻结决策 4）。
    void onAccepted(const std::vector<UserId>& recipients)
    {
        for (size_t i = 0; i < recipients.size(); ++i) {
            std::map<UserId, SessionIdentity>::iterator it = activeByUser_.find(recipients[i]);
            if (it != activeByUser_.end()) {
                claimFor(it->second);
            }
        }
    }

    // P3-08 内部 seam（非 P3-02 契约）：到期重投扫描——只处理活动会话的到期
    // InFlight（offline 不消耗重试额度、Paused 跳过）。next_attempt_at<=now 的
    // InFlight 回 Pending 并立即 claimFor 重投同一 MessageId（attempt+1，HOL 保持）。
    // 批次有界：store 侧一次 LIMIT 查询返回到期候选（config_.retryBatchLimit 行/轮），
    // 不按活动会话逐人扫描（避免 O(activeSessions) 查询风暴）。幂等：重复扫描无副作用。
    // 返回本扫描重投行新写入的 nextAttemptAtMs（供 scheduler 计算下一唤醒间隔，
    // 补缺轮 M1）；空 = 本 tick 无重投，调用方按 ackTimeoutMs 轮询。
    std::vector<int64_t> runRetryScan(int64_t now)
    {
        tickNextAttemptsMs_.clear();
        std::vector<Delivery> due = store_.deliveriesDueForRetry(now, config_.retryBatchLimit);
        if (due.empty()) {
            return std::vector<int64_t>();
        }
        std::set<SessionIdentity> toClaim;
        for (size_t i = 0; i < due.size(); ++i) {
            const Delivery& d = due[i];
            // 只回滚活动且未暂停会话名下的 InFlight（offline 不消耗额度；
            // 他人 lease 不触碰；generation fencing 已由 sessionAvailable 处理）。
            std::map<UserId, SessionIdentity>::iterator active =
                activeByUser_.find(d.recipient);
            if (active == activeByUser_.end()) {
                continue;  // 离线：不消耗重试额度，等重连立即 claim
            }
            if (paused_.count(active->second) != 0) {
                continue;  // 背压暂停：不自旋，等 low-water resume
            }
            if (!(d.leaseOwner == active->second)) {
                continue;  // 本会话不拥有该 lease（不重领他人 Delivery）
            }
            Delivery copy = d;
            copy.state = DeliveryState::Pending;
            copy.leaseOwner = SessionIdentity();
            copy.leaseBootId = 0;
            copy.leaseUntilMs = 0;
            store_.updateDelivery(copy);
            toClaim.insert(active->second);
        }
        // 立即重投到期头；同 conversation 前序未确认不越序（HOL 由 claimFor 保持）。
        for (std::set<SessionIdentity>::iterator it = toClaim.begin(); it != toClaim.end(); ++it) {
            claimFor(*it);
        }
        return tickNextAttemptsMs_;
    }

private:
    // P3-08 backoff（卡冻结公式）：min(base * 乘子^(attempt-1), cap)，±jitterFraction
    // 均匀分布（测试注入 jitter=0 时精确为 base*乘子^(n-1)）。attempt 为 1 基。
    int64_t backoffMs(uint32_t attempt) const
    {
        int64_t delay = config_.backoffBaseMs;
        for (uint32_t i = 1; i < attempt && delay < config_.backoffCapMs; ++i) {
            // 饱和乘算：delay 已超过 cap/乘子 时，本次与后续乘算必超 cap（且可能
            // int64 溢出 = UB），直接取 cap 退出（行为与"乘完再封顶"一致，方向安全）。
            if (config_.backoffMultiplier > 1 &&
                delay > config_.backoffCapMs / config_.backoffMultiplier) {
                delay = config_.backoffCapMs;
                break;
            }
            delay *= config_.backoffMultiplier;
        }
        if (delay > config_.backoffCapMs) {
            delay = config_.backoffCapMs;
        }
        if (config_.jitterFraction > 0.0 && delay > 1) {
            std::uniform_real_distribution<double> dist(-config_.jitterFraction,
                                                        config_.jitterFraction);
            const double f = 1.0 + dist(rng_);
            delay = static_cast<int64_t>(static_cast<double>(delay) * f);
            if (delay < 1) {
                delay = 1;
            }
        }
        return delay;
    }

    // 一次 attempt 后的重投间隔：max(ack_timeout, backoff)——ack_timeout 保证
    // 接收端有展示+ACK 时间（spec §4），backoff 增长后接管退避。
    int64_t retryDelayMs(uint32_t attempt) const
    {
        const int64_t backoff = backoffMs(attempt);
        return backoff > config_.ackTimeoutMs ? backoff : config_.ackTimeoutMs;
    }

    // 单在途 head-of-line claim：每 (recipient, conversation) 至多一个未确认
    // sequence；只有 sink 返回 Accepted 才落 InFlight+lease+attempt（spec §3：
    // socket 准入成功才转 InFlight；Closed 保留 Pending；Paused 暂停不自旋）。
    void claimFor(const SessionIdentity& session)
    {
        if (paused_.count(session) != 0) {
            return;  // 背压暂停：不自旋，等 low-water resume
        }
        std::vector<Delivery> deliveries = store_.deliveriesByRecipient(session.userId);
        std::map<uint64_t, std::vector<Delivery*> > byConversation;
        for (size_t i = 0; i < deliveries.size(); ++i) {
            Delivery& d = deliveries[i];
            if (d.state == DeliveryState::Acknowledged || d.state == DeliveryState::Expired) {
                continue;
            }
            byConversation[d.conversationId.value].push_back(&d);
        }

        const int64_t now = clock_.nowMs();
        for (std::map<uint64_t, std::vector<Delivery*> >::iterator it = byConversation.begin();
             it != byConversation.end(); ++it) {
            while (true) {
                // head-of-line：只考虑同 (recipient, conversation) 内 sequence
                // 最小的未确认 Delivery（sequence 由 store 随行返回，避免按
                // delivery 逐条 findMessage 的 O(conversations) 扫描）。
                Delivery* head = nullptr;
                ConversationSequence headSequence;
                for (size_t i = 0; i < it->second.size(); ++i) {
                    Delivery* d = it->second[i];
                    if (d->state == DeliveryState::Acknowledged ||
                        d->state == DeliveryState::Expired) {
                        continue;
                    }
                    if (head == nullptr || d->sequence.value < headSequence.value) {
                        head = d;
                        headSequence = d->sequence;
                    }
                }
                if (head == nullptr) {
                    break;
                }
                // 单在途：有效 lease 的 InFlight 不被重领；lease 到期可重领。
                // claimable 判定先于 findMessage：常规路径（前序在途未确认）
                // 不需要加载消息体。
                const bool claimable = head->state == DeliveryState::Pending ||
                                       (head->state == DeliveryState::InFlight &&
                                        head->leaseUntilMs <= now);
                if (!claimable) {
                    break;
                }
                const std::shared_ptr<const Message> headMessage =
                    store_.findMessage(head->messageId);
                if (!headMessage) {
                    break;  // 存储一致性防御：缺 Message 的 Delivery 不投递
                }

                DeliveryAttempt attempt;
                attempt.messageId = head->messageId;
                attempt.conversationId = head->conversationId;
                attempt.sequence = headSequence;
                attempt.senderId = headMessage->senderId;
                attempt.recipient = head->recipient;
                attempt.kind = headMessage->command.kind == SendMessageCommand::Kind::Direct
                                   ? AttemptKind::Direct
                                   : AttemptKind::Group;
                attempt.directRecipient = headMessage->command.directRecipient;
                attempt.groupId = headMessage->command.groupId;
                attempt.content = headMessage->command.content;
                attempt.attemptNumber = head->attemptCount + 1;

                const DeliverDisposition disposition = sink_.deliver(attempt);
                if (disposition == DeliverDisposition::Accepted) {
                    // 准入成功才落状态（claim token = lease_owner/lease_until）。
                    head->state = DeliveryState::InFlight;
                    head->leaseOwner = session;
                    head->leaseBootId = bootId_;
                    head->leaseUntilMs = now + static_cast<int64_t>(leaseMs_);
                    head->attemptCount += 1;
                    // P3-08：记录 ACK timeout 排程——下次允许重投时刻 =
                    // 本次投递 + max(ack_timeout, backoff(attemptCount))。
                    head->lastSentAtMs = now;
                    head->nextAttemptAtMs = now + retryDelayMs(head->attemptCount);
                    if (isLegacyClientMessageId(headMessage->command.clientMessageId.value())) {
                        // legacy implicit-ack（spec §5.1，P3-07 冻结决策 2）：
                        // socket 准入后即视为已确认，不进入 ACK timeout/重投循环；
                        // 同 conversation 链式放行下一 sequence。
                        head->state = DeliveryState::Acknowledged;
                        head->acknowledgedAtMs = now;
                    } else {
                        // 补缺轮 M1：记录本 tick 重投行的新 nextAttemptAtMs，
                        // 供 runRetryScan 返回给 scheduler 计算下一唤醒间隔。
                        tickNextAttemptsMs_.push_back(head->nextAttemptAtMs);
                    }
                    store_.updateDelivery(*head);
                    continue;
                }
                if (disposition == DeliverDisposition::Paused) {
                    paused_.insert(session);
                    return;  // 会话级背压：其余 conversation 也无法投递
                }
                break;  // Closed/TooLarge：保留 Pending（或 InFlight-expired），不记 attempt
            }
        }
    }

    MessageStore& store_;
    DeliverySink& sink_;
    Clock& clock_;
    uint64_t leaseMs_;
    RetryConfig config_;          // P3-08 重试/保留参数（生产默认 = 卡冻结值）
    uint64_t bootId_;             // P3-08 进程实例标识（ReliableMessaging 构造时生成）
    mutable std::mt19937 rng_;    // jitter 随机源（注入 seed 确定性测试）
    std::set<SessionIdentity> paused_;                     // 背压暂停的 Session（low-water 解除）
    std::map<UserId, SessionIdentity> activeByUser_;       // 上线 Session（在线投递寻址）
    // Highest Session generation observed for each user. It is intentionally
    // retained after close so delayed older events/ACKs remain fenced; users
    // with no entry preserve ACK-before-delivery compatibility.
    std::map<UserId, uint64_t> observedGenerationByUser_;
    // 补缺轮 M1：本 tick（runRetryScan）重投行新写入的 nextAttemptAtMs，供
    // scheduler 计算下一唤醒间隔（claimFor 在落 InFlight 时填充；runRetryScan
    // 起始清空、结束返回）。非 tick 路径的 claimFor 填充会在下一次 runRetryScan
    // 被清空，不污染唤醒计算。
    std::vector<int64_t> tickNextAttemptsMs_;
};
