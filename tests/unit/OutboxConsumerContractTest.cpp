// P4-04 OutboxConsumerContract RED：幂等 Delivery consumer 的公开接口契约
// （docs/tasks/P4-04.md §Interface/§RED/§冻结参数/§设计决定 D1-D6）。
//
// RED 依据（现状）：本文件引用尚不存在的 app/OutboxEventConsumer.hpp（驱动/处理
// 双 port + ConsumedOutboxRecord/ConsumeDisposition/OutboxConsumeResult 值类型）、
// app/KafkaEventConsumer.hpp（Kafka adapter）、app/WakeupProgressHandler.hpp
//（in-process 处理 adapter），以及 MessageStore port 尚不存在的 dead-letter 扩展
//（DeadLetterRecord/recordDeadLetter/deadLetters）→ 编译失败
//（`app/OutboxEventConsumer.hpp: No such file or directory`）即合法 RED
//（P4-04 卡 §RED：预期失败 = 类型不存在 → 编译失败，沿 P4-02/P4-03 先例）。
//
// 真实 Kafka 集成（127.0.0.1:9092，env KAFKA_TEST_HOST/PORT 可覆盖，沿 P4-03
// 惯例），**不 skip**：SetUp 连不上 broker 即测试失败（P4-04 完成定义）。
// 测试 topic 固定 `muduo-test-consume-outbox`（冻结参数，partitions=3/
// replication=1；fixture SetUp deleteTopic→createTopic 复位——topic 重建即
// offset 复位，与 P4-03 契约 topic `muduo-test-chat-outbox` 互扰隔离）；
// group id 固定 `muduo-test-consume-group`（冻结参数）。
//
// 本文件冻结的实现契约（GREEN 据此实现；卡 §Interface 签名草案的定案细化）：
//
//   // ---- chatserver/include/app/OutboxEventConsumer.hpp（新建）----
//   struct ConsumedOutboxRecord {
//       std::string topic;
//       int32_t partition = 0;
//       int64_t offset = 0;
//       uint64_t messageId = 0;
//       uint64_t conversationId = 0;
//       uint64_t sequence = 0;
//       std::string eventType;   // 只接受 "MessageAccepted"
//       std::string payload;     // 命令快照 JSON（P3-04 冻结编码）
//   };
//   enum class ConsumeDisposition { Advanced, DuplicateNoOp, DeadLettered };
//   class DeliveryProgressHandler {
//   public:
//       virtual ~DeliveryProgressHandler() = default;
//       virtual ConsumeDisposition handle(const ConsumedOutboxRecord& record) = 0;
//   };
//   struct OutboxConsumeResult {
//       std::vector<ConsumedOutboxRecord> records;     // 本批拉到的事件
//       std::vector<ConsumeDisposition> dispositions;  // 与 records 一一对应
//       bool processed = false;        // 本批全部终态（含 dead-letter 落库）
//       bool offsetCommitted = false;  // 仅在 processed 后才可能 true
//       std::map<int32_t, int64_t> committedThrough;   // partition → 已提交 offset+1
//       bool brokerOk = true;          // false = 本轮 fetch 阶段 broker 故障（无副作用）
//   };
//   class OutboxEventConsumer {
//   public:
//       virtual ~OutboxEventConsumer() = default;
//       virtual OutboxConsumeResult poll(int64_t deadlineMs) = 0;  // 不抛
//   };
//
//   // ---- chatserver/include/app/KafkaEventConsumer.hpp（新建）----
//   class KafkaEventConsumer : public OutboxEventConsumer {
//   public:
//       // 构造注入（卡 D1：host/port/topic/groupId/fetchBatchLimit/deadlineMs 同
//       // KafkaPublisher 形态）+ dead-letter 落库 store + 处理面 handler。
//       // P4-06 单测注入：seenConversationsCapacity 默认 0 = 冻结常量
//       // kSeenConversationsCapacity=100（容量驱逐单测以 0 以外小值注入）。
//       KafkaEventConsumer(const std::string& host, int port, const std::string& topic,
//                          const std::string& groupId, MessageStore& deadLetterStore,
//                          DeliveryProgressHandler& handler, uint32_t fetchBatchLimit,
//                          int64_t deadlineMs, size_t seenConversationsCapacity = 0);
//       OutboxConsumeResult poll(int64_t deadlineMs) override;
//       // preCommitHook（适配器具体 seam，不进 port；P4-01 injectFailure 先例）：
//       // OffsetCommit 发出前回调；回调抛出 = 模拟"处理后 commit 前 kill"，
//       // 本批不提交（processed 仍可为 true：批已全部终态）。
//       void setPreCommitHook(std::function<void()> hook);
//   };
//   // 行为（D1/D2/冻结参数）：
//   //   启动恢复 = OffsetFetch(group, topic) 读回；无提交历史（-1/分区未知）→
//   //   ListOffsets(-2) earliest；越界（高于 log-end）/OFFSET_OUT_OF_RANGE/
//   //   UNKNOWN_TOPIC_OR_PARTITION → earliest 回退（M-2）。
//   //   manual assign topic 全部分区；每 poll：fetch（批 ≤ fetchBatchLimit，
//   //   maxBytes 1MiB/分区，maxWait 300ms）→ 逐 record 处置 → 全部终态后
//   //   preCommitHook → OffsetCommit（simple-consumer 形态：generation=-1、
//   //   member_id 空串，commit deadline 5000ms）→ offsetCommitted=true、
//   //   committedThrough[partition] = 本批该分区最后 offset+1。
//   //   逐 record 处置顺序（全部经 MessageStore dead-letter port 落库，绝不只日志）：
//   //     1) 信封非 JSON/缺字段/message_id/conversation_id/sequence 类型错，或
//   //        payload 字段非合法 JSON 对象 → DeadLettered reason=poison_payload
//   //        （handler 不被调用）。
//   //     2) eventType != "MessageAccepted" → DeadLettered reason=unknown_event_type
//   //        （handler 不被调用）。
//   //     3) 同 (conversationId, messageId) 已见（先前已处置成功 Advanced/
//   //        DuplicateNoOp）→ DuplicateNoOp（H 修复：置于 seq<lastSeen 回归规则
//   //        之前——未提交批重放时，先前已 Advanced 的 record 不能被 lastSeen 已
//   //        推进的 seq 误判 sequence_regression；handler 不被调用）。
//   //     4) sequence < lastSeen[conversationId] → DeadLettered
//   //        reason=sequence_regression（不推进、lastSeen 不变、handler 不被调用）。
//   //     5) sequence == lastSeen[conversationId] 且 messageId 不同 → DeadLettered
//   //        reason=sequence_conflict（防御断言；handler 不被调用）。
//   //     6) 其余 → handler.handle(record)，disposition 取返回值；处置成功（非
//   //        DeadLettered）则记入 seenMessages；lastSeen[conversationId] =
//   //        max(lastSeen, record.sequence)。
//   //   lastSeen 为 per-conversation 内存态（重启重建，不做持久化，D2）。
//   //   9) handler 抛出（store 瞬时异常面）：poll 不抛；该批不提交 offset
//   //      （broker 停留上一提交点）；下轮 poll 重放同批；重放成功后无重复
//   //      副作用（幂等由规则 3 已见 DuplicateNoOp/UNIQUE 兜底）；lastSeen 在
//   //      处置成功时更新，批中止保留已更新值（重放安全由规则 3 同
//   //      (conversationId,messageId) 已见分支保证——L-3 一并钉死）。
//   //   空批：records/dispositions 空、processed=true（空批无未终态事件）、
//   //   offsetCommitted=false、committedThrough 空、brokerOk=true。
//   //   broker 不可达（连接拒绝/断连/Blackhole）：brokerOk=false、records 空、
//   //   无任何副作用、poll 不抛、deadline 有界。fetch 成功而 commit 传输失败
//   //   （代理拆链）：brokerOk=true、processed=true、offsetCommitted=false。
//
//   // ---- chatserver/include/app/WakeupProgressHandler.hpp（新建）----
//   class WakeupProgressHandler : public DeliveryProgressHandler {
//   public:
//       WakeupProgressHandler(MessageStore& store, ReliableMessaging& rm);
//       // D2：findMessage(messageId) → 缺行 dead-letter（reason=message_missing，
//       // 经 store.recordDeadLetter）→ outboxRecipientsFor(msg->command)
//       //（LocalWakeupPublisher.hpp 公开 inline）→ rm.wakeupAccepted(recipients)。
//       // 绝不调 insertMessage/insertDelivery/accept（只推进不重建）。
//       // 返回：wakeup 使 Delivery 状态/attemptCount 前进 → Advanced；
//       // 无变化（同 message_id 重放被 claimFor fencing）→ DuplicateNoOp。
//       ConsumeDisposition handle(const ConsumedOutboxRecord& record) override;
//   };
//
//   // ---- chatserver/include/app/MessageStore.hpp 扩展（默认 no-op，沿 P3-09 模式；
//   //      InMemory/MySQL override，契约双跑）----
//   struct DeadLetterRecord {
//       std::string topic;
//       int32_t partitionId = 0;
//       int64_t kafkaOffset = 0;
//       uint64_t messageId = 0;        // 信封 message_id（不可解析时 0）
//       uint64_t conversationId = 0;
//       uint64_t sequence = 0;
//       std::string eventType;
//       std::string reason;   // poison_payload|unknown_event_type|sequence_regression|
//                              // sequence_conflict|message_missing
//       std::string rawValue;  // 原始 record bytes（信封原文）；message_missing 为
//                               // handler 侧重建文本（handler 无原始 bytes）
//   };
//   // 幂等落库：UNIQUE(topic,partition_id,kafka_offset) 冲突 = 已存在，成功返回。
//   virtual void recordDeadLetter(const DeadLetterRecord& r) { (void)r; }
//   // 可查询谓词（消费侧 poison 绝不只日志丢弃的证据面），最多 limit 行。
//   virtual std::vector<DeadLetterRecord> deadLetters(uint64_t limit)
//   { (void)limit; return {}; }
//
// 测试自身实现最小 Kafka wire 客户端（Metadata v1/ListOffsets v1/CreateTopics v2/
// DeleteTopics v1/OffsetFetch v2，P4-03 KafkaTestConsumer 同源精简——fetch/record
// batch 解析归被测 KafkaEventConsumer）+ BlackholeServer/KafkaTestProxy（P4-03 形态
// 原样：跨线程 listener/running 全 atomic、析构 join 覆盖）。
//
// 消费侧 harness 的"recipient 离线"建模：sink 返回 Closed（P3-07 Closed 语义 =
// 连接未就绪、保留 Pending、不记 attempt）——accept 时 bob 在线但 sink Closed →
// Delivery 停留 Pending；poll 时 sink 切 Accepted → Pending→InFlight 可归因于
// consumer 的 wakeup（claimFor 只对 active 会话触发，真离线下 wakeup 无效）。
//
// 无固定 sleep：broker 等待全部有界轮询（deadline 上限）；MySQL 断言直连 SQL。

#include "app/OutboxEventConsumer.hpp"   // RED：尚不存在 → 编译失败即合法 RED
#include "app/KafkaEventConsumer.hpp"    // RED：尚不存在 → 编译失败即合法 RED
#include "app/WakeupProgressHandler.hpp" // RED：尚不存在 → 编译失败即合法 RED

#include "app/DeliverySink.hpp"
#include "app/DomainTypes.hpp"
#include "app/InMemoryMessageStore.hpp"
#include "app/KafkaPublisher.hpp"
#include "app/MessageStore.hpp"  // DeadLetterRecord/recordDeadLetter/deadLetters：RED（P4-04 扩展尚不存在）
#include "app/MySQLMessageStore.hpp"
#include "app/OutboxPublisher.hpp"
#include "app/ReliableMessaging.hpp"
#include "db/SchemaMigration.hpp"

#include "FakeClock.hpp"
#include "MySqlTestFixture.hpp"

#include <gtest/gtest.h>
#include <mysql/mysql.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

// ---- P4-04 冻结参数（§冻结参数，RED 前定案）----

const char* kTestTopic = "muduo-test-consume-outbox";  // 固定测试 topic（非 pid 后缀）
const char* kGroupId = "muduo-test-consume-group";
const int32_t kPartitions = 3;
const uint32_t kFetchBatchLimit = 100;  // 单轮 poll 事件数上限
const int64_t kApiTimeoutMs = 3000;
const int64_t kReadDeadlineMs = 8000;
const int64_t kPollDeadlineMs = 10000;
const int64_t kConsumeDeadlineMs = 5000;  // commit deadline（对齐 P4-03 publish）

const UserId kAlice{1};
const UserId kBob{2};
const UserId kCarol{3};
const UserId kDave{4};
const UserId kEve{5};
const UserId kFrank{6};
const UserId kGrace{7};
const int64_t kNow = 1000000;
// Delivery lease 显著大于测试时长（FakeClock 冻结 → lease 永不过期，重放 fencing 确定）。
const uint64_t kLeaseMs = 10000;
const uint64_t kFanOutCap = 100;

std::string testHost()
{
    const char* h = getenv("KAFKA_TEST_HOST");
    return h ? std::string(h) : std::string("127.0.0.1");
}

int testPort()
{
    const char* p = getenv("KAFKA_TEST_PORT");
    return p ? std::atoi(p) : 9092;
}

int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

RetryConfig consumeRetryConfig()
{
    RetryConfig c;
    c.cleanupCycleMs = 0;  // 不触发 Expired/cleanup，隔离消费关注点
    return c;
}

SendMessageCommand directTo(UserId recipient, const std::string& cmid, const std::string& content)
{
    SendMessageCommand cmd;
    cmd.clientMessageId = ClientMessageId(cmid);
    cmd.kind = SendMessageCommand::Kind::Direct;
    cmd.directRecipient = recipient;
    cmd.content = content;
    return cmd;
}

// P3-04 冻结编码的合法命令快照 JSON（payload 对 consumer 只要求"合法 JSON 对象"）。
std::string payloadJson(const std::string& cmid)
{
    return std::string("{\"kind\":\"direct\",\"cmid\":\"") + cmid + "\"}";
}

// ---- 发布侧值构造（真实 KafkaPublisher 注入事件，沿 P4-03 makeRequest）----

uint64_t nextRequestId()
{
    static std::atomic<uint64_t> counter{0};
    return counter.fetch_add(1) + 1;
}

// 真实 accept 结果 → 信封请求（sequence/conversationId 用真实值）。
OutboxPublishRequest envelopeFor(const AcceptOutcome& a, const std::string& topic)
{
    OutboxPublishRequest r;
    r.event.id = nextRequestId();
    r.event.aggregateMessageId = a.messageId;
    r.event.eventType = "MessageAccepted";
    r.event.payload = payloadJson("env-" + std::to_string(a.messageId.value));
    r.event.availableAtMs = kNow;
    r.event.attemptCount = 1;
    r.event.processedAtMs = 0;
    r.conversationId = a.conversationId;
    r.sequence = a.sequence.value;
    r.topic = topic;
    return r;
}

// 手工注入信封（乱序/poison/缺行场景：字段可任意编造）。
OutboxPublishRequest craftedRequest(uint64_t messageId, uint64_t conversationId,
                                    uint64_t sequence, const std::string& eventType,
                                    const std::string& payload, const std::string& topic)
{
    OutboxPublishRequest r;
    r.event.id = nextRequestId();
    r.event.aggregateMessageId = MessageId(messageId);
    r.event.eventType = eventType;
    r.event.payload = payload;
    r.event.availableAtMs = kNow;
    r.event.attemptCount = 1;
    r.event.processedAtMs = 0;
    r.conversationId = ConversationId(conversationId);
    r.sequence = sequence;
    r.topic = topic;
    return r;
}

void publishOne(KafkaPublisher& publisher, const OutboxPublishRequest& req)
{
    std::vector<OutboxPublishRequest> batch;
    batch.push_back(req);
    std::vector<OutboxPublishOutcome> out = publisher.publish(batch, 5000);
    ASSERT_EQ(1u, out.size());
    ASSERT_TRUE(out[0].ok) << "publish failed: " << out[0].error;
}

// ---- Kafka wire 最小客户端（P4-03 KafkaTestConsumer 同源精简 + OffsetFetch v2）----

class TcpClient {
public:
    TcpClient(const std::string& host, int port, int64_t connectTimeoutMs)
        : host_(host), port_(port), connectTimeoutMs_(connectTimeoutMs)
    {
    }
    ~TcpClient() { close(); }

    bool connect()
    {
        if (fd_ >= 0) {
            return true;
        }
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) {
            return false;
        }
        struct sockaddr_in addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(port_));
        if (::inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) != 1) {
            close();
            return false;
        }
        setNonBlocking(true);
        int rc = ::connect(fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
        if (rc != 0 && errno != EINPROGRESS) {
            close();
            return false;
        }
        if (rc != 0) {
            struct pollfd p;
            p.fd = fd_;
            p.events = POLLOUT;
            p.revents = 0;
            rc = ::poll(&p, 1, static_cast<int>(connectTimeoutMs_));
            if (rc <= 0 || (p.revents & (POLLERR | POLLHUP)) != 0) {
                close();
                return false;
            }
        }
        setNonBlocking(false);
        return true;
    }

    bool sendAll(const char* data, size_t len, int64_t timeoutMs)
    {
        if (fd_ < 0) {
            return false;
        }
        size_t sent = 0;
        while (sent < len) {
            struct pollfd p;
            p.fd = fd_;
            p.events = POLLOUT;
            p.revents = 0;
            if (::poll(&p, 1, static_cast<int>(timeoutMs)) <= 0) {
                return false;
            }
            ssize_t n = ::send(fd_, data + sent, len - sent, MSG_NOSIGNAL);
            if (n <= 0) {
                return false;
            }
            sent += static_cast<size_t>(n);
        }
        return true;
    }

    bool recvExact(char* buf, size_t len, int64_t timeoutMs)
    {
        if (fd_ < 0) {
            return false;
        }
        size_t got = 0;
        while (got < len) {
            struct pollfd p;
            p.fd = fd_;
            p.events = POLLIN;
            p.revents = 0;
            if (::poll(&p, 1, static_cast<int>(timeoutMs)) <= 0) {
                return false;
            }
            ssize_t n = ::recv(fd_, buf + got, len - got, 0);
            if (n <= 0) {
                return false;
            }
            got += static_cast<size_t>(n);
        }
        return true;
    }

    void close()
    {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

private:
    void setNonBlocking(bool nb)
    {
        int flags = ::fcntl(fd_, F_GETFL, 0);
        if (flags < 0) {
            return;
        }
        if (nb) {
            flags |= O_NONBLOCK;
        } else {
            flags &= ~O_NONBLOCK;
        }
        (void)::fcntl(fd_, F_SETFL, flags);
    }

    std::string host_;
    int port_ = 0;
    int64_t connectTimeoutMs_ = 3000;
    int fd_ = -1;
};

class ByteWriter {
public:
    void i8(int8_t v) { buf_.append(1, static_cast<char>(v)); }
    void bool_(bool b) { i8(b ? 1 : 0); }
    void i16(int16_t v)
    {
        char b[2];
        b[0] = static_cast<char>((v >> 8) & 0xFF);
        b[1] = static_cast<char>(v & 0xFF);
        buf_.append(b, 2);
    }
    void i32(int32_t v)
    {
        char b[4];
        b[0] = static_cast<char>((v >> 24) & 0xFF);
        b[1] = static_cast<char>((v >> 16) & 0xFF);
        b[2] = static_cast<char>((v >> 8) & 0xFF);
        b[3] = static_cast<char>(v & 0xFF);
        buf_.append(b, 4);
    }
    void i64(int64_t v)
    {
        char b[8];
        for (int i = 0; i < 8; ++i) {
            b[i] = static_cast<char>((v >> (56 - 8 * i)) & 0xFF);
        }
        buf_.append(b, 8);
    }
    void str(const std::string& s)
    {
        i16(static_cast<int16_t>(s.size()));
        buf_.append(s);
    }
    const std::string& data() const { return buf_; }

private:
    std::string buf_;
};

class ByteReader {
public:
    explicit ByteReader(const std::string& data) : data_(data) {}

    bool i8(int8_t* out)
    {
        if (remaining() < 1) {
            return false;
        }
        *out = static_cast<int8_t>(data_[pos_]);
        pos_ += 1;
        return true;
    }
    bool bool_(bool* out)
    {
        int8_t v;
        if (!i8(&v)) {
            return false;
        }
        *out = v != 0;
        return true;
    }
    bool i16(int16_t* out)
    {
        if (remaining() < 2) {
            return false;
        }
        *out = static_cast<int16_t>((uint8_t(data_[pos_]) << 8) | uint8_t(data_[pos_ + 1]));
        pos_ += 2;
        return true;
    }
    bool i32(int32_t* out)
    {
        if (remaining() < 4) {
            return false;
        }
        *out = (uint8_t(data_[pos_]) << 24) | (uint8_t(data_[pos_ + 1]) << 16)
            | (uint8_t(data_[pos_ + 2]) << 8) | uint8_t(data_[pos_ + 3]);
        pos_ += 4;
        return true;
    }
    bool i64(int64_t* out)
    {
        if (remaining() < 8) {
            return false;
        }
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) {
            v = (v << 8) | uint8_t(data_[pos_ + i]);
        }
        pos_ += 8;
        *out = static_cast<int64_t>(v);
        return true;
    }
    bool str(std::string* out)
    {
        int16_t len;
        if (!i16(&len) || len < 0) {
            return false;
        }
        if (remaining() < static_cast<size_t>(len)) {
            return false;
        }
        out->assign(data_, pos_, static_cast<size_t>(len));
        pos_ += static_cast<size_t>(len);
        return true;
    }
    bool nullableStr(std::string* out, bool* isNull)
    {
        int16_t len;
        if (!i16(&len)) {
            return false;
        }
        if (len < 0) {
            *isNull = true;
            out->clear();
            return true;
        }
        *isNull = false;
        if (remaining() < static_cast<size_t>(len)) {
            return false;
        }
        out->assign(data_, pos_, static_cast<size_t>(len));
        pos_ += static_cast<size_t>(len);
        return true;
    }
    void skip(size_t n) { pos_ += n; }
    size_t remaining() const { return data_.size() - pos_; }

private:
    std::string data_;
    size_t pos_ = 0;
};

// 单连接 Kafka wire 客户端：请求/响应帧（correlation 匹配）。
class KafkaConn {
public:
    static std::unique_ptr<KafkaConn> open(const std::string& host, int port)
    {
        std::unique_ptr<KafkaConn> c(new KafkaConn(host, port));
        if (!c->tcp_.connect()) {
            c.reset();
        }
        return c;
    }

    bool request(int16_t apiKey, int16_t apiVersion, const std::string& body,
                 int64_t timeoutMs, std::string* respBody)
    {
        ByteWriter hdr;
        hdr.i16(apiKey);
        hdr.i16(apiVersion);
        hdr.i32(correlation_);
        hdr.str("muduo-test");  // client id
        std::string payload = hdr.data() + body;
        if (payload.size() > 0x7FFFFFFF) {
            return false;
        }
        ByteWriter frame;
        frame.i32(static_cast<int32_t>(payload.size()));
        std::string wire = frame.data() + payload;
        if (!tcp_.sendAll(wire.data(), wire.size(), timeoutMs)) {
            return false;
        }
        char sz[4];
        if (!tcp_.recvExact(sz, 4, timeoutMs)) {
            return false;
        }
        int32_t respSize = (uint8_t(sz[0]) << 24) | (uint8_t(sz[1]) << 16)
            | (uint8_t(sz[2]) << 8) | uint8_t(sz[3]);
        if (respSize < 4) {
            return false;
        }
        std::string resp;
        resp.resize(static_cast<size_t>(respSize));
        if (!tcp_.recvExact(&resp[0], static_cast<size_t>(respSize), timeoutMs)) {
            return false;
        }
        int32_t corr = (uint8_t(resp[0]) << 24) | (uint8_t(resp[1]) << 16)
            | (uint8_t(resp[2]) << 8) | uint8_t(resp[3]);
        if (corr != correlation_) {
            return false;
        }
        ++correlation_;
        respBody->assign(resp, 4, resp.size() - 4);
        return true;
    }

private:
    explicit KafkaConn(const std::string& host, int port) : tcp_(host, port, kApiTimeoutMs)
    {
    }

    TcpClient tcp_;
    int32_t correlation_ = 0;
};

// 第三实例断言面（L-7）：Metadata v1 + ListOffsets v1 + CreateTopics v2 +
// DeleteTopics v1 + OffsetFetch v2（读回 group 已提交 offset，核验
// __consumer_offsets 与被测 consumer 的 committedThrough 一致）。
class KafkaTestWire {
public:
    explicit KafkaTestWire(const std::string& host, int port)
        : host_(host), port_(port), conn_(KafkaConn::open(host, port))
    {
        if (!conn_) {
            throw std::runtime_error("KafkaTestWire: connect failed (broker unavailable)");
        }
    }

    bool brokerReachable()
    {
        std::string resp;
        ByteWriter w;
        w.i32(1);
        w.str(std::string("muduo-test-probe-") + std::to_string(getpid()));
        return conn_->request(3, 1, w.data(), 3000, &resp);
    }

    std::vector<int32_t> partitions(const std::string& topic)
    {
        std::string resp;
        ByteWriter w;
        w.i32(1);
        w.str(topic);
        if (!conn_->request(3, 1, w.data(), kApiTimeoutMs, &resp)) {
            throw std::runtime_error("metadata request failed");
        }
        ByteReader r(resp);
        int32_t brokerCount = 0;
        if (!r.i32(&brokerCount)) {
            throw std::runtime_error("bad metadata brokers");
        }
        for (int32_t i = 0; i < brokerCount; ++i) {
            int32_t nodeId;
            std::string host;
            int32_t bport;
            std::string rack;
            bool nullRack;
            if (!r.i32(&nodeId) || !r.str(&host) || !r.i32(&bport)
                || !r.nullableStr(&rack, &nullRack)) {
                throw std::runtime_error("bad metadata broker entry");
            }
        }
        int32_t controllerId = 0;
        if (!r.i32(&controllerId)) {
            throw std::runtime_error("bad metadata controller");
        }
        int32_t topicCount = 0;
        if (!r.i32(&topicCount)) {
            throw std::runtime_error("bad metadata topics");
        }
        std::vector<int32_t> out;
        for (int32_t i = 0; i < topicCount; ++i) {
            int16_t err;
            std::string name;
            bool internal;
            int32_t pcount;
            if (!r.i16(&err) || !r.str(&name) || !r.bool_(&internal) || !r.i32(&pcount)) {
                throw std::runtime_error("bad metadata topic entry");
            }
            for (int32_t p = 0; p < pcount; ++p) {
                int16_t perr;
                int32_t idx;
                int32_t leader;
                int32_t rcount;
                if (!r.i16(&perr) || !r.i32(&idx) || !r.i32(&leader) || !r.i32(&rcount)) {
                    throw std::runtime_error("bad metadata partition");
                }
                for (int32_t k = 0; k < rcount; ++k) {
                    int32_t rid;
                    if (!r.i32(&rid)) {
                        throw std::runtime_error("bad metadata replicas");
                    }
                }
                int32_t icount;
                if (!r.i32(&icount)) {
                    throw std::runtime_error("bad metadata isr");
                }
                for (int32_t k = 0; k < icount; ++k) {
                    int32_t iid;
                    if (!r.i32(&iid)) {
                        throw std::runtime_error("bad metadata isr");
                    }
                }
                if (name == topic && perr == 0) {
                    out.push_back(idx);
                }
            }
            if (name == topic && err != 0) {
                throw std::runtime_error("metadata error for " + topic + ": "
                    + std::to_string(err));
            }
        }
        return out;
    }

    // 返回 per-topic error_code：0=NONE（成功）；36=TOPIC_ALREADY_EXISTS（调用方
    // 有界重删重试，见 fixture SetUp 抗 auto-create 竞态残余）；其它非零 = 失败
    // （由调用方断言）。传输失败/响应布局异常仍抛 runtime_error。
    int16_t createTopic(const std::string& topic, int32_t numPartitions, int16_t replication,
                        int32_t timeoutMs)
    {
        std::string resp;
        ByteWriter w;
        w.i32(1);              // topics
        w.str(topic);
        w.i32(numPartitions);
        w.i16(replication);
        w.i32(0);              // assignments
        w.i32(0);              // configs
        w.i32(timeoutMs);
        w.bool_(false);        // validateOnly
        if (!conn_->request(19, 2, w.data(), timeoutMs + kApiTimeoutMs, &resp)) {
            throw std::runtime_error("createTopic transport failed for " + topic);
        }
        ByteReader r(resp);
        int32_t throttle = 0;
        int32_t tc = 0;
        if (!r.i32(&throttle) || !r.i32(&tc)) {
            throw std::runtime_error("bad createTopic response");
        }
        int16_t out = 0;
        for (int32_t i = 0; i < tc; ++i) {
            std::string name;
            int16_t err;
            std::string emsg;
            bool null;
            if (!r.str(&name) || !r.i16(&err) || !r.nullableStr(&emsg, &null)) {
                throw std::runtime_error("bad createTopic result");
            }
            if (name == topic) {
                out = err;
            }
        }
        return out;
    }

    // 返回 DeleteTopics 的 per-topic error_code：0=NONE（删除受理——topic 仍在
    // 或删除传播中，KRaft 异步）；3=UNKNOWN_TOPIC_OR_PARTITION（已删净）；
    // -1=传输失败/响应布局异常（best-effort 语义保留）。DeleteTopics 对不存在
    // topic 不触发 auto-create（区别于 Metadata 探针——本 broker
    // auto.create.topics.enable=true 下 Metadata 会异步物化同名 1 分区 topic，
    // 见卡 GREEN 解阻记录），故兼作 waitTopicGone 的"已删净"探针。
    int16_t deleteTopic(const std::string& topic, int32_t timeoutMs)
    {
        if (topic.empty()) {
            return 0;
        }
        std::string resp;
        ByteWriter w;
        w.i32(1);              // topic names
        w.str(topic);
        w.i32(timeoutMs);
        // apiKey 20 = DeleteTopics（44 是 IncrementalAlterConfigs，误用会让 broker
        // 解析失败并留残留 topic——P4-03 教训）。
        if (!conn_->request(20, 1, w.data(), timeoutMs + kApiTimeoutMs, &resp)) {
            return -1;  // 清理 best-effort
        }
        ByteReader r(resp);
        int32_t throttle = 0;
        int32_t tc = 0;
        if (!r.i32(&throttle) || !r.i32(&tc)) {
            return -1;
        }
        for (int32_t i = 0; i < tc; ++i) {
            std::string name;
            int16_t err;
            if (!r.str(&name) || !r.i16(&err)) {
                return -1;
            }
            if (name == topic) {
                return err;
            }
        }
        return -1;
    }

    int64_t earliestOffset(const std::string& topic, int32_t partition)
    {
        std::string resp;
        ByteWriter w;
        w.i32(-1);             // replicaId
        w.i32(1);              // topics
        w.str(topic);
        w.i32(1);              // partitions
        w.i32(partition);
        w.i64(-2);             // timestamp = earliest
        if (!conn_->request(2, 1, w.data(), kApiTimeoutMs, &resp)) {
            throw std::runtime_error("listOffsets request failed");
        }
        ByteReader r(resp);
        int32_t tc = 0;
        if (!r.i32(&tc)) {
            throw std::runtime_error("bad listOffsets response");
        }
        for (int32_t i = 0; i < tc; ++i) {
            std::string name;
            int32_t pc;
            if (!r.str(&name) || !r.i32(&pc)) {
                throw std::runtime_error("bad listOffsets topic");
            }
            for (int32_t p = 0; p < pc; ++p) {
                int32_t idx;
                int16_t err;
                int64_t ts;
                int64_t off;
                if (!r.i32(&idx) || !r.i16(&err) || !r.i64(&ts) || !r.i64(&off)) {
                    throw std::runtime_error("bad listOffsets partition");
                }
                if (name == topic && idx == partition) {
                    if (err != 0) {
                        throw std::runtime_error("listOffsets error: " + std::to_string(err));
                    }
                    return off;
                }
            }
        }
        throw std::runtime_error("listOffsets: partition not found");
    }

    // OffsetFetch v2（apiKey 9）：读回 group 在 topic 各分区已提交 offset。
    // 返回该 topic 全部分区（无提交历史 = -1）。第三实例核验面（卡 RED ③/L-7）。
    std::map<int32_t, int64_t> fetchCommitted(const std::string& groupId,
                                              const std::string& topic)
    {
        std::vector<int32_t> parts = partitions(topic);
        std::map<int32_t, int64_t> out;
        for (size_t i = 0; i < parts.size(); ++i) {
            out[parts[i]] = -1;  // 无提交历史
        }
        std::string resp;
        ByteWriter w;
        w.str(groupId);
        w.i32(1);              // topics
        w.str(topic);
        w.i32(static_cast<int32_t>(parts.size()));
        for (size_t i = 0; i < parts.size(); ++i) {
            w.i32(parts[i]);
        }
        if (!conn_->request(9, 2, w.data(), kApiTimeoutMs, &resp)) {
            throw std::runtime_error("offsetFetch transport failed");
        }
        ByteReader r(resp);
        int32_t tc = 0;
        if (!r.i32(&tc)) {
            throw std::runtime_error("bad offsetFetch response");
        }
        for (int32_t i = 0; i < tc; ++i) {
            std::string name;
            int32_t pc;
            if (!r.str(&name) || !r.i32(&pc)) {
                throw std::runtime_error("bad offsetFetch topic");
            }
            for (int32_t p = 0; p < pc; ++p) {
                int32_t idx;
                int64_t off;
                std::string metadata;
                bool nullMeta;
                int16_t err;
                if (!r.i32(&idx) || !r.i64(&off) || !r.nullableStr(&metadata, &nullMeta)
                    || !r.i16(&err)) {
                    throw std::runtime_error("bad offsetFetch partition");
                }
                if (name == topic) {
                    if (err != 0) {
                        throw std::runtime_error("offsetFetch error: " + std::to_string(err));
                    }
                    out[idx] = off;
                }
            }
        }
        // v2 顶层 error_code（尾部 2 字节）：非零即失败；缺省（旧 broker 布局）容忍。
        int16_t topLevelError = 0;
        if (r.i16(&topLevelError) && topLevelError != 0) {
            throw std::runtime_error("offsetFetch top-level error: "
                + std::to_string(topLevelError));
        }
        return out;
    }

    // DeleteGroups v1（apiKey 42，H-1）：删 group 即连带清其在 __consumer_offsets
    // 的已提交 offset。v0/v1 请求/响应 schema 相同（Kafka 4.3.1 wire：请求
    // groups 数组字符串，响应 throttle + per-group [error_code, group_id]）——
    // 采用 v1。best-effort：group 不存在/无 offset（GROUP_ID_NOT_FOUND 类错误码
    // 或 NONE）均视为清理完成；传输/布局异常静默放弃（清理非硬前提，未覆盖分区
    // 的读数断言已由 expectBrokerOffsetsMatch 收窄兜底）。
    void deleteGroupOffsets(const std::string& groupId)
    {
        std::string resp;
        ByteWriter w;
        w.i32(1);              // groups 数组
        w.str(groupId);
        if (!conn_->request(42, 1, w.data(), kApiTimeoutMs, &resp)) {
            return;  // best-effort
        }
        ByteReader r(resp);
        int32_t throttle = 0;
        int32_t gc = 0;
        if (!r.i32(&throttle) || !r.i32(&gc)) {
            return;  // 布局异常 → 容错放弃
        }
        for (int32_t i = 0; i < gc; ++i) {
            int16_t err = 0;
            std::string gid;
            if (!r.i16(&err) || !r.str(&gid)) {
                return;
            }
            (void)err;  // NONE/GROUP_ID_NOT_FOUND/…：幂等清理，任何返回码均放行
        }
    }

private:
    std::string host_;
    int port_ = 0;
    std::unique_ptr<KafkaConn> conn_;
};

// ---- P4-03 形态原样：BlackholeServer / KafkaTestProxy（atomic harness + join 覆盖）----

// accept 后不回包：被测 consumer 的 fetch/metadata 超 deadline → brokerOk=false
// 且 poll 有界（RED ⑧）。
class BlackholeServer {
public:
    BlackholeServer()
    {
        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listener_ < 0) {
            throw std::runtime_error("BlackholeServer: socket failed");
        }
        int one = 1;
        (void)::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        struct sockaddr_in addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(listener_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0
            || ::listen(listener_, 16) != 0) {
            throw std::runtime_error("BlackholeServer: bind/listen failed");
        }
        socklen_t alen = sizeof(addr);
        if (::getsockname(listener_, reinterpret_cast<struct sockaddr*>(&addr), &alen) != 0) {
            throw std::runtime_error("BlackholeServer: getsockname failed");
        }
        port_ = ntohs(addr.sin_port);
        running_ = true;
        acceptThread_ = std::thread([this] {
            while (running_) {
                int c = ::accept(listener_, nullptr, nullptr);
                if (c < 0) {
                    if (!running_) {
                        break;
                    }
                    continue;
                }
                std::lock_guard<std::mutex> lk(mu_);
                conns_.push_back(c);
            }
        });
    }

    ~BlackholeServer()
    {
        running_ = false;
        if (listener_ >= 0) {
            ::shutdown(listener_, SHUT_RDWR);
            ::close(listener_);
            listener_ = -1;
        }
        if (acceptThread_.joinable()) {
            acceptThread_.join();
        }
        std::vector<int> conns;
        {
            std::lock_guard<std::mutex> lk(mu_);
            conns = conns_;
            conns_.clear();
        }
        for (size_t i = 0; i < conns.size(); ++i) {
            ::close(conns[i]);
        }
    }

    int port() const { return port_; }

private:
    std::atomic<int> listener_{-1};
    int port_ = 0;
    std::atomic<bool> running_{false};
    std::mutex mu_;
    std::vector<int> conns_;
    std::thread acceptThread_;
};

// 可控 TCP 转发代理：up = 转发到真实 broker；down = 关闭监听（连接被拒/断连）。
// 同一端点可 down/up toggle（RED ⑧：停 broker → 重启 broker）+ RED ⑦：在
// preCommitHook 内 setDown = fetch 成功而 commit 传输失败。
class KafkaTestProxy {
public:
    KafkaTestProxy(const std::string& brokerHost, int brokerPort)
        : brokerHost_(brokerHost), brokerPort_(brokerPort)
    {
    }

    ~KafkaTestProxy() { setDown(); }

    void setUp()
    {
        if (listener_ >= 0) {
            return;
        }
        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listener_ < 0) {
            throw std::runtime_error("KafkaTestProxy: socket failed");
        }
        int one = 1;
        (void)::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        struct sockaddr_in addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(static_cast<uint16_t>(port_));  // 0 = 首次临时端口
        if (::bind(listener_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0
            || ::listen(listener_, 32) != 0) {
            throw std::runtime_error("KafkaTestProxy: bind/listen failed");
        }
        if (port_ == 0) {
            socklen_t alen = sizeof(addr);
            if (::getsockname(listener_, reinterpret_cast<struct sockaddr*>(&addr), &alen) != 0) {
                throw std::runtime_error("KafkaTestProxy: getsockname failed");
            }
            port_ = ntohs(addr.sin_port);
        }
        running_ = true;
        acceptThread_ = std::thread([this] { acceptLoop(); });
    }

    void setDown()
    {
        running_ = false;
        if (listener_ >= 0) {
            ::shutdown(listener_, SHUT_RDWR);
            ::close(listener_);
            listener_ = -1;
        }
        if (acceptThread_.joinable()) {
            acceptThread_.join();
        }
        std::vector<int> conns;
        {
            std::lock_guard<std::mutex> lk(mu_);
            conns = conns_;
        }
        for (size_t i = 0; i < conns.size(); ++i) {
            ::shutdown(conns[i], SHUT_RDWR);
            ::close(conns[i]);
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            conns_.clear();
        }
        for (size_t i = 0; i < copyThreads_.size(); ++i) {
            if (copyThreads_[i].joinable()) {
                copyThreads_[i].join();
            }
        }
        copyThreads_.clear();
    }

    int port() const { return port_; }

private:
    void acceptLoop()
    {
        while (running_) {
            int c = ::accept(listener_, nullptr, nullptr);
            if (c < 0) {
                if (!running_) {
                    break;
                }
                continue;
            }
            {
                std::lock_guard<std::mutex> lk(mu_);
                conns_.push_back(c);
            }
            copyThreads_.push_back(std::thread([this, c] { relayLoop(c); }));
        }
    }

    void relayLoop(int clientFd)
    {
        int brokerFd = ::socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(brokerPort_));
        if (::inet_pton(AF_INET, brokerHost_.c_str(), &addr.sin_addr) != 1
            || ::connect(brokerFd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
            ::close(brokerFd);
            ::close(clientFd);
            removeConn(clientFd);
            return;
        }
        char buf[65536];
        bool openC = true;
        bool openB = true;
        while ((openC || openB) && running_) {
            struct pollfd fds[2];
            int nfds = 0;
            if (openC) {
                fds[nfds].fd = clientFd;
                fds[nfds].events = POLLIN;
                fds[nfds].revents = 0;
                ++nfds;
            }
            if (openB) {
                fds[nfds].fd = brokerFd;
                fds[nfds].events = POLLIN;
                fds[nfds].revents = 0;
                ++nfds;
            }
            if (nfds == 0) {
                break;
            }
            int rc = ::poll(fds, nfds, 200);
            if (rc <= 0) {
                if (!running_) {
                    break;
                }
                continue;
            }
            for (int i = 0; i < nfds; ++i) {
                const bool fromClient = (fds[i].fd == clientFd);
                const int dst = fromClient ? brokerFd : clientFd;
                if ((fds[i].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
                    ssize_t n = ::recv(fds[i].fd, buf, sizeof(buf), 0);
                    if (n <= 0) {
                        if (fromClient) {
                            openC = false;
                            ::shutdown(brokerFd, SHUT_WR);
                        } else {
                            openB = false;
                            ::shutdown(clientFd, SHUT_WR);
                        }
                    } else {
                        ssize_t sent = ::send(dst, buf, static_cast<size_t>(n), MSG_NOSIGNAL);
                        if (sent != n) {
                            openC = false;
                            openB = false;
                        }
                    }
                }
            }
        }
        ::close(clientFd);
        ::close(brokerFd);
        removeConn(clientFd);
    }

    void removeConn(int fd)
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (size_t i = 0; i < conns_.size(); ++i) {
            if (conns_[i] == fd) {
                conns_[i] = conns_[conns_.size() - 1];
                conns_.pop_back();
                return;
            }
        }
    }

    std::string brokerHost_;
    int brokerPort_ = 0;
    std::atomic<int> listener_{-1};
    int port_ = 0;
    std::atomic<bool> running_{false};
    std::mutex mu_;
    std::vector<int> conns_;
    std::thread acceptThread_;
    std::vector<std::thread> copyThreads_;
};

// ---- 消费侧 harness（InMemory leg）----

// 记录全部 attempt 且 disposition 可脚本切换（Closed = "投递通道未就绪"，P3-07
// 语义：保留 Pending、不记 attempt 计数）。
class ScriptedDeliverySink : public DeliverySink {
public:
    explicit ScriptedDeliverySink(DeliverDisposition initial) : next_(initial) {}

    void setNext(DeliverDisposition d) { next_ = d; }

    DeliverDisposition deliver(const DeliveryAttempt& attempt) override
    {
        attempts_.push_back(attempt);
        return next_;
    }

    const std::vector<DeliveryAttempt>& attempts() const { return attempts_; }

private:
    DeliverDisposition next_;
    std::vector<DeliveryAttempt> attempts_;
};

// InMemory 消费 harness：FakeClock 冻结 → lease 永不过期（重放 fencing 确定）；
// bob..grace 全部在线（wakeupAccepted 的 claimFor 只对 active 会话生效）。
struct ConsumeHarness {
    FakeClock clock;
    InMemoryMessageStore store;
    ScriptedDeliverySink sink;
    ReliableMessaging rm;
    WakeupProgressHandler handler;

    ConsumeHarness() : sink(DeliverDisposition::Closed), rm(store, sink, clock, kLeaseMs,
                                                            consumeRetryConfig()),
                       handler(store, rm)
    {
        clock.set(kNow);
        const UserId recipients[] = {kBob, kCarol, kDave, kEve, kFrank, kGrace};
        for (size_t i = 0; i < sizeof(recipients) / sizeof(recipients[0]); ++i) {
            rm.sessionAvailable(SessionIdentity(recipients[i], 1));
        }
    }

    // "recipient 离线"建模：sink Closed 下 accept——提交后在线 claim 被拒，
    // Delivery 停留 Pending（消费侧 wakeup 才是 Pending→InFlight 的唯一推手）。
    AcceptOutcome acceptPending(UserId recipient, const std::string& cmid)
    {
        sink.setNext(DeliverDisposition::Closed);
        return rm.accept(SessionIdentity(kAlice, 1),
                         directTo(recipient, cmid, "content-" + cmid));
    }

    std::vector<Delivery> deliveryOf(MessageId mid) { return store.deliveriesByMessage(mid); }
};

// poll 累积流：跨多次 poll 聚合 records/dispositions（长轮询 minBytes=1 可能分批
// 返回），committedThrough 每 partition 取 max。
struct PollStream {
    std::vector<ConsumedOutboxRecord> records;
    std::vector<ConsumeDisposition> dispositions;
    std::map<int32_t, int64_t> committedThrough;
    bool sawBrokerDown = false;

    void merge(const OutboxConsumeResult& r)
    {
        for (size_t i = 0; i < r.records.size(); ++i) {
            records.push_back(r.records[i]);
            if (i < r.dispositions.size()) {
                dispositions.push_back(r.dispositions[i]);
            }
        }
        if (!r.brokerOk) {
            sawBrokerDown = true;
        }
        for (std::map<int32_t, int64_t>::const_iterator it = r.committedThrough.begin();
             it != r.committedThrough.end(); ++it) {
            std::map<int32_t, int64_t>::iterator cur = committedThrough.find(it->first);
            if (cur == committedThrough.end() || it->second > cur->second) {
                committedThrough[it->first] = it->second;
            }
        }
    }

    std::set<uint64_t> distinctMessageIds() const
    {
        std::set<uint64_t> ids;
        for (size_t i = 0; i < records.size(); ++i) {
            ids.insert(records[i].messageId);
        }
        return ids;
    }
};

// 有界轮询到 expected 条（broker 正常期：brokerOk=false 即失败——零容忍）。
void pollUntilRecords(OutboxEventConsumer& consumer, size_t expected, int64_t deadlineMs,
                      PollStream* out)
{
    const int64_t deadline = nowMs() + deadlineMs;
    while (out->records.size() < expected) {
        ASSERT_LT(nowMs(), deadline) << "pollUntil: only " << out->records.size() << "/"
            << expected << " records within " << deadlineMs << "ms";
        const int64_t remain = deadline - nowMs();
        OutboxConsumeResult r = consumer.poll(remain > 800 ? remain : 800);
        ASSERT_TRUE(r.brokerOk) << "poll saw broker failure while broker is up";
        ASSERT_EQ(r.records.size(), r.dispositions.size());
        out->merge(r);
    }
}

// RED ⑦/⑧ 用：容忍 brokerOk=false（commit 注入拆链/代理 down 期），持续累积到
// expected 条或 deadline（brokerOk=false 的 poll 无 records，不推进但可重试）。
void pollUntilRecordsTolerant(OutboxEventConsumer& consumer, size_t expected,
                              int64_t deadlineMs, PollStream* out)
{
    const int64_t deadline = nowMs() + deadlineMs;
    while (out->records.size() < expected) {
        ASSERT_LT(nowMs(), deadline) << "pollUntil(tolerant): only " << out->records.size()
            << "/" << expected << " records within " << deadlineMs << "ms";
        const int64_t remain = deadline - nowMs();
        OutboxConsumeResult r = consumer.poll(remain > 800 ? remain : 800);
        ASSERT_EQ(r.records.size(), r.dispositions.size());
        out->merge(r);
    }
}

// 第三实例核验：broker 侧 __consumer_offsets（OffsetFetch 读回）与 consumer 的
// committedThrough 一致——语义收窄（H-2）：仅对 committedThrough 覆盖的分区做
// 强相等断言（提交过的分区 == offset+1）；未覆盖分区不再断言 -1（fixture SetUp
// 的 DeleteGroups 清理是 best-effort，清理竞态/失败时 __consumer_offsets 残留
// 可读回非 -1 值——记为观察项，不构成失败；"未提交"的强证据由各用例的
// preCommitHook 计数 + broker 停留上一提交点断言承担）。
void expectBrokerOffsetsMatch(KafkaTestWire& wire, const std::string& topic,
                              const std::map<int32_t, int64_t>& committedThrough)
{
    std::map<int32_t, int64_t> broker = wire.fetchCommitted(kGroupId, topic);
    ASSERT_FALSE(broker.empty());
    for (std::map<int32_t, int64_t>::const_iterator it = committedThrough.begin();
         it != committedThrough.end(); ++it) {
        ASSERT_EQ(1u, broker.count(it->first)) << "partition " << it->first;
        EXPECT_EQ(it->second, broker.find(it->first)->second)
            << "partition " << it->first << " (committedThrough vs broker)";
    }
    for (std::map<int32_t, int64_t>::const_iterator it = broker.begin(); it != broker.end();
         ++it) {
        if (committedThrough.count(it->first) == 0) {
            // 观察项：未覆盖分区的 broker 读数（无提交 = -1；残留/清理竞态可为
            // 其它值）——记录到测试属性，不断言具体值。
            ::testing::Test::RecordProperty(
                "uncoveredBrokerOffset",
                std::string("partition ") + std::to_string(it->first) + " offset "
                    + std::to_string(it->second));
        }
    }
}

// 处理面 port 的测试替身（RED ⑦）：记录 handle 顺序（经共享 Sequencer 打时标，
// 与 preCommitHook 的 commit 时标可比）。单线程 poll 驱动，无跨线程 → 无需 atomic。
struct Sequencer {
    int64_t next() { return ++n; }
    int64_t n = 0;
};

class RecordingProgressHandler : public DeliveryProgressHandler {
public:
    explicit RecordingProgressHandler(Sequencer* seq) : seq_(seq) {}

    ConsumeDisposition handle(const ConsumedOutboxRecord& record) override
    {
        handled_.push_back(record.messageId);
        lastTick_ = seq_->next();
        return ConsumeDisposition::Advanced;
    }

    const std::vector<uint64_t>& handled() const { return handled_; }
    int64_t lastHandleTick() const { return lastTick_; }

private:
    Sequencer* seq_;
    std::vector<uint64_t> handled_;
    int64_t lastTick_ = 0;
};

// P4-06 容量驱逐单测的处理面替身：记录全部 handle 调用 + 按 message_id 幂等
// （重复 handle 返回 DuplicateNoOp，模拟生产 handler 的 DB/claimFor-fencing 幂等
// 兜底——WakeupProgressHandler 对同 message_id 重放即返回 DuplicateNoOp）。这样
// 驱逐（消费者内存 seen 去重失效 → 重复 record 重新到达 handler）经 handled()
// 计数可观察，而重复投递不产生第二个 Advanced 副作用（disposition 仍
// DuplicateNoOp）。单线程 poll 驱动，无跨线程 → 无需 atomic。
class IdempotentRecordingHandler : public DeliveryProgressHandler {
public:
    ConsumeDisposition handle(const ConsumedOutboxRecord& record) override
    {
        handled_.push_back(record.messageId);
        if (seen_.count(record.messageId) != 0) {
            return ConsumeDisposition::DuplicateNoOp;  // store/DB 幂等兜底
        }
        seen_.insert(record.messageId);
        return ConsumeDisposition::Advanced;
    }

    const std::vector<uint64_t>& handled() const { return handled_; }

private:
    std::vector<uint64_t> handled_;
    std::set<uint64_t> seen_;
};

// preCommitHook 控制器（适配器 seam 注入面；单线程 poll 驱动）：
// Passive 记录 commit 时标；Throw = 每次 commit 前抛出（kill 注入，armed 期间
// 持续拦截，setPassive 解除）；TearDownOnce = 下次 commit 前拆代理（commit 传输
// 失败注入，一次性）。
class PreCommitController {
public:
    enum class Mode { Passive, Throw, TearDownOnce };

    explicit PreCommitController(Sequencer* seq) : seq_(seq) {}

    void setPassive() { mode_ = Mode::Passive; }
    void setThrow() { mode_ = Mode::Throw; }
    void setTearDownOnce(KafkaTestProxy* proxy)
    {
        proxy_ = proxy;
        mode_ = Mode::TearDownOnce;
    }

    std::function<void()> hook()
    {
        return [this] {
            lastCommitTick_ = seq_->next();
            ++commitAttempts_;
            const Mode m = mode_;
            if (m == Mode::TearDownOnce) {
                mode_ = Mode::Passive;  // 一次性注入后回到 passive
            }
            if (m == Mode::Throw) {
                throw std::runtime_error("injected kill before offset commit");
            }
            if (m == Mode::TearDownOnce && proxy_ != nullptr) {
                proxy_->setDown();
            }
        };
    }

    int64_t commitAttempts() const { return commitAttempts_; }
    int64_t lastCommitTick() const { return lastCommitTick_; }

private:
    Sequencer* seq_;
    Mode mode_ = Mode::Passive;
    KafkaTestProxy* proxy_ = nullptr;
    int64_t commitAttempts_ = 0;
    int64_t lastCommitTick_ = 0;
};

// ---- Fixture：固定 topic delete→create 复位（冻结参数；offset 随删除复位）----

class KafkaConsumeFixture : public ::testing::Test {
protected:
    void SetUp() override
    {
        host_ = testHost();
        port_ = testPort();
        wire_.reset(new KafkaTestWire(host_, port_));
        ASSERT_TRUE(wire_->brokerReachable())
            << "Kafka unavailable (requires local broker on " << host_ << ":" << port_ << ")";
        wire_->deleteTopic(kTestTopic, 5000);  // best-effort 首删（不存在即 err=3）
        ASSERT_TRUE(waitTopicGone(kReadDeadlineMs));
        createTopicWithRetry();
        ASSERT_TRUE(waitPartitionsReady(kReadDeadlineMs));
        // H-1：topic 重建不复位 __consumer_offsets——固定 group 跨用例残留的
        // 已提交 offset 会击穿"新 topic 无提交历史（-1）"假设。createTopic
        // 就绪后 DeleteGroups 清固定 group 的提交残留（best-effort，容错）。
        wire_->deleteGroupOffsets(kGroupId);
    }

    void TearDown() override
    {
        if (wire_) {
            wire_->deleteTopic(kTestTopic, 5000);
        }
    }

    std::unique_ptr<KafkaEventConsumer> makeConsumer(MessageStore& store,
                                                     DeliveryProgressHandler& handler)
    {
        return std::unique_ptr<KafkaEventConsumer>(new KafkaEventConsumer(
            host_, port_, kTestTopic, kGroupId, store, handler, kFetchBatchLimit,
            kConsumeDeadlineMs));
    }

    // P4-06 容量驱逐注入：小 cap 确定性触发 touchConversation 驱逐（生产默认 0 =
    // 冻结常量 100；测试以 0 以外小值注入）。
    std::unique_ptr<KafkaEventConsumer> makeConsumerWithCapacity(
        MessageStore& store, DeliveryProgressHandler& handler, size_t capacity)
    {
        return std::unique_ptr<KafkaEventConsumer>(new KafkaEventConsumer(
            host_, port_, kTestTopic, kGroupId, store, handler, kFetchBatchLimit,
            kConsumeDeadlineMs, capacity));
    }

    KafkaPublisher makePublisher()
    {
        return KafkaPublisher(host_, port_, "muduo-test-", 5000);
    }

    const char* topic() const { return kTestTopic; }

    // "已删净"探针 = DeleteTopics（绝不以 Metadata 作探针）：broker
    // auto.create.topics.enable=true 下 Metadata 对未知 topic 会异步自动建同名
    // 1 分区 topic，紧随的 createTopic(3 分区) 撞 err=36（GREEN 解阻根因，见卡）；
    // DeleteTopics 对不存在 topic 只回 err=3、不触发 auto-create。探针循环：
    // err=3（UNKNOWN_TOPIC_OR_PARTITION）即删净；err=NONE（仍在删，KRaft 传播
    // 延迟）/-1（传输失败）继续有界轮询（50ms 步进）。
    bool waitTopicGone(int64_t deadlineMs)
    {
        const int64_t deadline = nowMs() + deadlineMs;
        for (;;) {
            const int16_t err = wire_->deleteTopic(kTestTopic, 5000);
            if (err == 3) {
                return true;  // UNKNOWN_TOPIC_OR_PARTITION：已删净
            }
            if (nowMs() > deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    // createTopic 抗 err=36（TOPIC_ALREADY_EXISTS）防御：auto-create 竞态残余/
    // KRaft 删除传播延迟下，有界重删重试（≤5 轮 deleteTopic→wait-gone→
    // createTopic）；非 36 错误或重试耗尽仍失败即 ASSERT 失败（零 skip 纪律）。
    void createTopicWithRetry()
    {
        for (int attempt = 0;; ++attempt) {
            const int16_t err = wire_->createTopic(kTestTopic, kPartitions, 1, 5000);
            if (err == 0) {
                return;
            }
            ASSERT_EQ(36, err) << "createTopic unexpected error (attempt " << attempt << ")";
            ASSERT_LT(attempt, 5) << "createTopic still TOPIC_ALREADY_EXISTS after "
                                     "bounded delete/recreate retries";
            wire_->deleteTopic(kTestTopic, 5000);
            ASSERT_TRUE(waitTopicGone(kReadDeadlineMs));
        }
    }

    bool waitPartitionsReady(int64_t deadlineMs)
    {
        const int64_t deadline = nowMs() + deadlineMs;
        for (;;) {
            std::vector<int32_t> parts;
            try {
                parts = wire_->partitions(kTestTopic);
            } catch (const std::exception&) {
                parts.clear();
            }
            bool ready = parts.size() == static_cast<size_t>(kPartitions);
            for (size_t i = 0; i < parts.size() && ready; ++i) {
                try {
                    wire_->earliestOffset(kTestTopic, parts[i]);
                } catch (const std::exception&) {
                    ready = false;  // leader 未就绪（KRaft 异步分配）
                }
            }
            if (ready) {
                return true;
            }
            if (nowMs() > deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    std::string host_;
    int port_ = 9092;
    std::unique_ptr<KafkaTestWire> wire_;
};

// InMemory leg：真实 Kafka + InMemoryMessageStore + ReliableMessaging + 真实
// KafkaPublisher 注入 + 被测 KafkaEventConsumer/WakeupProgressHandler。
class InMemoryConsumeFixture : public KafkaConsumeFixture {
};

// ---- RED ①：round-trip 只推进不重建 ----
// 经 store+accept 建 Message/Delivery（sink Closed = 通道未就绪，Delivery 停留
// Pending）→ KafkaPublisher 发布事件 → consumer poll → Advanced、Delivery
// Pending→InFlight（sink 观察）、Message/Delivery 行数不变；offset 提交且 broker
// 侧（第三实例 OffsetFetch）与 committedThrough 一致。
TEST_F(InMemoryConsumeFixture, RoundTripAdvancesExistingDeliveryWithoutRebuild)
{
    ConsumeHarness h;
    const AcceptOutcome a = h.acceptPending(kBob, "rt-1");
    ASSERT_TRUE(a.ok);
    ASSERT_EQ(1u, h.deliveryOf(a.messageId).size());
    ASSERT_EQ(DeliveryState::Pending, h.deliveryOf(a.messageId)[0].state);
    const size_t deliveriesBefore = h.store.deliveriesByRecipient(kBob).size();
    ASSERT_EQ(1u, deliveriesBefore);

    KafkaPublisher publisher = makePublisher();
    publishOne(publisher, envelopeFor(a, topic()));

    // 投递通道就绪：poll 时刻的 claim 才 Accepted（Pending→InFlight 归因于 wakeup）。
    h.sink.setNext(DeliverDisposition::Accepted);

    std::unique_ptr<KafkaEventConsumer> consumer = makeConsumer(h.store, h.handler);
    PollStream s;
    pollUntilRecords(*consumer, 1, kPollDeadlineMs, &s);
    ASSERT_EQ(1u, s.records.size());
    EXPECT_EQ(ConsumeDisposition::Advanced, s.dispositions[0]);

    // 信封字段回读（P4-03 冻结信封，一字段不改）。
    EXPECT_EQ(a.messageId.value, s.records[0].messageId);
    EXPECT_EQ(a.conversationId.value, s.records[0].conversationId);
    EXPECT_EQ(a.sequence.value, s.records[0].sequence);
    EXPECT_EQ(std::string("MessageAccepted"), s.records[0].eventType);
    EXPECT_EQ(std::string(kTestTopic), s.records[0].topic);

    // Delivery Pending→InFlight、attempt=1；sink 在 poll 期间观察到 1 次 attempt
    //（accept 期 1 次 Closed + poll 期 1 次 Accepted）。
    std::vector<Delivery> ds = h.deliveryOf(a.messageId);
    ASSERT_EQ(1u, ds.size());
    EXPECT_EQ(DeliveryState::InFlight, ds[0].state);
    EXPECT_EQ(1u, ds[0].attemptCount);
    ASSERT_EQ(2u, h.sink.attempts().size());
    EXPECT_EQ(1u, h.sink.attempts()[1].attemptNumber);

    // 只推进不重建：Delivery 行数不变（无新 Message/Delivery）、幂等键仍可回读。
    EXPECT_EQ(deliveriesBefore, h.store.deliveriesByRecipient(kBob).size());
    EXPECT_TRUE(h.store.findAccepted(ClientMessageId("rt-1"), kAlice) != nullptr);
    EXPECT_TRUE(h.store.deadLetters(10).empty());

    // offset 提交：committedThrough[分区] = offset+1，且与 broker 侧一致。
    ASSERT_EQ(1u, s.committedThrough.count(s.records[0].partition));
    EXPECT_EQ(s.records[0].offset + 1, s.committedThrough[s.records[0].partition]);
    expectBrokerOffsetsMatch(*wire_, topic(), s.committedThrough);
}

// ---- RED ②：处理后 offset 提交前 kill（preCommitHook 注入抛出等价）----
// 事件已 handle（sink 已见 attempt / dead-letter 已落库）→ preCommitHook 抛出 →
// offset 未提交（broker 侧全 -1）→ 新 consumer（同 group）从 earliest 回退重放：
// 好事件 DuplicateNoOp、attemptCount 不重复加、poison dead-letter 不双插
//（UNIQUE 幂等）——重启不产生重复副作用。
TEST_F(InMemoryConsumeFixture, KillBeforeCommitReplaysIdempotently)
{
    ConsumeHarness h;
    const AcceptOutcome good = h.acceptPending(kBob, "kill-good");
    ASSERT_TRUE(good.ok);
    // poison 同批：未知 event_type（消费侧 dead-letter，handler 不被调用）。
    const uint64_t poisonMid = 999021;

    KafkaPublisher publisher = makePublisher();
    publishOne(publisher, envelopeFor(good, topic()));
    publishOne(publisher, craftedRequest(poisonMid, 888, 1, "MessageRejected",
                                         payloadJson("kill-poison"), topic()));

    h.sink.setNext(DeliverDisposition::Accepted);

    Sequencer seq;
    PreCommitController killHook(&seq);
    killHook.setThrow();  // armed：每次 commit 前抛出（kill 注入），直到解除
    {
        std::unique_ptr<KafkaEventConsumer> consumer = makeConsumer(h.store, h.handler);
        consumer->setPreCommitHook(killHook.hook());

        PollStream s1;
        const int64_t d1 = nowMs() + kPollDeadlineMs;
        while (s1.distinctMessageIds().size() < 2) {
            ASSERT_LT(nowMs(), d1) << "phase1: expected 2 distinct records";
            OutboxConsumeResult r = consumer->poll(2000);
            ASSERT_TRUE(r.brokerOk);
            ASSERT_EQ(r.records.size(), r.dispositions.size());
            s1.merge(r);
        }
        // 处理已发生：good 至少一次 Advanced、poison 至少一次 DeadLettered
        //（未提交 → 同批复 read，重放记录可为多份）。
        bool sawAdvanced = false;
        bool sawDeadLetter = false;
        for (size_t i = 0; i < s1.records.size(); ++i) {
            if (s1.records[i].messageId == good.messageId.value
                && s1.dispositions[i] == ConsumeDisposition::Advanced) {
                sawAdvanced = true;
            }
            if (s1.records[i].messageId == poisonMid
                && s1.dispositions[i] == ConsumeDisposition::DeadLettered) {
                sawDeadLetter = true;
            }
        }
        EXPECT_TRUE(sawAdvanced);
        EXPECT_TRUE(sawDeadLetter);
        EXPECT_GE(killHook.commitAttempts(), 1);  // commit 曾被尝试（被 kill 注入拦截）

        // kill 等价：broker 侧 __consumer_offsets 无任何提交。kill 窗口证据已由
        // hook 计数（commit_attempts≥1 全被拦截）+ phase2 重放行为承担；broker
        // 侧读数降级为观察项（H-2：committedThrough 为空 → 只记录不断言具体值，
        // 残留/清理竞态不再造成假失败）。
        expectBrokerOffsetsMatch(*wire_, topic(), std::map<int32_t, int64_t>());

        // 副作用幂等：尽管复 read，attemptCount 只加过一次、dead-letter 只一行。
        std::vector<Delivery> ds = h.deliveryOf(good.messageId);
        ASSERT_EQ(1u, ds.size());
        EXPECT_EQ(DeliveryState::InFlight, ds[0].state);
        EXPECT_EQ(1u, ds[0].attemptCount);
        ASSERT_EQ(1u, h.store.deadLetters(10).size());
        EXPECT_EQ(std::string("unknown_event_type"), h.store.deadLetters(10)[0].reason);
    }  // 实例 A 销毁（kill 后进程消失的等价；未提交 offset 遗留 broker 侧）

    const size_t attemptsAfterPhase1 = h.sink.attempts().size();
    const size_t deadLettersAfterPhase1 = h.store.deadLetters(10).size();

    // ---- 重启接管：新 consumer 实例（同 group）从 earliest 回退重放 ----
    killHook.setPassive();
    {
        std::unique_ptr<KafkaEventConsumer> consumer = makeConsumer(h.store, h.handler);
        consumer->setPreCommitHook(killHook.hook());

        PollStream s2;
        const int64_t d2 = nowMs() + kPollDeadlineMs;
        while (s2.distinctMessageIds().size() < 2) {
            ASSERT_LT(nowMs(), d2) << "phase2: expected 2 distinct records on replay";
            OutboxConsumeResult r = consumer->poll(2000);
            ASSERT_TRUE(r.brokerOk);
            ASSERT_EQ(r.records.size(), r.dispositions.size());
            s2.merge(r);
        }
        bool sawDuplicateNoOp = false;
        for (size_t i = 0; i < s2.records.size(); ++i) {
            if (s2.records[i].messageId == good.messageId.value
                && s2.dispositions[i] == ConsumeDisposition::DuplicateNoOp) {
                sawDuplicateNoOp = true;
            }
            if (s2.records[i].messageId == poisonMid) {
                EXPECT_EQ(ConsumeDisposition::DeadLettered, s2.dispositions[i]);
            }
        }
        EXPECT_TRUE(sawDuplicateNoOp) << "replayed good event must be DuplicateNoOp";

        // 重启不产生重复副作用：attemptCount/行数不变、dead-letter 不双插。
        std::vector<Delivery> ds = h.deliveryOf(good.messageId);
        ASSERT_EQ(1u, ds.size());
        EXPECT_EQ(1u, ds[0].attemptCount);
        EXPECT_EQ(DeliveryState::InFlight, ds[0].state);
        EXPECT_EQ(attemptsAfterPhase1, h.sink.attempts().size());
        ASSERT_EQ(deadLettersAfterPhase1, h.store.deadLetters(10).size());

        // 本轮 commit 成功：broker 侧与 committedThrough 一致（每分区 = offset+1）。
        expectBrokerOffsetsMatch(*wire_, topic(), s2.committedThrough);
    }
}

// ---- RED ③：重启接管全部分区恢复（manual assign 的 rebalance 语义）----
// 实例 A 消费提交 3 事件（3 独立 conversation = 3 分区路由）→ 销毁 A → 实例 B 同
// group 从已提交 offset 继续、绝不重放已提交区间（B 期间 sink 只见新消息 attempt）
// → 第三实例 OffsetFetch 读回与 B 的 committedThrough 一致。
TEST_F(InMemoryConsumeFixture, RestartTakesOverPartitionsFromCommittedOffsets)
{
    ConsumeHarness h;
    // 3 独立 conversation（alice→bob/carol/dave）：每会话单消息 → 无 HOL 耦合，
    // 全部可在 poll 期独立 Advanced。
    AcceptOutcome m1 = h.acceptPending(kBob, "restart-1");
    AcceptOutcome m2 = h.acceptPending(kCarol, "restart-2");
    AcceptOutcome m3 = h.acceptPending(kDave, "restart-3");
    ASSERT_TRUE(m1.ok);
    ASSERT_TRUE(m2.ok);
    ASSERT_TRUE(m3.ok);

    KafkaPublisher publisher = makePublisher();
    publishOne(publisher, envelopeFor(m1, topic()));
    publishOne(publisher, envelopeFor(m2, topic()));
    publishOne(publisher, envelopeFor(m3, topic()));

    h.sink.setNext(DeliverDisposition::Accepted);

    // 实例 A：消费并提交。
    {
        std::unique_ptr<KafkaEventConsumer> a = makeConsumer(h.store, h.handler);
        PollStream sa;
        pollUntilRecords(*a, 3, kPollDeadlineMs, &sa);
        ASSERT_EQ(3u, sa.records.size());
        for (size_t i = 0; i < 3; ++i) {
            EXPECT_EQ(ConsumeDisposition::Advanced, sa.dispositions[i]);
        }
        expectBrokerOffsetsMatch(*wire_, topic(), sa.committedThrough);
    }  // A 析构（销毁实例）

    const size_t attemptsAfterA = h.sink.attempts().size();

    // 实例 B 前的新事件（同 group 接管后消费）。
    AcceptOutcome m4 = h.acceptPending(kEve, "restart-4");
    AcceptOutcome m5 = h.acceptPending(kFrank, "restart-5");
    AcceptOutcome m6 = h.acceptPending(kGrace, "restart-6");
    ASSERT_TRUE(m4.ok);
    ASSERT_TRUE(m5.ok);
    ASSERT_TRUE(m6.ok);
    publishOne(publisher, envelopeFor(m4, topic()));
    publishOne(publisher, envelopeFor(m5, topic()));
    publishOne(publisher, envelopeFor(m6, topic()));
    // [仲裁修复] acceptPending 留下 sink=Closed：B 的 poll 前必须恢复 Accepted，
    // 否则 wakeup 的投递按 P3-07 Closed 语义停留 Pending（不记 attempt、不转
    // InFlight）→ 只能 DuplicateNoOp，Advanced 断言不可满足。
    h.sink.setNext(DeliverDisposition::Accepted);

    // 实例 B：同 group，从 A 已提交 offset 继续（绝不重放 1..3）。
    std::unique_ptr<KafkaEventConsumer> b = makeConsumer(h.store, h.handler);
    PollStream sb;
    pollUntilRecords(*b, 3, kPollDeadlineMs, &sb);
    ASSERT_EQ(3u, sb.records.size());
    std::set<uint64_t> expectNew;
    expectNew.insert(m4.messageId.value);
    expectNew.insert(m5.messageId.value);
    expectNew.insert(m6.messageId.value);
    for (size_t i = 0; i < sb.records.size(); ++i) {
        EXPECT_EQ(1u, expectNew.count(sb.records[i].messageId))
            << "consumer B must not replay the committed range (mid="
            << sb.records[i].messageId << ")";
        EXPECT_EQ(ConsumeDisposition::Advanced, sb.dispositions[i]);
    }

    // 不重放的 sink 证据：4..6 的 accept 期 3 次 Closed + B 的 poll 期 3 次
    // Accepted（沿 RED④ "accept 期 1 次 Closed" 计数语义），全部是新消息（4..6）。
    ASSERT_EQ(attemptsAfterA + 6, h.sink.attempts().size());
    for (size_t i = attemptsAfterA; i < h.sink.attempts().size(); ++i) {
        EXPECT_EQ(1u, expectNew.count(h.sink.attempts()[i].messageId.value));
    }

    // broker 侧 __consumer_offsets（第三实例 OffsetFetch）与 B 的 committedThrough 一致。
    expectBrokerOffsetsMatch(*wire_, topic(), sb.committedThrough);
}

// ---- RED ④：重复事件 no-op ----
// 同 message_id 连投两条 record（同 key 同分区，Kafka 保序）→ 第一条 Advanced
//（Pending→InFlight）、第二条 DuplicateNoOp；Delivery 行/attemptCount/state 不变。
TEST_F(InMemoryConsumeFixture, DuplicateEventIsNoOp)
{
    ConsumeHarness h;
    const AcceptOutcome a = h.acceptPending(kBob, "dup-1");
    ASSERT_TRUE(a.ok);

    KafkaPublisher publisher = makePublisher();
    const OutboxPublishRequest req = envelopeFor(a, topic());
    publishOne(publisher, req);
    publishOne(publisher, req);  // 同 message_id/sequence 第二条 record

    h.sink.setNext(DeliverDisposition::Accepted);

    std::unique_ptr<KafkaEventConsumer> consumer = makeConsumer(h.store, h.handler);
    PollStream s;
    pollUntilRecords(*consumer, 2, kPollDeadlineMs, &s);
    ASSERT_EQ(2u, s.records.size());
    // 同 key → 同分区 → offset 序 = 发布序。
    EXPECT_EQ(s.records[0].partition, s.records[1].partition);
    EXPECT_LT(s.records[0].offset, s.records[1].offset);
    EXPECT_EQ(s.records[0].messageId, s.records[1].messageId);
    EXPECT_EQ(ConsumeDisposition::Advanced, s.dispositions[0]);
    EXPECT_EQ(ConsumeDisposition::DuplicateNoOp, s.dispositions[1]);

    // 重放幂等：行数不变、attemptCount 恰好 1、state 不再迁移。
    std::vector<Delivery> ds = h.deliveryOf(a.messageId);
    ASSERT_EQ(1u, ds.size());
    EXPECT_EQ(DeliveryState::InFlight, ds[0].state);
    EXPECT_EQ(1u, ds[0].attemptCount);
    EXPECT_EQ(1u, h.store.deliveriesByRecipient(kBob).size());
    // sink 只见 1 次 Accepted attempt（accept 期 1 次 Closed + 首条 record 1 次）。
    ASSERT_EQ(2u, h.sink.attempts().size());
    EXPECT_TRUE(h.store.deadLetters(10).empty());
    expectBrokerOffsetsMatch(*wire_, topic(), s.committedThrough);
}

// ---- RED ⑤：乱序（sequence 倒退 → sequence_regression；同 sequence 异 id →
// sequence_conflict）dead-letter 且不 head-of-line 卡死 ----
TEST_F(InMemoryConsumeFixture, SequenceRegressionAndConflictDeadLetteredWithoutBlocking)
{
    ConsumeHarness h;
    // 同一 conversation（alice→bob）两条消息：m1 真实 seq=1、m2 真实 seq=2。
    // 信封 sequence 手工编造（P4-04 卡"手工注入乱序 record"）：同 key 同分区，
    // consumer 的 per-conversation lastSeen 跟踪信封 sequence。
    const AcceptOutcome m1 = h.acceptPending(kBob, "seq-m1");
    const AcceptOutcome m2 = h.acceptPending(kBob, "seq-m2");
    ASSERT_TRUE(m1.ok);
    ASSERT_TRUE(m2.ok);
    ASSERT_EQ(m1.conversationId.value, m2.conversationId.value);
    const uint64_t conv = m1.conversationId.value;

    KafkaPublisher publisher = makePublisher();
    h.sink.setNext(DeliverDisposition::Accepted);

    std::unique_ptr<KafkaEventConsumer> consumer = makeConsumer(h.store, h.handler);
    PollStream s;
    // 步进累积：每步期望累计 records 达到下一总数（同流聚合 committedThrough）。

    // 1) 正常事件（信封 sequence=5）：Advanced，lastSeen[conv]=5。
    publishOne(publisher, craftedRequest(m1.messageId.value, conv, 5, "MessageAccepted",
                                         payloadJson("seq-m1"), topic()));
    pollUntilRecords(*consumer, 1, kPollDeadlineMs, &s);
    ASSERT_EQ(1u, s.records.size());
    EXPECT_EQ(ConsumeDisposition::Advanced, s.dispositions[0]);
    ASSERT_EQ(1u, h.deliveryOf(m1.messageId).size());
    EXPECT_EQ(DeliveryState::InFlight, h.deliveryOf(m1.messageId)[0].state);

    // 2) sequence 倒退（3 < lastSeen 5，不同 message_id）：dead-letter、不推进。
    publishOne(publisher, craftedRequest(999031, conv, 3, "MessageAccepted",
                                         payloadJson("seq-reg"), topic()));
    pollUntilRecords(*consumer, 2, kPollDeadlineMs, &s);
    ASSERT_EQ(2u, s.records.size());
    EXPECT_EQ(ConsumeDisposition::DeadLettered, s.dispositions[1]);
    {
        ASSERT_EQ(1u, h.store.deadLetters(10).size());
        // [仲裁修复] deadLetters 按值返回：绑定 const& 到临时 vector 元素 =
        // 悬垂 UB（SIGSEGV）。改按值拷贝（同本用例第 4 段 dls 的写法）。
        const DeadLetterRecord dl = h.store.deadLetters(10)[0];
        EXPECT_EQ(std::string("sequence_regression"), dl.reason);
        EXPECT_EQ(std::string(kTestTopic), dl.topic);
        EXPECT_EQ(s.records[1].partition, dl.partitionId);
        EXPECT_EQ(s.records[1].offset, dl.kafkaOffset);
        EXPECT_EQ(999031u, dl.messageId);
    }
    // 不破坏：m1 仍 InFlight、m2 仍 Pending。
    EXPECT_EQ(DeliveryState::InFlight, h.deliveryOf(m1.messageId)[0].state);
    EXPECT_EQ(DeliveryState::Pending, h.deliveryOf(m2.messageId)[0].state);

    // 3) 后续正常 sequence（6）事件继续推进：不 head-of-line 卡死。m2 是同
    // conversation 下一消息——先 ACK m1 放行 HOL（sink 保持 Closed 使 ACK 触发
    // 的 claim 不提前投递 m2，Pending→InFlight 仍归因于 consumer 的 wakeup）。
    h.sink.setNext(DeliverDisposition::Closed);
    {
        const AckOutcome ack = h.rm.acknowledge(SessionIdentity(kBob, 1), m1.messageId);
        ASSERT_EQ(AckResult::Acknowledged, ack.result);
    }
    ASSERT_EQ(DeliveryState::Pending, h.deliveryOf(m2.messageId)[0].state);
    h.sink.setNext(DeliverDisposition::Accepted);
    publishOne(publisher, craftedRequest(m2.messageId.value, conv, 6, "MessageAccepted",
                                         payloadJson("seq-m2"), topic()));
    pollUntilRecords(*consumer, 3, kPollDeadlineMs, &s);
    ASSERT_EQ(3u, s.records.size());
    EXPECT_EQ(ConsumeDisposition::Advanced, s.dispositions[2]);
    ASSERT_EQ(1u, h.deliveryOf(m2.messageId).size());
    EXPECT_EQ(DeliveryState::InFlight, h.deliveryOf(m2.messageId)[0].state);
    EXPECT_EQ(1u, h.deliveryOf(m2.messageId)[0].attemptCount);

    // 4) 同 sequence 异 id（== lastSeen 6、message_id 不同）：sequence_conflict。
    publishOne(publisher, craftedRequest(999032, conv, 6, "MessageAccepted",
                                         payloadJson("seq-conflict"), topic()));
    pollUntilRecords(*consumer, 4, kPollDeadlineMs, &s);
    ASSERT_EQ(4u, s.records.size());
    EXPECT_EQ(ConsumeDisposition::DeadLettered, s.dispositions[3]);
    {
        std::vector<DeadLetterRecord> dls = h.store.deadLetters(10);
        ASSERT_EQ(2u, dls.size());
        bool sawConflict = false;
        for (size_t i = 0; i < dls.size(); ++i) {
            if (dls[i].reason == "sequence_conflict") {
                sawConflict = true;
                EXPECT_EQ(999032u, dls[i].messageId);
                EXPECT_EQ(s.records[3].partition, dls[i].partitionId);
                EXPECT_EQ(s.records[3].offset, dls[i].kafkaOffset);
            }
        }
        EXPECT_TRUE(sawConflict);
    }
    // 防御断言不破坏状态：m2 仍 InFlight attempt=1。
    EXPECT_EQ(DeliveryState::InFlight, h.deliveryOf(m2.messageId)[0].state);
    EXPECT_EQ(1u, h.deliveryOf(m2.messageId)[0].attemptCount);

    // 行数不变（只推进不重建）；乱序事件被 dead-letter 后 offset 仍提交（不卡死）。
    EXPECT_EQ(2u, h.store.deliveriesByRecipient(kBob).size());
    expectBrokerOffsetsMatch(*wire_, topic(), s.committedThrough);
}

// ---- RED ⑥（InMemory 谓词 leg）：poison 事件 dead-letter 可查询 ----
// 坏 payload JSON / 未知 event_type / 缺 Message 行 → 各自 reason dead-letter、
// deadLetters 谓词可查询（含 record 定位字段），offset 在 dead-letter 落库后提交
//（后续事件不阻塞）。
TEST_F(InMemoryConsumeFixture, PoisonEventsDeadLetteredAndQueryableInMemory)
{
    ConsumeHarness h;
    const AcceptOutcome good = h.acceptPending(kBob, "poison-good");
    ASSERT_TRUE(good.ok);

    KafkaPublisher publisher = makePublisher();
    publishOne(publisher, craftedRequest(999011, 601, 1, "MessageAccepted",
                                         "{not json", topic()));           // 坏 payload
    publishOne(publisher, craftedRequest(999012, 602, 1, "MessageRejected",
                                         payloadJson("poison-type"), topic()));  // 未知 type
    publishOne(publisher, craftedRequest(999013, 603, 1, "MessageAccepted",
                                         payloadJson("poison-missing"), topic()));  // 缺行
    publishOne(publisher, envelopeFor(good, topic()));                      // 正常事件

    h.sink.setNext(DeliverDisposition::Accepted);

    std::unique_ptr<KafkaEventConsumer> consumer = makeConsumer(h.store, h.handler);
    PollStream s;
    pollUntilRecords(*consumer, 4, kPollDeadlineMs, &s);
    ASSERT_EQ(4u, s.records.size());

    std::map<uint64_t, ConsumeDisposition> byMid;
    for (size_t i = 0; i < s.records.size(); ++i) {
        byMid[s.records[i].messageId] = s.dispositions[i];
    }
    EXPECT_EQ(ConsumeDisposition::DeadLettered, byMid[999011]);
    EXPECT_EQ(ConsumeDisposition::DeadLettered, byMid[999012]);
    EXPECT_EQ(ConsumeDisposition::DeadLettered, byMid[999013]);
    EXPECT_EQ(ConsumeDisposition::Advanced, byMid[good.messageId.value]);

    // 谓词可查询：3 行、reason 各异、record 定位（topic/partition/offset）可回对。
    std::vector<DeadLetterRecord> dls = h.store.deadLetters(10);
    ASSERT_EQ(3u, dls.size());
    std::set<std::string> reasons;
    for (size_t i = 0; i < dls.size(); ++i) {
        reasons.insert(dls[i].reason);
        EXPECT_EQ(std::string(kTestTopic), dls[i].topic);
        EXPECT_FALSE(dls[i].rawValue.empty());
        // 定位唯一：能对回某条已消费 record 的 (partition, offset)。
        bool matched = false;
        for (size_t j = 0; j < s.records.size(); ++j) {
            if (s.records[j].partition == dls[i].partitionId
                && s.records[j].offset == dls[i].kafkaOffset
                && s.records[j].messageId == dls[i].messageId) {
                matched = true;
            }
        }
        EXPECT_TRUE(matched) << "dead letter row must carry record location (p="
            << dls[i].partitionId << " off=" << dls[i].kafkaOffset << ")";
    }
    EXPECT_EQ(1u, reasons.count("poison_payload"));
    EXPECT_EQ(1u, reasons.count("unknown_event_type"));
    EXPECT_EQ(1u, reasons.count("message_missing"));

    // dead-letter 不阻塞：good 事件已 Advanced、offset 提交覆盖全部 4 条。
    EXPECT_EQ(DeliveryState::InFlight, h.deliveryOf(good.messageId)[0].state);
    EXPECT_EQ(1u, h.deliveryOf(good.messageId)[0].attemptCount);
    expectBrokerOffsetsMatch(*wire_, topic(), s.committedThrough);
}

// ---- RED ⑦a：offset 提交时序断言（handle 先于 commit）----
// RecordingHandler 记录 handle 顺序 + preCommitHook 记录 commit 时标（共享
// Sequencer，单线程 poll 驱动）→ 断言全部 handle（含 dead-letter 落库的批）先于
// commit。
TEST_F(InMemoryConsumeFixture, OffsetCommitFollowsHandlingOrder)
{
    InMemoryMessageStore store;  // RecordingHandler 无 store 语义，仅供 dead-letter port
    Sequencer seq;
    RecordingProgressHandler handler(&seq);
    PreCommitController hook(&seq);
    hook.setPassive();

    KafkaPublisher publisher = makePublisher();
    publishOne(publisher, craftedRequest(990101, 701, 1, "MessageAccepted",
                                         payloadJson("order-1"), topic()));
    publishOne(publisher, craftedRequest(990102, 702, 1, "MessageAccepted",
                                         payloadJson("order-2"), topic()));
    publishOne(publisher, craftedRequest(990103, 703, 1, "MessageAccepted",
                                         payloadJson("order-3"), topic()));
    // 同批 poison（未知 event_type）：dead-letter 落库也必须先于 commit。
    publishOne(publisher, craftedRequest(990104, 704, 1, "MessageRejected",
                                         payloadJson("order-poison"), topic()));

    std::unique_ptr<KafkaEventConsumer> consumer = makeConsumer(store, handler);
    consumer->setPreCommitHook(hook.hook());

    PollStream s;
    pollUntilRecords(*consumer, 4, kPollDeadlineMs, &s);
    ASSERT_EQ(4u, s.records.size());

    size_t advanced = 0;
    size_t deadLettered = 0;
    for (size_t i = 0; i < s.records.size(); ++i) {
        if (s.dispositions[i] == ConsumeDisposition::Advanced) {
            ++advanced;
        }
        if (s.dispositions[i] == ConsumeDisposition::DeadLettered) {
            ++deadLettered;
        }
    }
    EXPECT_EQ(3u, advanced);
    EXPECT_EQ(1u, deadLettered);
    ASSERT_EQ(3u, handler.handled().size());  // poison 不进 handler

    // 时序：全部 handle 先于 commit（hook 在 OffsetCommit 发出前打时标）。
    ASSERT_GE(hook.commitAttempts(), 1);
    EXPECT_LT(handler.lastHandleTick(), hook.lastCommitTick())
        << "every handle must complete before offset commit";
    // dead-letter 已落库（含 dead-letter 的批全部终态后才 commit 的证据面之一）。
    ASSERT_EQ(1u, store.deadLetters(10).size());
    EXPECT_EQ(std::string("unknown_event_type"), store.deadLetters(10)[0].reason);
    expectBrokerOffsetsMatch(*wire_, topic(), s.committedThrough);
}

// ---- RED ⑦b：commit 传输失败注入 → 本轮不提交、下轮重放安全 ----
// preCommitHook 内拆代理（fetch 已成功、commit 无法送达）→ 本轮 offsetCommitted
// 不发生（broker 侧停留上一提交点）→ 代理恢复 + hook 被动 → 下轮重放同一事件：
// DuplicateNoOp、attemptCount 不重复加（重放结果与 RED ② 一致）。
TEST_F(InMemoryConsumeFixture, CommitTransportFailureReplaysSafely)
{
    ConsumeHarness h;
    const AcceptOutcome m1 = h.acceptPending(kBob, "ctf-1");
    ASSERT_TRUE(m1.ok);

    KafkaPublisher publisher = makePublisher();
    publishOne(publisher, envelopeFor(m1, topic()));

    h.sink.setNext(DeliverDisposition::Accepted);

    KafkaTestProxy proxy(host_, port_);
    proxy.setUp();

    Sequencer seq;
    PreCommitController hook(&seq);

    std::unique_ptr<KafkaEventConsumer> consumer(new KafkaEventConsumer(
        host_, proxy.port(), kTestTopic, kGroupId, h.store, h.handler, kFetchBatchLimit,
        kConsumeDeadlineMs));
    consumer->setPreCommitHook(hook.hook());

    // phase 1（代理通）：正常消费提交。
    {
        PollStream s1;
        pollUntilRecordsTolerant(*consumer, 1, kPollDeadlineMs, &s1);
        ASSERT_EQ(1u, s1.records.size());
        EXPECT_EQ(ConsumeDisposition::Advanced, s1.dispositions[0]);
        expectBrokerOffsetsMatch(*wire_, topic(), s1.committedThrough);
        const std::vector<Delivery> ds = h.deliveryOf(m1.messageId);
        ASSERT_EQ(1u, ds.size());
        EXPECT_EQ(DeliveryState::InFlight, ds[0].state);
    }

    // phase 2：commit 传输失败注入（hook 在 commit 前拆代理——fetch 已完成，
    // OffsetCommit 无法送达 broker）。
    const AcceptOutcome m2 = h.acceptPending(kBob, "ctf-2");
    ASSERT_TRUE(m2.ok);
    publishOne(publisher, envelopeFor(m2, topic()));
    // [仲裁修复] m1 未 ACK 时同 conversation HOL 使 m2 永远 Pending（P3-07 单在途
    // 语义）——沿 RED⑤ 先例先 ACK m1 放行；sink 保持 Closed 使 ACK 触发的 claim
    // 不提前投递 m2（Pending→InFlight 仍归因于 consumer 的 wakeup）。
    h.sink.setNext(DeliverDisposition::Closed);
    {
        const AckOutcome ack = h.rm.acknowledge(SessionIdentity(kBob, 1), m1.messageId);
        ASSERT_EQ(AckResult::Acknowledged, ack.result);
    }
    ASSERT_EQ(DeliveryState::Pending, h.deliveryOf(m2.messageId)[0].state);
    // [仲裁修复] 捕获点后移到 ACK 块之后（ACK 期 claim 对 m2 的 1 次 Closed 计入
    // 基线），phase 2 的 1 次 Accepted 才是 attemptsBeforePhase2+1 的增量。
    const size_t attemptsBeforePhase2 = h.sink.attempts().size();

    hook.setTearDownOnce(&proxy);
    h.sink.setNext(DeliverDisposition::Accepted);
    PollStream s2;
    pollUntilRecordsTolerant(*consumer, 1, kPollDeadlineMs, &s2);
    ASSERT_EQ(1u, s2.records.size());
    EXPECT_EQ(m2.messageId.value, s2.records[0].messageId);
    EXPECT_EQ(ConsumeDisposition::Advanced, s2.dispositions[0]);  // 已 handle
    {
        const std::vector<Delivery> ds = h.deliveryOf(m2.messageId);
        ASSERT_EQ(1u, ds.size());
        EXPECT_EQ(DeliveryState::InFlight, ds[0].state);  // 处理已生效
        EXPECT_EQ(1u, ds[0].attemptCount);
    }

    // 本轮不提交：m2 所在分区的 broker 侧提交点仍停在 phase 1（< offset+1）。
    {
        std::map<int32_t, int64_t> broker = wire_->fetchCommitted(kGroupId, topic());
        ASSERT_EQ(1u, broker.count(s2.records[0].partition));
        EXPECT_LT(broker[s2.records[0].partition], s2.records[0].offset + 1)
            << "commit transport failure must leave the offset uncommitted";
    }

    // 代理恢复 + hook 被动：下轮重放同一事件（未提交 → 重 fetch），重放安全。
    proxy.setUp();
    hook.setPassive();
    pollUntilRecordsTolerant(*consumer, 2, kPollDeadlineMs, &s2);
    ASSERT_GE(s2.records.size(), 2u);
    EXPECT_EQ(m2.messageId.value, s2.records[s2.records.size() - 1].messageId);
    EXPECT_EQ(ConsumeDisposition::DuplicateNoOp,
              s2.dispositions[s2.dispositions.size() - 1]);
    {
        const std::vector<Delivery> ds = h.deliveryOf(m2.messageId);
        ASSERT_EQ(1u, ds.size());
        EXPECT_EQ(1u, ds[0].attemptCount);  // attemptCount 不重复加
        EXPECT_EQ(DeliveryState::InFlight, ds[0].state);
    }
    EXPECT_EQ(attemptsBeforePhase2 + 1, h.sink.attempts().size());  // 无重复投递

    // 重放后提交成功：broker 侧推进到 m2.offset+1。
    {
        std::map<int32_t, int64_t> broker = wire_->fetchCommitted(kGroupId, topic());
        ASSERT_EQ(1u, broker.count(s2.records[0].partition));
        EXPECT_EQ(s2.records[0].offset + 1, broker[s2.records[0].partition]);
    }
}

// ---- RED ⑧：broker 故障（停/起）→ poll 无副作用不崩溃，重启后恢复消费 ----
TEST_F(InMemoryConsumeFixture, BrokerDownPollHasNoSideEffectsAndRecovers)
{
    ConsumeHarness h;
    const AcceptOutcome m1 = h.acceptPending(kBob, "brk-1");
    ASSERT_TRUE(m1.ok);
    KafkaPublisher publisher = makePublisher();
    publishOne(publisher, envelopeFor(m1, topic()));
    h.sink.setNext(DeliverDisposition::Accepted);

    KafkaTestProxy proxy(host_, port_);
    proxy.setUp();

    std::unique_ptr<KafkaEventConsumer> consumer(new KafkaEventConsumer(
        host_, proxy.port(), kTestTopic, kGroupId, h.store, h.handler, kFetchBatchLimit,
        kConsumeDeadlineMs));

    // 代理通：正常消费。
    {
        PollStream s;
        pollUntilRecords(*consumer, 1, kPollDeadlineMs, &s);
        ASSERT_EQ(1u, s.records.size());
        EXPECT_EQ(ConsumeDisposition::Advanced, s.dispositions[0]);
    }

    // 停 broker（代理 down = 连接被拒/断连）→ brokerOk=false、无副作用、不崩溃。
    const size_t attemptsBeforeDown = h.sink.attempts().size();
    proxy.setDown();
    {
        const int64_t t0 = nowMs();
        const OutboxConsumeResult r = consumer->poll(1000);
        const int64_t elapsed = nowMs() - t0;
        EXPECT_LT(elapsed, 5000) << "poll must be bounded on broker failure";
        EXPECT_FALSE(r.brokerOk);
        EXPECT_TRUE(r.records.empty());
        EXPECT_TRUE(r.dispositions.empty());
        EXPECT_FALSE(r.offsetCommitted);
        // 无副作用：状态不变、无 dead-letter、无新 attempt。
        EXPECT_EQ(DeliveryState::InFlight, h.deliveryOf(m1.messageId)[0].state);
        EXPECT_TRUE(h.store.deadLetters(10).empty());
        EXPECT_EQ(attemptsBeforeDown, h.sink.attempts().size());
    }

    // BlackholeServer（accept 后不回包）：poll deadline 有界返回 brokerOk=false。
    {
        BlackholeServer blackhole;
        KafkaEventConsumer dead(host_, blackhole.port(), kTestTopic, kGroupId, h.store,
                                h.handler, kFetchBatchLimit, 300);
        const int64_t t0 = nowMs();
        const OutboxConsumeResult r = dead.poll(300);
        const int64_t elapsed = nowMs() - t0;
        EXPECT_LT(elapsed, 3000) << "poll must respect deadline against blackhole";
        EXPECT_FALSE(r.brokerOk);
        EXPECT_TRUE(r.records.empty());
    }

    // 重启 broker → 恢复消费（新事件经被测 consumer 正常推进）。
    const AcceptOutcome m2 = h.acceptPending(kBob, "brk-2");
    ASSERT_TRUE(m2.ok);
    publishOne(publisher, envelopeFor(m2, topic()));
    // [仲裁修复] 同 RED⑦：m1 未 ACK 时同 conversation HOL 使 m2 停留 Pending；
    // 先 ACK m1 放行（sink 保持 Closed 使 ACK 触发的 claim 不提前投递 m2），
    // 恢复 Accepted 后 m2 的 Pending→InFlight 才归因于 consumer 的 wakeup。
    h.sink.setNext(DeliverDisposition::Closed);
    {
        const AckOutcome ack = h.rm.acknowledge(SessionIdentity(kBob, 1), m1.messageId);
        ASSERT_EQ(AckResult::Acknowledged, ack.result);
    }
    ASSERT_EQ(DeliveryState::Pending, h.deliveryOf(m2.messageId)[0].state);
    proxy.setUp();
    h.sink.setNext(DeliverDisposition::Accepted);
    {
        PollStream s;
        pollUntilRecords(*consumer, 1, kPollDeadlineMs, &s);
        ASSERT_EQ(1u, s.records.size());
        EXPECT_EQ(m2.messageId.value, s.records[0].messageId);
        EXPECT_EQ(ConsumeDisposition::Advanced, s.dispositions[0]);
        EXPECT_EQ(DeliveryState::InFlight, h.deliveryOf(m2.messageId)[0].state);
        expectBrokerOffsetsMatch(*wire_, topic(), s.committedThrough);
    }
}

// ---- RED ⑨：handler 抛出（store 瞬时异常面）→ 批中止不提交、下轮重放安全 ----
// 前缀事件处置成功 + 目标事件 handler 抛出（等价 store 瞬时异常，D2：不
// dead-letter、不提交 offset）→ poll 不抛；本批零提交（preCommitHook 零调用 +
// broker 停留上一提交点）；下轮 poll 重放同批：前缀 DuplicateNoOp（lastSeen
// 批中止保留 + claimFor fencing）、目标 Advanced、Delivery 无重复副作用、
// broker offset 一次推进到位（L-2 升格 RED 场景；L-3 lastSeen 语义一并钉死）。
//
// 批前提：前缀/目标在首次 poll 前均已发布（publishOne 同步 ack）且远小于
// fetchBatchLimit=100 与 1MiB/分区上限 → 首个 fetch 批必同时含两者；前缀与
// 目标分属独立 conversation（无 HOL 耦合，重放轮目标才能独立 Advanced）。
class ThrowingOnceProgressHandler : public DeliveryProgressHandler {
public:
    ThrowingOnceProgressHandler(WakeupProgressHandler& inner, uint64_t targetMessageId)
        : inner_(inner), target_(targetMessageId) {}

    void disarm() { armed_ = false; }

    ConsumeDisposition handle(const ConsumedOutboxRecord& record) override
    {
        if (armed_ && record.messageId == target_) {
            ++throws_;
            throw std::runtime_error("injected transient store failure");
        }
        return inner_.handle(record);
    }

    int throws() const { return throws_; }

private:
    WakeupProgressHandler& inner_;
    uint64_t target_;
    bool armed_ = true;
    int throws_ = 0;
};

TEST_F(InMemoryConsumeFixture, HandlerThrowAbortsBatchAndReplaysSafely)
{
    ConsumeHarness h;
    // 独立 conversation：prefix=alice→bob、target=alice→carol（无 HOL 耦合，
    // 目标重放轮可独立 Advanced）。
    const AcceptOutcome prefix = h.acceptPending(kBob, "throw-1");
    const AcceptOutcome target = h.acceptPending(kCarol, "throw-2");
    ASSERT_TRUE(prefix.ok);
    ASSERT_TRUE(target.ok);

    KafkaPublisher publisher = makePublisher();
    publishOne(publisher, envelopeFor(prefix, topic()));
    publishOne(publisher, envelopeFor(target, topic()));

    h.sink.setNext(DeliverDisposition::Accepted);

    ThrowingOnceProgressHandler thrower(h.handler, target.messageId.value);
    Sequencer seq;
    PreCommitController hook(&seq);  // passive：只计数 commit 尝试（"无 commit"证据）

    std::unique_ptr<KafkaEventConsumer> consumer = makeConsumer(h.store, thrower);
    consumer->setPreCommitHook(hook.hook());

    // 第一轮：目标 handler 抛出 → poll 不抛、批中止（批内已成功处置的前缀
    // 记录也不提交）。
    PollStream s1;
    try {
        const int64_t d1 = nowMs() + kPollDeadlineMs;
        while (thrower.throws() == 0) {
            ASSERT_LT(nowMs(), d1) << "phase1: handler never saw the target record";
            OutboxConsumeResult r = consumer->poll(2000);
            s1.merge(r);
            // 批中止语义：目标抛出前，本用例不允许任何 commit（首 fetch 批必含
            // 前缀+目标——见用例头"批前提"）。
            ASSERT_EQ(0, hook.commitAttempts())
                << "phase1: no offset commit may happen before the aborted batch";
        }
    } catch (...) {
        ADD_FAILURE() << "poll must not propagate handler exceptions";
    }
    ASSERT_GE(thrower.throws(), 1);
    // 批中止前 prefix 已被处置成功，Delivery 已推进（InFlight、attempt=1）。
    {
        std::vector<Delivery> ds = h.deliveryOf(prefix.messageId);
        ASSERT_EQ(1u, ds.size());
        EXPECT_EQ(DeliveryState::InFlight, ds[0].state);
        EXPECT_EQ(1u, ds[0].attemptCount);
    }
    // 零提交证据面：broker 侧停留上一提交点（批内每个分区 < 该批 offset+1）。
    {
        std::map<int32_t, int64_t> broker = wire_->fetchCommitted(kGroupId, topic());
        ASSERT_FALSE(s1.records.empty())
            << "aborted batch must still surface the records it fetched";
        for (size_t i = 0; i < s1.records.size(); ++i) {
            ASSERT_EQ(1u, broker.count(s1.records[i].partition));
            EXPECT_LT(broker[s1.records[i].partition], s1.records[i].offset + 1)
                << "aborted batch must stay uncommitted (p=" << s1.records[i].partition
                << " off=" << s1.records[i].offset << ")";
        }
    }
    // store 瞬时异常绝不 dead-letter（D2：非 poison，重放而非落库）。
    EXPECT_TRUE(h.store.deadLetters(10).empty());
    const size_t attemptsAfterAbort = h.sink.attempts().size();

    // 第二轮：解除注入 → 下轮 poll 重放同批（未提交 → 同区间重 fetch）。
    thrower.disarm();
    PollStream s2;
    bool sawPrefixNoOp = false;
    bool sawTargetAdvanced = false;
    {
        const int64_t d2 = nowMs() + kPollDeadlineMs;
        while (!(sawPrefixNoOp && sawTargetAdvanced)) {
            ASSERT_LT(nowMs(), d2) << "phase2: replay must re-handle the aborted batch";
            OutboxConsumeResult r = consumer->poll(2000);
            ASSERT_EQ(r.records.size(), r.dispositions.size());
            s2.merge(r);
            for (size_t i = 0; i < s2.records.size(); ++i) {
                if (s2.records[i].messageId == prefix.messageId.value
                    && s2.dispositions[i] == ConsumeDisposition::DuplicateNoOp) {
                    sawPrefixNoOp = true;  // lastSeen 批中止保留 + claimFor fencing
                }
                if (s2.records[i].messageId == target.messageId.value
                    && s2.dispositions[i] == ConsumeDisposition::Advanced) {
                    sawTargetAdvanced = true;
                }
            }
        }
    }
    EXPECT_TRUE(sawPrefixNoOp) << "replayed prefix must be DuplicateNoOp";
    EXPECT_TRUE(sawTargetAdvanced) << "replayed target must reach Advanced";

    // 无重复副作用：行数不变、attemptCount 各恰 1、无新 dead-letter。
    {
        std::vector<Delivery> da = h.deliveryOf(prefix.messageId);
        ASSERT_EQ(1u, da.size());
        EXPECT_EQ(DeliveryState::InFlight, da[0].state);
        EXPECT_EQ(1u, da[0].attemptCount);
        std::vector<Delivery> db = h.deliveryOf(target.messageId);
        ASSERT_EQ(1u, db.size());
        EXPECT_EQ(DeliveryState::InFlight, db[0].state);
        EXPECT_EQ(1u, db[0].attemptCount);
        EXPECT_EQ(1u, h.store.deliveriesByRecipient(kBob).size());
        EXPECT_EQ(1u, h.store.deliveriesByRecipient(kCarol).size());
        EXPECT_TRUE(h.store.deadLetters(10).empty());
    }
    // 重放仅补目标的 1 次 attempt（前缀 fencing 无 attempt、无重复投递）。
    ASSERT_EQ(attemptsAfterAbort + 1, h.sink.attempts().size());
    EXPECT_EQ(target.messageId.value, h.sink.attempts().back().messageId.value);

    // offset 一次推进到位：重放批 commit 后 broker 每分区 == 该分区 max offset+1，
    // 且与 committedThrough 强一致（收窄语义下覆盖分区的强断言面）。
    EXPECT_GE(hook.commitAttempts(), 1);
    expectBrokerOffsetsMatch(*wire_, topic(), s2.committedThrough);
    {
        std::map<int32_t, int64_t> maxOff;
        for (size_t i = 0; i < s2.records.size(); ++i) {
            std::map<int32_t, int64_t>::iterator cur = maxOff.find(s2.records[i].partition);
            if (cur == maxOff.end() || s2.records[i].offset > cur->second) {
                maxOff[s2.records[i].partition] = s2.records[i].offset;
            }
        }
        std::map<int32_t, int64_t> broker = wire_->fetchCommitted(kGroupId, topic());
        for (std::map<int32_t, int64_t>::const_iterator it = maxOff.begin();
             it != maxOff.end(); ++it) {
            ASSERT_EQ(1u, broker.count(it->first));
            EXPECT_EQ(it->second + 1, broker[it->first])
                << "partition " << it->first << " must commit through replayed batch end";
        }
    }
}

// ---- H 修复：同 conversation 多条 record 一批处理、批未提交 → 重放批 DuplicateNoOp ----
// 同 conversation（alice→bob）两条 record（seq1/seq2）同一 fetch 批处理（seq1
// Advanced、seq2 因 HOL 而 DuplicateNoOp），批未提交（preCommitHook 抛出 = kill，
// 与 RED ② 同款注入 seam）→ 解除 kill 后同实例重放同批：seq1 是先前已 Advanced 的
// record，重放必须 DuplicateNoOp（同 (conversation,messageId) 已见 → DuplicateNoOp
// 置于 seq<lastSeen 回归规则之前），绝不误判 sequence_regression 落 dead-letter。
TEST_F(InMemoryConsumeFixture, ReplayBatchWithMultipleRecordsPerConversationIsDuplicateNoOp)
{
    ConsumeHarness h;
    // 同 conversation：alice→bob 两条消息（真实 accept 分配的 seq1/seq2）。
    const AcceptOutcome m1 = h.acceptPending(kBob, "replay-m1");
    const AcceptOutcome m2 = h.acceptPending(kBob, "replay-m2");
    ASSERT_TRUE(m1.ok);
    ASSERT_TRUE(m2.ok);
    ASSERT_EQ(m1.conversationId.value, m2.conversationId.value);
    ASSERT_LT(m1.sequence.value, m2.sequence.value);

    KafkaPublisher publisher = makePublisher();
    publishOne(publisher, envelopeFor(m1, topic()));
    publishOne(publisher, envelopeFor(m2, topic()));

    h.sink.setNext(DeliverDisposition::Accepted);

    Sequencer seq;
    PreCommitController killHook(&seq);
    killHook.setThrow();  // 批未提交：commit 前 kill（与 RED ② 同款注入）

    std::unique_ptr<KafkaEventConsumer> consumer = makeConsumer(h.store, h.handler);
    consumer->setPreCommitHook(killHook.hook());

    // 首轮：seq1/seq2 同一批处理（seq1 Advanced、seq2 HOL DuplicateNoOp），
    // lastSeen[conv] 推进到 seq2；批未提交（commit 全被 kill 拦截）。
    PollStream s1;
    {
        const int64_t d1 = nowMs() + kPollDeadlineMs;
        while (s1.distinctMessageIds().size() < 2) {
            ASSERT_LT(nowMs(), d1) << "phase1: expected 2 distinct records";
            OutboxConsumeResult r = consumer->poll(2000);
            ASSERT_TRUE(r.brokerOk);
            ASSERT_EQ(r.records.size(), r.dispositions.size());
            s1.merge(r);
        }
        bool sawSeq1Advanced = false;
        bool sawSeq2 = false;
        for (size_t i = 0; i < s1.records.size(); ++i) {
            if (s1.records[i].messageId == m1.messageId.value
                && s1.dispositions[i] == ConsumeDisposition::Advanced) {
                sawSeq1Advanced = true;
            }
            if (s1.records[i].messageId == m2.messageId.value) {
                sawSeq2 = true;
            }
        }
        EXPECT_TRUE(sawSeq1Advanced);
        EXPECT_TRUE(sawSeq2);
        EXPECT_GE(killHook.commitAttempts(), 1);  // commit 曾被尝试（被 kill 拦截）
    }

    // 重放：解除 kill，同实例下一轮 re-fetch 同批（cursor 未推进 → 未提交区间重放）。
    killHook.setPassive();
    PollStream s2;
    {
        const int64_t d2 = nowMs() + kPollDeadlineMs;
        bool sawSeq1NoOp = false;
        while (!sawSeq1NoOp) {
            ASSERT_LT(nowMs(), d2) << "phase2: replay must re-observe seq1 as DuplicateNoOp";
            OutboxConsumeResult r = consumer->poll(2000);
            ASSERT_TRUE(r.brokerOk);
            ASSERT_EQ(r.records.size(), r.dispositions.size());
            s2.merge(r);
            for (size_t i = 0; i < s2.records.size(); ++i) {
                if (s2.records[i].messageId == m1.messageId.value
                    && s2.dispositions[i] == ConsumeDisposition::DuplicateNoOp) {
                    sawSeq1NoOp = true;
                }
            }
        }
        // H 断言：seq1 重放 DuplicateNoOp、绝不 dead-letter（sequence_regression）。
        for (size_t i = 0; i < s2.records.size(); ++i) {
            if (s2.records[i].messageId == m1.messageId.value) {
                EXPECT_EQ(ConsumeDisposition::DuplicateNoOp, s2.dispositions[i]);
            }
        }
        // 无 dead-letter 行（seq1 未被误判 sequence_regression 落库）。
        EXPECT_TRUE(h.store.deadLetters(10).empty());
        // 重放不重复副作用：Delivery 行/attemptCount/state 不变。
        std::vector<Delivery> ds = h.deliveryOf(m1.messageId);
        ASSERT_EQ(1u, ds.size());
        EXPECT_EQ(DeliveryState::InFlight, ds[0].state);
        EXPECT_EQ(1u, ds[0].attemptCount);
        // 重放批最终提交。
        expectBrokerOffsetsMatch(*wire_, topic(), s2.committedThrough);
    }
}

// ---- P4-06 L-5 容量驱逐（KafkaEventConsumer 有界 seen；docs/tasks/P4-06.md）----
// 注入小 cap=3（构造参数 seenConversationsCapacity，默认 0 = 冻结常量 100）。
// 所有 conversation 取同一分区（partition = conversationId % 3），故消费顺序 =
// 分区内 offset 序 = 发布序（确定性）。驱逐语义：超限丢弃最旧（conversationOrder
// 队首），只影响重放去重窗口；被驱逐 conversation 的重复 record 不再被内存 seen
// 去重、重新到达 handler，由 handler 幂等（IdempotentRecordingHandler 模拟生产
// DB/claimFor-fencing 兜底）收敛为 DuplicateNoOp——无重复副作用。
TEST_F(InMemoryConsumeFixture, SeenCapacityEvictsOldestConversation)
{
    InMemoryMessageStore store;  // 仅作 dead-letter port（本用例无 dead-letter）
    IdempotentRecordingHandler handler;
    KafkaPublisher publisher = makePublisher();

    // 5 个不同 conversation（conv % 3 == 2 → 全进 partition 2，offset 序=发布序）。
    // A=1001/B=1004/C=1007/D=1010/E=1013；cap=3 下第 4 个（D）驱逐 A、第 5 个
    // （E）驱逐 B——最旧两个被驱逐，C/D/E 保留。
    const uint64_t kPartition = 2;
    const uint64_t convA = 1001, convB = 1004, convC = 1007, convD = 1010, convE = 1013;
    const uint64_t midA = 990201, midB = 990202, midC = 990203, midD = 990204, midE = 990205;

    std::unique_ptr<KafkaEventConsumer> consumer = makeConsumerWithCapacity(store, handler, 3);

    // 阶段 1：消费 5 个不同 conversation（4+），全部 Advanced。
    publishOne(publisher, craftedRequest(midA, convA, 1, "MessageAccepted",
                                         payloadJson("evict-a"), topic()));
    publishOne(publisher, craftedRequest(midB, convB, 1, "MessageAccepted",
                                         payloadJson("evict-b"), topic()));
    publishOne(publisher, craftedRequest(midC, convC, 1, "MessageAccepted",
                                         payloadJson("evict-c"), topic()));
    publishOne(publisher, craftedRequest(midD, convD, 1, "MessageAccepted",
                                         payloadJson("evict-d"), topic()));
    publishOne(publisher, craftedRequest(midE, convE, 1, "MessageAccepted",
                                         payloadJson("evict-e"), topic()));
    {
        PollStream s;
        pollUntilRecords(*consumer, 5, kPollDeadlineMs, &s);
        ASSERT_EQ(5u, s.records.size());
        for (size_t i = 0; i < s.records.size(); ++i) {
            EXPECT_EQ(kPartition, s.records[i].partition) << "record " << i;
            EXPECT_EQ(ConsumeDisposition::Advanced, s.dispositions[i]);
        }
        ASSERT_EQ(5u, handler.handled().size());
    }

    // 阶段 2：重投保留 conversation（C/D/E）的重复 record——内存 seen 仍去重 →
    // DuplicateNoOp、handler 不被再次调用（保留集无重复投递）。
    publishOne(publisher, craftedRequest(midC, convC, 1, "MessageAccepted",
                                         payloadJson("evict-c"), topic()));
    publishOne(publisher, craftedRequest(midD, convD, 1, "MessageAccepted",
                                         payloadJson("evict-d"), topic()));
    publishOne(publisher, craftedRequest(midE, convE, 1, "MessageAccepted",
                                         payloadJson("evict-e"), topic()));
    {
        PollStream s;
        pollUntilRecords(*consumer, 3, kPollDeadlineMs, &s);
        ASSERT_EQ(3u, s.records.size());
        for (size_t i = 0; i < s.records.size(); ++i) {
            EXPECT_EQ(kPartition, s.records[i].partition) << "record " << i;
            EXPECT_EQ(ConsumeDisposition::DuplicateNoOp, s.dispositions[i]);
        }
        // 未驱逐的保持 FIFO 去重：C/D/E 各恰被 handler 处理一次（无重复投递）。
        ASSERT_EQ(5u, handler.handled().size());
    }

    // 阶段 3：重投被驱逐 conversation（A/B）的重复 record——内存 seen 不再去重
    // （最旧已驱逐）→ 重新到达 handler（handled 计数 +1），但 handler 幂等兜底
    // 返回 DuplicateNoOp → 无重复副作用（无第二个 Advanced）。
    publishOne(publisher, craftedRequest(midA, convA, 1, "MessageAccepted",
                                         payloadJson("evict-a"), topic()));
    publishOne(publisher, craftedRequest(midB, convB, 1, "MessageAccepted",
                                         payloadJson("evict-b"), topic()));
    {
        PollStream s;
        pollUntilRecords(*consumer, 2, kPollDeadlineMs, &s);
        ASSERT_EQ(2u, s.records.size());
        for (size_t i = 0; i < s.records.size(); ++i) {
            EXPECT_EQ(kPartition, s.records[i].partition) << "record " << i;
            EXPECT_EQ(ConsumeDisposition::DuplicateNoOp, s.dispositions[i]);
        }
    }

    // 聚合断言：A/B（最旧、FIFO 先驱逐）的重复 record 已重投到 handler（各 2 次）；
    // C/D/E（保留）各 1 次（重投被内存 seen 去重、未到达 handler）。
    std::map<uint64_t, int> counts;
    for (size_t i = 0; i < handler.handled().size(); ++i) {
        ++counts[handler.handled()[i]];
    }
    EXPECT_EQ(2, counts[midA]) << "evicted oldest conversation re-delivered to handler";
    EXPECT_EQ(2, counts[midB]) << "FIFO: second-oldest evicted next";
    EXPECT_EQ(1, counts[midC]) << "retained conversation: no duplicate delivery";
    EXPECT_EQ(1, counts[midD]) << "retained conversation: no duplicate delivery";
    EXPECT_EQ(1, counts[midE]) << "retained conversation: no duplicate delivery";
    ASSERT_EQ(7u, handler.handled().size());
    // 无 dead-letter（全部合法 MessageAccepted；驱逐不引入 sequence_regression）。
    EXPECT_TRUE(store.deadLetters(10).empty());
}

// ---- P4-06 L-5 容量边界：恰好 cap 内不驱逐；等于 cap 时最新保留 ----
TEST_F(InMemoryConsumeFixture, SeenCapacityBoundaryKeepsRecent)
{
    InMemoryMessageStore store;
    IdempotentRecordingHandler handler;
    KafkaPublisher publisher = makePublisher();

    // 4 个不同 conversation（conv % 3 == 0 → 全进 partition 0，offset 序=发布序）。
    // cap=3：A/B/C 恰好填满（不驱逐）；第 4 个 D 到达时驱逐最旧 A、最新 D 保留。
    const uint64_t kPartition = 0;
    const uint64_t convA = 2001, convB = 2004, convC = 2007, convD = 2010;
    const uint64_t midA = 990301, midB = 990302, midC = 990303, midD = 990304;

    std::unique_ptr<KafkaEventConsumer> consumer = makeConsumerWithCapacity(store, handler, 3);

    // 阶段 1：恰好 cap 内 3 个不同 conversation，全部 Advanced（无驱逐）。
    publishOne(publisher, craftedRequest(midA, convA, 1, "MessageAccepted",
                                         payloadJson("bound-a"), topic()));
    publishOne(publisher, craftedRequest(midB, convB, 1, "MessageAccepted",
                                         payloadJson("bound-b"), topic()));
    publishOne(publisher, craftedRequest(midC, convC, 1, "MessageAccepted",
                                         payloadJson("bound-c"), topic()));
    {
        PollStream s;
        pollUntilRecords(*consumer, 3, kPollDeadlineMs, &s);
        ASSERT_EQ(3u, s.records.size());
        for (size_t i = 0; i < s.records.size(); ++i) {
            EXPECT_EQ(kPartition, s.records[i].partition) << "record " << i;
            EXPECT_EQ(ConsumeDisposition::Advanced, s.dispositions[i]);
        }
        ASSERT_EQ(3u, handler.handled().size());
    }

    // 阶段 2：恰好 cap 内不驱逐——A/B/C 的重复 record 全部被内存 seen 去重
    // （DuplicateNoOp）、handler 不被再次调用。
    publishOne(publisher, craftedRequest(midA, convA, 1, "MessageAccepted",
                                         payloadJson("bound-a"), topic()));
    publishOne(publisher, craftedRequest(midB, convB, 1, "MessageAccepted",
                                         payloadJson("bound-b"), topic()));
    publishOne(publisher, craftedRequest(midC, convC, 1, "MessageAccepted",
                                         payloadJson("bound-c"), topic()));
    {
        PollStream s;
        pollUntilRecords(*consumer, 3, kPollDeadlineMs, &s);
        ASSERT_EQ(3u, s.records.size());
        for (size_t i = 0; i < s.records.size(); ++i) {
            EXPECT_EQ(kPartition, s.records[i].partition) << "record " << i;
            EXPECT_EQ(ConsumeDisposition::DuplicateNoOp, s.dispositions[i]);
        }
        ASSERT_EQ(3u, handler.handled().size());  // 无重复投递
    }

    // 阶段 3a：等于 cap 时第 4 个 conversation 到达 → 最新 D 保留（Advanced）。
    publishOne(publisher, craftedRequest(midD, convD, 1, "MessageAccepted",
                                         payloadJson("bound-d"), topic()));
    {
        PollStream s;
        pollUntilRecords(*consumer, 1, kPollDeadlineMs, &s);
        ASSERT_EQ(1u, s.records.size());
        EXPECT_EQ(kPartition, s.records[0].partition);
        EXPECT_EQ(ConsumeDisposition::Advanced, s.dispositions[0]);
    }
    // 阶段 3b：D（最新）的重复 record 仍被内存 seen 去重（最新保留）。
    publishOne(publisher, craftedRequest(midD, convD, 1, "MessageAccepted",
                                         payloadJson("bound-d"), topic()));
    {
        PollStream s;
        pollUntilRecords(*consumer, 1, kPollDeadlineMs, &s);
        ASSERT_EQ(1u, s.records.size());
        EXPECT_EQ(ConsumeDisposition::DuplicateNoOp, s.dispositions[0]);
    }
    // 阶段 3c：A（最旧，被 D 驱逐）的重复 record 重新到达 handler（内存 seen
    // 已失效）→ handler 幂等兜底 DuplicateNoOp。
    publishOne(publisher, craftedRequest(midA, convA, 1, "MessageAccepted",
                                         payloadJson("bound-a"), topic()));
    {
        PollStream s;
        pollUntilRecords(*consumer, 1, kPollDeadlineMs, &s);
        ASSERT_EQ(1u, s.records.size());
        EXPECT_EQ(ConsumeDisposition::DuplicateNoOp, s.dispositions[0]);
    }

    // 聚合断言：A 被驱逐（handler 处理 2 次）；B/C/D 各 1 次（无重复投递）。
    std::map<uint64_t, int> counts;
    for (size_t i = 0; i < handler.handled().size(); ++i) {
        ++counts[handler.handled()[i]];
    }
    EXPECT_EQ(2, counts[midA]) << "oldest evicted when cap reached (A dropped)";
    EXPECT_EQ(1, counts[midB]) << "within-cap conversation retained";
    EXPECT_EQ(1, counts[midC]) << "within-cap conversation retained";
    EXPECT_EQ(1, counts[midD]) << "newest retained at cap boundary";
    ASSERT_EQ(5u, handler.handled().size());
    EXPECT_TRUE(store.deadLetters(10).empty());
}

// ---- RED ⑥（MySQL SQL leg）：KafkaDeadLetter 行直接 SQL 断言（契约双跑）----

const char* kMySqlTestDb = "chat_p404_consume";

std::string repoRoot()
{
    std::string file(__FILE__);
    size_t pos = file.find("tests/unit/");
    if (pos == std::string::npos) {
        return "";
    }
    return file.substr(0, pos);
}

void resetMySqlDb()
{
    MySQL admin;
    ASSERT_TRUE(admin.connect("127.0.0.1", "root", MySqlTestFixture::password(), "", 3306))
        << "MySQL unavailable (this test requires a local MySQL server)";
    ASSERT_TRUE(admin.update(std::string("DROP DATABASE IF EXISTS ") + kMySqlTestDb));
    ASSERT_TRUE(admin.update(std::string("CREATE DATABASE ") + kMySqlTestDb
        + " DEFAULT CHARSET utf8"));
    schema_migration::Migrator migrator("127.0.0.1", "root", MySqlTestFixture::password(),
                                        kMySqlTestDb, 3306);
    schema_migration::MigrateResult r =
        migrator.migrateTo(repoRoot() + "sql/migrations", "", 30);
    ASSERT_TRUE(r.ok) << "schema migration failed: " << r.error;
}

void seedMySqlUsers()
{
    MySQL conn;
    ASSERT_TRUE(conn.connect("127.0.0.1", "root", MySqlTestFixture::password(), kMySqlTestDb,
                             3306));
    ASSERT_TRUE(conn.update("INSERT INTO User(id, name, password) VALUES (1,'u1','x'),(2,'u2','x')"));
}

ConnectionPool& mySqlPool()
{
    static ConnectionPool* instance = [] {
        ConnectionPool* p = &ConnectionPool::getInstance();
        p->init("127.0.0.1", "root", MySqlTestFixture::password(), kMySqlTestDb, 3306, 8);
        return p;
    }();
    return *instance;
}

long long sqlCount(MySQL& conn, const std::string& sql)
{
    MYSQL_RES* res = conn.query(sql);
    if (!res) {
        return -1;
    }
    MYSQL_ROW row = mysql_fetch_row(res);
    long long n = (row && row[0]) ? atoll(row[0]) : -1;
    mysql_free_result(res);
    return n;
}

// MySQL dead-letter 契约：与 InMemory leg 同构造（poison 三类 + good 一条），
// 断言经直接 SQL（KafkaDeadLetter 行存在、reason 可区分、Message/Delivery 只推进
// 不重建）+ deadLetters 谓词双面一致。串行约束：独占 chat_p404_consume 库与单例
// 连接池（SetUpTestSuite 重建），不与其它测试二进制混跑。
class MySqlDeadLetterDb : public KafkaConsumeFixture {
protected:
    static void SetUpTestSuite()
    {
        resetMySqlDb();
        seedMySqlUsers();
        (void)mySqlPool();
    }

    void TearDown() override
    {
        {
            MySQL conn;
            ASSERT_TRUE(conn.connect("127.0.0.1", "root", MySqlTestFixture::password(),
                                     kMySqlTestDb, 3306));
            ASSERT_TRUE(conn.update("DELETE FROM KafkaDeadLetter"));
            ASSERT_TRUE(conn.update("DELETE FROM ChatMessage"));
            ASSERT_TRUE(conn.update("DELETE FROM DirectConversation"));
            ASSERT_TRUE(conn.update("DELETE FROM GroupConversation"));
            ASSERT_TRUE(conn.update("DELETE FROM Conversation"));
        }
        KafkaConsumeFixture::TearDown();
    }
};

TEST_F(MySqlDeadLetterDb, PoisonDeadLetterRowsQueryableViaSql)
{
    FakeClock clock;
    clock.set(kNow);
    MySQLMessageStore store(mySqlPool(), kFanOutCap);
    ScriptedDeliverySink sink(DeliverDisposition::Closed);
    ReliableMessaging rm(store, sink, clock, kLeaseMs, consumeRetryConfig());
    WakeupProgressHandler handler(store, rm);
    rm.sessionAvailable(SessionIdentity(kBob, 1));

    const AcceptOutcome good = rm.accept(SessionIdentity(kAlice, 1),
                                         directTo(kBob, "mysql-poison", "content"));
    ASSERT_TRUE(good.ok);
    ASSERT_EQ(1u, store.deliveriesByMessage(good.messageId).size());

    KafkaPublisher publisher = makePublisher();
    publishOne(publisher, craftedRequest(999041, 611, 1, "MessageAccepted",
                                         "{not json", topic()));
    publishOne(publisher, craftedRequest(999042, 612, 1, "MessageRejected",
                                         payloadJson("mysql-type"), topic()));
    publishOne(publisher, craftedRequest(999043, 613, 1, "MessageAccepted",
                                         payloadJson("mysql-missing"), topic()));
    publishOne(publisher, envelopeFor(good, topic()));

    sink.setNext(DeliverDisposition::Accepted);

    std::unique_ptr<KafkaEventConsumer> consumer = makeConsumer(store, handler);
    PollStream s;
    pollUntilRecords(*consumer, 4, kPollDeadlineMs, &s);
    ASSERT_EQ(4u, s.records.size());

    std::map<uint64_t, ConsumeDisposition> byMid;
    for (size_t i = 0; i < s.records.size(); ++i) {
        byMid[s.records[i].messageId] = s.dispositions[i];
    }
    EXPECT_EQ(ConsumeDisposition::DeadLettered, byMid[999041]);
    EXPECT_EQ(ConsumeDisposition::DeadLettered, byMid[999042]);
    EXPECT_EQ(ConsumeDisposition::DeadLettered, byMid[999043]);
    EXPECT_EQ(ConsumeDisposition::Advanced, byMid[good.messageId.value]);

    // 直接 SQL 断言：行存在、reason 可区分（绝不只日志丢弃）。
    MySQL conn;
    ASSERT_TRUE(conn.connect("127.0.0.1", "root", MySqlTestFixture::password(), kMySqlTestDb,
                             3306));
    const std::string where = "WHERE topic='" + std::string(kTestTopic) + "'";
    EXPECT_EQ(3, sqlCount(conn, "SELECT COUNT(*) FROM KafkaDeadLetter " + where));
    EXPECT_EQ(1, sqlCount(conn, "SELECT COUNT(*) FROM KafkaDeadLetter " + where
        + " AND reason='poison_payload'"));
    EXPECT_EQ(1, sqlCount(conn, "SELECT COUNT(*) FROM KafkaDeadLetter " + where
        + " AND reason='unknown_event_type'"));
    EXPECT_EQ(1, sqlCount(conn, "SELECT COUNT(*) FROM KafkaDeadLetter " + where
        + " AND reason='message_missing'"));

    // 只推进不重建：good 消息恰好一行 MessageDelivery 且已推进 InFlight。
    std::vector<Delivery> ds = store.deliveriesByMessage(good.messageId);
    ASSERT_EQ(1u, ds.size());
    EXPECT_EQ(DeliveryState::InFlight, ds[0].state);
    EXPECT_EQ(1u, ds[0].attemptCount);
    EXPECT_EQ(1, sqlCount(conn, "SELECT COUNT(*) FROM MessageDelivery WHERE message_id="
        + std::to_string(good.messageId.value)));

    // 谓词面与 SQL 面一致（双跑同构）。
    EXPECT_EQ(3u, store.deadLetters(10).size());
    expectBrokerOffsetsMatch(*wire_, topic(), s.committedThrough);
}

} // namespace
