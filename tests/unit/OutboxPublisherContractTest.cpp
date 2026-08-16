// P4-03 OutboxPublisherContract RED：OutboxPublisher port 与 Recording/Kafka 双
// adapter 的公开接口契约（docs/tasks/P4-03.md §Interface/§RED/§冻结参数）。
//
// RED 依据（现状）：本文件引用尚不存在的 app/OutboxPublisher.hpp（抽象 port +
// OutboxPublishRequest/OutboxPublishOutcome/PublishFailure 值类型）、
// app/RecordingPublisher.hpp（测试 adapter）、app/KafkaPublisher.hpp（真实 broker
// adapter）→ 编译失败（`app/OutboxPublisher.hpp: No such file or directory`）即合法
// RED（P4-03 卡 §RED：预期失败 = 类型不存在 → 编译失败 exit 1）。
//
// 真实 Kafka 集成（127.0.0.1:9092，env KAFKA_TEST_HOST/PORT 可覆盖，沿 P4-02
// REDIS_TEST_* 惯例），**不 skip**：SetUp 连不上 broker 即测试失败（P4-03 完成
// 定义）。测试 topic 前缀 `muduo-test-`（冻结参数），每个测试用唯一 topic
// （`muduo-test-outbox-<pid>-<seq>`）在 SetUp 显式建 3 分区（CreateTopics v2）、
// TearDown 删除（DeleteTopics v1）——隔离与 key=ConversationId 的分区路由断言都
// 成立；生产 topic 名 `muduo-outbox` 不在测试范围。
//
// 本文件冻结的实现契约（GREEN 据此实现；命名/语义冻结，参照 P4-01/P4-02 先例）：
//
//   // ---- app/OutboxPublisher.hpp ----
//   struct OutboxPublishRequest {
//       OutboxEvent event;             // relay 已 claim 的事件（含递增后 attemptCount）
//       ConversationId conversationId; // Kafka 分区键（字符串化，冻结参数）
//       std::string topic;             // 目标 topic（测试注入 muduo-test- 前缀隔离）
//   };
//   enum class PublishFailure { None, Timeout, BrokerUnavailable, PayloadTooLarge, Other };
//   struct OutboxPublishOutcome {
//       bool ok = false;
//       PublishFailure failure = PublishFailure::Other;
//       std::string error;             // 失败原因摘要（日志/指标，不含敏感 payload）
//   };
//   class OutboxPublisher {
//   public:
//       virtual ~OutboxPublisher() = default;
//       // 批量 publish（batch 有界，调用方不超上限）；返回逐事件结果（顺序与入参
//       // 一致）。deadlineMs 为整体操作软期限；超时/断连/broker 错误 → 对应事件
//       // 失败，不抛、不写库（publish 不改变 Message/Delivery，P4-04 消费者幂等）。
//       virtual std::vector<OutboxPublishOutcome> publish(
//           const std::vector<OutboxPublishRequest>& batch, int64_t deadlineMs) = 0;
//   };
//
//   // ---- app/RecordingPublisher.hpp（测试 adapter，进 chatserver_core）----
//   // 记录每个 publish 请求与逐事件结果；默认全成功，可脚本化注入失败（按
//   // eventId 或 topic），失败不阻断同批（逐事件）。
//   class RecordingPublisher : public OutboxPublisher {
//   public:
//       RecordingPublisher();
//       std::vector<OutboxPublishOutcome> publish(
//           const std::vector<OutboxPublishRequest>& batch, int64_t deadlineMs) override;
//       void failEvent(uint64_t eventId, PublishFailure failure);  // 注入失败
//       void failTopic(const std::string& topic, PublishFailure failure);
//       void clearFailures();
//       size_t publishCalls() const;              // publish() 调用次数
//       const std::vector<std::vector<OutboxPublishRequest> >& batches() const; // 每次入参
//       const std::vector<OutboxPublishRequest>& requests() const; // 全部事件扁平化（保序）
//       const std::vector<OutboxPublishOutcome>& lastOutcomes() const; // 最近一次逐事件结果
//       int64_t lastDeadlineMs() const;           // 最近一次 publish 的 deadline
//   };
//
//   // ---- app/KafkaPublisher.hpp（真实 broker adapter，进 chatserver_core）----
//   class KafkaPublisher : public OutboxPublisher {
//   public:
//       // topicPrefix：安全前缀守卫——request.topic 不以该前缀开头的请求返回
//       // PublishFailure::Other（fail-safe，测试隔离由 muduo-test- 保证）；非空时
//       // 强制。deadlineMs 为默认操作期限（每次 publish 的显式 deadlineMs 覆盖之）。
//       KafkaPublisher(const std::string& host, int port, const std::string& topicPrefix,
//                      int64_t deadlineMs);
//       std::vector<OutboxPublishOutcome> publish(
//           const std::vector<OutboxPublishRequest>& batch, int64_t deadlineMs) override;
//       // 每个事件一条 record，key = ConversationId 字符串；value = JSON 信封
//       //   {"message_id":...,"conversation_id":...,"sequence":...,"event_type":
//       //    "MessageAccepted","payload":<命令快照 JSON>}
//       // sequence 为 Conversation 内序号（供 P4-04 断言 partition 内顺序不倒退）；
//       // 请求结构不含 sequence 时，GREEN 须在 relay/publisher 侧解决来源（可扩展
//       // OutboxPublishRequest 或由 relay 传 Message），本测试仅断言信封含整数
//       // sequence 字段。
//   };
//
// 测试自身实现最小 Kafka wire 客户端（Metadata/ListOffsets/Fetch/CreateTopics/
// DeleteTopics，Fetch v4 读 magic-2 record batch）作消费侧断言——P4-04 无 consumer
// 组件，卡 §非目标；消费侧只读，经主题内消息数/内容与 publish 一致断言
// "publish 不写库"（RED 3）。
//
// 无固定 sleep：broker 重启（KafkaTestProxy 端点上 toggle）、broker 恢复（有界
// 轮询 readAll 到 deadline）、timeout 注入（BlackholeServer）全部有界。

#include "app/Config.hpp"             // OutboxConfig（batch 上限，P3-09 冻结）
#include "app/DomainTypes.hpp"        // OutboxEvent/MessageId/ConversationId
#include "app/OutboxPublisher.hpp"    // RED：尚不存在 → 编译失败即合法 RED
#include "app/RecordingPublisher.hpp" // RED：尚不存在 → 编译失败即合法 RED
#include "app/KafkaPublisher.hpp"     // RED：尚不存在 → 编译失败即合法 RED

#include "json.hpp"  // thirdparty 单头 nlohmann（与 ProtocolCodec.hpp 同源）

#include <gtest/gtest.h>

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
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

const char* kTestTopicPrefix = "muduo-test-";
const int32_t kPartitions = 3;  // 冻结测试 topic 分区数（P4-03 §冻结参数）
const int64_t kApiTimeoutMs = 3000;
const int64_t kReadDeadlineMs = 8000;

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

std::string nextTestTopic()
{
    static std::atomic<uint64_t> counter{0};
    const uint64_t c = counter.fetch_add(1);
    return std::string(kTestTopicPrefix) + "outbox-" + std::to_string(getpid()) + "-"
        + std::to_string(c);
}

// ---- 值类型构造 ----

OutboxPublishRequest makeRequest(uint64_t eventId, MessageId mid, ConversationId conv,
                                 const std::string& topic, const std::string& payload)
{
    OutboxPublishRequest r;
    r.event.id = eventId;
    r.event.aggregateMessageId = mid;
    r.event.eventType = "MessageAccepted";
    r.event.payload = payload;
    r.event.availableAtMs = 1000000;
    r.event.attemptCount = 1;  // relay claim 后 attempt+1
    r.event.processedAtMs = 0;
    r.conversationId = conv;
    r.topic = topic;
    return r;
}

// P3-04 冻结的命令快照 JSON（测试用合法小 JSON；payload 对 publisher 不透明）。
std::string payloadJson(const std::string& cmid)
{
    return std::string("{\"kind\":\"direct\",\"cmid\":\"") + cmid + "\"}";
}

// 消费侧读回的 record（partition 由读取器填充，供 key=ConversationId 分区路由断言）。
struct KafkaRecord {
    int32_t partition = -1;
    int64_t offset = 0;
    std::string key;
    std::string value;
};

// 信封断言：value = 冻结 JSON 信封；key = ConversationId 字符串（RED 1 / 冻结参数）。
void expectEnvelope(const KafkaRecord& rec, const OutboxPublishRequest& req)
{
    EXPECT_EQ(std::to_string(req.conversationId.value), rec.key);
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(rec.value);
    } catch (const std::exception& e) {
        FAIL() << "envelope not JSON: " << e.what();
        return;
    }
    EXPECT_TRUE(j.contains("message_id"));
    EXPECT_TRUE(j.contains("conversation_id"));
    EXPECT_TRUE(j.contains("event_type"));
    EXPECT_TRUE(j.contains("payload"));
    if (!j.contains("message_id") || !j.contains("conversation_id") || !j.contains("event_type")
        || !j.contains("payload")) {
        return;
    }
    EXPECT_EQ(req.event.aggregateMessageId.value, j["message_id"].get<uint64_t>());
    EXPECT_EQ(req.conversationId.value, j["conversation_id"].get<uint64_t>());
    EXPECT_EQ(req.event.eventType, j["event_type"].get<std::string>());
    EXPECT_EQ(req.event.payload, j["payload"].get<std::string>());
    ASSERT_TRUE(j.contains("sequence"));
    ASSERT_TRUE(j["sequence"].is_number_integer());
    EXPECT_GE(j["sequence"].get<int64_t>(), 0);
}

// ---- Kafka wire 最小客户端（只读/管理，P4-04 无 consumer 组件前的消费侧断言）----

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
    bool bytes(std::string* out)
    {
        int32_t len;
        if (!i32(&len) || len < 0) {
            return false;
        }
        if (remaining() < static_cast<size_t>(len)) {
            return false;
        }
        out->assign(data_, pos_, static_cast<size_t>(len));
        pos_ += static_cast<size_t>(len);
        return true;
    }
    bool nullableBytes(std::string* out, bool* isNull)
    {
        int32_t len;
        if (!i32(&len)) {
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

// 单连接 Kafka wire 客户端：请求/响应帧（correlation 匹配），供消费侧断言。
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

// 消费侧读取器：Metadata（v1）+ ListOffsets（v1）+ Fetch（v4，magic-2 record
// batch）+ CreateTopics（v2）+ DeleteTopics（v1）。
class KafkaTestConsumer {
public:
    explicit KafkaTestConsumer(const std::string& host, int port)
        : host_(host), port_(port), conn_(KafkaConn::open(host, port))
    {
        if (!conn_) {
            throw std::runtime_error("KafkaTestConsumer: connect failed (broker unavailable)");
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
                throw std::runtime_error("metadata error for " + topic + ": " + std::to_string(err));
            }
        }
        return out;
    }

    void createTopic(const std::string& topic, int32_t numPartitions, int16_t replication,
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
        for (int32_t i = 0; i < tc; ++i) {
            std::string name;
            int16_t err;
            std::string emsg;
            bool null;
            if (!r.str(&name) || !r.i16(&err) || !r.nullableStr(&emsg, &null)) {
                throw std::runtime_error("bad createTopic result");
            }
            if (name == topic && err != 0) {
                throw std::runtime_error("createTopic " + topic + " failed: err=" + std::to_string(err));
            }
        }
    }

    void deleteTopic(const std::string& topic, int32_t timeoutMs)
    {
        if (topic.empty()) {
            return;
        }
        std::string resp;
        ByteWriter w;
        w.i32(1);              // topic names
        w.str(topic);
        w.i32(timeoutMs);
        // apiKey 20 = DeleteTopics（44 是 IncrementalAlterConfigs，误用会让 broker
        // 解析失败并留残留 topic）。
        if (!conn_->request(20, 1, w.data(), timeoutMs + kApiTimeoutMs, &resp)) {
            return;  // 清理 best-effort
        }
        ByteReader r(resp);
        int32_t throttle = 0;
        int32_t tc = 0;
        if (!r.i32(&throttle) || !r.i32(&tc)) {
            return;
        }
        for (int32_t i = 0; i < tc; ++i) {
            std::string name;
            int16_t err;
            if (!r.str(&name) || !r.i16(&err)) {
                return;
            }
            (void)err;
        }
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

    struct FetchResult {
        std::vector<KafkaRecord> records;
        int64_t highWatermark = 0;
        int16_t errorCode = 0;
    };

    FetchResult fetch(const std::string& topic, int32_t partition, int64_t fetchOffset,
                      int64_t maxWaitMs)
    {
        std::string resp;
        ByteWriter w;
        w.i32(-1);                       // replicaId
        w.i32(static_cast<int32_t>(maxWaitMs));
        w.i32(1);                        // minBytes
        w.i32(1 << 20);                  // maxBytes
        w.i8(0);                         // isolationLevel (read_uncommitted)
        w.i32(1);                        // topics
        w.str(topic);
        w.i32(1);                        // partitions
        w.i32(partition);
        w.i64(fetchOffset);
        w.i32(1 << 20);                  // partitionMaxBytes
        if (!conn_->request(1, 4, w.data(), kApiTimeoutMs, &resp)) {
            throw std::runtime_error("fetch request failed");
        }
        ByteReader r(resp);
        int32_t throttle = 0;
        int32_t tc = 0;
        if (!r.i32(&throttle) || !r.i32(&tc)) {
            throw std::runtime_error("bad fetch response");
        }
        FetchResult out;
        for (int32_t i = 0; i < tc; ++i) {
            std::string name;
            int32_t pc;
            if (!r.str(&name) || !r.i32(&pc)) {
                throw std::runtime_error("bad fetch topic");
            }
            for (int32_t p = 0; p < pc; ++p) {
                int32_t idx;
                int16_t err;
                int64_t hw;
                int64_t lso;
                int32_t abortCnt;
                if (!r.i32(&idx) || !r.i16(&err) || !r.i64(&hw) || !r.i64(&lso)
                    || !r.i32(&abortCnt)) {
                    throw std::runtime_error("bad fetch partition header");
                }
                if (abortCnt > 0) {
                    for (int32_t a = 0; a < abortCnt; ++a) {
                        int64_t pid;
                        int64_t foff;
                        if (!r.i64(&pid) || !r.i64(&foff)) {
                            throw std::runtime_error("bad aborted transaction");
                        }
                    }
                }
                std::string records;
                bool nullRecords;
                if (!r.nullableBytes(&records, &nullRecords)) {
                    throw std::runtime_error("bad fetch records");
                }
                if (name == topic && idx == partition) {
                    out.errorCode = err;
                    out.highWatermark = hw;
                    if (!nullRecords && !parseRecordSet(records, &out.records)) {
                        throw std::runtime_error("parse record set failed");
                    }
                }
            }
        }
        return out;
    }

    // 有界轮询读全分区到 expected 条（无固定 sleep；deadline 上限）。
    std::vector<KafkaRecord> readAll(const std::string& topic, size_t expected,
                                     int64_t deadlineMs)
    {
        std::vector<KafkaRecord> all;
        if (expected == 0) {
            return all;
        }
        std::vector<int32_t> parts = partitions(topic);
        if (parts.empty()) {
            throw std::runtime_error("readAll: no partitions for " + topic);
        }
        std::map<int32_t, int64_t> cursors;
        for (size_t i = 0; i < parts.size(); ++i) {
            cursors[parts[i]] = earliestOffset(topic, parts[i]);
        }
        const int64_t deadline = nowMs() + deadlineMs;
        while (all.size() < expected) {
            if (nowMs() > deadline) {
                throw std::runtime_error("readAll deadline exceeded: got "
                    + std::to_string(all.size()) + " expected " + std::to_string(expected));
            }
            bool progress = false;
            for (size_t i = 0; i < parts.size(); ++i) {
                const int32_t p = parts[i];
                FetchResult fr = fetch(topic, p, cursors[p], 300);
                for (size_t j = 0; j < fr.records.size(); ++j) {
                    KafkaRecord rec = fr.records[j];
                    rec.partition = p;
                    all.push_back(rec);
                    if (rec.offset >= cursors[p]) {
                        cursors[p] = rec.offset + 1;
                    }
                }
                if (!fr.records.empty()) {
                    progress = true;
                }
            }
            if (!progress) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
        return all;
    }

    bool topicHasNoRecords(const std::string& topic)
    {
        std::vector<int32_t> parts = partitions(topic);
        for (size_t i = 0; i < parts.size(); ++i) {
            const int32_t p = parts[i];
            const int64_t start = earliestOffset(topic, p);
            FetchResult fr = fetch(topic, p, start, 300);
            if (!fr.records.empty()) {
                return false;
            }
        }
        return true;
    }

private:
    // magic-2 record batch 解析（KafkaRecord.offset = baseOffset + offsetDelta）。
    static bool readVarint(const std::string& data, size_t* pos, int64_t* out)
    {
        uint64_t result = 0;
        int shift = 0;
        for (int i = 0; i < 10; ++i) {
            if (*pos >= data.size()) {
                return false;
            }
            const uint8_t b = static_cast<uint8_t>(data[*pos]);
            ++*pos;
            result |= static_cast<uint64_t>(b & 0x7F) << shift;
            if ((b & 0x80) == 0) {
                break;
            }
            shift += 7;
        }
        const int64_t decoded = static_cast<int64_t>(result >> 1);
        *out = (result & 1) ? ~decoded : decoded;
        return true;
    }

    static bool parseRecordSet(const std::string& bytes, std::vector<KafkaRecord>* out)
    {
        const size_t n = bytes.size();
        if (n == 0 || (n == 1 && bytes[0] == 0)) {
            return true;  // 空 record set 标记
        }
        size_t pos = 0;
        while (pos + 12 <= n) {
            const int64_t baseOffset = (static_cast<int64_t>(uint8_t(bytes[pos])) << 56)
                | (static_cast<int64_t>(uint8_t(bytes[pos + 1])) << 48)
                | (static_cast<int64_t>(uint8_t(bytes[pos + 2])) << 40)
                | (static_cast<int64_t>(uint8_t(bytes[pos + 3])) << 32)
                | (static_cast<int64_t>(uint8_t(bytes[pos + 4])) << 24)
                | (static_cast<int64_t>(uint8_t(bytes[pos + 5])) << 16)
                | (static_cast<int64_t>(uint8_t(bytes[pos + 6])) << 8)
                | static_cast<int64_t>(uint8_t(bytes[pos + 7]));
            const int32_t batchLength = (uint8_t(bytes[pos + 8]) << 24)
                | (uint8_t(bytes[pos + 9]) << 16) | (uint8_t(bytes[pos + 10]) << 8)
                | uint8_t(bytes[pos + 11]);
            pos += 12;
            const size_t batchEnd = pos + static_cast<size_t>(batchLength);
            if (batchEnd > n) {
                return false;
            }
            pos += 4;  // partitionLeaderEpoch
            const uint8_t magic = static_cast<uint8_t>(bytes[pos]);
            pos += 1;
            if (magic != 2) {
                return false;  // 仅 magic-2 record batch
            }
            pos += 4 + 2 + 4 + 8 + 8 + 8 + 2 + 4;  // crc/attributes/lastOffsetDelta/.../baseSequence
            int32_t recordCount = (uint8_t(bytes[pos]) << 24) | (uint8_t(bytes[pos + 1]) << 16)
                | (uint8_t(bytes[pos + 2]) << 8) | uint8_t(bytes[pos + 3]);
            pos += 4;
            for (int32_t i = 0; i < recordCount; ++i) {
                int64_t recLen = 0;
                if (!readVarint(bytes, &pos, &recLen) || recLen < 0) {
                    return false;
                }
                const size_t recEnd = pos + static_cast<size_t>(recLen);
                if (recEnd > batchEnd) {
                    return false;
                }
                int64_t attributes = 0;
                int64_t tsDelta = 0;
                int64_t offsetDelta = 0;
                if (!readVarint(bytes, &pos, &attributes) || !readVarint(bytes, &pos, &tsDelta)
                    || !readVarint(bytes, &pos, &offsetDelta)) {
                    return false;
                }
                int64_t keyLen = 0;
                if (!readVarint(bytes, &pos, &keyLen)) {
                    return false;
                }
                KafkaRecord rec;
                if (keyLen >= 0) {
                    rec.key.assign(bytes, pos, static_cast<size_t>(keyLen));
                    pos += static_cast<size_t>(keyLen);
                }
                int64_t valueLen = 0;
                if (!readVarint(bytes, &pos, &valueLen)) {
                    return false;
                }
                if (valueLen >= 0) {
                    rec.value.assign(bytes, pos, static_cast<size_t>(valueLen));
                    pos += static_cast<size_t>(valueLen);
                }
                int64_t headerCount = 0;
                if (!readVarint(bytes, &pos, &headerCount)) {
                    return false;
                }
                for (int64_t h = 0; h < headerCount; ++h) {
                    int64_t hkLen = 0;
                    if (!readVarint(bytes, &pos, &hkLen)) {
                        return false;
                    }
                    if (hkLen >= 0) {
                        pos += static_cast<size_t>(hkLen);
                    }
                    int64_t hvLen = 0;
                    if (!readVarint(bytes, &pos, &hvLen)) {
                        return false;
                    }
                    if (hvLen >= 0) {
                        pos += static_cast<size_t>(hvLen);
                    }
                }
                rec.offset = baseOffset + offsetDelta;
                out->push_back(rec);
                pos = recEnd;
            }
            pos = batchEnd;
        }
        return true;
    }

    std::string host_;
    int port_ = 0;
    std::unique_ptr<KafkaConn> conn_;
};

// accept 后不回包：publisher 操作超 deadline → Timeout 故障注入（RED 4 / RED 6）。
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

// 可控 TCP 转发代理：up = 转发到真实 broker；down = 关闭监听（连接被拒）→
// BrokerUnavailable。同一端点（代理端口）可 down/up toggle，模拟"停 broker →
// 重启 broker"（RED 4：重启后重 publish 成功且已发布事件可消费）。
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

// 信封断言经 consumer 读回（RED 1）。夹具：建/删唯一 3 分区测试 topic；broker
// 不可用 → SetUp 失败（不 skip，P4-03 完成定义）。
class KafkaOutboxFixture : public ::testing::Test {
protected:
    void SetUp() override
    {
        host_ = testHost();
        port_ = testPort();
        consumer_.reset(new KafkaTestConsumer(host_, port_));
        ASSERT_TRUE(consumer_->brokerReachable())
            << "Kafka unavailable (requires local broker on " << host_ << ":" << port_ << ")";
        topic_ = nextTestTopic();
        consumer_->createTopic(topic_, kPartitions, 1, 5000);
        // KRaft 单节点下 createTopic 返回后 broker 的 metadata cache 可能尚未同步
        // （Metadata 对刚建的 topic 短暂返回 UNKNOWN_TOPIC_OR_PARTITION）；分区
        // leader 也异步分配（ListOffsets 短暂 NOT_LEADER）。有界轮询：分区数就绪 +
        // 全部 leader 就绪（无固定 sleep；deadline 上限）后才继续。
        const int64_t readyDeadline = nowMs() + kReadDeadlineMs;
        for (;;) {
            std::vector<int32_t> parts;
            try {
                parts = consumer_->partitions(topic_);
            } catch (const std::exception&) {
                parts.clear();
            }
            bool ready = parts.size() == static_cast<size_t>(kPartitions);
            for (size_t i = 0; i < parts.size() && ready; ++i) {
                try {
                    consumer_->earliestOffset(topic_, parts[i]);
                } catch (const std::exception&) {
                    ready = false;
                }
            }
            if (ready) {
                break;
            }
            if (nowMs() > readyDeadline) {
                FAIL() << "partition metadata/leaders not ready for " << topic_;
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    void TearDown() override
    {
        if (consumer_) {
            consumer_->deleteTopic(topic_, 5000);
        }
    }

    std::unique_ptr<KafkaPublisher> makePublisher(int64_t deadlineMs = 5000)
    {
        return std::unique_ptr<KafkaPublisher>(
            new KafkaPublisher(host_, port_, kTestTopicPrefix, deadlineMs));
    }

    const std::string& topic() const { return topic_; }
    KafkaTestConsumer& consumer() { return *consumer_; }

    std::string host_;
    int port_ = 9092;
    std::unique_ptr<KafkaTestConsumer> consumer_;
    std::string topic_;
};

// 双 adapter 共用的"批量 publish → 逐事件结果全 ok"契约（RED 1）：
// 结果个数 = 入参个数、全部 ok、顺序与入参一致。
void expectAllOkOutcomes(const std::vector<OutboxPublishOutcome>& outcomes, size_t expected)
{
    ASSERT_EQ(expected, outcomes.size());
    for (size_t i = 0; i < outcomes.size(); ++i) {
        EXPECT_TRUE(outcomes[i].ok) << "event " << i << " error: " << outcomes[i].error;
        EXPECT_EQ(PublishFailure::None, outcomes[i].failure);
    }
}

// 取一个当前无监听的端口（bind 到 ephemeral 再关闭 → 连接被拒，模拟 broker 不可达）。
int freePort()
{
    int s = ::socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    (void)::bind(s, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    socklen_t len = sizeof(addr);
    (void)::getsockname(s, reinterpret_cast<struct sockaddr*>(&addr), &len);
    int p = ntohs(addr.sin_port);
    ::close(s);
    return p;
}

// RED 1：批量 publish → 逐事件结果（个数 = 入参、全 ok、顺序与入参一致）。
// Recording 与 Kafka 双 adapter 同契约。
TEST_F(KafkaOutboxFixture, PublishReturnsPerEventResults)
{
    std::vector<OutboxPublishRequest> batch;
    batch.push_back(makeRequest(1, MessageId(11), ConversationId(10), topic(), payloadJson("r-1")));
    batch.push_back(makeRequest(2, MessageId(12), ConversationId(20), topic(), payloadJson("r-2")));
    batch.push_back(makeRequest(3, MessageId(13), ConversationId(30), topic(), payloadJson("r-3")));

    RecordingPublisher rec;
    std::vector<OutboxPublishOutcome> recOut = rec.publish(batch, 5000);
    expectAllOkOutcomes(recOut, batch.size());
    ASSERT_EQ(1u, rec.publishCalls());
    ASSERT_EQ(batch.size(), rec.requests().size());
    EXPECT_EQ(batch[1].event.id, rec.requests()[1].event.id);
    EXPECT_EQ(batch[2].conversationId.value, rec.requests()[2].conversationId.value);

    std::unique_ptr<KafkaPublisher> kp = makePublisher();
    std::vector<OutboxPublishOutcome> kOut = kp->publish(batch, 5000);
    expectAllOkOutcomes(kOut, batch.size());

    std::vector<KafkaRecord> recs = consumer().readAll(topic(), 3, kReadDeadlineMs);
    ASSERT_EQ(3u, recs.size());
    // readAll 返回按 (partition, offset) 序，与入参序不一致——按 message_id 回读映射。
    std::map<uint64_t, KafkaRecord> byMid;
    for (size_t i = 0; i < recs.size(); ++i) {
        nlohmann::json j = nlohmann::json::parse(recs[i].value);
        byMid[j["message_id"].get<uint64_t>()] = recs[i];
    }
    for (size_t i = 0; i < batch.size(); ++i) {
        ASSERT_EQ(1u, byMid.count(batch[i].event.aggregateMessageId.value));
        expectEnvelope(byMid[batch[i].event.aggregateMessageId.value], batch[i]);
    }
}

// RED 1（round-trip 的无副作用侧）：空批 publish → 空逐事件结果、零副作用。
// Recording 与 Kafka 双 adapter。
TEST_F(KafkaOutboxFixture, PublishEmptyBatchIsNoop)
{
    RecordingPublisher rec;
    std::vector<OutboxPublishOutcome> recOut = rec.publish(std::vector<OutboxPublishRequest>(), 5000);
    EXPECT_TRUE(recOut.empty());
    EXPECT_EQ(1u, rec.publishCalls());
    EXPECT_TRUE(rec.requests().empty());
    EXPECT_TRUE(rec.lastOutcomes().empty());

    std::unique_ptr<KafkaPublisher> kp = makePublisher();
    std::vector<OutboxPublishOutcome> kOut = kp->publish(std::vector<OutboxPublishRequest>(), 5000);
    EXPECT_TRUE(kOut.empty());
    EXPECT_TRUE(consumer().topicHasNoRecords(topic()));
}

// RED 6（有界 batch）：batch 达到冻结上限（OutboxConfig.claimBatchSize=100）时单次
// publish 完整执行、逐事件结果齐全，绝不超限、无无界等待。双 adapter。
TEST_F(KafkaOutboxFixture, BatchIsBoundedByConfig)
{
    OutboxConfig cfg;
    const uint32_t kBound = cfg.claimBatchSize;
    ASSERT_GT(kBound, 0u);

    std::vector<OutboxPublishRequest> batch;
    batch.reserve(kBound);
    for (uint32_t i = 0; i < kBound; ++i) {
        batch.push_back(makeRequest(i, MessageId(1000 + i), ConversationId(i),
                                    topic(), payloadJson("b-" + std::to_string(i))));
    }

    RecordingPublisher rec;
    expectAllOkOutcomes(rec.publish(batch, 5000), kBound);
    ASSERT_EQ(kBound, rec.requests().size());
    EXPECT_EQ(batch.size(), rec.requests().size());

    const int64_t t0 = nowMs();
    std::unique_ptr<KafkaPublisher> kp = makePublisher();
    expectAllOkOutcomes(kp->publish(batch, 5000), kBound);
    const int64_t elapsedMs = nowMs() - t0;
    EXPECT_LT(elapsedMs, 30000) << "batch publish must be bounded";

    std::vector<KafkaRecord> recs = consumer().readAll(topic(), kBound, kReadDeadlineMs);
    ASSERT_EQ(kBound, recs.size());
    std::set<uint64_t> mids;
    for (size_t i = 0; i < recs.size(); ++i) {
        nlohmann::json j = nlohmann::json::parse(recs[i].value);
        mids.insert(j["message_id"].get<uint64_t>());
    }
    EXPECT_EQ(kBound, mids.size());
}

// 冻结参数：key = ConversationId 字符串化。Recording 断言请求携带 conversationId；
// Kafka 侧经 consumer 读回 key 断言（同 key → 同分区）。双 adapter。
TEST_F(KafkaOutboxFixture, KeyIsConversationId)
{
    std::vector<OutboxPublishRequest> batch;
    batch.push_back(makeRequest(1, MessageId(21), ConversationId(10), topic(), payloadJson("k-1")));
    batch.push_back(makeRequest(2, MessageId(22), ConversationId(20), topic(), payloadJson("k-2")));

    RecordingPublisher rec;
    expectAllOkOutcomes(rec.publish(batch, 5000), 2);
    ASSERT_EQ(2u, rec.requests().size());
    EXPECT_EQ(10u, rec.requests()[0].conversationId.value);
    EXPECT_EQ(20u, rec.requests()[1].conversationId.value);

    std::unique_ptr<KafkaPublisher> kp = makePublisher();
    expectAllOkOutcomes(kp->publish(batch, 5000), 2);
    std::vector<KafkaRecord> recs = consumer().readAll(topic(), 2, kReadDeadlineMs);
    ASSERT_EQ(2u, recs.size());
    std::map<uint64_t, KafkaRecord> byMid;
    for (size_t i = 0; i < recs.size(); ++i) {
        nlohmann::json j = nlohmann::json::parse(recs[i].value);
        byMid[j["message_id"].get<uint64_t>()] = recs[i];
    }
    ASSERT_EQ(1u, byMid.count(21));
    ASSERT_EQ(1u, byMid.count(22));
    EXPECT_EQ("10", byMid[21].key);
    EXPECT_EQ("20", byMid[22].key);
}

// RED 1（Recording harness）：RecordingPublisher 的记录可查询——每次入参（batches）、
// 全部事件扁平化（requests）、逐事件结果（lastOutcomes）、deadline 透传。纯单元。
TEST(OutboxPublisherContractTest, RecordingPublisherRecordsEventsForAssertion)
{
    RecordingPublisher rec;
    const std::string t = std::string(kTestTopicPrefix) + "recording";

    std::vector<OutboxPublishRequest> b1;
    b1.push_back(makeRequest(1, MessageId(31), ConversationId(10), t, payloadJson("rec-1")));
    b1.push_back(makeRequest(2, MessageId(32), ConversationId(20), t, payloadJson("rec-2")));
    expectAllOkOutcomes(rec.publish(b1, 1111), 2);

    std::vector<OutboxPublishRequest> b2;
    b2.push_back(makeRequest(3, MessageId(33), ConversationId(30), t, payloadJson("rec-3")));
    expectAllOkOutcomes(rec.publish(b2, 2222), 1);

    EXPECT_EQ(2u, rec.publishCalls());
    ASSERT_EQ(2u, rec.batches().size());
    ASSERT_EQ(2u, rec.batches()[0].size());
    ASSERT_EQ(1u, rec.batches()[1].size());
    EXPECT_EQ(1u, rec.batches()[0][0].event.id);
    EXPECT_EQ(2u, rec.batches()[0][1].event.id);
    EXPECT_EQ(3u, rec.batches()[1][0].event.id);
    ASSERT_EQ(3u, rec.requests().size());
    EXPECT_EQ(30u, rec.requests()[2].conversationId.value);
    EXPECT_EQ(t, rec.requests()[2].topic);
    ASSERT_EQ(1u, rec.lastOutcomes().size());
    EXPECT_TRUE(rec.lastOutcomes()[0].ok);
    EXPECT_EQ(2222, rec.lastDeadlineMs());
}

// RED 1（真实 Kafka round-trip）：publish 后 consumer 读回 payload/key，顺序按分区
// ——同 conversation 事件同分区且按发布顺序（offset 递增）。
TEST_F(KafkaOutboxFixture, KafkaPublishThenConsumeRoundtrip)
{
    std::vector<OutboxPublishRequest> batch;
    batch.push_back(makeRequest(1, MessageId(41), ConversationId(10), topic(), payloadJson("c10-1")));
    batch.push_back(makeRequest(2, MessageId(42), ConversationId(20), topic(), payloadJson("c20-1")));
    batch.push_back(makeRequest(3, MessageId(43), ConversationId(10), topic(), payloadJson("c10-2")));
    batch.push_back(makeRequest(4, MessageId(44), ConversationId(10), topic(), payloadJson("c10-3")));

    std::unique_ptr<KafkaPublisher> kp = makePublisher();
    expectAllOkOutcomes(kp->publish(batch, 5000), 4);

    std::vector<KafkaRecord> recs = consumer().readAll(topic(), 4, kReadDeadlineMs);
    ASSERT_EQ(4u, recs.size());
    std::map<uint64_t, KafkaRecord> byMid;
    for (size_t i = 0; i < recs.size(); ++i) {
        nlohmann::json j = nlohmann::json::parse(recs[i].value);
        byMid[j["message_id"].get<uint64_t>()] = recs[i];
    }
    // payload/key 逐条回读。
    for (size_t i = 0; i < batch.size(); ++i) {
        ASSERT_EQ(1u, byMid.count(batch[i].event.aggregateMessageId.value));
        expectEnvelope(byMid[batch[i].event.aggregateMessageId.value], batch[i]);
    }

    // 顺序按分区：key="10" 的三条（41/43/44）同分区、offset 递增、与发布顺序一致。
    std::vector<KafkaRecord> c10;
    for (size_t i = 0; i < recs.size(); ++i) {
        if (recs[i].key == "10") {
            c10.push_back(recs[i]);
        }
    }
    ASSERT_EQ(3u, c10.size());
    EXPECT_EQ(c10[0].partition, c10[1].partition);
    EXPECT_EQ(c10[1].partition, c10[2].partition);
    EXPECT_LT(c10[0].offset, c10[1].offset);
    EXPECT_LT(c10[1].offset, c10[2].offset);
    std::vector<uint64_t> idsInOrder;
    for (size_t i = 0; i < c10.size(); ++i) {
        nlohmann::json j = nlohmann::json::parse(c10[i].value);
        idsInOrder.push_back(j["message_id"].get<uint64_t>());
    }
    // readAll 天然按 (partition, offset) 排序；同分区内即发布顺序 41→43→44。
    EXPECT_EQ(41u, idsInOrder[0]);
    EXPECT_EQ(43u, idsInOrder[1]);
    EXPECT_EQ(44u, idsInOrder[2]);
}

// RED 4（broker timeout → 逐事件失败且错误可区分，不抛未分类异常）：
// Recording 注入四类故障（Timeout/BrokerUnavailable/PayloadTooLarge/Other）逐事件
// 区分、失败不阻断同批；Kafka 侧停 broker → 逐事件 BrokerUnavailable 且不抛。
TEST_F(KafkaOutboxFixture, BrokerTimeoutFailsEventsDistinctly)
{
    std::vector<OutboxPublishRequest> batch;
    batch.push_back(makeRequest(1, MessageId(51), ConversationId(10), topic(), payloadJson("f-1")));
    batch.push_back(makeRequest(2, MessageId(52), ConversationId(20), topic(), payloadJson("f-2")));
    batch.push_back(makeRequest(3, MessageId(53), ConversationId(30), topic(), payloadJson("f-3")));
    batch.push_back(makeRequest(4, MessageId(54), ConversationId(40), topic(), payloadJson("f-4")));

    RecordingPublisher rec;
    rec.failEvent(1, PublishFailure::Timeout);
    rec.failEvent(2, PublishFailure::BrokerUnavailable);
    rec.failEvent(3, PublishFailure::PayloadTooLarge);
    rec.failEvent(4, PublishFailure::Other);
    std::vector<OutboxPublishOutcome> recOut = rec.publish(batch, 5000);
    ASSERT_EQ(4u, recOut.size());
    EXPECT_FALSE(recOut[0].ok);
    EXPECT_EQ(PublishFailure::Timeout, recOut[0].failure);
    EXPECT_FALSE(recOut[1].ok);
    EXPECT_EQ(PublishFailure::BrokerUnavailable, recOut[1].failure);
    EXPECT_FALSE(recOut[2].ok);
    EXPECT_EQ(PublishFailure::PayloadTooLarge, recOut[2].failure);
    EXPECT_FALSE(recOut[3].ok);
    EXPECT_EQ(PublishFailure::Other, recOut[3].failure);
    for (size_t i = 0; i < recOut.size(); ++i) {
        EXPECT_FALSE(recOut[i].error.empty()) << "event " << i << " must carry error summary";
    }

    // Kafka 侧：停 broker（代理 down = 连接被拒）→ 逐事件 BrokerUnavailable，不抛。
    KafkaTestProxy proxy(host_, port_);
    proxy.setUp();
    proxy.setDown();
    KafkaPublisher dead(host_, proxy.port(), kTestTopicPrefix, 5000);
    std::vector<OutboxPublishOutcome> kOut = dead.publish(batch, 5000);
    ASSERT_EQ(4u, kOut.size());
    for (size_t i = 0; i < kOut.size(); ++i) {
        EXPECT_FALSE(kOut[i].ok) << "event " << i;
        EXPECT_EQ(PublishFailure::BrokerUnavailable, kOut[i].failure) << "event " << i;
        EXPECT_FALSE(kOut[i].error.empty()) << "event " << i;
    }
    EXPECT_TRUE(consumer().topicHasNoRecords(topic()));
}

// RED 2（commit 后 publish 前 kill → 未发布事件可重放）接口契约：publish 失败只
// 返回逐事件结果，不改变事件（无 processed 标记面、不写库）；失败事件可重放。
// "不标 processed"的 relay 集成断言在 OutboxRelay 回归（P3-09 语义）验证，此处
// 记录 publisher 接口契约（卡 §完成定义）。
TEST_F(KafkaOutboxFixture, PublishFailureLeavesEventUnprocessed)
{
    std::vector<OutboxPublishRequest> batch;
    batch.push_back(makeRequest(1, MessageId(61), ConversationId(10), topic(), payloadJson("unproc")));

    RecordingPublisher rec;
    rec.failTopic(topic(), PublishFailure::BrokerUnavailable);
    std::vector<OutboxPublishOutcome> out = rec.publish(batch, 5000);
    ASSERT_EQ(1u, out.size());
    EXPECT_FALSE(out[0].ok);
    ASSERT_EQ(1u, rec.requests().size());
    // 事件原样保留（processedAtMs 仍为 0、attemptCount 未被 publisher 改动）。
    EXPECT_EQ(0, rec.requests()[0].event.processedAtMs);
    EXPECT_EQ(1u, rec.requests()[0].event.attemptCount);

    // 失败后可重放：清故障，同一事件重 publish 成功。
    rec.clearFailures();
    expectAllOkOutcomes(rec.publish(batch, 5000), 1);
    ASSERT_EQ(2u, rec.publishCalls());

    // Kafka 侧：broker 不可达 publish 失败（事件未发布），broker 恢复后同一事件
    // 重 publish 成功且可消费（失败未消费/丢失事件）。
    const int deadPort = freePort();
    {
        KafkaPublisher dead(host_, deadPort, kTestTopicPrefix, 1000);
        std::vector<OutboxPublishOutcome> kOut = dead.publish(batch, 1000);
        ASSERT_EQ(1u, kOut.size());
        EXPECT_FALSE(kOut[0].ok);
        EXPECT_TRUE(!kOut[0].error.empty());
    }
    EXPECT_TRUE(consumer().topicHasNoRecords(topic()));

    std::unique_ptr<KafkaPublisher> kp = makePublisher();
    expectAllOkOutcomes(kp->publish(batch, 5000), 1);
    std::vector<KafkaRecord> recs = consumer().readAll(topic(), 1, kReadDeadlineMs);
    ASSERT_EQ(1u, recs.size());
    expectEnvelope(recs[0], batch[0]);
}

// RED 5 / RED 3（重复 publish 幂等）：同一事件连续 publish 两次，两次逐事件结果均
// ok；publisher 无 store 面 → Message/Delivery 状态不变（集成断言归 OutboxRelay
// 回归）；Kafka 中为两条同 key 同 value 的可重放 record（P4-04 消费者幂等去重）。
TEST_F(KafkaOutboxFixture, DuplicatePublishIsIdempotentAtPublisherInterface)
{
    std::vector<OutboxPublishRequest> one;
    one.push_back(makeRequest(1, MessageId(71), ConversationId(10), topic(), payloadJson("dup")));

    RecordingPublisher rec;
    expectAllOkOutcomes(rec.publish(one, 5000), 1);
    expectAllOkOutcomes(rec.publish(one, 5000), 1);
    EXPECT_EQ(2u, rec.publishCalls());
    ASSERT_EQ(2u, rec.requests().size());
    EXPECT_EQ(rec.requests()[0].event.id, rec.requests()[1].event.id);
    EXPECT_EQ(rec.requests()[0].conversationId.value, rec.requests()[1].conversationId.value);
    EXPECT_EQ(rec.requests()[0].topic, rec.requests()[1].topic);

    std::unique_ptr<KafkaPublisher> kp = makePublisher();
    expectAllOkOutcomes(kp->publish(one, 5000), 1);
    expectAllOkOutcomes(kp->publish(one, 5000), 1);

    std::vector<KafkaRecord> recs = consumer().readAll(topic(), 2, kReadDeadlineMs);
    ASSERT_EQ(2u, recs.size());
    // 两条可重放 record：同分区、同 key、同 value（内容一致，仅 offset 递增）。
    EXPECT_EQ(recs[0].partition, recs[1].partition);
    EXPECT_EQ(recs[0].key, recs[1].key);
    EXPECT_EQ(recs[0].value, recs[1].value);
    EXPECT_LT(recs[0].offset, recs[1].offset);
    expectEnvelope(recs[0], one[0]);
    expectEnvelope(recs[1], one[0]);
}

// RED 6（deadline 有界）：BlackholeServer（accept 后不回包）→ 单次 publish 受
// deadline 约束（无无界等待），逐事件 Timeout、不抛。deadline 经接口透传
// （Recording 侧断言 lastDeadlineMs）。
TEST_F(KafkaOutboxFixture, DeadlineBounded)
{
    BlackholeServer blackhole;

    std::vector<OutboxPublishRequest> batch;
    for (uint64_t i = 0; i < 5; ++i) {
        batch.push_back(makeRequest(i + 1, MessageId(80 + i), ConversationId(10 + i),
                                    topic(), payloadJson("d-" + std::to_string(i))));
    }

    // deadline 透传到 adapter（Recording 侧契约）。
    RecordingPublisher rec;
    (void)rec.publish(batch, 1234);
    EXPECT_EQ(1234, rec.lastDeadlineMs());

    const int64_t t0 = nowMs();
    KafkaPublisher dead("127.0.0.1", blackhole.port(), kTestTopicPrefix, 200);
    std::vector<OutboxPublishOutcome> out = dead.publish(batch, 200);
    const int64_t elapsedMs = nowMs() - t0;
    EXPECT_LT(elapsedMs, 5000) << "publish must be bounded by deadline (no unbounded wait)";
    ASSERT_EQ(5u, out.size());
    for (size_t i = 0; i < out.size(); ++i) {
        EXPECT_FALSE(out[i].ok) << "event " << i;
        EXPECT_EQ(PublishFailure::Timeout, out[i].failure) << "event " << i;
        EXPECT_FALSE(out[i].error.empty()) << "event " << i;
    }
}

// SP-1（安全前缀守卫，fail-safe）：publish 到非 muduo-test- 前缀 topic 的请求逐
// 事件返回 PublishFailure::Other 且不触 broker（守卫在连接 broker 前逐事件拒绝，
// 即 KafkaPublisher 绝不向生产 topic 名 muduo-outbox 误写）。前缀约束仅
// KafkaPublisher 有（RecordingPublisher 无该守卫，不在此契约内）。
TEST_F(KafkaOutboxFixture, NonTestTopicPrefixRejected)
{
    std::vector<OutboxPublishRequest> batch;
    batch.push_back(makeRequest(1, MessageId(111), ConversationId(10), "muduo-outbox",
                                payloadJson("g-1")));
    batch.push_back(makeRequest(2, MessageId(112), ConversationId(20), "prod-topic",
                                payloadJson("g-2")));

    std::unique_ptr<KafkaPublisher> kp = makePublisher();
    std::vector<OutboxPublishOutcome> out = kp->publish(batch, 5000);
    ASSERT_EQ(2u, out.size());
    for (size_t i = 0; i < out.size(); ++i) {
        EXPECT_FALSE(out[i].ok) << "event " << i;
        EXPECT_EQ(PublishFailure::Other, out[i].failure) << "event " << i;
        EXPECT_FALSE(out[i].error.empty()) << "event " << i;
    }
    // 不触 broker：守卫逐事件拒绝且不连接/不建 topic；合法测试 topic 内无 record。
    EXPECT_TRUE(consumer().topicHasNoRecords(topic()));
}

// RED 4（重启 broker 后重 publish 成功且已发布事件可消费）：同一端点（代理端口）
// 停 broker → publish 逐事件失败；重启 broker → 重 publish 成功，事件可消费。
TEST_F(KafkaOutboxFixture, BrokerRestartRecovers)
{
    KafkaTestProxy proxy(host_, port_);
    proxy.setUp();

    std::vector<OutboxPublishRequest> batch1;
    batch1.push_back(makeRequest(1, MessageId(91), ConversationId(10), topic(), payloadJson("s-1")));
    batch1.push_back(makeRequest(2, MessageId(92), ConversationId(20), topic(), payloadJson("s-2")));
    {
        KafkaPublisher p(host_, proxy.port(), kTestTopicPrefix, 5000);
        expectAllOkOutcomes(p.publish(batch1, 5000), 2);
    }
    std::vector<KafkaRecord> recsA = consumer().readAll(topic(), 2, kReadDeadlineMs);
    ASSERT_EQ(2u, recsA.size());

    // 停 broker：同一代理端口连接被拒 → 逐事件失败。
    proxy.setDown();
    std::vector<OutboxPublishRequest> batch2;
    batch2.push_back(makeRequest(3, MessageId(93), ConversationId(30), topic(), payloadJson("s-3")));
    {
        KafkaPublisher p(host_, proxy.port(), kTestTopicPrefix, 5000);
        std::vector<OutboxPublishOutcome> out = p.publish(batch2, 5000);
        ASSERT_EQ(1u, out.size());
        EXPECT_FALSE(out[0].ok);
        EXPECT_TRUE(!out[0].error.empty());
    }

    // 重启 broker：同一端点重 publish 成功，事件可消费（失败未丢失）。
    proxy.setUp();
    {
        KafkaPublisher p(host_, proxy.port(), kTestTopicPrefix, 5000);
        expectAllOkOutcomes(p.publish(batch2, 5000), 1);
    }
    std::vector<KafkaRecord> all = consumer().readAll(topic(), 3, kReadDeadlineMs);
    ASSERT_EQ(3u, all.size());
    bool found = false;
    for (size_t i = 0; i < all.size(); ++i) {
        nlohmann::json j = nlohmann::json::parse(all[i].value);
        if (j["message_id"].get<uint64_t>() == 93) {
            found = true;
            expectEnvelope(all[i], batch2[0]);
        }
    }
    EXPECT_TRUE(found) << "re-published event must be consumable after broker restart";
}

// RED 3（publish 不写库 → Message/Delivery 不变）消费侧只读验证：topic 内消息
// 数/内容与 publish 完全一致（无多无少），即"投递"就是这批 record，publisher 无
// store 面。集成层 Message/Delivery 行数断言归 OutboxRelay 回归。
TEST_F(KafkaOutboxFixture, KafkaConsumerSideDeliveryDoesNotMutateStore)
{
    std::vector<OutboxPublishRequest> batch;
    batch.push_back(makeRequest(1, MessageId(101), ConversationId(10), topic(), payloadJson("n-1")));
    batch.push_back(makeRequest(2, MessageId(102), ConversationId(20), topic(), payloadJson("n-2")));
    batch.push_back(makeRequest(3, MessageId(103), ConversationId(30), topic(), payloadJson("n-3")));

    std::unique_ptr<KafkaPublisher> kp = makePublisher();
    expectAllOkOutcomes(kp->publish(batch, 5000), 3);

    // 消费侧只读：读回集合 == 发布集合（数量/message_id/key/payload 一致，无多无少）。
    std::vector<KafkaRecord> recs = consumer().readAll(topic(), 3, kReadDeadlineMs);
    ASSERT_EQ(3u, recs.size());
    std::set<uint64_t> seen;
    for (size_t i = 0; i < recs.size(); ++i) {
        nlohmann::json j = nlohmann::json::parse(recs[i].value);
        const uint64_t mid = j["message_id"].get<uint64_t>();
        seen.insert(mid);
        EXPECT_EQ(std::to_string(j["conversation_id"].get<uint64_t>()), recs[i].key);
    }
    EXPECT_EQ(3u, seen.size());
    EXPECT_EQ(1u, seen.count(101));
    EXPECT_EQ(1u, seen.count(102));
    EXPECT_EQ(1u, seen.count(103));
    for (size_t i = 0; i < batch.size(); ++i) {
        EXPECT_EQ(1u, seen.count(batch[i].event.aggregateMessageId.value));
    }

    // 再读一次无新增（读幂等，topic 内消息数与 publish 一致）。
    std::vector<KafkaRecord> again = consumer().readAll(topic(), 3, kReadDeadlineMs);
    ASSERT_EQ(3u, again.size());
}

} // namespace
