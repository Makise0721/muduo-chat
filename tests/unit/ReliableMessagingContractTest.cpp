// P3-02 contract：测试只穿过 ReliableMessaging interface，不读取模块容器。
// runReliableMessagingContract 与 InMemory/MySQL（P3-04）双 adapter 共用。

#include "app/DeliverySink.hpp"
#include "app/InMemoryMessageStore.hpp"
#include "app/MessageStore.hpp"
#include "app/ReliableMessaging.hpp"

#include "FakeClock.hpp"
#include "RecordingDeliverySink.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <map>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

const UserId kAlice{1};
const UserId kBob{2};
const UserId kCarol{3};
const uint64_t kLeaseMs = 1000;
const int64_t kNow = 1000000;

bool hasAttempt(const RecordingDeliverySink& sink, MessageId messageId, UserId recipient,
                uint32_t attemptNumber)
{
    for (size_t i = 0; i < sink.attempts().size(); ++i) {
        const DeliveryAttempt& a = sink.attempts()[i];
        if (a.messageId.value == messageId.value && a.recipient.value == recipient.value &&
            a.attemptNumber == attemptNumber) {
            return true;
        }
    }
    return false;
}

} // namespace

// 双 adapter 必须共同满足的契约（P3-04 MySQL adapter 复用本函数；确定性时钟为契约一部分）。
void runReliableMessagingContract(ReliableMessaging& rm, FakeClock& clock, RecordingDeliverySink& sink)
{
    const SessionIdentity alice{kAlice, 1};
    const SessionIdentity bob{kBob, 1};
    const SessionIdentity carol{kCarol, 1};

    // --- 幂等 accept：同 key 重试返回同 identity，不同 payload 复用 key 冲突 ---
    SendMessageCommand cmd;
    cmd.clientMessageId = ClientMessageId("msg-1");
    cmd.kind = SendMessageCommand::Kind::Direct;
    cmd.directRecipient = kBob;
    cmd.content = "hello";

    AcceptOutcome a1 = rm.accept(alice, cmd);
    EXPECT_TRUE(a1.ok);
    EXPECT_FALSE(a1.duplicate);
    EXPECT_NE(0u, a1.messageId.value);
    EXPECT_EQ(1u, a1.sequence.value);
    EXPECT_NE(0u, a1.conversationId.value);

    AcceptOutcome a2 = rm.accept(alice, cmd);
    EXPECT_TRUE(a2.ok);
    EXPECT_TRUE(a2.duplicate);
    EXPECT_EQ(a1.messageId.value, a2.messageId.value);
    EXPECT_EQ(a1.conversationId.value, a2.conversationId.value);
    EXPECT_EQ(a1.sequence.value, a2.sequence.value);

    SendMessageCommand conflict = cmd;
    conflict.content = "different payload";
    AcceptOutcome c1 = rm.accept(alice, conflict);
    EXPECT_FALSE(c1.ok);
    EXPECT_EQ(AcceptError::IdempotencyConflict, c1.error);

    SendMessageCommand conflict2 = cmd;
    conflict2.directRecipient = kCarol;
    AcceptOutcome c2 = rm.accept(alice, conflict2);
    EXPECT_FALSE(c2.ok);
    EXPECT_EQ(AcceptError::IdempotencyConflict, c2.error);

    // --- accept 不产生投递；session 上线才 claim ---
    EXPECT_TRUE(sink.attempts().empty());

    rm.sessionAvailable(bob);
    ASSERT_EQ(1u, sink.attempts().size());
    EXPECT_EQ(a1.messageId.value, sink.attempts()[0].messageId.value);
    EXPECT_EQ(kBob.value, sink.attempts()[0].recipient.value);
    EXPECT_EQ(1u, sink.attempts()[0].sequence.value);
    EXPECT_EQ(1u, sink.attempts()[0].attemptNumber);
    EXPECT_EQ("hello", sink.attempts()[0].content);

    // 幂等重试不产生新 attempt
    rm.accept(alice, cmd);
    EXPECT_EQ(1u, sink.attempts().size());

    // --- ACK：首次 Acknowledged，重复 Idempotent，未知 NotFound ---
    AckOutcome ac1 = rm.acknowledge(bob, a1.messageId);
    EXPECT_EQ(AckResult::Acknowledged, ac1.result);
    AckOutcome ac2 = rm.acknowledge(bob, a1.messageId);
    EXPECT_EQ(AckResult::Idempotent, ac2.result);
    EXPECT_EQ(1u, sink.attempts().size());

    AckOutcome nf = rm.acknowledge(bob, MessageId{999});
    EXPECT_EQ(AckResult::NotFound, nf.result);

    // --- 他人 ACK 不越权；ack 前未投递（Pending）的 Delivery 也可确认（迟到 ACK） ---
    SendMessageCommand ccmd;
    ccmd.clientMessageId = ClientMessageId("msg-2");
    ccmd.kind = SendMessageCommand::Kind::Direct;
    ccmd.directRecipient = kCarol;
    ccmd.content = "to carol";
    AcceptOutcome ca = rm.accept(alice, ccmd);
    EXPECT_TRUE(ca.ok);

    AckOutcome wrong = rm.acknowledge(bob, ca.messageId);
    EXPECT_EQ(AckResult::NotRecipient, wrong.result);

    rm.sessionAvailable(carol);
    ASSERT_EQ(2u, sink.attempts().size());
    EXPECT_EQ(ca.messageId.value, sink.attempts()[1].messageId.value);

    AckOutcome right = rm.acknowledge(carol, ca.messageId);
    EXPECT_EQ(AckResult::Acknowledged, right.result);

    // --- 群接受：快照成员创建各自 Delivery，每接收者独立状态 ---
    SendMessageCommand gcmd;
    gcmd.clientMessageId = ClientMessageId("group-1");
    gcmd.kind = SendMessageCommand::Kind::Group;
    gcmd.groupId = GroupId{7};
    gcmd.members.push_back(kBob);
    gcmd.members.push_back(kCarol);
    gcmd.content = "all";
    AcceptOutcome g1 = rm.accept(alice, gcmd);
    EXPECT_TRUE(g1.ok);
    EXPECT_NE(a1.conversationId.value, g1.conversationId.value);

    AcceptOutcome g1dup = rm.accept(alice, gcmd);
    EXPECT_TRUE(g1dup.ok);
    EXPECT_TRUE(g1dup.duplicate);
    EXPECT_EQ(g1.messageId.value, g1dup.messageId.value);

    // 同 Group 的后续消息解析到同一 Conversation
    SendMessageCommand gcmd2;
    gcmd2.clientMessageId = ClientMessageId("group-2");
    gcmd2.kind = SendMessageCommand::Kind::Group;
    gcmd2.groupId = GroupId{7};
    gcmd2.members.push_back(kBob);
    gcmd2.content = "subset";
    AcceptOutcome g2 = rm.accept(alice, gcmd2);
    EXPECT_TRUE(g2.ok);
    EXPECT_EQ(g1.conversationId.value, g2.conversationId.value);

    rm.sessionAvailable(bob);
    ASSERT_EQ(3u, sink.attempts().size());
    EXPECT_EQ(g1.messageId.value, sink.attempts()[2].messageId.value);
    EXPECT_EQ(kBob.value, sink.attempts()[2].recipient.value);
    // bob 上线只 claim 自己名下 Delivery，carol 的保持 Pending
    EXPECT_FALSE(hasAttempt(sink, g1.messageId, kCarol, 1));

    rm.sessionAvailable(carol);
    ASSERT_EQ(4u, sink.attempts().size());
    EXPECT_EQ(g1.messageId.value, sink.attempts()[3].messageId.value);
    EXPECT_EQ(kCarol.value, sink.attempts()[3].recipient.value);

    // --- Conversation 内顺序稳定：同 conv 前序未确认时不投后续 sequence ---
    SendMessageCommand cmd3;
    cmd3.clientMessageId = ClientMessageId("msg-3");
    cmd3.kind = SendMessageCommand::Kind::Direct;
    cmd3.directRecipient = kBob;
    cmd3.content = "second";
    AcceptOutcome a3 = rm.accept(alice, cmd3);
    EXPECT_TRUE(a3.ok);
    EXPECT_EQ(a1.conversationId.value, a3.conversationId.value);
    EXPECT_EQ(2u, a3.sequence.value);

    rm.sessionAvailable(bob);
    ASSERT_EQ(5u, sink.attempts().size());
    EXPECT_EQ(a3.messageId.value, sink.attempts()[4].messageId.value);
    EXPECT_EQ(2u, sink.attempts()[4].sequence.value);

    SendMessageCommand cmd4;
    cmd4.clientMessageId = ClientMessageId("msg-4");
    cmd4.kind = SendMessageCommand::Kind::Direct;
    cmd4.directRecipient = kBob;
    cmd4.content = "third";
    AcceptOutcome a4 = rm.accept(alice, cmd4);
    EXPECT_TRUE(a4.ok);
    EXPECT_EQ(3u, a4.sequence.value);

    // msg-3 在途（lease 未到期），msg-4 被 head-of-line 阻塞
    rm.sessionAvailable(bob);
    EXPECT_EQ(5u, sink.attempts().size());

    // ACK 后放行下一 sequence
    AckOutcome ac3 = rm.acknowledge(bob, a3.messageId);
    EXPECT_EQ(AckResult::Acknowledged, ac3.result);
    ASSERT_EQ(6u, sink.attempts().size());
    EXPECT_EQ(a4.messageId.value, sink.attempts()[5].messageId.value);
    EXPECT_EQ(3u, sink.attempts()[5].sequence.value);

    // --- 租约到期可重领；有效 lease 不被重领；断开立即回 Pending 后可再 claim ---
    SendMessageCommand cmd5;
    cmd5.clientMessageId = ClientMessageId("msg-5");
    cmd5.kind = SendMessageCommand::Kind::Direct;
    cmd5.directRecipient = kCarol;
    cmd5.content = "lease test";
    AcceptOutcome a5 = rm.accept(alice, cmd5);
    EXPECT_TRUE(a5.ok);
    EXPECT_EQ(ca.conversationId.value, a5.conversationId.value);
    EXPECT_EQ(2u, a5.sequence.value);

    rm.sessionAvailable(carol);
    ASSERT_EQ(7u, sink.attempts().size());
    EXPECT_TRUE(hasAttempt(sink, a5.messageId, kCarol, 1));

    clock.advance(static_cast<int64_t>(kLeaseMs) + 1);
    rm.sessionAvailable(carol);
    ASSERT_EQ(9u, sink.attempts().size());
    EXPECT_TRUE(hasAttempt(sink, a5.messageId, kCarol, 2));
    EXPECT_TRUE(hasAttempt(sink, g1.messageId, kCarol, 2));

    // lease 未到期：不重投
    rm.sessionAvailable(carol);
    EXPECT_EQ(9u, sink.attempts().size());

    rm.sessionClosed(carol);
    rm.sessionAvailable(carol);
    ASSERT_EQ(11u, sink.attempts().size());
    EXPECT_TRUE(hasAttempt(sink, a5.messageId, kCarol, 3));
    EXPECT_TRUE(hasAttempt(sink, g1.messageId, kCarol, 3));

    // --- 跨 Conversation 并行：不同 conv 可同时各有一个在途，互不阻塞 ---
    SendMessageCommand cmd6;
    cmd6.clientMessageId = ClientMessageId("msg-6");
    cmd6.kind = SendMessageCommand::Kind::Direct;
    cmd6.directRecipient = kCarol;
    cmd6.content = "other conversation";
    AcceptOutcome a6 = rm.accept(bob, cmd6);
    EXPECT_TRUE(a6.ok);
    EXPECT_NE(a5.conversationId.value, a6.conversationId.value);
    EXPECT_EQ(1u, a6.sequence.value);

    rm.sessionAvailable(carol);
    ASSERT_EQ(12u, sink.attempts().size());
    EXPECT_TRUE(hasAttempt(sink, a6.messageId, kCarol, 1));
    // a5 仍未 ACK，但新 conv 的消息并行进入在途
    EXPECT_TRUE(hasAttempt(sink, a5.messageId, kCarol, 3));
}

TEST(ReliableMessagingContractTest, InMemoryAdapterSatisfiesContract)
{
    FakeClock clock;
    clock.set(kNow);
    InMemoryMessageStore store;
    RecordingDeliverySink sink;
    ReliableMessaging rm(store, sink, clock, kLeaseMs);
    runReliableMessagingContract(rm, clock, sink);
}

TEST(ReliableMessagingContractTest, ClientMessageIdValidation)
{
    EXPECT_TRUE(ClientMessageId::isValid("a"));
    EXPECT_TRUE(ClientMessageId::isValid(std::string(64, 'a')));
    EXPECT_TRUE(ClientMessageId::isValid("a b"));
    EXPECT_FALSE(ClientMessageId::isValid(""));
    EXPECT_FALSE(ClientMessageId::isValid(std::string(65, 'a')));
    EXPECT_FALSE(ClientMessageId::isValid(std::string("\x01", 1)));
    EXPECT_FALSE(ClientMessageId::isValid("中文"));
    EXPECT_FALSE(ClientMessageId::isValid("caf\xC3\xA9"));

    EXPECT_THROW(ClientMessageId(""), std::invalid_argument);
    EXPECT_THROW(ClientMessageId(std::string(65, 'a')), std::invalid_argument);
    EXPECT_THROW(ClientMessageId("中文"), std::invalid_argument);

    ClientMessageId id("stable-1");
    EXPECT_EQ("stable-1", id.value());
    EXPECT_EQ(ClientMessageId("k"), ClientMessageId("k"));
    EXPECT_NE(ClientMessageId("k1"), ClientMessageId("k2"));
}

// 固定种子随机 retry/ACK/clock 序列：不变量由 interface + recording sink 可观测。
TEST(ReliableMessagingContractTest, FixedSeedRandomOpsPreserveInvariants)
{
    FakeClock clock;
    clock.set(kNow);
    InMemoryMessageStore store;
    RecordingDeliverySink sink;
    ReliableMessaging rm(store, sink, clock, kLeaseMs);

    std::mt19937 rng(42);
    const std::vector<UserId> users = {kAlice, kBob, kCarol};
    const std::vector<std::string> contents = {"hi", "hello", "longer message text", "中文内容"};
    const std::vector<SessionIdentity> sessions = {
        SessionIdentity{kAlice, 1}, SessionIdentity{kBob, 1}, SessionIdentity{kCarol, 1},
        SessionIdentity{kAlice, 2}, SessionIdentity{kBob, 2}, SessionIdentity{kCarol, 2},
    };
    std::map<std::pair<uint64_t, std::string>, AcceptOutcome> identityByKey;
    std::map<std::pair<uint64_t, std::string>, SendMessageCommand> commandsByKey;
    // 群消息同一 messageId 有多个接收者 Delivery，按 (messageId, recipient) 记录已确认。
    std::set<std::pair<uint64_t, uint64_t> > ackedDeliveries;

    auto randomSession = [&rng, &sessions]() -> const SessionIdentity& {
        return sessions[rng() % sessions.size()];
    };
    auto randomCommand = [&rng, &users, &contents]() {
        SendMessageCommand cmd;
        cmd.clientMessageId = ClientMessageId("k" + std::to_string(rng() % 400));
        cmd.content = contents[rng() % contents.size()];
        if (rng() % 2 == 0) {
            cmd.kind = SendMessageCommand::Kind::Direct;
            size_t si = rng() % users.size();
            size_t ri = (si + 1 + rng() % (users.size() - 1)) % users.size();
            cmd.directRecipient = users[ri];
        } else {
            cmd.kind = SendMessageCommand::Kind::Group;
            cmd.groupId = GroupId{1 + rng() % 3};
            cmd.members = users;
            std::shuffle(cmd.members.begin(), cmd.members.end(), rng);
            cmd.members.resize(1 + rng() % 3);
        }
        return cmd;
    };

    auto checkInvariants = [&sink, &ackedDeliveries](size_t fromIndex) {
        std::map<std::pair<uint64_t, uint64_t>, uint64_t> lastSeqByConversation;
        std::map<std::pair<uint64_t, uint64_t>, uint32_t> lastAttemptNumberByMessage;
        for (size_t i = 0; i < sink.attempts().size(); ++i) {
            const DeliveryAttempt& a = sink.attempts()[i];
            if (i >= fromIndex) {
                // 只检查本次 op 新增的 attempts：已确认 Delivery 不得再被尝试。
                ASSERT_FALSE(ackedDeliveries.count(std::make_pair(a.messageId.value, a.recipient.value)))
                    << "acked delivery must never be re-attempted"
                    << " mid=" << a.messageId.value << " rec=" << a.recipient.value
                    << " n=" << a.attemptNumber << " seq=" << a.sequence.value
                    << " conv=" << a.conversationId.value;
            }
            std::pair<uint64_t, uint64_t> convKey(a.recipient.value, a.conversationId.value);
            std::map<std::pair<uint64_t, uint64_t>, uint64_t>::iterator it = lastSeqByConversation.find(convKey);
            if (it != lastSeqByConversation.end()) {
                EXPECT_LE(it->second, a.sequence.value) << "per-(recipient,conversation) sequence must not regress";
            }
            lastSeqByConversation[convKey] = a.sequence.value;
            std::pair<uint64_t, uint64_t> msgKey(a.messageId.value, a.recipient.value);
            std::map<std::pair<uint64_t, uint64_t>, uint32_t>::iterator jt =
                lastAttemptNumberByMessage.find(msgKey);
            if (jt == lastAttemptNumberByMessage.end()) {
                EXPECT_EQ(1u, a.attemptNumber) << "first attempt must be number 1";
            } else {
                EXPECT_EQ(jt->second + 1, a.attemptNumber) << "attempt numbers must increase by one";
            }
            lastAttemptNumberByMessage[msgKey] = a.attemptNumber;
        }
    };

    for (int step = 0; step < 500; ++step) {
        const size_t attemptsBefore = sink.attempts().size();
        switch (rng() % 6) {
        case 0:
        case 1: {
            SendMessageCommand cmd;
            bool reuse = !identityByKey.empty() && (rng() % 3 == 0);
            if (reuse) {
                std::map<std::pair<uint64_t, std::string>, SendMessageCommand>::iterator it =
                    commandsByKey.begin();
                std::advance(it, static_cast<long>(rng() % commandsByKey.size()));
                cmd = it->second;
                if (rng() % 2 == 0) {
                    cmd.content += "-mutated";
                }
            } else {
                cmd = randomCommand();
            }
            SessionIdentity sender = randomSession();
            AcceptOutcome out = rm.accept(sender, cmd);
            std::pair<uint64_t, std::string> key(sender.userId.value, cmd.clientMessageId.value());
            std::map<std::pair<uint64_t, std::string>, AcceptOutcome>::iterator existing =
                identityByKey.find(key);
            if (out.ok) {
                if (out.duplicate) {
                    ASSERT_NE(existing, identityByKey.end()) << "duplicate requires a prior accept";
                    EXPECT_EQ(existing->second.messageId.value, out.messageId.value);
                    EXPECT_EQ(existing->second.conversationId.value, out.conversationId.value);
                    EXPECT_EQ(existing->second.sequence.value, out.sequence.value);
                    EXPECT_TRUE(samePayload(commandsByKey[key], cmd)) << "duplicate requires same payload";
                } else {
                    ASSERT_EQ(existing, identityByKey.end())
                        << "same key must never be accepted twice as fresh";
                    identityByKey[key] = out;
                    commandsByKey[key] = cmd;
                }
            } else {
                ASSERT_EQ(AcceptError::IdempotencyConflict, out.error);
                ASSERT_NE(existing, identityByKey.end());
                EXPECT_FALSE(samePayload(commandsByKey[key], cmd)) << "conflict requires different payload";
            }
            break;
        }
        case 2:
            rm.sessionAvailable(randomSession());
            break;
        case 3:
            rm.sessionClosed(randomSession());
            break;
        case 4: {
            if (identityByKey.empty()) {
                break;
            }
            std::map<std::pair<uint64_t, std::string>, AcceptOutcome>::iterator it =
                identityByKey.begin();
            std::advance(it, static_cast<long>(rng() % identityByKey.size()));
            MessageId mid = it->second.messageId;
            const SessionIdentity& acker = randomSession();
            AckOutcome out = rm.acknowledge(acker, mid);
            if (out.result == AckResult::Acknowledged) {
                ackedDeliveries.insert(std::make_pair(mid.value, acker.userId.value));
            } else if (out.result == AckResult::Idempotent) {
                EXPECT_TRUE(ackedDeliveries.count(std::make_pair(mid.value, acker.userId.value)))
                    << "idempotent ack requires prior ack";
            }
            break;
        }
        default:
            clock.advance(static_cast<int64_t>(rng() % (kLeaseMs * 3)));
            break;
        }
        checkInvariants(attemptsBefore);
    }
}
