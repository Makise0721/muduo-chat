#include "app/ProtocolCodec.hpp"

#include "app/Clock.hpp"
#include "app/Config.hpp"  // RetryConfig/OutboxConfig/GatewayConfig（P3-08/P3-09/P4-05 生产参数注入）
#include "app/DeliverySink.hpp"  // DeliverySink 完整定义（ReliableMessaging.hpp 仅前向声明）
#include "app/GatewayAwareDeliverySink.hpp"
#include "app/InProcessGatewayTransport.hpp"
#include "app/KafkaEventConsumer.hpp"
#include "app/KafkaPublisher.hpp"
#include "app/LocalOutboxRelay.hpp"
#include "app/MySQLMessageStore.hpp"
#include "app/ReliableMessageMetrics.hpp"  // P3-12 快照（reliable_* METRICS 行）
#include "app/ReliableMessaging.hpp"
#include "app/RedisPresenceDirectory.hpp"
#include "app/WakeupProgressHandler.hpp"
#include "db/MySQLGuards.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sstream>
#include <thread>

namespace {

// legacy identity：legacy:<UserId>:<进程内单调计数>（spec §5.1）。跨重启/重试
// 不稳定是明确能力差异，legacy 不享幂等保证。
std::string makeLegacyIdentity(int64_t userId, uint64_t counter)
{
    return "legacy:" + std::to_string(userId) + ":" + std::to_string(counter);
}

// 群成员数 fan-out cap（P3-04 冻结 100，adapter 构造参数）。
const uint64_t kFanOutCap = 100;
// lease 时长占位：accept 路径不使用时钟/租约（ack/claim 属 P3-07/P3-08，
// 正式参数在 P3-08 RED 前冻结）。
const uint64_t kLeaseMsPlaceholder = 30000;
// P4-05 生产 relay publish / consumer poll 软期限（P4-03/P4-04 冻结惯例 5000ms）。
const int64_t kPublishDeadlineMs = 5000;

// P3-07 接线：ChatService::bindLoop 注册 SessionDeliverySink. The stable
// delegating adapter binds the sink once, including after lazy wiring.
DeliverySink* g_deliverySink = nullptr;
struct MessagingWiring;
MessagingWiring* g_wiring = nullptr;

// P3-08 生产可靠消息参数：main 经 configureReliableMessaging 注入（首用前）；
// 缺省 = 卡冻结值（docs/tasks/P3-08.md）。测试不经本入口。
RetryConfig g_reliableConfig;

// P3-09 生产 outbox relay 参数：main 经 configureOutboxRelay 注入（首用前）；
// 缺省 = 卡冻结值（docs/tasks/P3-09.md §冻结参数）。测试不经本入口。
OutboxConfig g_outboxConfig;

// P4-05 生产 Gateway 参数：main 经 configureGateway 注入（wiring 首用前）；
// 缺省 = 卡冻结值（docs/tasks/P4-05.md §冻结参数：GatewayId=1、TTL 30s→renew 15s、
// topic muduo-outbox、group muduo-outbox-consumer、fetchBatchLimit=100）。测试不经本入口。
GatewayConfig g_gatewayConfig;

// P4-05 生产 consumer poll 载体（docs/tasks/P4-05.md 开卡待定 2）：wiring 内独立
// poll 线程 + 有界 poll deadline（对齐 P4-04 冻结参数：fetch maxWait 300ms、
// commit/publish deadline 5000ms → 每 poll 有界返回 → stop 有界 join）。幂等
// start/stop；stop 不在持锁时 join（避免与 poll 线程 re-lock 互等死锁）。
class OutboxConsumerPoller {
public:
    OutboxConsumerPoller(OutboxEventConsumer& consumer, int64_t pollDeadlineMs)
        : consumer_(consumer), pollDeadlineMs_(pollDeadlineMs)
    {
    }
    ~OutboxConsumerPoller() { stop(5000); }

    void start()
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (running_) {
            return;
        }
        running_ = true;
        stopRequested_ = false;
        thread_ = std::thread([this] { loop(); });
    }

    void stop(int64_t deadlineMs)
    {
        (void)deadlineMs;  // 有界：每 poll 有界（fetch maxWait 300ms / deadline 5000ms）→
                           // join 即有界；deadline 为软提示，无需无界等待。
        std::unique_lock<std::mutex> lk(mutex_);
        if (!running_ && !thread_.joinable()) {
            return;  // 幂等：未 start 或已 stop 直接返回
        }
        stopRequested_ = true;
        cv_.notify_all();
        lk.unlock();
        if (thread_.joinable()) {
            thread_.join();
        }
        lk.lock();
        running_ = false;
    }

private:
    void loop()
    {
        std::unique_lock<std::mutex> lk(mutex_);
        while (!stopRequested_) {
            lk.unlock();
            (void)consumer_.poll(pollDeadlineMs_);  // 不抛（P4-04 契约）；broker 故障
                                                    // brokerOk=false 无副作用，下轮重试
            lk.lock();
        }
    }

    OutboxEventConsumer& consumer_;
    int64_t pollDeadlineMs_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::thread thread_;
    bool running_ = false;
    bool stopRequested_ = false;
};

// P3-06 接线单例：store/clock/messaging 全部领域侧对象，保持本 TU（mymuduo
// 无关）承接 P3-06 结构；P3-07 的 SessionDeliverySink（依赖 TcpConnection）
// 经 registerDeliverySink 在 ChatService::bindLoop 注册（wiring() 惰性构造时
// 绑定；bindLoop 先于首个 accept/claim，无竞态）。P3-08：startReliableMessaging
// 在 bindLoop 显式构造并启动 scheduler（pool 已 init、sink 已绑定）；首个 accept
// 也可惰性触发（等价，幂等）。ReliableMessaging 由 executor 单 worker 串行驱动
// （单一调用者语义）。
// P4-05（docs/tasks/P4-05.md 设计决定 D1/D2/D3/D5）：在 RM 与真实 TCP sink 之间
// 插入 GatewayAwareDeliverySink（deliver 前 Presence locate + 影子 epoch 校验 +
// 本地直投/跨节点 transport/丢弃重路由）；relay 出口从 LocalWakeupPublisher 换为
// KafkaPublisher（CF-1）；生产 consumer poll（KafkaEventConsumer +
// WakeupProgressHandler）接进 wiring——本地/跨节点全部投递经同一 durable 路径，
// ACK 与 MessageId 语义与单机完全一致。
struct MessagingWiring {
    MessagingWiring()
        : store(ConnectionPool::getInstance(), kFanOutCap),
          clock(),
          presence(clock, g_gatewayConfig.presence.host, g_gatewayConfig.presence.port,
                   g_gatewayConfig.presence.db, g_gatewayConfig.presence.ttlMs,
                   g_gatewayConfig.presence.connectTimeoutMs,
                   g_gatewayConfig.presence.commandTimeoutMs),
          transport(),
          localSink(),
          gatewaySink(presence, GatewayId(g_gatewayConfig.id), transport, localSink),
          sink(),
          messaging(store, sink, clock, kLeaseMsPlaceholder, g_reliableConfig),
          kafkaPublisher(g_gatewayConfig.kafka.host, g_gatewayConfig.kafka.port,
                         g_gatewayConfig.consumer.topic, kPublishDeadlineMs),
          wakeupHandler(store, messaging),
           consumer(g_gatewayConfig.kafka.host, g_gatewayConfig.kafka.port,
                    g_gatewayConfig.consumer.topic,
                    g_gatewayConfig.effectiveConsumerGroupId(),
                    store, wakeupHandler, g_gatewayConfig.consumer.fetchBatchLimit,
                    g_gatewayConfig.consumer.pollDeadlineMs),
          relay(store, kafkaPublisher, clock, g_outboxConfig,
                g_gatewayConfig.consumer.topic, kPublishDeadlineMs),
          poller(consumer, g_gatewayConfig.consumer.pollDeadlineMs)
    {
        // RM 面：DelegatingDeliverySink → GatewayAwareDeliverySink（deliver 前
        // locate + epoch 校验）；wrapper 本地投递经 inner localSink → 真实 TCP
        // sink（ChatService::bindLoop 注册）。
        sink.bind(&gatewaySink);
        gatewaySink.setClock(&clock);  // renew 失败指数退避（D4 生产）
        if (g_deliverySink != nullptr) {
            localSink.bind(g_deliverySink);
        }
        g_wiring = this;
    }
    MySQLMessageStore store;
    UnixEpochClock clock;
    RedisPresenceDirectory presence;
    InProcessGatewayTransport transport;
    DelegatingDeliverySink localSink;  // inner 稳定委托：wrapper 本地投递 → 真实 sink
    GatewayAwareDeliverySink gatewaySink;
    DelegatingDeliverySink sink;       // RM 面稳定委托
    ReliableMessaging messaging;
    // P3-09：relay 单 worker 周期扫描 outbox 并对在线接收者重放幂等 wakeup
    // （accept 提交后的 best-effort wakeup 丢失/进程崩溃由周期扫描恢复）。
    // P4-03：relay 出口为 OutboxPublisher port。P4-05 D1：生产出口 = KafkaPublisher
    //（真实 broker）；LocalWakeupPublisher 保留为测试/回退 adapter。
    KafkaPublisher kafkaPublisher;
    WakeupProgressHandler wakeupHandler;
    KafkaEventConsumer consumer;
    LocalOutboxRelay relay;
    OutboxConsumerPoller poller;
};

MessagingWiring& wiring()
{
    static MessagingWiring w;
    return w;
}

// legacy-mode 计数：每条生成 legacy identity（到达 accept）的 legacy 命令 +1。
std::atomic<uint64_t>& legacyCounter()
{
    static std::atomic<uint64_t> c{0};
    return c;
}

// worker 线程：目标用户存在性（spec §2.4 NotFound → 106；池超时/查询失败 →
// 104，B-19 收紧语义：依赖故障不假装成功）。0=存在。
int checkUserExists(int64_t userId)
{
    ConnectionPool::AcquireResult acq = ConnectionPool::getInstance().acquire(5000);
    if (!acq.lease) {
        return kErrnoDependencyBusy;
    }
    MySQL* mysql = acq.lease.get();
    PreparedStatementGuard stmt(mysql->prepareStatement("SELECT id FROM User WHERE id = ?"));
    if (!stmt.stmt) {
        return kErrnoDependencyBusy;
    }
    long long uid = userId;
    MYSQL_BIND param;
    memset(&param, 0, sizeof(param));
    param.buffer_type = MYSQL_TYPE_LONGLONG;
    param.buffer = &uid;
    if (mysql_stmt_bind_param(stmt.stmt, &param) != 0 || mysql_stmt_execute(stmt.stmt) != 0) {
        return kErrnoDependencyBusy;
    }
    long long outUid = 0;
    MYSQL_BIND out;
    memset(&out, 0, sizeof(out));
    out.buffer_type = MYSQL_TYPE_LONGLONG;
    out.buffer = &outUid;
    if (mysql_stmt_bind_result(stmt.stmt, &out) != 0 || mysql_stmt_store_result(stmt.stmt) != 0) {
        return kErrnoDependencyBusy;
    }
    return mysql_stmt_fetch(stmt.stmt) == 0 ? 0 : kErrnoNotFound;
}

// AcceptError → errno（101..107 全映射；未知值防御回退 104）。
int acceptErrorToErrno(AcceptError e)
{
    switch (e) {
        case AcceptError::NotConversationMember:
            return kErrnoNotConversationMember;
        case AcceptError::TooManyRecipients:
            return kErrnoTooManyRecipients;
        case AcceptError::IdempotencyConflict:
            return kErrnoIdempotencyConflict;
        case AcceptError::DependencyBusy:
            return kErrnoDependencyBusy;
        case AcceptError::NotFound:
            return kErrnoNotFound;
        case AcceptError::InvalidClientMessageId:
            return kErrnoInvalidClientMessageId;
    }
    return kErrnoDependencyBusy;
}

// P3-04 契约缺口闭合：StoreErrorKind → errno（决策表第 5 行）。Storage → 104：
// spec §2.4 冻结（存储层错误同 key 重试无副作用——未接受则重试，已接受返回
// 原结果），不做成不可重试的假稳定码。
int storeErrorKindToErrno(StoreErrorKind k)
{
    switch (k) {
        case StoreErrorKind::DependencyBusy:
            return kErrnoDependencyBusy;
        case StoreErrorKind::IdempotencyConflict:
            return kErrnoIdempotencyConflict;
        case StoreErrorKind::TooManyRecipients:
            return kErrnoTooManyRecipients;
        case StoreErrorKind::NotFound:
            return kErrnoNotFound;
        case StoreErrorKind::Storage:
        default:
            return kErrnoDependencyBusy;
    }
}

} // namespace

int parseChatMessage(ChatCommandKind kind, const nlohmann::json& js,
                     ParsedChatMessage* out)
{
    out->legacy = !js.contains("client_message_id");
    if (!out->legacy) {
        out->clientMessageId = js["client_message_id"].get<std::string>();
        out->hasClientMessageId = true;
        if (!ClientMessageId::isValid(out->clientMessageId)) {
            return kErrnoInvalidClientMessageId;
        }
    }
    // content 为冻结字段名（spec §2.1）；缺失时回退旧字段别名 msg（spec §5.1
    // 兼容通道，旧客户端照常成功）。两者均缺失/类型错误 → .at() 抛 type_error
    // （而非 const operator[] 的 assert/UB）→ handler 层 B-25 静默，连接保持。
    const char* contentKey = js.contains("content") ? "content" : "msg";
    out->content = js.at(contentKey).get<std::string>();
    if (out->content.size() > kMaxContentBytes) {
        return kErrnoContentTooLong;
    }
    if (kind == ChatCommandKind::Direct) {
        out->directRecipient = js.at("toid").get<int64_t>();
    } else {
        out->groupId = js.at("groupid").get<int64_t>();
    }
    return 0;
}

void acceptChatCommand(ChatApplication* app, int64_t userId, int64_t generation,
                       const ParsedChatMessage& parsed, ChatCommandKind kind,
                       AcceptResultView* view)
{
    const SendMessageCommand::Kind domainKind = (kind == ChatCommandKind::Direct)
                                                    ? SendMessageCommand::Kind::Direct
                                                    : SendMessageCommand::Kind::Group;
    SendMessageCommand cmd;
    cmd.kind = domainKind;
    cmd.content = parsed.content;

    if (kind == ChatCommandKind::Direct) {
        // 目标存在性（spec §2.4 NotFound → 106）；池超时/查询失败 → 104。
        int err = checkUserExists(parsed.directRecipient);
        if (err != 0) {
            view->errnoCode = err;
            return;
        }
        cmd.directRecipient = UserId(static_cast<uint64_t>(parsed.directRecipient));
    } else {
        // 群成员快照（决策表第 8 行）：保序、含发送者（第 9 行不过滤自投递）。
        // 空列表 = 群不存在 → 106；查询失败 → 104（B-19 收紧，不再 errno=0）。
        MembersResult members = app->groupMembers(parsed.groupId);
        if (!members.ok) {
            view->errnoCode = kErrnoDependencyBusy;
            return;
        }
        if (members.userIds.empty()) {
            view->errnoCode = kErrnoNotFound;
            return;
        }
        bool senderIsMember = false;
        for (size_t i = 0; i < members.userIds.size(); ++i) {
            if (members.userIds[i] == userId) {
                senderIsMember = true;
            }
            cmd.members.push_back(UserId(static_cast<uint64_t>(members.userIds[i])));
        }
        if (!senderIsMember) {
            view->errnoCode = kErrnoNotConversationMember;  // B-18 收紧
            return;
        }
        cmd.groupId = GroupId(static_cast<uint64_t>(parsed.groupId));
    }

    // 幂等键：v2 用原值；legacy 用生成 identity（spec §5.1，跨重启不稳定是
    // 明确能力差异）。
    if (parsed.legacy) {
        cmd.clientMessageId = ClientMessageId(
            makeLegacyIdentity(userId, legacyCounter().fetch_add(1)));
    } else {
        cmd.clientMessageId = ClientMessageId(parsed.clientMessageId);
    }
    view->clientMessageId = cmd.clientMessageId.value();

    try {
        AcceptOutcome outcome = wiring().messaging.accept(
            SessionIdentity(UserId(static_cast<uint64_t>(userId)),
                            static_cast<uint64_t>(generation)),
            cmd);
        view->ok = outcome.ok;
        view->duplicate = outcome.duplicate;
        view->messageId = outcome.messageId.value;
        view->conversationId = outcome.conversationId.value;
        view->sequence = outcome.sequence.value;
        if (!outcome.ok) {
            view->errnoCode = acceptErrorToErrno(outcome.error);
        }
    } catch (const MessageStoreError& e) {
        // P3-04 契约缺口应用层闭合：adapter 异常 → 稳定 errno（决策表第 5 行）。
        view->errnoCode = storeErrorKindToErrno(e.kind());
    } catch (...) {
        // 未知异常兜底：不崩溃、不假装成功；104 = 同 key 重试无副作用。
        view->errnoCode = kErrnoDependencyBusy;
    }
}

const char* protocolErrmsg(int errnoCode)
{
    switch (errnoCode) {
        case kErrnoNotConversationMember:
            return "not a conversation member";
        case kErrnoTooManyRecipients:
            return "too many recipients";
        case kErrnoIdempotencyConflict:
            return "idempotency conflict";
        case kErrnoDependencyBusy:
            return "dependency busy";
        case kErrnoContentTooLong:
            return "content too long";
        case kErrnoNotFound:
            return "target not found";
        case kErrnoInvalidClientMessageId:
            return "invalid client_message_id";
        default:
            return "protocol error";
    }
}

nlohmann::json buildMessageAcceptedReply(int msgid, const std::string& clientMessageId,
                                         uint64_t messageId, uint64_t conversationId,
                                         uint64_t sequence, bool duplicate)
{
    nlohmann::json reply;
    reply["msgid"] = msgid;
    reply["client_message_id"] = clientMessageId;
    reply["message_id"] = messageId;
    reply["conversation_id"] = conversationId;
    reply["sequence"] = sequence;
    reply["duplicate"] = duplicate;
    return reply;
}

nlohmann::json buildErrorReply(int msgid, int errnoCode, const std::string& errmsg,
                               const std::string* clientMessageId)
{
    nlohmann::json reply;
    reply["msgid"] = msgid;
    reply["errno"] = errnoCode;
    reply["errmsg"] = errmsg;
    if (clientMessageId != nullptr) {
        reply["client_message_id"] = *clientMessageId;
    }
    return reply;
}

// ---- P5-03B 零拷贝 encode seam ----
// 直接字符串构建 wire 字节，与 `buildXReply(...).dump() + "\n"` 逐字节一致
// （ProtocolEncodeContractTest 7 用例 + ReliableProtocolGolden golden 锚定）。
// 序列化规则与 nlohmann dump(indent=-1, ensure_ascii=false) 对齐：
// - 键序 = std::map 升序（MESSAGE_ACCEPTED: client_message_id, conversation_id,
//   duplicate, message_id, msgid, sequence；错误: client_message_id, errmsg,
//   errno, msgid）；
// - 字符串转义：\b \t \n \f \r \" \\ 与其它 <0x20 控制字符 \u00xx，其余字节原样
//   （ensure_ascii=false，非 ASCII UTF-8 字节不转义）；
// - 整数/布尔：纯十进制 / true|false（无前导零）。

namespace {

// 追加 JSON 字符串字面量（含首尾双引号），字节等价 nlohmann dump_escaped。
void appendJsonString(std::string& out, const std::string& s)
{
    out.push_back('"');
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        switch (c) {
            case 0x08:
                out += "\\b";
                break;
            case 0x09:
                out += "\\t";
                break;
            case 0x0A:
                out += "\\n";
                break;
            case 0x0C:
                out += "\\f";
                break;
            case 0x0D:
                out += "\\r";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            default:
                if (c <= 0x1F) {
                    char buf[7];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                    out.append(buf, 6);
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    out.push_back('"');
}

} // namespace

std::string encodeMessageAcceptedReply(int msgid, const std::string& clientMessageId,
                                       uint64_t messageId, uint64_t conversationId,
                                       uint64_t sequence, bool duplicate)
{
    std::string out;
    out.reserve(96 + clientMessageId.size());
    out += "{\"client_message_id\":";
    appendJsonString(out, clientMessageId);
    out += ",\"conversation_id\":";
    out += std::to_string(conversationId);
    out += ",\"duplicate\":";
    out += duplicate ? "true" : "false";
    out += ",\"message_id\":";
    out += std::to_string(messageId);
    out += ",\"msgid\":";
    out += std::to_string(msgid);
    out += ",\"sequence\":";
    out += std::to_string(sequence);
    out += "}\n";
    return out;
}

std::string encodeErrorReply(int msgid, int errnoCode, const std::string& errmsg,
                             const std::string* clientMessageId)
{
    std::string out;
    out.reserve(64 + errmsg.size() + (clientMessageId != nullptr ? clientMessageId->size() : 0));
    out += '{';
    if (clientMessageId != nullptr) {
        out += "\"client_message_id\":";
        appendJsonString(out, *clientMessageId);
        out += ',';
    }
    out += "\"errmsg\":";
    appendJsonString(out, errmsg);
    out += ",\"errno\":";
    out += std::to_string(errnoCode);
    out += ",\"msgid\":";
    out += std::to_string(msgid);
    out += "}\n";
    return out;
}

uint64_t legacyModeCount()
{
    return legacyCounter().load();
}

std::string reliableMetricsLine()
{
    // P3-12：可靠消息指标快照 → METRICS 行 reliable_* 字段（字段名冻结于
    // ReliableMessageFaultProcess 的 RELIABLE_FIELDS）。整行由 main.cpp SIGUSR1
    // 处理器单次输出，此处只返回 key=value 片段。
    const ReliableMessageMetrics::Snapshot s = ReliableMessageMetrics::instanceSnapshot();
    std::ostringstream os;
    os << "reliable_accepts=" << s.accepts
       << " reliable_duplicates=" << s.duplicates
       << " reliable_conflicts=" << s.conflicts
       << " reliable_rejected_too_many_recipients=" << s.rejectedTooManyRecipients
       << " reliable_pending=" << s.pending
       << " reliable_inflight=" << s.inflight
       << " reliable_acked=" << s.acked
       << " reliable_expired=" << s.expired
       << " reliable_attempts=" << s.attempts
       << " reliable_retries=" << s.retries
       << " reliable_legacy_mode=" << s.legacyModeCount
       << " reliable_outbox_lag=" << s.outboxLag
       << " reliable_outbox_poison=" << s.outboxPoison
       << " reliable_ack_latency_p50_ms=" << s.ackLatencyP50Ms
       << " reliable_ack_latency_p95_ms=" << s.ackLatencyP95Ms
       << " reliable_ack_latency_p99_ms=" << s.ackLatencyP99Ms
       << " reliable_oldest_pending_age_ms=" << s.oldestPendingAgeMs
       << " consumer_seen_conversations=" << s.consumerSeenConversations;
    return os.str();
}

// ---- P3-07 投递与 ACK（executor worker 线程；wiring().messaging 单一调用者）----

bool registerDeliverySink(DeliverySink* sink)
{
    if (sink == nullptr) {
        return false;
    }
    if (g_deliverySink != nullptr && g_deliverySink != sink) {
        return false;
    }
    if (g_wiring != nullptr) {
        // P4-05 D3：wiring 已构造时把真实 sink 绑进 inner localSink（wrapper 本地
        // 投递出口），RM 面 delegating sink 已在 wiring 构造时绑到 gatewaySink。
        if (!g_wiring->sink.bind(&g_wiring->gatewaySink)) {
            return false;
        }
        if (!g_wiring->localSink.bind(sink)) {
            return false;
        }
    }
    g_deliverySink = sink;
    return true;
}

void acknowledgeDelivery(int64_t userId, uint64_t generation, uint64_t messageId)
{
    wiring().messaging.acknowledge(
        SessionIdentity(UserId(static_cast<uint64_t>(userId)), generation), MessageId{messageId});
}

void sessionAvailableDelivery(int64_t userId, uint64_t generation)
{
    wiring().messaging.sessionAvailable(
        SessionIdentity(UserId(static_cast<uint64_t>(userId)), generation));
}

void sessionClosedDelivery(int64_t userId, uint64_t generation)
{
    wiring().messaging.sessionClosed(
        SessionIdentity(UserId(static_cast<uint64_t>(userId)), generation));
}

void resumeDelivery(int64_t userId, uint64_t generation)
{
    wiring().messaging.resume(SessionIdentity(UserId(static_cast<uint64_t>(userId)), generation));
}

// ---- P3-08 生产接线：可靠消息生命周期与参数注入 ----

void configureReliableMessaging(const RetryConfig& cfg)
{
    g_reliableConfig = cfg;
}

void configureOutboxRelay(const OutboxConfig& cfg)
{
    g_outboxConfig = cfg;
}

void configureGateway(const GatewayConfig& cfg)
{
    g_gatewayConfig = cfg;
}

void startOutboxRelay()
{
    wiring().relay.start();
}

void stopOutboxRelay(int64_t deadlineMs)
{
    // wiring 从未构造（无任何消息活动）时 no-op，避免 shutdown 期反构造 store。
    if (g_wiring != nullptr) {
        g_wiring->relay.stop(deadlineMs);
    }
}

void startReliableMessaging()
{
    wiring().messaging.start();
}

void stopReliableMessaging(int64_t deadlineMs)
{
    // wiring 从未构造（无任何消息活动）时 no-op，避免 shutdown 期反构造 store。
    if (g_wiring != nullptr) {
        g_wiring->messaging.stop(deadlineMs);
    }
}

// ---- P4-05 Presence 生产接线（ChatService 登录/登出/断开调用；wiring 惰性）----

bool claimPresence(int64_t userId, uint64_t connId)
{
    const ClaimResult c = wiring().gatewaySink.bindUser(
        UserId(static_cast<uint64_t>(userId)), ConnectionId(connId));
    return c.ok;
}

void releasePresence(int64_t userId, uint64_t connId)
{
    if (g_wiring == nullptr) {
        return;
    }
    (void)g_wiring->gatewaySink.releaseUser(UserId(static_cast<uint64_t>(userId)),
                                            ConnectionId(connId));
}

void renewAllPresence()
{
    if (g_wiring == nullptr) {
        return;
    }
    g_wiring->gatewaySink.renewAllClaimed();
}

// ---- P4-05 生产 consumer poll（P4-04 D1 延期项落地点）----

void startOutboxConsumerPoll()
{
    wiring().poller.start();
}

void stopOutboxConsumerPoll(int64_t deadlineMs)
{
    if (g_wiring != nullptr) {
        g_wiring->poller.stop(deadlineMs);
    }
}

// ---- P5-00 H-1 统一快照缺口字段 wiring 只读 getter（main.cpp 同源取数）----
// 未构造 wiring（无消息/presence 活动）时 no-op 返回 0，不反构造连接。

uint64_t presenceFencingConflicts()
{
    if (g_wiring == nullptr) {
        return 0;
    }
    return g_wiring->presence.fencingConflicts();
}

uint64_t consumerLag()
{
    if (g_wiring == nullptr) {
        return 0;
    }
    return g_wiring->consumer.consumerLag();
}

uint64_t rebalanceCount()
{
    if (g_wiring == nullptr) {
        return 0;
    }
    return g_wiring->consumer.rebalanceCount();
}
