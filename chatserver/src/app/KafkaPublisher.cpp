#include "app/KafkaPublisher.hpp"

#include "app/KafkaWire.hpp"
#include "json.hpp"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstring>
#include <ctime>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// P4-03 KafkaPublisher：真实 broker adapter（docs/tasks/P4-03.md §Interface/
// §冻结参数/§最小实现）。内置最小 Kafka wire 客户端：
//   - Metadata v1：取 topic 分区数（partition = conversationId % partitions，
//     同一 Conversation 恒进同一分区，P4-04 顺序断言依赖）。
//   - Produce v3：每事件一条 magic-2 record batch，key = ConversationId 字符串，
//     value = JSON 信封 {"message_id","conversation_id","sequence","event_type",
//     "payload"}。
// 故障分类（不抛、逐事件失败，relay 继续同批后续）：
//   - 连接拒绝/连接中断 → BrokerUnavailable；
//   - 操作超 deadline（poll 超时）→ Timeout；
//   - Produce 分区 error 3 (MESSAGE_TOO_LARGE) → PayloadTooLarge；
//   - 其余 wire/解码/broker 错误 → Other。
// deadline 有界：每次 socket 操作传入剩余 deadline（整体软期限，无无界等待）。

namespace {

int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// wallMs()（系统时钟毫秒）随 encodeRecordBatch 移入 KafkaWire.cpp（record batch
// 时间戳编码唯一使用点）。

int64_t remainingMs(int64_t startMs, int64_t deadlineMs)
{
    return deadlineMs - (nowMs() - startMs);
}

// P4-04 待定②：wire 原语（大端编解码/TcpClient/请求响应帧/varint/record batch
// 编码/CRC32C）已提取至 app/KafkaWire.{hpp,cpp} 共享 seam（KafkaEventConsumer
// 复用）；本文件保留 Metadata/Produce 编解码与故障分类，行为零变化（P4-03
// 回归作证）。clientId 固定 "muduo-publisher"（提取前内嵌于 sendRequest）。
using namespace kafka_wire;

const char* kClientId = "muduo-publisher";

// Metadata v1：返回 topic 的分区数（有界等待全部分区 leader 就绪——主题刚创建
// 时 leader 异步分配，未就绪即 produce 会得 NOT_LEADER_FOR_PARTITION 瞬时失败；
// 等待受剩余 deadline 约束，绝无无界等待）。-1 = 传输失败，0 = 不可用/无分区/
// deadline 超时。correlation 为指针：本函数可能多次请求，自行递增。
int metadataPartitions(TcpClient* tcp, const std::string& topic, int32_t* correlation,
                       int64_t startMs, int64_t deadlineMs)
{
    for (;;) {
        int64_t remain = remainingMs(startMs, deadlineMs);
        if (remain < 1) {
            remain = 1;
        }
        const int32_t corr = (*correlation)++;
        ByteWriter body;
        body.i32(1);
        body.str(topic);
        if (!sendRequest(tcp, 3, 1, kClientId, body.data(), corr, remain)) {
            return -1;
        }
        std::string resp;
        if (!recvResponse(tcp, corr, &resp, remain)) {
            return -1;
        }
        ByteReader r(resp);
        int32_t brokerCount = 0;
        if (!r.i32(&brokerCount)) {
            return -1;
        }
        for (int32_t i = 0; i < brokerCount; ++i) {
            int32_t nodeId = 0;
            std::string host;
            int32_t bport = 0;
            std::string rack;
            bool nullRack = false;
            if (!r.i32(&nodeId) || !r.str(&host) || !r.i32(&bport)
                || !r.nullableStr(&rack, &nullRack)) {
                return -1;
            }
        }
        int32_t controllerId = 0;
        if (!r.i32(&controllerId)) {
            return -1;
        }
        int32_t topicCount = 0;
        if (!r.i32(&topicCount)) {
            return -1;
        }
        bool found = false;
        for (int32_t i = 0; i < topicCount; ++i) {
            int16_t err = 0;
            std::string name;
            bool internal = false;
            int32_t pcount = 0;
            if (!r.i16(&err) || !r.str(&name) || !r.bool_(&internal) || !r.i32(&pcount)) {
                return -1;
            }
            int okPartitions = 0;
            bool leaderReady = true;
            for (int32_t p = 0; p < pcount; ++p) {
                int16_t perr = 0;
                int32_t idx = 0;
                int32_t leader = 0;
                int32_t rcount = 0;
                if (!r.i16(&perr) || !r.i32(&idx) || !r.i32(&leader) || !r.i32(&rcount)) {
                    return -1;
                }
                for (int32_t k = 0; k < rcount; ++k) {
                    int32_t rid = 0;
                    if (!r.i32(&rid)) {
                        return -1;
                    }
                }
                int32_t icount = 0;
                if (!r.i32(&icount)) {
                    return -1;
                }
                for (int32_t k = 0; k < icount; ++k) {
                    int32_t iid = 0;
                    if (!r.i32(&iid)) {
                        return -1;
                    }
                }
                if (perr == 0) {
                    ++okPartitions;
                }
                if (leader < 0) {
                    leaderReady = false;
                }
            }
            if (name == topic) {
                found = true;
                if (err != 0 || okPartitions == 0) {
                    // 主题不可用/无分区：KRaft 单节点 createTopic 后 metadata cache
                    // 可能短暂未同步（UNKNOWN_TOPIC_OR_PARTITION）——有界重试
                    // （deadline 到则失败，绝不无界等待）。
                    if (remainingMs(startMs, deadlineMs) <= 0) {
                        return 0;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    break;
                }
                if (leaderReady) {
                    return okPartitions;
                }
                // leader 未就绪（主题刚创建）：有界等待后重查；deadline 到则失败。
                if (remainingMs(startMs, deadlineMs) <= 0) {
                    return 0;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                break;
            }
        }
        if (!found) {
            return 0;
        }
    }
    return 0;
}

// Produce v3（单 topic 单 partition）：recordsBlob = 拼接的 record batches。
// brokerTimeoutMs 为请求内 broker 侧等待上限；socketTimeoutMs 为本次 socket
// 操作的 deadline（整体 publish deadline 的剩余），绝不无界。
bool produceGroup(TcpClient* tcp, const std::string& topic, int32_t partition,
                  const std::string& recordsBlob, int32_t brokerTimeoutMs,
                  int64_t socketTimeoutMs, int32_t correlation, int16_t* errorCode)
{
    ByteWriter body;
    body.nullableStrNull();     // transactional_id
    body.i16(1);                // acks = leader
    body.i32(brokerTimeoutMs);
    body.i32(1);                // topics
    body.str(topic);
    body.i32(1);                // partitions
    body.i32(partition);
    body.bytes(recordsBlob);
    if (!sendRequest(tcp, 0, 3, kClientId, body.data(), correlation, socketTimeoutMs)) {
        return false;
    }
    std::string resp;
    if (!recvResponse(tcp, correlation, &resp, socketTimeoutMs)) {
        return false;
    }
    ByteReader r(resp);
    int32_t topicCount = 0;
    if (!r.i32(&topicCount)) {
        return false;
    }
    for (int32_t i = 0; i < topicCount; ++i) {
        std::string name;
        int32_t pc = 0;
        if (!r.str(&name) || !r.i32(&pc)) {
            return false;
        }
        for (int32_t p = 0; p < pc; ++p) {
            int32_t idx = 0;
            int16_t err = 0;
            int64_t baseOff = 0;
            int64_t appendTime = 0;
            if (!r.i32(&idx) || !r.i16(&err) || !r.i64(&baseOff) || !r.i64(&appendTime)) {
                return false;
            }
            if (idx == partition) {
                *errorCode = err;
                return true;
            }
        }
    }
    return false;
}

OutboxPublishOutcome failedOutcome(PublishFailure f, const std::string& msg)
{
    OutboxPublishOutcome oc;
    oc.ok = false;
    oc.failure = f;
    oc.error = msg;
    return oc;
}

PublishFailure transportFailure(int64_t startMs, int64_t deadlineMs)
{
    return remainingMs(startMs, deadlineMs) <= 0 ? PublishFailure::Timeout
                                                 : PublishFailure::BrokerUnavailable;
}

} // namespace

KafkaPublisher::KafkaPublisher(const std::string& host, int port, const std::string& topicPrefix,
                               int64_t deadlineMs)
    : host_(host), port_(port), topicPrefix_(topicPrefix), deadlineMs_(deadlineMs)
{
}

std::vector<OutboxPublishOutcome> KafkaPublisher::publish(
    const std::vector<OutboxPublishRequest>& batch, int64_t deadlineMs)
{
    std::vector<OutboxPublishOutcome> outcomes;
    if (batch.empty()) {
        return outcomes;
    }
    outcomes.assign(batch.size(), OutboxPublishOutcome());

    const int64_t dl = (deadlineMs > 0) ? deadlineMs : deadlineMs_;
    const int64_t start = nowMs();

    // 1) topic 前缀守卫（fail-safe）：不合前缀的事件直接失败，不触 broker。
    std::vector<size_t> pending;
    for (size_t i = 0; i < batch.size(); ++i) {
        if (!topicPrefix_.empty()
            && batch[i].topic.compare(0, topicPrefix_.size(), topicPrefix_) != 0) {
            outcomes[i] = failedOutcome(PublishFailure::Other,
                                        "topic prefix guard rejected: " + batch[i].topic);
        } else {
            pending.push_back(i);
        }
    }
    if (pending.empty()) {
        return outcomes;
    }

    // 2) 连接 broker（deadline 有界）。
    TcpClient tcp;
    int64_t remain = remainingMs(start, dl);
    if (remain < 1) {
        remain = 1;
    }
    if (!tcp.connectTo(host_, port_, remain)) {
        const PublishFailure f = transportFailure(start, dl);
        const std::string msg = "broker connect failed on " + host_ + ":" + std::to_string(port_);
        for (size_t k = 0; k < pending.size(); ++k) {
            outcomes[pending[k]] = failedOutcome(f, msg);
        }
        return outcomes;
    }

    // 3) Metadata 取分区数（每 distinct topic 一次）。
    std::set<std::string> topics;
    for (size_t k = 0; k < pending.size(); ++k) {
        topics.insert(batch[pending[k]].topic);
    }
    std::map<std::string, int> partitionsByTopic;
    std::vector<size_t> remainingPending;
    int32_t correlation = 0;
    for (std::set<std::string>::const_iterator it = topics.begin(); it != topics.end(); ++it) {
        const int parts = metadataPartitions(&tcp, *it, &correlation, start, dl);
        if (parts < 0) {
            // 传输失败：分类后整体失败（剩余请求一并处理）。
            const PublishFailure f = transportFailure(start, dl);
            const std::string msg = "metadata request failed for " + *it;
            for (size_t k = 0; k < pending.size(); ++k) {
                outcomes[pending[k]] = failedOutcome(f, msg);
            }
            return outcomes;
        }
        if (parts == 0) {
            const std::string msg = "topic unavailable or has no partitions: " + *it;
            for (size_t k = 0; k < pending.size(); ++k) {
                if (batch[pending[k]].topic == *it) {
                    outcomes[pending[k]] = failedOutcome(PublishFailure::Other, msg);
                } else {
                    remainingPending.push_back(pending[k]);
                }
            }
            pending.swap(remainingPending);
            remainingPending.clear();
        } else {
            partitionsByTopic[*it] = parts;
        }
    }
    if (pending.empty()) {
        return outcomes;
    }

    // 4) 计算分区（key = ConversationId 字符串，partition = conv % partitions）并
    //    构造信封与 (key, value) 对，按 (topic, partition) 分组（每组合并进单个
    //    record batch——broker magic-2 约束只允许单 batch/produce）。
    struct Group {
        std::string topic;
        int32_t partition = 0;
        std::vector<std::pair<std::string, std::string> > kv;  // (key, value) 保序
        std::string records;  // 单 record batch（含全部 records）
        std::vector<size_t> indexes;
    };
    std::vector<Group> groups;
    std::map<std::pair<std::string, int32_t>, size_t> groupByKey;
    for (size_t k = 0; k < pending.size(); ++k) {
        const OutboxPublishRequest& req = batch[pending[k]];
        const int parts = partitionsByTopic[req.topic];
        const int32_t partition = static_cast<int32_t>(req.conversationId.value % parts);
        nlohmann::json env;
        env["message_id"] = req.event.aggregateMessageId.value;
        env["conversation_id"] = req.conversationId.value;
        env["sequence"] = req.sequence;
        env["event_type"] = req.event.eventType;
        env["payload"] = req.event.payload;
        const std::string key = std::to_string(req.conversationId.value);
        const std::string value = env.dump();
        const std::pair<std::string, int32_t> gk(req.topic, partition);
        std::map<std::pair<std::string, int32_t>, size_t>::iterator gIt = groupByKey.find(gk);
        if (gIt == groupByKey.end()) {
            Group g;
            g.topic = req.topic;
            g.partition = partition;
            g.kv.push_back(std::make_pair(key, value));
            g.indexes.push_back(pending[k]);
            groupByKey[gk] = groups.size();
            groups.push_back(g);
        } else {
            Group& g = groups[gIt->second];
            g.kv.push_back(std::make_pair(key, value));
            g.indexes.push_back(pending[k]);
        }
    }
    // 5) 逐组 Produce（同一连接顺序请求/响应）；单组单个 record batch。
    for (size_t g = 0; g < groups.size(); ++g) {
        groups[g].records = encodeRecordBatch(groups[g].kv);

        // 瞬时 leader 错误（NOT_LEADER_FOR_PARTITION=6 / LEADER_NOT_AVAILABLE=5）：
        // 主题刚创建时 leader 异步分配，produce 可能短暂命中；有界刷新 metadata
        // （内部等待 leader 就绪）后重试同一组，直至 deadline 或其它错误。
        int16_t errorCode = 0;
        bool transportFail = false;
        for (;;) {
            remain = remainingMs(start, dl);
            if (remain < 1) {
                remain = 1;
            }
            if (!produceGroup(&tcp, groups[g].topic, groups[g].partition, groups[g].records,
                              static_cast<int32_t>(3000), remain, correlation++, &errorCode)) {
                transportFail = true;
                break;
            }
            if (errorCode == 6 || errorCode == 5) {
                if (remainingMs(start, dl) <= 0) {
                    break;  // deadline 到：保留 errorCode（分类见下）
                }
                const int parts = metadataPartitions(&tcp, groups[g].topic, &correlation, start, dl);
                if (parts < 0) {
                    transportFail = true;  // metadata 传输失败：连接已不可信，须重建
                    break;
                }
                if (parts == 0) {
                    break;  // topic 不可用/无分区：保留 errorCode
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            break;
        }

        if (transportFail) {
            const PublishFailure f = transportFailure(start, dl);
            const std::string msg = "produce request failed for " + groups[g].topic
                + " partition " + std::to_string(groups[g].partition);
            for (size_t j = 0; j < groups[g].indexes.size(); ++j) {
                outcomes[groups[g].indexes[j]] = failedOutcome(f, msg);
            }
            // 传输失败/超时后连接处于不可信状态（可能残留半帧响应）——关闭并重建，
            // 后续组不复用旧连接（不跨组复用半帧）。deadline 已尽或重建失败 → 剩余
            // 组整体按传输故障失败（不无限重连；remainingMs 有界）。
            tcp.close();
            if (g + 1 < groups.size()) {
                if (remainingMs(start, dl) < 1) {
                    const PublishFailure ft = PublishFailure::Timeout;
                    const std::string mt = "deadline exceeded after transport failure";
                    for (size_t gg = g + 1; gg < groups.size(); ++gg) {
                        for (size_t j = 0; j < groups[gg].indexes.size(); ++j) {
                            outcomes[groups[gg].indexes[j]] = failedOutcome(ft, mt);
                        }
                    }
                    return outcomes;
                }
                remain = remainingMs(start, dl);
                if (remain < 1) {
                    remain = 1;
                }
                if (!tcp.connectTo(host_, port_, remain)) {
                    const PublishFailure fr = transportFailure(start, dl);
                    const std::string mr = "broker reconnect failed after transport failure";
                    for (size_t gg = g + 1; gg < groups.size(); ++gg) {
                        for (size_t j = 0; j < groups[gg].indexes.size(); ++j) {
                            outcomes[groups[gg].indexes[j]] = failedOutcome(fr, mr);
                        }
                    }
                    return outcomes;
                }
            }
            continue;
        }
        PublishFailure f = PublishFailure::None;
        std::string msg;
        if (errorCode != 0) {
            if (errorCode == 3) {
                f = PublishFailure::PayloadTooLarge;
                msg = "message too large";
            } else if (errorCode == 7) {
                f = PublishFailure::Timeout;
                msg = "broker request timed out";
            } else {
                f = PublishFailure::Other;
                msg = "produce error code " + std::to_string(errorCode);
            }
        }
        for (size_t j = 0; j < groups[g].indexes.size(); ++j) {
            if (f == PublishFailure::None) {
                outcomes[groups[g].indexes[j]].ok = true;
                outcomes[groups[g].indexes[j]].failure = PublishFailure::None;
            } else {
                outcomes[groups[g].indexes[j]] = failedOutcome(f, msg);
            }
        }
    }
    return outcomes;
}
