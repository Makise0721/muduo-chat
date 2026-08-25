// P5-03A RED：批量 ACK 合并契约测试（引用尚不存在的
// MessageStore::updateDeliveries(const std::vector<Delivery>&) → 编译失败即合法 RED）。
//
// 非构建目标（P5-03A FAILED/reverted，updateDeliveries 不存在）；保留为批量 ACK 候选规格。
//
// 用例（契约语义，只穿公开 interface，不读 adapter 内部）：
//   - BatchAckAppliesAll              ：一批 N 条 ACK 一次提交全部生效
//   - BatchAckIdempotent              ：重复批量幂等（无副作用、不重复）
//   - BatchAckPreservesConversationOrder：批量内 acknowledgedAtMs 单调（顺序保持）
//   - BatchAckEquivalentToIndividual  ：批量与逐条 updateDelivery 状态机语义等价
//   - BatchAckPartialFailureAtomic    ：部分失败无半态（若实现不支持原子则登记约束）
//   - BatchAckConcurrentSafe          ：并发批量无丢失
//
// InMemory 与 MySQL 双 adapter 各以真实 store 实例跑 runBatchedAckContract
// （沿 ReliableMessagingContract 双跑先例）。

#include "BatchedAckContract.hpp"

#include "app/InMemoryMessageStore.hpp"
#include "app/MySQLMessageStore.hpp"

#include "MySqlTestFixture.hpp"

#include "db/SchemaMigration.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

const UserId kAlice{1};
const UserId kBob{2};
const UserId kCarol{3};

SendMessageCommand directTo(UserId recipient, const std::string& cmid, const std::string& content)
{
    SendMessageCommand cmd;
    cmd.clientMessageId = ClientMessageId(cmid);
    cmd.kind = SendMessageCommand::Kind::Direct;
    cmd.directRecipient = recipient;
    cmd.content = content;
    return cmd;
}

// 从持久层回读单条 Delivery（按 messageId + recipient）。返回是否存在。
bool readDelivery(MessageStore& store, MessageId mid, UserId recipient, Delivery* out)
{
    std::vector<Delivery> rows = store.deliveriesByMessage(mid);
    for (size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].recipient.value == recipient.value) {
            *out = rows[i];
            return true;
        }
    }
    return false;
}

}  // namespace

namespace batched_ack_contract {

std::vector<Delivery> seedPendingDeliveries(MessageStore& store, UserId recipient, unsigned n)
{
    std::vector<Delivery> out;
    for (unsigned i = 0; i < n; ++i) {
        const std::string cmid = "batch-" + std::to_string(recipient.value) + "-" +
                                 std::to_string(i);
        Message draft;
        draft.senderId = kAlice;
        draft.command = directTo(recipient, cmid, "payload-" + std::to_string(i));
        // 先经 getOrCreateConversation 建 Conversation 行（MySQL insertMessage 的
        // `SELECT ... FOR UPDATE` 需要 Conversation 行存在，否则 "conversation row
        // missing"；InMemory 无此约束但调用无害）。同 (sender,recipient) 幂等复用。
        draft.conversationId =
            store.getOrCreateConversation(SessionIdentity{kAlice, 1}, draft.command);
        Message m = store.insertMessage(draft);
        Delivery d;
        d.messageId = m.id;
        d.conversationId = m.conversationId;
        d.recipient = recipient;
        d.state = DeliveryState::Pending;
        store.insertDelivery(d);
        out.push_back(d);
    }
    return out;
}

void runBatchedAckContract(MessageStore& store, UserId recipient, UserId other)
{
    const int64_t t0 = 1000000;
    const unsigned N = 4;

    // --- BatchAckAppliesAll + PreservesConversationOrder ---
    std::vector<Delivery> batch = seedPendingDeliveries(store, recipient, N);
    ASSERT_EQ(N, batch.size());
    for (unsigned i = 0; i < batch.size(); ++i) {
        batch[i].state = DeliveryState::Acknowledged;
        batch[i].acknowledgedAtMs = t0 + static_cast<int64_t>(i) * 1000;  // 严格递增 + 秒对齐
    }
    // 批量 ACK seam（RED：当前不存在 → 编译失败即合法 RED）。
    store.updateDeliveries(batch);

    // 全部生效 + 顺序保持：逐条回读，acknowledgedAtMs 与入批顺序一致（单调递增）。
    int64_t prev = -1;
    for (unsigned i = 0; i < batch.size(); ++i) {
        Delivery got;
        ASSERT_TRUE(readDelivery(store, batch[i].messageId, recipient, &got))
            << "delivery " << i << " missing after batch ack";
        EXPECT_EQ(DeliveryState::Acknowledged, got.state) << "delivery " << i;
        EXPECT_EQ(batch[i].acknowledgedAtMs, got.acknowledgedAtMs) << "delivery " << i;
        EXPECT_GT(got.acknowledgedAtMs, prev) << "order violated at " << i;
        prev = got.acknowledgedAtMs;
    }

    // --- BatchAckIdempotent ---
    // 对已 Acknowledged 的行重复批量（同 state/acknowledgedAtMs）无副作用：状态与
    // 时间戳不变，acknowledgedAtMs 不被清空/重置。
    store.updateDeliveries(batch);
    for (unsigned i = 0; i < batch.size(); ++i) {
        Delivery got;
        ASSERT_TRUE(readDelivery(store, batch[i].messageId, recipient, &got));
        EXPECT_EQ(DeliveryState::Acknowledged, got.state);
        EXPECT_EQ(batch[i].acknowledgedAtMs, got.acknowledgedAtMs);
    }

    // --- BatchAckEquivalentToIndividual ---
    // 另一组 Pending 行：一半走批量 updateDeliveries、一半走逐条 updateDelivery，
    // 状态机结果（state / acknowledgedAtMs）必须一致。
    std::vector<Delivery> indiv = seedPendingDeliveries(store, other, N);
    ASSERT_EQ(N, indiv.size());
    for (unsigned i = 0; i < indiv.size(); ++i) {
        indiv[i].state = DeliveryState::Acknowledged;
        indiv[i].acknowledgedAtMs = t0 + 5000 + static_cast<int64_t>(i) * 1000;
    }
    std::vector<Delivery> batchHalf, indivHalf;
    for (unsigned i = 0; i < indiv.size(); ++i) {
        (i % 2 == 0 ? batchHalf : indivHalf).push_back(indiv[i]);
    }
    store.updateDeliveries(batchHalf);
    for (size_t i = 0; i < indivHalf.size(); ++i) {
        store.updateDelivery(indivHalf[i]);
    }
    for (unsigned i = 0; i < indiv.size(); ++i) {
        Delivery got;
        ASSERT_TRUE(readDelivery(store, indiv[i].messageId, other, &got));
        EXPECT_EQ(DeliveryState::Acknowledged, got.state) << "delivery " << i;
        EXPECT_EQ(indiv[i].acknowledgedAtMs, got.acknowledgedAtMs) << "delivery " << i;
    }

    // --- BatchAckPartialFailureAtomic ---
    // 入批含一条 not-recipient 的越权 Delivery（recipient != 批主 recipient）——
    // 批量路径必须原子处理：要么全部生效（该越权行一并忽略/不产生半态），
    // 要么整体失败；绝不出现"批内部分行已 Acknowledged、另一部分保持 Pending"。
    // 当前实现不支持原子则此处登记约束（由实现阶段决定 skip 或断言）。
    {
        std::vector<Delivery> badBatch = seedPendingDeliveries(store, other, 1);
        badBatch[0].recipient = UserId{9999};  // 越权/不存在接收者
        badBatch[0].state = DeliveryState::Acknowledged;
        // 批量实现须不静默丢行、不产生半态；本用例标记为契约面（见实现阶段注释）。
        EXPECT_NO_THROW(store.updateDeliveries(badBatch));
    }

    // --- BatchAckConcurrentSafe ---
    // 并发对同一批行调用批量 ACK：无丢失（最终全部 Acknowledged）。两线程各自
    // 提交互不重叠的子集，或重叠提交同一批——最终状态必为 Acknowledged。
    {
        std::vector<Delivery> conc = seedPendingDeliveries(store, recipient, N);
        for (size_t i = 0; i < conc.size(); ++i) {
            conc[i].state = DeliveryState::Acknowledged;
            conc[i].acknowledgedAtMs = t0 + 9000 + static_cast<int64_t>(i) * 1000;
        }
        std::thread a([&] { store.updateDeliveries(conc); });
        std::thread b([&] { store.updateDeliveries(conc); });
        a.join();
        b.join();
        for (size_t i = 0; i < conc.size(); ++i) {
            Delivery got;
            ASSERT_TRUE(readDelivery(store, conc[i].messageId, recipient, &got));
            EXPECT_EQ(DeliveryState::Acknowledged, got.state) << "concurrent " << i;
        }
    }
}

}  // namespace batched_ack_contract

namespace {

// ---- InMemory adapter 契约跑 ----
TEST(BatchedAckContractTest, InMemoryAdapterSatisfiesContract)
{
    InMemoryMessageStore store;
    batched_ack_contract::runBatchedAckContract(store, kBob, kCarol);
}

// ---- MySQL adapter 契约跑（真实 chat_test 库，不 skip；沿 MySQLReliableMessagingTest）----
class BatchedAckContractMySqlTest : public ::testing::Test {
protected:
    static void SetUpTestSuite()
    {
        MySqlTestFixture::resetSchema();
        // seed users（MessageDelivery 有 User FK）
        MySQL conn;
        ASSERT_TRUE(conn.connect("127.0.0.1", "root", MySqlTestFixture::password(),
                                 "chat_test", 3306));
        ASSERT_TRUE(conn.update("INSERT INTO User(id,name,password) VALUES "
                                "(1,'u1','x'),(2,'u2','x'),(3,'u3','x'),(9999,'ux','x')"));
    }

    void SetUp() override { (void)MySqlTestFixture::pool(); }
};

TEST_F(BatchedAckContractMySqlTest, MySqlAdapterSatisfiesContract)
{
    MySQLMessageStore store(MySqlTestFixture::pool(), 100);
    batched_ack_contract::runBatchedAckContract(store, kBob, kCarol);
}

}  // namespace
