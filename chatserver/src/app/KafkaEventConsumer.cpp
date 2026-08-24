#include "app/KafkaEventConsumer.hpp"

#include "app/KafkaWire.hpp"
#include "app/ReliableMessageMetrics.hpp"
#include "json.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <map>
#include <set>
#include <thread>
#include <utility>
#include <vector>

// P4-04 KafkaEventConsumer（docs/tasks/P4-04.md §Interface/§冻结参数/设计决定
// D1/D2/M-2）：
//   - manual assign topic 全部分区（Metadata v1）+ offset 存 Kafka：启动恢复 =
//     OffsetFetch v2 读回 group 已提交 offset；无提交历史（-1）→ ListOffsets(-2)
//     earliest；越界（高于 log-end）/OFFSET_OUT_OF_RANGE/UNKNOWN_TOPIC_OR_PARTITION
//     → earliest 回退（M-2，防 fixture 跨 test 的 offset 残留 flake）。
//   - 每 poll：Fetch v4（批 ≤ fetchBatchLimit、maxBytes 1MiB/分区、长轮询
//     maxWait 300ms、minBytes=1）→ 逐 record 处置管线（9 条，见下）→ 全部终态后
//     preCommitHook → OffsetCommit v2（simple-consumer 形态：generation=-1、
//     member_id 空串，deadline 5000ms 对齐冻结参数；2026-08-16 实测校准 NONE，
//     卡待定①）→ offsetCommitted=true、committedThrough[partition]=最后 offset+1。
//   - 逐 record 处置顺序（全部经 MessageStore dead-letter port 落库，绝不只日志）：
//       1) 信封非 JSON/缺字段/message_id/conversation_id/sequence 类型错，或
//          payload 字段非合法 JSON 对象 → DeadLettered reason=poison_payload。
//       2) eventType != "MessageAccepted" → DeadLettered reason=unknown_event_type。
//       3) 同 (conversationId, messageId) 已见（先前已处置成功 Advanced/
//          DuplicateNoOp）→ DuplicateNoOp（H 修复：置于 seq<lastSeen 之前——
//          未提交批重放时，先前已 Advanced 的 record 不能被 lastSeen 已推进的
//          seq 误判 sequence_regression）。
//       4) sequence < lastSeen[conversationId] → DeadLettered
//          reason=sequence_regression（不推进、lastSeen 不变）。
//       5) sequence == lastSeen 且 messageId 不同 → DeadLettered
//          reason=sequence_conflict（防御断言，DB UNIQUE 下理论不可达）。
//       6) 其余 → handler.handle(record)，disposition 取返回值；
//          处置成功（非 DeadLettered）则记入 seenMessages；
//          lastSeen[conversationId] = max(lastSeen, record.sequence)。
//       9) handler 抛出（store 瞬时异常面）：poll 不抛；批中止（本批不提交、
//          broker 停留上一提交点、下轮重放同批；已处置前缀记录仍返回）；lastSeen
//          处置成功即更新、批中止保留已更新值（L-3）。
//   - lastSeen 为 per-conversation 内存态（重启重建，不持久化，D2/L-2）。
//   - cursor（每分区下一 fetch offset）只在 commit 成功后推进 → commit 传输失败
//     /preCommitHook kill = 该批未提交（下轮重放，at-least-once）。
//   - 空批：processed=true（无未终态事件）、offsetCommitted=false、brokerOk=true。
//   - broker 不可达（连接拒绝/断连/blackhole）：brokerOk=false、records 空、无任何
//     副作用、poll 不抛、deadline 有界。fetch 成功而 commit 传输失败：
//     brokerOk=true、processed=true、offsetCommitted=false。

namespace {

const char* kClientId = "muduo-consumer";
const int32_t kFetchMaxWaitMs = 300;            // 长轮询 maxWait（冻结参数）
const int32_t kFetchMinBytes = 1;
const int32_t kPartitionMaxBytes = 1 << 20;     // 1 MiB/分区（同 KafkaTestConsumer）
const int64_t kIdleRetrySleepMs = 50;           // 无进展时退避（冻结参数）
const int16_t kErrOffsetOutOfRange = 1;
const int16_t kErrUnknownTopicOrPartition = 3;

// P4-06 L-5 容量保护（卡登记）：consumer 同时追踪的不同 conversation 数有界。
// 超过上限丢弃最旧的 seen——只影响重放去重窗口（旧 conversation 的重复投递将
// 不再被内存去重，而由 DB 幂等兜底 at-least-once），lastSeen/seenMessages 均为
// 重启重建的内存态，无持久语义。
const size_t kSeenConversationsCapacity = 100;

int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// deadline 为绝对时刻（start + deadlineMs）：剩余 = deadline - now。此前误写成
// `deadlineMs - (nowMs() - startMs)`（叠加 start，返回 ~2*start 的巨大值），
// static_cast<int> 截断后 poll timeout 随机变成巨大/负/-1——无响应等待面
//（blackhole/断连）无限阻塞（BrokerDown 用例卡死根因，2026-08-16 诊断）。
int64_t remainingMs(int64_t deadlineMs)
{
    return deadlineMs - nowMs();
}

int64_t clampTimeout(int64_t t)
{
    return t < 1 ? 1 : t;
}

struct FetchedRecord {
    int32_t partition = 0;
    int64_t offset = 0;
    std::string value;  // 信封原始 bytes（dead-letter rawValue 证据面）
};

// magic-2 record batch 解析（KafkaTestConsumer 已实证形态，P4-03 先例）。
// record.offset = baseOffset + offsetDelta；空 record set（0 长度或单字节 0）合法。
bool parseRecordSet(const std::string& bytes, int32_t partition, std::vector<FetchedRecord>* out)
{
    const size_t n = bytes.size();
    if (n == 0 || (n == 1 && bytes[0] == 0)) {
        return true;
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
            if (!kafka_wire::readVarint(bytes, &pos, &recLen) || recLen < 0) {
                return false;
            }
            const size_t recEnd = pos + static_cast<size_t>(recLen);
            if (recEnd > batchEnd) {
                return false;
            }
            int64_t attributes = 0;
            int64_t tsDelta = 0;
            int64_t offsetDelta = 0;
            if (!kafka_wire::readVarint(bytes, &pos, &attributes)
                || !kafka_wire::readVarint(bytes, &pos, &tsDelta)
                || !kafka_wire::readVarint(bytes, &pos, &offsetDelta)) {
                return false;
            }
            int64_t keyLen = 0;
            if (!kafka_wire::readVarint(bytes, &pos, &keyLen)) {
                return false;
            }
            if (keyLen >= 0) {
                pos += static_cast<size_t>(keyLen);
            }
            int64_t valueLen = 0;
            if (!kafka_wire::readVarint(bytes, &pos, &valueLen)) {
                return false;
            }
            FetchedRecord rec;
            rec.partition = partition;
            if (valueLen >= 0) {
                rec.value.assign(bytes, pos, static_cast<size_t>(valueLen));
                pos += static_cast<size_t>(valueLen);
            }
            int64_t headerCount = 0;
            if (!kafka_wire::readVarint(bytes, &pos, &headerCount)) {
                return false;
            }
            for (int64_t h = 0; h < headerCount; ++h) {
                int64_t hkLen = 0;
                if (!kafka_wire::readVarint(bytes, &pos, &hkLen)) {
                    return false;
                }
                if (hkLen >= 0) {
                    pos += static_cast<size_t>(hkLen);
                }
                int64_t hvLen = 0;
                if (!kafka_wire::readVarint(bytes, &pos, &hvLen)) {
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

} // namespace

struct KafkaEventConsumer::Impl {
    std::string host;
    int port = 0;
    std::string topic;
    std::string groupId;
    MessageStore* deadLetterStore = nullptr;
    DeliveryProgressHandler* handler = nullptr;
    uint32_t fetchBatchLimit = 100;
    int64_t deadlineMs = 5000;
    std::function<void()> preCommitHook;

    kafka_wire::TcpClient tcp;
    bool connected = false;
    int32_t correlation = 0;
    bool assigned = false;
    std::vector<int32_t> partitions;                // 升序（manual assign 全部分区）
    std::map<int32_t, int64_t> cursor;              // partition → 下一 fetch offset
    // per-conversation lastSeen（内存态，重启重建，D2/L-2）：conversationId →
    // (sequence, 该 sequence 上的 message_id)——rule 4 的 sequence_conflict 防御
    // 断言需要"同 sequence 异 id"比对。
    std::map<uint64_t, std::pair<uint64_t, uint64_t> > lastSeen;
    // per-conversation 已处置成功的 messageId 集合（H 修复，内存态，重启重建）：
    // 同 (conversationId, messageId) 重放 → DuplicateNoOp，置于 seq<lastSeen
    // 回归规则之前——未提交批重放时，先前已 Advanced 的 record 不被 lastSeen 已
    // 推进的 seq 误判 sequence_regression。
    std::map<uint64_t, std::set<uint64_t> > seenMessages;
    // P4-06 L-5 容量保护：conversation 插入序（evict 时丢弃最旧）；容量上限对齐
    // 冻结 kSeenConversationsCapacity（默认 100，常量注入）。
    std::deque<uint64_t> conversationOrder;
    size_t seenConversationsCapacity = kSeenConversationsCapacity;
    // P5-00 D9：highWatermark 存储（ListOffsets latest 落点）与 lag/rebalance 计数。
    std::map<int32_t, int64_t> highWatermark;
    std::atomic<uint64_t> consumerLag{0};
    std::atomic<uint64_t> rebalanceCount{0};
};

namespace {

// 单请求 RPC：失败即关连接（不可信状态可能残留半帧响应），下轮 poll 重连。
bool requestRpc(KafkaEventConsumer::Impl& s, int16_t apiKey, int16_t apiVersion,
                const std::string& body, int64_t timeoutMs, std::string* resp)
{
    const int32_t corr = s.correlation++;
    const int64_t t = clampTimeout(timeoutMs);
    if (!kafka_wire::sendRequest(&s.tcp, apiKey, apiVersion, kClientId, body, corr, t)
        || !kafka_wire::recvResponse(&s.tcp, corr, resp, t)) {
        s.tcp.close();
        s.connected = false;
        return false;
    }
    return true;
}

// Metadata v1：topic 全部分区（有界等待 leader 就绪，deadline 内重试）。
bool metadataPartitions(KafkaEventConsumer::Impl& s, int64_t start, int64_t dl,
                        std::vector<int32_t>* out)
{
    for (;;) {
        std::string resp;
        kafka_wire::ByteWriter w;
        w.i32(1);
        w.str(s.topic);
        if (!requestRpc(s, 3, 1, w.data(), remainingMs(dl), &resp)) {
            return false;
        }
        kafka_wire::ByteReader r(resp);
        int32_t brokerCount = 0;
        if (!r.i32(&brokerCount)) {
            return false;
        }
        for (int32_t i = 0; i < brokerCount; ++i) {
            int32_t nodeId = 0;
            std::string host;
            int32_t bport = 0;
            std::string rack;
            bool nullRack = false;
            if (!r.i32(&nodeId) || !r.str(&host) || !r.i32(&bport)
                || !r.nullableStr(&rack, &nullRack)) {
                return false;
            }
        }
        int32_t controllerId = 0;
        if (!r.i32(&controllerId)) {
            return false;
        }
        int32_t topicCount = 0;
        if (!r.i32(&topicCount)) {
            return false;
        }
        std::vector<int32_t> parts;
        bool found = false;
        bool ready = true;
        for (int32_t i = 0; i < topicCount; ++i) {
            int16_t err = 0;
            std::string name;
            bool internal = false;
            int32_t pcount = 0;
            if (!r.i16(&err) || !r.str(&name) || !r.bool_(&internal) || !r.i32(&pcount)) {
                return false;
            }
            for (int32_t p = 0; p < pcount; ++p) {
                int16_t perr = 0;
                int32_t idx = 0;
                int32_t leader = 0;
                int32_t rcount = 0;
                if (!r.i16(&perr) || !r.i32(&idx) || !r.i32(&leader) || !r.i32(&rcount)) {
                    return false;
                }
                for (int32_t k = 0; k < rcount; ++k) {
                    int32_t rid = 0;
                    if (!r.i32(&rid)) {
                        return false;
                    }
                }
                int32_t icount = 0;
                if (!r.i32(&icount)) {
                    return false;
                }
                for (int32_t k = 0; k < icount; ++k) {
                    int32_t iid = 0;
                    if (!r.i32(&iid)) {
                        return false;
                    }
                }
                if (name == s.topic) {
                    if (perr == 0) {
                        parts.push_back(idx);
                    } else {
                        ready = false;  // KRaft 异步分配：有界重试
                    }
                    if (leader < 0) {
                        ready = false;
                    }
                }
            }
            if (name == s.topic) {
                found = true;
                if (err != 0) {
                    ready = false;
                }
            }
        }
        if (found && ready && !parts.empty()) {
            std::sort(parts.begin(), parts.end());
            *out = parts;
            return true;
        }
        if (!found || !ready) {
            if (remainingMs(dl) <= 0) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(kIdleRetrySleepMs));
            continue;
        }
        return false;
    }
}

// ListOffsets v1：一次请求读全部分区（timestamp=-2 earliest / -1 latest）。
bool listOffsets(KafkaEventConsumer::Impl& s, int64_t timestamp, int64_t timeoutMs,
                 const std::vector<int32_t>& parts, std::map<int32_t, int64_t>* out)
{
    kafka_wire::ByteWriter w;
    w.i32(-1);  // replicaId
    w.i32(1);
    w.str(s.topic);
    w.i32(static_cast<int32_t>(parts.size()));
    for (size_t i = 0; i < parts.size(); ++i) {
        w.i32(parts[i]);
        w.i64(timestamp);
    }
    std::string resp;
    if (!requestRpc(s, 2, 1, w.data(), timeoutMs, &resp)) {
        return false;
    }
    kafka_wire::ByteReader r(resp);
    int32_t tc = 0;
    if (!r.i32(&tc)) {
        return false;
    }
    for (int32_t i = 0; i < tc; ++i) {
        std::string name;
        int32_t pc = 0;
        if (!r.str(&name) || !r.i32(&pc)) {
            return false;
        }
        for (int32_t p = 0; p < pc; ++p) {
            int32_t idx = 0;
            int16_t err = 0;
            int64_t ts = 0;
            int64_t off = 0;
            if (!r.i32(&idx) || !r.i16(&err) || !r.i64(&ts) || !r.i64(&off)) {
                return false;
            }
            if (name == s.topic) {
                if (err != 0) {
                    return false;
                }
                (*out)[idx] = off;
            }
        }
    }
    return true;
}

// OffsetFetch v2：读回 group 在 topic 各分区的已提交 offset（无提交历史 = -1；
// 分区级错误码记入 errByPartition 供 earliest 回退，顶层错误 = 传输面失败）。
bool offsetFetchAll(KafkaEventConsumer::Impl& s, int64_t timeoutMs,
                    std::map<int32_t, int64_t>* out)
{
    kafka_wire::ByteWriter w;
    w.str(s.groupId);
    w.i32(1);
    w.str(s.topic);
    w.i32(static_cast<int32_t>(s.partitions.size()));
    for (size_t i = 0; i < s.partitions.size(); ++i) {
        w.i32(s.partitions[i]);
    }
    std::string resp;
    if (!requestRpc(s, 9, 2, w.data(), timeoutMs, &resp)) {
        return false;
    }
    kafka_wire::ByteReader r(resp);
    int32_t tc = 0;
    if (!r.i32(&tc)) {
        return false;
    }
    for (int32_t i = 0; i < tc; ++i) {
        std::string name;
        int32_t pc = 0;
        if (!r.str(&name) || !r.i32(&pc)) {
            return false;
        }
        for (int32_t p = 0; p < pc; ++p) {
            int32_t idx = 0;
            int64_t off = 0;
            std::string metadata;
            bool nullMeta = false;
            int16_t err = 0;
            if (!r.i32(&idx) || !r.i64(&off) || !r.nullableStr(&metadata, &nullMeta)
                || !r.i16(&err)) {
                return false;
            }
            if (name == s.topic) {
                // 分区级错误（OFFSET_OUT_OF_RANGE/UNKNOWN_TOPIC_OR_PARTITION 等）
                // → -1 触发 earliest 回退（M-2）；不视为传输失败。
                (*out)[idx] = (err == 0) ? off : -1;
            }
        }
    }
    int16_t topLevelError = 0;
    if (r.i16(&topLevelError) && topLevelError != 0) {
        return false;
    }
    return true;
}

// OffsetCommit v2（simple-consumer 形态：generation=-1、member_id 空串、
// retention_time_ms=-1）。返回 per-partition error_code；NONE 的分区写入
// committedOk（cursor 推进 + committedThrough 证据面）。
bool offsetCommitAll(KafkaEventConsumer::Impl& s, const std::map<int32_t, int64_t>& toCommit,
                     int64_t timeoutMs, std::map<int32_t, int64_t>* committedOk)
{
    kafka_wire::ByteWriter w;
    w.str(s.groupId);
    w.i32(-1);  // consumer_group_generation_id = -1（simple consumer）
    w.str("");  // consumer_id = 空串（simple consumer）
    w.i64(-1);  // retention_time_ms（broker 默认保留）
    w.i32(1);
    w.str(s.topic);
    w.i32(static_cast<int32_t>(toCommit.size()));
    for (std::map<int32_t, int64_t>::const_iterator it = toCommit.begin();
         it != toCommit.end(); ++it) {
        w.i32(it->first);
        w.i64(it->second);
        w.nullableStrNull();  // committed metadata
    }
    std::string resp;
    if (!requestRpc(s, 8, 2, w.data(), timeoutMs, &resp)) {
        return false;
    }
    kafka_wire::ByteReader r(resp);
    int32_t tc = 0;
    if (!r.i32(&tc)) {
        return false;
    }
    for (int32_t i = 0; i < tc; ++i) {
        std::string name;
        int32_t pc = 0;
        if (!r.str(&name) || !r.i32(&pc)) {
            return false;
        }
        for (int32_t p = 0; p < pc; ++p) {
            int32_t idx = 0;
            int16_t err = 0;
            if (!r.i32(&idx) || !r.i16(&err)) {
                return false;
            }
            if (name == s.topic && err == 0) {
                std::map<int32_t, int64_t>::const_iterator it = toCommit.find(idx);
                if (it != toCommit.end()) {
                    (*committedOk)[idx] = it->second;
                }
            }
        }
    }
    return true;
}

// Fetch v4：单请求全部分区（maxWait 300ms、minBytes=1、1MiB/分区）。
// outErrPartitions = 返回 OFFSET_OUT_OF_RANGE/UNKNOWN_TOPIC_OR_PARTITION 的分区
// （M-2 earliest 回退）。分区记录按分区升序聚合（处理顺序确定）。
bool fetchAll(KafkaEventConsumer::Impl& s, int64_t start, int64_t dl,
              std::vector<FetchedRecord>* out, std::vector<int32_t>* outErrPartitions)
{
    int64_t remain = remainingMs(dl);
    int32_t maxWait = kFetchMaxWaitMs;
    if (remain < maxWait) {
        maxWait = static_cast<int32_t>(remain < 1 ? 1 : remain);
    }
    kafka_wire::ByteWriter w;
    w.i32(-1);  // replicaId
    w.i32(maxWait);
    w.i32(kFetchMinBytes);
    w.i32(kPartitionMaxBytes * static_cast<int32_t>(s.partitions.size() > 0
                                                         ? s.partitions.size()
                                                         : 1));  // response max_bytes
    w.i8(0);    // isolationLevel = read_uncommitted
    w.i32(1);
    w.str(s.topic);
    w.i32(static_cast<int32_t>(s.partitions.size()));
    for (size_t i = 0; i < s.partitions.size(); ++i) {
        const int32_t p = s.partitions[i];
        std::map<int32_t, int64_t>::const_iterator c = s.cursor.find(p);
        w.i32(p);
        w.i64(c != s.cursor.end() ? c->second : 0);
        w.i32(kPartitionMaxBytes);
    }
    std::string resp;
    // 长轮询：broker 最长 maxWait 后必回；socket 超时给 maxWait + 余量，但绝不
    // 超过整体剩余 deadline（blackhole 有界性）。
    int64_t sockTimeout = maxWait + 2000;
    if (remainingMs(dl) < sockTimeout) {
        sockTimeout = remainingMs(dl);
    }
    if (!requestRpc(s, 1, 4, w.data(), sockTimeout, &resp)) {
        return false;
    }
    kafka_wire::ByteReader r(resp);
    int32_t throttle = 0;
    int32_t tc = 0;
    if (!r.i32(&throttle) || !r.i32(&tc)) {
        s.tcp.close();
        s.connected = false;
        return false;
    }
    std::map<int32_t, std::vector<FetchedRecord> > byPartition;
    for (int32_t i = 0; i < tc; ++i) {
        std::string name;
        int32_t pc = 0;
        if (!r.str(&name) || !r.i32(&pc)) {
            s.tcp.close();
            s.connected = false;
            return false;
        }
        for (int32_t p = 0; p < pc; ++p) {
            int32_t idx = 0;
            int16_t err = 0;
            int64_t hw = 0;
            int64_t lso = 0;
            int32_t abortCnt = 0;
            if (!r.i32(&idx) || !r.i16(&err) || !r.i64(&hw) || !r.i64(&lso)
                || !r.i32(&abortCnt)) {
                s.tcp.close();
                s.connected = false;
                return false;
            }
            (void)lso;
            if (abortCnt > 0) {
                for (int32_t a = 0; a < abortCnt; ++a) {
                    int64_t pid = 0;
                    int64_t foff = 0;
                    if (!r.i64(&pid) || !r.i64(&foff)) {
                        s.tcp.close();
                        s.connected = false;
                        return false;
                    }
                }
            }
            std::string records;
            bool nullRecords = false;
            if (!r.nullableBytes(&records, &nullRecords)) {
                s.tcp.close();
                s.connected = false;
                return false;
            }
            if (name != s.topic) {
                continue;
            }
            if (err == kErrOffsetOutOfRange || err == kErrUnknownTopicOrPartition) {
                outErrPartitions->push_back(idx);  // M-2：earliest 回退
                continue;
            }
            if (err != 0) {
                // 其它分区级错误（leader 瞬时未就绪等）：按本轮 broker 故障处理
                //（无副作用、poll 有界），下轮重试。
                s.tcp.close();
                s.connected = false;
                return false;
            }
            if (!nullRecords) {
                std::vector<FetchedRecord> parsed;
                if (!parseRecordSet(records, idx, &parsed)) {
                    s.tcp.close();
                    s.connected = false;
                    return false;
                }
                byPartition[idx].insert(byPartition[idx].end(), parsed.begin(), parsed.end());
            }
        }
    }
    for (std::map<int32_t, std::vector<FetchedRecord> >::const_iterator it =
             byPartition.begin();
         it != byPartition.end(); ++it) {
        out->insert(out->end(), it->second.begin(), it->second.end());
    }
    return true;
}

// P4-06 L-5 容量保护：登记一个 conversation 为已见（进入 lastSeen 前调用）。
// 若为新 conversation 且当前追踪数已达上限，则丢弃最旧 seen（只影响重放去重
// 窗口；at-least-once 由 DB 幂等兜底）。已在案（lastSeen 含该 conversation）则
// 空操作（保序不动，避免重复 push_back）。map 迭代器不受其它元素 erase 影响，
// 调用方持有的 seen 迭代器仍有效。
void touchConversation(KafkaEventConsumer::Impl& s, uint64_t conversationId)
{
    if (s.lastSeen.count(conversationId) != 0) {
        return;
    }
    if (s.conversationOrder.size() >= s.seenConversationsCapacity) {
        const uint64_t oldest = s.conversationOrder.front();
        s.conversationOrder.pop_front();
        s.lastSeen.erase(oldest);
        s.seenMessages.erase(oldest);
    }
    s.conversationOrder.push_back(conversationId);
}

} // namespace

KafkaEventConsumer::KafkaEventConsumer(const std::string& host, int port,
                                       const std::string& topic, const std::string& groupId,
                                       MessageStore& deadLetterStore,
                                       DeliveryProgressHandler& handler,
                                       uint32_t fetchBatchLimit, int64_t deadlineMs,
                                       size_t seenConversationsCapacity)
    : impl_(new Impl)
{
    impl_->host = host;
    impl_->port = port;
    impl_->topic = topic;
    impl_->groupId = groupId;
    impl_->deadLetterStore = &deadLetterStore;
    impl_->handler = &handler;
    impl_->fetchBatchLimit = fetchBatchLimit > 0 ? fetchBatchLimit : 1;
    impl_->deadlineMs = deadlineMs;
    // P4-06 单测注入：>0 覆盖冻结常量（默认 0 = kSeenConversationsCapacity=100）。
    impl_->seenConversationsCapacity = seenConversationsCapacity > 0
        ? seenConversationsCapacity : kSeenConversationsCapacity;
}

KafkaEventConsumer::~KafkaEventConsumer()
{
}

void KafkaEventConsumer::setPreCommitHook(std::function<void()> hook)
{
    impl_->preCommitHook = hook;
}

uint64_t KafkaEventConsumer::consumerLag() const
{
    return impl_->consumerLag.load();
}

uint64_t KafkaEventConsumer::rebalanceCount() const
{
    return impl_->rebalanceCount.load();
}

OutboxConsumeResult KafkaEventConsumer::poll(int64_t deadlineMs)
{
    OutboxConsumeResult result;  // 默认 brokerOk=true
    Impl& s = *impl_;
    const int64_t start = nowMs();
    const int64_t dl = start + (deadlineMs > 0 ? deadlineMs : s.deadlineMs);
    // P4-06 L-5 观测：consumer 当前 seen 集合大小 gauge（有界）。每 poll 更新
    // 一次（含空批/故障路径——始终反映当前追踪数；recordBestEffort 不抛）。
    ReliableMessageMetrics::recordBestEffort([&] {
        ReliableMessageMetrics::instance().updateConsumerSeenConversations(
            s.lastSeen.size());
    });
    try {
        // 1) 连接（懒建立；断连/传输失败后由 requestRpc 关闭，此处重连）。
        if (!s.connected) {
            if (!s.tcp.connectTo(s.host, s.port, clampTimeout(remainingMs(dl)))) {
                result.brokerOk = false;
                return result;  // 连接拒绝/断连：无副作用
            }
            s.connected = true;
        }

        // 2) 启动恢复（首 poll）：Metadata 分区发现 → OffsetFetch 读回 → 无历史/
        //    越界回退 earliest（M-2）。
        if (!s.assigned) {
            std::vector<int32_t> parts;
            if (!metadataPartitions(s, start, dl, &parts)) {
                result.brokerOk = false;
                return result;
            }
            s.partitions = parts;
            std::map<int32_t, int64_t> committed;
            if (!offsetFetchAll(s, remainingMs(dl), &committed)) {
                result.brokerOk = false;
                return result;
            }
            bool anyCommitted = false;
            for (size_t i = 0; i < parts.size(); ++i) {
                std::map<int32_t, int64_t>::const_iterator it = committed.find(parts[i]);
                if (it != committed.end() && it->second >= 0) {
                    anyCommitted = true;
                }
            }
            std::map<int32_t, int64_t> earliest;
            if (!listOffsets(s, -2, remainingMs(dl), parts, &earliest)) {
                result.brokerOk = false;
                return result;
            }
            std::map<int32_t, int64_t> latest;
            if (anyCommitted
                && !listOffsets(s, -1, remainingMs(dl), parts, &latest)) {
                result.brokerOk = false;
                return result;
            }
            for (size_t i = 0; i < parts.size(); ++i) {
                const int32_t p = parts[i];
                std::map<int32_t, int64_t>::const_iterator it = committed.find(p);
                const int64_t off = (it == committed.end()) ? -1 : it->second;
                std::map<int32_t, int64_t>::const_iterator e = earliest.find(p);
                const int64_t er = (e == earliest.end()) ? 0 : e->second;
                if (off < 0) {
                    s.cursor[p] = er;  // 无提交历史 → earliest
                    s.rebalanceCount.fetch_add(1);  // P5-00 D9：M-2 earliest 回退代理计数
                    continue;
                }
                std::map<int32_t, int64_t>::const_iterator l = latest.find(p);
                if (l != latest.end() && off > l->second) {
                    s.cursor[p] = er;  // 越界（高于 log-end）→ earliest（M-2）
                    s.rebalanceCount.fetch_add(1);  // P5-00 D9
                } else {
                    s.cursor[p] = off;  // 从已提交 offset 恢复（重启接管）
                }
            }
            s.assigned = true;
            // P5-00 D9：highWatermark 存储（latest 落点）与 consumer lag 计算
            //（只读 cursor 差值；无 latest 数据时 lag=0）。
            s.highWatermark = latest;
            uint64_t totalLag = 0;
            for (size_t i = 0; i < parts.size(); ++i) {
                const int32_t p = parts[i];
                std::map<int32_t, int64_t>::const_iterator h = latest.find(p);
                std::map<int32_t, int64_t>::const_iterator c = s.cursor.find(p);
                if (h != latest.end() && c != s.cursor.end() && h->second > c->second) {
                    totalLag += static_cast<uint64_t>(h->second - c->second);
                }
            }
            s.consumerLag.store(totalLag);
        }

        // 3) Fetch v4 单批拉取（批 ≤ fetchBatchLimit）。
        std::vector<FetchedRecord> fetched;
        std::vector<int32_t> errPartitions;
        if (!fetchAll(s, start, dl, &fetched, &errPartitions)) {
            result.brokerOk = false;
            return result;
        }
        if (!errPartitions.empty()) {
            // M-2 fetch 期越界回退：该分区 cursor 重置 earliest（本轮不消费该
            // 分区——fetch 已返回，下一轮从 earliest 重读）。
            for (size_t i = 0; i < errPartitions.size(); ++i) {
                std::map<int32_t, int64_t> er;
                if (!listOffsets(s, -2, remainingMs(dl),
                                 std::vector<int32_t>(1, errPartitions[i]), &er)) {
                    result.brokerOk = false;
                    return result;
                }
                s.cursor[errPartitions[i]] = er.count(errPartitions[i])
                                                 ? er[errPartitions[i]]
                                                 : 0;
                s.rebalanceCount.fetch_add(1);  // P5-00 D9：M-2 earliest 回退代理计数
            }
        }

        // 4) 逐 record 处置管线（9 条；处置顺序冻结）。
        bool aborted = false;
        for (size_t i = 0; i < fetched.size() && result.records.size() < s.fetchBatchLimit;
             ++i) {
            const FetchedRecord& raw = fetched[i];
            ConsumedOutboxRecord rec;
            rec.topic = s.topic;
            rec.partition = raw.partition;
            rec.offset = raw.offset;

            // 1) poison_payload：信封非 JSON/缺字段/数值类型错，或 payload 非合法
            //    JSON 对象。P4-03 冻结信封（KafkaPublisher.cpp）的 payload 是
            //    JSON 字符串字段（P3-04 冻结编码的命令快照 JSON 文本——字符串
            //    内嵌转义 JSON，非嵌套对象），故"合法 JSON 对象"= payload 字符串
            //    内容可解析且为 JSON 对象。
            nlohmann::json env =
                nlohmann::json::parse(raw.value, nullptr, false);
            bool envelopeOk = !env.is_discarded() && env.is_object()
                && env.contains("message_id") && env["message_id"].is_number_unsigned()
                && env.contains("conversation_id")
                && env["conversation_id"].is_number_unsigned()
                && env.contains("sequence") && env["sequence"].is_number_unsigned()
                && env.contains("event_type") && env["event_type"].is_string()
                && env.contains("payload") && env["payload"].is_string();
            if (envelopeOk) {
                const nlohmann::json inner = nlohmann::json::parse(
                    env["payload"].get<std::string>(), nullptr, false);
                envelopeOk = !inner.is_discarded() && inner.is_object();
            }
            if (!envelopeOk) {
                if (env.is_object() && env.contains("message_id")
                    && env["message_id"].is_number_unsigned()) {
                    rec.messageId = env["message_id"].get<uint64_t>();
                }
                DeadLetterRecord dlr;
                dlr.topic = rec.topic;
                dlr.partitionId = rec.partition;
                dlr.kafkaOffset = rec.offset;
                dlr.messageId = rec.messageId;  // 不可解析时 0
                dlr.conversationId = rec.conversationId;
                dlr.sequence = rec.sequence;
                dlr.eventType = rec.eventType;
                dlr.reason = "poison_payload";
                dlr.rawValue = raw.value;
                try {
                    s.deadLetterStore->recordDeadLetter(dlr);  // 幂等（UNIQUE）
                } catch (...) {
                    aborted = true;  // dead-letter 落库失败 = 未终态 → 重放
                    break;
                }
                result.records.push_back(rec);
                result.dispositions.push_back(ConsumeDisposition::DeadLettered);
                continue;
            }
            rec.messageId = env["message_id"].get<uint64_t>();
            rec.conversationId = env["conversation_id"].get<uint64_t>();
            rec.sequence = env["sequence"].get<uint64_t>();
            rec.eventType = env["event_type"].get<std::string>();
            rec.payload = env["payload"].get<std::string>();

            // 2) unknown_event_type。
            if (rec.eventType != "MessageAccepted") {
                DeadLetterRecord dlr;
                dlr.topic = rec.topic;
                dlr.partitionId = rec.partition;
                dlr.kafkaOffset = rec.offset;
                dlr.messageId = rec.messageId;
                dlr.conversationId = rec.conversationId;
                dlr.sequence = rec.sequence;
                dlr.eventType = rec.eventType;
                dlr.reason = "unknown_event_type";
                dlr.rawValue = raw.value;
                try {
                    s.deadLetterStore->recordDeadLetter(dlr);
                } catch (...) {
                    aborted = true;
                    break;
                }
                result.records.push_back(rec);
                result.dispositions.push_back(ConsumeDisposition::DeadLettered);
                continue;
            }

            // 3) 同 (conversationId, messageId) 已见（先前已处置成功）→
            //    DuplicateNoOp（H 修复：置于 seq<lastSeen 回归规则之前——未提交
            //    批重放时，先前已 Advanced 的 record 不能被 lastSeen 已推进的
            //    seq 误判 sequence_regression）。
            std::map<uint64_t, std::set<uint64_t> >::iterator seenMsgs =
                s.seenMessages.find(rec.conversationId);
            if (seenMsgs != s.seenMessages.end()
                && seenMsgs->second.count(rec.messageId) != 0) {
                result.records.push_back(rec);
                result.dispositions.push_back(ConsumeDisposition::DuplicateNoOp);
                continue;
            }

            std::map<uint64_t, std::pair<uint64_t, uint64_t> >::iterator seen =
                s.lastSeen.find(rec.conversationId);
            // 4) sequence_regression：倒退不推进、lastSeen 不变。
            // 5) sequence_conflict：同 sequence 异 id（防御断言）。
            if (seen != s.lastSeen.end() && rec.sequence < seen->second.first) {
                DeadLetterRecord dlr;
                dlr.topic = rec.topic;
                dlr.partitionId = rec.partition;
                dlr.kafkaOffset = rec.offset;
                dlr.messageId = rec.messageId;
                dlr.conversationId = rec.conversationId;
                dlr.sequence = rec.sequence;
                dlr.eventType = rec.eventType;
                dlr.reason = "sequence_regression";
                dlr.rawValue = raw.value;
                try {
                    s.deadLetterStore->recordDeadLetter(dlr);
                } catch (...) {
                    aborted = true;
                    break;
                }
                result.records.push_back(rec);
                result.dispositions.push_back(ConsumeDisposition::DeadLettered);
                continue;
            }
            if (seen != s.lastSeen.end() && rec.sequence == seen->second.first
                && rec.messageId != seen->second.second) {
                DeadLetterRecord dlr;
                dlr.topic = rec.topic;
                dlr.partitionId = rec.partition;
                dlr.kafkaOffset = rec.offset;
                dlr.messageId = rec.messageId;
                dlr.conversationId = rec.conversationId;
                dlr.sequence = rec.sequence;
                dlr.eventType = rec.eventType;
                dlr.reason = "sequence_conflict";
                dlr.rawValue = raw.value;
                try {
                    s.deadLetterStore->recordDeadLetter(dlr);
                } catch (...) {
                    aborted = true;
                    break;
                }
                result.records.push_back(rec);
                result.dispositions.push_back(ConsumeDisposition::DeadLettered);
                continue;
            }

            // 6) handler.handle（异常 = store 瞬时异常面：批中止不提交，下轮重放）。
            ConsumeDisposition d;
            try {
                d = s.handler->handle(rec);
            } catch (...) {
                aborted = true;
                break;  // 抛出记录及其后不进本批（重放轮重新处置）
            }
            result.records.push_back(rec);
            result.dispositions.push_back(d);
            // 处置成功（Advanced/DuplicateNoOp）→ 记入 seenMessages（H 修复：
            // 后续同 (conversationId, messageId) 重放 → 规则 3 DuplicateNoOp）。
            if (d != ConsumeDisposition::DeadLettered) {
                s.seenMessages[rec.conversationId].insert(rec.messageId);
            }
            // P4-06 L-5 容量保护：新 conversation 进入 lastSeen 前登记插入序；
            // 超限则丢弃最旧（只影响重放去重窗口，at-least-once 由 DB 幂等兜底）。
            // 已在案的 conversation 空操作；map 迭代器不受其它元素 erase 影响，
            // 下方 seen 迭代器仍有效。
            touchConversation(s, rec.conversationId);
            if (seen == s.lastSeen.end() || rec.sequence > seen->second.first) {
                s.lastSeen[rec.conversationId] =
                    std::make_pair(rec.sequence, rec.messageId);
            }
        }

        if (aborted) {
            // 批中止：本批零提交（cursor 未推进 → 下轮重放同批）；已处置成功的
            // 前缀记录仍返回（含 dispositions）；processed=false。
            result.processed = false;
            return result;
        }

        result.processed = true;  // 全部终态（含 dead-letter 落库）
        if (result.records.empty()) {
            // 空批：无未终态事件，但无 offset 可提交。
            return result;
        }

        // 5) preCommitHook（OffsetCommit 发出前；抛出 = kill 注入 → 本批不提交）。
        if (s.preCommitHook) {
            try {
                s.preCommitHook();
            } catch (...) {
                return result;  // processed=true、offsetCommitted=false
            }
        }

        // 6) OffsetCommit：每分区 = 本批该分区最后 offset+1；成功分区才推进
        //    cursor（传输失败/错误分区不推进 → 下轮重放，at-least-once）。
        std::map<int32_t, int64_t> toCommit;
        for (size_t i = 0; i < result.records.size(); ++i) {
            const ConsumedOutboxRecord& rec = result.records[i];
            std::map<int32_t, int64_t>::iterator it = toCommit.find(rec.partition);
            if (it == toCommit.end() || rec.offset + 1 > it->second) {
                toCommit[rec.partition] = rec.offset + 1;
            }
        }
        std::map<int32_t, int64_t> committedOk;
        if (!offsetCommitAll(s, toCommit, remainingMs(dl), &committedOk)) {
            // commit 传输失败：fetch 已成功（brokerOk=true）、处理已终态
            //（processed=true）、本批未提交（下轮重放）。
            return result;
        }
        for (std::map<int32_t, int64_t>::const_iterator it = committedOk.begin();
             it != committedOk.end(); ++it) {
            s.cursor[it->first] = it->second;
            result.committedThrough[it->first] = it->second;
        }
        result.offsetCommitted = !result.committedThrough.empty();
        return result;
    } catch (...) {
        // poll 契约：绝不抛。未预期异常面按"批未处理、无提交"返回（broker 本身
        // 未见故障 → brokerOk=true；下轮重放）。
        result.processed = false;
        result.offsetCommitted = false;
        result.committedThrough.clear();
        return result;
    }
}
