#include "db/OfflineBackfill.hpp"

#include "app/MySQLMessageStore.hpp"
#include "db/MySQLGuards.hpp"
#include "json.hpp"

#include <mysql/mysql.h>

#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace offline_backfill {

namespace {

const char* kCheckpointScope = "offline_message";
// P3-04 冻结 fan-out cap=100（群迁移每行成员快照 = 单离线用户，远小于 cap）。
const uint64_t kFanOutCap = 100;
// 旧表上限：整条序列化 payload <= 500 字节（B-13 旧上限）。VARCHAR(500) 按字符
// 计数，超界快照只能以"字节超限、字符不超"存在（旧 bug），故按字节判界。
const size_t kLegacyPayloadLimit = 500;
const size_t kReasonLimit = 255;

const char* kCheckpointCreate =
    "CREATE TABLE IF NOT EXISTS OfflineBackfillCheckpoint("
    "scope VARCHAR(32) NOT NULL PRIMARY KEY,"
    "last_id BIGINT NOT NULL"
    ") ENGINE=InnoDB DEFAULT CHARSET=utf8";
const char* kQuarantineCreate =
    "CREATE TABLE IF NOT EXISTS OfflineBackfillQuarantine("
    "offline_id BIGINT NOT NULL PRIMARY KEY,"
    "user_id INT NOT NULL,"
    "reason VARCHAR(255) NOT NULL,"
    "payload MEDIUMBLOB NOT NULL"
    ") ENGINE=InnoDB DEFAULT CHARSET=utf8";

std::string lastError(MySQL& m)
{
    return mysql_error(m.getConnection());
}

std::string hex64(uint64_t v)
{
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(16);
    for (int i = 0; i < 16; ++i) {
        out.push_back(hex[(v >> (60 - 4 * i)) & 0xf]);
    }
    return out;
}

// 解析旧表 message 整段协议 JSON（README.md 协议形状：ONE_CHAT=6 / GROUP_CHAT=10）。
// content 缺失时按 B-11 字段别名 `msg` 解析。不可解析返回 ok=false + reason。
struct ParsedRow {
    bool ok = false;
    std::string reason;
    int msgid = 0;
    int64_t sender = 0;
    int64_t toid = 0;
    int64_t groupid = 0;
    std::string content;
};

ParsedRow parsePayload(const std::string& payload)
{
    ParsedRow r;
    if (payload.size() > kLegacyPayloadLimit) {
        r.ok = false;
        r.reason = "payload exceeds legacy 500-byte limit";
        return r;
    }
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(payload);
    } catch (...) {
        r.ok = false;
        r.reason = "malformed json";
        return r;
    }
    if (!j.is_object()) {
        r.ok = false;
        r.reason = "not a json object";
        return r;
    }
    if (!j.contains("msgid") || !j["msgid"].is_number_integer()) {
        r.ok = false;
        r.reason = "missing msgid";
        return r;
    }
    r.msgid = j["msgid"].get<int>();
    if (r.msgid != 6 && r.msgid != 10) {
        r.ok = false;
        r.reason = "unknown msgid";
        return r;
    }
    if (!j.contains("id") || !j["id"].is_number_integer()) {
        r.ok = false;
        r.reason = "missing sender id";
        return r;
    }
    r.sender = j["id"].get<int64_t>();
    if (r.sender <= 0) {
        r.ok = false;
        r.reason = "invalid sender id";
        return r;
    }
    if (j.contains("content")) {
        if (!j["content"].is_string()) {
            r.ok = false;
            r.reason = "content not a string";
            return r;
        }
        r.content = j["content"].get<std::string>();
    } else if (j.contains("msg")) {
        if (!j["msg"].is_string()) {
            r.ok = false;
            r.reason = "msg not a string";
            return r;
        }
        r.content = j["msg"].get<std::string>();  // B-11 字段别名
    } else {
        r.ok = false;
        r.reason = "missing content";
        return r;
    }
    if (r.msgid == 6) {
        if (!j.contains("toid") || !j["toid"].is_number_integer()) {
            r.ok = false;
            r.reason = "missing toid";
            return r;
        }
        r.toid = j["toid"].get<int64_t>();
        if (r.toid <= 0) {
            r.ok = false;
            r.reason = "invalid toid";
            return r;
        }
    } else {
        if (!j.contains("groupid") || !j["groupid"].is_number_integer()) {
            r.ok = false;
            r.reason = "missing groupid";
            return r;
        }
        r.groupid = j["groupid"].get<int64_t>();
        if (r.groupid <= 0) {
            r.ok = false;
            r.reason = "invalid groupid";
            return r;
        }
    }
    r.ok = true;
    return r;
}

// 旧表源行（payload 按 CAST(message AS BINARY) 取回，与测试 storedPayload 同源，
// 免连接字符集转换，非 UTF-8/乱码字节原样保留）。
struct OfflineRow {
    int64_t id = 0;
    int64_t userId = 0;
    std::string payload;
};

// BLOB 列二段读取（MySQLMessageStore fetchBlob 同款）。
std::string fetchBlob(MYSQL_STMT* stmt, unsigned col, unsigned long len)
{
    if (len == 0) {
        return std::string();
    }
    std::string out(len, '\0');
    MYSQL_BIND fb;
    memset(&fb, 0, sizeof(fb));
    fb.buffer_type = MYSQL_TYPE_BLOB;
    fb.buffer = &out[0];
    fb.buffer_length = len;
    unsigned long got = 0;
    fb.length = &got;
    if (mysql_stmt_fetch_column(stmt, &fb, col, 0) != 0) {
        throw std::runtime_error("backfill fetch_column failed");
    }
    return out;
}

void bindU64(MYSQL_BIND& b, uint64_t* p)
{
    memset(&b, 0, sizeof(b));
    b.buffer_type = MYSQL_TYPE_LONGLONG;
    b.is_unsigned = 1;
    b.buffer = p;
}

void bindString(MYSQL_BIND& b, const std::string* s, unsigned long* len)
{
    memset(&b, 0, sizeof(b));
    b.buffer_type = MYSQL_TYPE_STRING;
    b.buffer = const_cast<char*>(s->data());
    b.buffer_length = *len = static_cast<unsigned long>(s->size());
    b.length = len;
}

void bindBlob(MYSQL_BIND& b, const std::string* s, unsigned long* len)
{
    memset(&b, 0, sizeof(b));
    b.buffer_type = MYSQL_TYPE_BLOB;
    b.buffer = const_cast<char*>(s->data());
    b.buffer_length = *len = static_cast<unsigned long>(s->size());
    b.length = len;
}

// 执行带参语句（无结果集），返回 affected rows；失败抛 std::runtime_error。
uint64_t execStmt(MySQL& m, const char* sql, MYSQL_BIND* binds, unsigned nBinds)
{
    MYSQL_STMT* stmt = m.prepareStatement(sql);
    if (!stmt) {
        throw std::runtime_error("backfill prepare failed: " + lastError(m) + ": " + sql);
    }
    uint64_t affected = 0;
    {
        PreparedStatementGuard guard(stmt);
        if (binds != nullptr && mysql_stmt_bind_param(stmt, binds) != 0) {
            throw std::runtime_error("backfill bind failed: " + lastError(m));
        }
        if (mysql_stmt_execute(stmt) != 0) {
            throw std::runtime_error("backfill stmt failed: " + lastError(m));
        }
        affected = static_cast<uint64_t>(mysql_stmt_affected_rows(stmt));
    }
    return affected;
}

// 读取 id > afterId 的一批源行（LIMIT 有界，id 升序）。
std::vector<OfflineRow> readBatch(MySQL& m, uint64_t afterId, uint32_t limit)
{
    const std::string sql =
        "SELECT id, userid, CAST(message AS BINARY) FROM OfflineMessage "
        "WHERE id > ? ORDER BY id LIMIT ?";
    MYSQL_STMT* stmt = m.prepareStatement(sql.c_str());
    if (!stmt) {
        throw std::runtime_error("backfill prepare batch failed: " + lastError(m));
    }
    std::vector<OfflineRow> out;
    {
        PreparedStatementGuard guard(stmt);
        uint64_t pAfter = afterId;
        uint64_t pLimit = limit;
        MYSQL_BIND params[2];
        bindU64(params[0], &pAfter);
        bindU64(params[1], &pLimit);
        if (mysql_stmt_bind_param(stmt, params) != 0) {
            throw std::runtime_error("backfill bind batch params failed: " + lastError(m));
        }
        if (mysql_stmt_execute(stmt) != 0) {
            throw std::runtime_error("backfill execute batch failed: " + lastError(m));
        }
        uint64_t id = 0;
        int32_t userId = 0;
        char payloadProbe = 0;
        unsigned long payloadLen = 0;
        MYSQL_BIND outBinds[3];
        memset(outBinds, 0, sizeof(outBinds));
        bindU64(outBinds[0], &id);
        outBinds[1].buffer_type = MYSQL_TYPE_LONG;
        outBinds[1].buffer = &userId;
        outBinds[2].buffer_type = MYSQL_TYPE_BLOB;
        outBinds[2].buffer = &payloadProbe;
        outBinds[2].buffer_length = 1;
        outBinds[2].length = &payloadLen;
        if (mysql_stmt_bind_result(stmt, outBinds) != 0 || mysql_stmt_store_result(stmt) != 0) {
            throw std::runtime_error("backfill store batch result failed: " + lastError(m));
        }
        while (true) {
            int rc = mysql_stmt_fetch(stmt);
            if (rc == MYSQL_NO_DATA) {
                break;
            }
            if (rc != 0 && rc != MYSQL_DATA_TRUNCATED) {
                throw std::runtime_error("backfill fetch batch failed: " + lastError(m));
            }
            OfflineRow row;
            row.id = static_cast<int64_t>(id);
            row.userId = static_cast<int64_t>(userId);
            row.payload = fetchBlob(stmt, 2, payloadLen);
            out.push_back(std::move(row));
        }
    }
    return out;
}

// 全部源行 FNV-1a 64 哈希（id 升序聚合，跨 batch/进度稳定：每行喂 id 字节 + 分隔
// 符 + payload 字节）。
std::string computeSourceHash(MySQL& m)
{
    MYSQL_RES* res = m.query(
        "SELECT id, CAST(message AS BINARY) FROM OfflineMessage ORDER BY id");
    if (!res) {
        throw std::runtime_error("backfill hash query failed: " + lastError(m));
    }
    uint64_t h = 1469598103934665603ULL;
    {
        MySQLResultGuard guard(res);
        MYSQL_ROW row;
        while ((row = mysql_fetch_row(res))) {
            unsigned long* lens = mysql_fetch_lengths(res);
            const char* idStr = row[0];
            const char* payload = row[1];
            for (unsigned long i = 0; i < lens[0]; ++i) {
                h ^= static_cast<unsigned char>(idStr[i]);
                h *= 1099511628211ULL;
            }
            h ^= 0;
            h *= 1099511628211ULL;
            for (unsigned long i = 0; i < lens[1]; ++i) {
                h ^= static_cast<unsigned char>(payload[i]);
                h *= 1099511628211ULL;
            }
        }
    }
    return hex64(h);
}

void ensureTables(MySQL& m)
{
    if (!m.update(kCheckpointCreate) || !m.update(kQuarantineCreate)) {
        throw std::runtime_error("backfill: cannot create helper tables: " + lastError(m));
    }
}

// 已迁移高水位；checkpoint 表缺失/无行 → 0（幂等，重复处理安全）。
uint64_t readCheckpoint(MySQL& m)
{
    MYSQL_RES* res = m.query(
        "SELECT last_id FROM OfflineBackfillCheckpoint WHERE scope='offline_message'");
    if (!res) {
        return 0;
    }
    MySQLResultGuard guard(res);
    MYSQL_ROW row = mysql_fetch_row(res);
    if (!row || !row[0]) {
        return 0;
    }
    return static_cast<uint64_t>(strtoull(row[0], nullptr, 10));
}

void writeCheckpoint(MySQL& m, uint64_t lastId)
{
    uint64_t pId = lastId;
    MYSQL_BIND binds[1];
    bindU64(binds[0], &pId);
    (void)execStmt(m,
        "INSERT INTO OfflineBackfillCheckpoint(scope, last_id) VALUES('offline_message', ?) "
        "ON DUPLICATE KEY UPDATE last_id=VALUES(last_id)",
        binds, 1);
}

// INSERT IGNORE：已 quarantine 行重跑幂等（不重复插入）；返回是否新增。
bool insertQuarantine(MySQL& m, int64_t offlineId, int64_t userId, const std::string& reason,
                      const std::string& payload)
{
    const std::string r = reason.size() > kReasonLimit ? reason.substr(0, kReasonLimit) : reason;
    uint64_t pId = static_cast<uint64_t>(offlineId);
    uint64_t pUser = static_cast<uint64_t>(userId);
    unsigned long reasonLen = 0;
    unsigned long payloadLen = 0;
    MYSQL_BIND binds[4];
    bindU64(binds[0], &pId);
    bindU64(binds[1], &pUser);
    bindString(binds[2], &r, &reasonLen);
    bindBlob(binds[3], &payload, &payloadLen);
    return execStmt(m,
        "INSERT IGNORE INTO OfflineBackfillQuarantine(offline_id, user_id, reason, payload) "
        "VALUES(?,?,?,?)",
        binds, 4) > 0;
}

// 按幂等键查已有 Message（同 sender_id + client_message_id）；返回存在性与
// 原 Message 的 id/conversation_id/sequence。
bool chatMessageExists(MySQL& m, int64_t senderId, const std::string& cmid, uint64_t* messageId,
                       uint64_t* conversationId, uint64_t* sequence)
{
    const std::string sql =
        "SELECT id, conversation_id, sequence FROM ChatMessage WHERE sender_id=? "
        "AND client_message_id=? LIMIT 1";
    MYSQL_STMT* stmt = m.prepareStatement(sql.c_str());
    if (!stmt) {
        throw std::runtime_error("backfill prepare exists failed: " + lastError(m));
    }
    bool found = false;
    {
        PreparedStatementGuard guard(stmt);
        uint64_t pSender = static_cast<uint64_t>(senderId);
        unsigned long cmidLen = 0;
        MYSQL_BIND params[2];
        bindU64(params[0], &pSender);
        bindString(params[1], &cmid, &cmidLen);
        if (mysql_stmt_bind_param(stmt, params) != 0) {
            throw std::runtime_error("backfill bind exists params failed: " + lastError(m));
        }
        if (mysql_stmt_execute(stmt) != 0) {
            throw std::runtime_error("backfill execute exists failed: " + lastError(m));
        }
        uint64_t id = 0;
        uint64_t conv = 0;
        uint64_t seq = 0;
        MYSQL_BIND outBinds[3];
        bindU64(outBinds[0], &id);
        bindU64(outBinds[1], &conv);
        bindU64(outBinds[2], &seq);
        if (mysql_stmt_bind_result(stmt, outBinds) != 0 || mysql_stmt_store_result(stmt) != 0) {
            throw std::runtime_error("backfill store exists result failed: " + lastError(m));
        }
        if (mysql_stmt_fetch(stmt) == 0) {
            found = true;
            *messageId = id;
            *conversationId = conv;
            *sequence = seq;
        }
    }
    return found;
}

// 命令快照（P3-04 冻结编码：kind/direct_recipient|group_id/members），与
// MySQLMessageStore::encodeCommandSnapshot 同形状（decodeCommandSnapshot 读回）。
std::string encodeCommandSnapshot(const SendMessageCommand& cmd)
{
    nlohmann::json j;
    if (cmd.kind == SendMessageCommand::Kind::Direct) {
        j["kind"] = "DIRECT";
        j["direct_recipient"] = cmd.directRecipient.value;
    } else {
        j["kind"] = "GROUP";
        j["group_id"] = cmd.groupId.value;
        nlohmann::json members = nlohmann::json::array();
        for (size_t i = 0; i < cmd.members.size(); ++i) {
            members.push_back(cmd.members[i].value);
        }
        j["members"] = members;
    }
    return j.dump();
}

// 已存在同键行（此前迁移已落库但 checkpoint 未记录）→ 补齐 ledger 契约片段：
// OutboxEvent 快照（findAccepted 的 JOIN 依赖，UNIQUE(event_type, aggregate_message_id)
// 幂等）与 MessageDelivery（recipient = 旧行 userid，INSERT IGNORE）。不产生第二行
// ChatMessage。
void adoptExisting(MySQL& m, uint64_t messageId, const SendMessageCommand& cmd, int64_t recipientId)
{
    const std::string payload = encodeCommandSnapshot(cmd);
    unsigned long payloadLen = 0;
    MYSQL_BIND binds[2];
    bindU64(binds[0], &messageId);
    bindBlob(binds[1], &payload, &payloadLen);
    (void)execStmt(m,
        "INSERT INTO OutboxEvent(aggregate_message_id, event_type, payload) "
        "VALUES(?, 'MessageAccepted', ?) "
        "ON DUPLICATE KEY UPDATE aggregate_message_id=VALUES(aggregate_message_id)",
        binds, 2);
    {
        uint64_t pMid = messageId;
        uint64_t pRecipient = static_cast<uint64_t>(recipientId);
        MYSQL_BIND dbinds[2];
        bindU64(dbinds[0], &pMid);
        bindU64(dbinds[1], &pRecipient);
        (void)execStmt(m,
            "INSERT IGNORE INTO MessageDelivery(message_id, recipient_id, state, attempt_count, "
            "next_attempt_at, lease_owner, lease_until, last_sent_at, acknowledged_at, expires_at) "
            "VALUES(?,?,0,0,NULL,NULL,NULL,NULL,NULL,NULL)",
            dbinds, 2);
    }
}

// 构建 backfill 命令：幂等键 legacy:<offline_id>，群消息成员快照 = 离线行所属用户
//（旧表 per-recipient 形状，每行独立 Message+Delivery）。
SendMessageCommand buildCommand(const OfflineRow& row, const ParsedRow& parsed)
{
    SendMessageCommand cmd;
    cmd.clientMessageId = ClientMessageId("legacy:" + std::to_string(row.id));
    cmd.content = parsed.content;
    if (parsed.msgid == 6) {
        cmd.kind = SendMessageCommand::Kind::Direct;
        cmd.directRecipient = UserId(static_cast<uint64_t>(parsed.toid));
    } else {
        cmd.kind = SendMessageCommand::Kind::Group;
        cmd.groupId = GroupId(static_cast<uint64_t>(parsed.groupid));
        cmd.members.push_back(UserId(static_cast<uint64_t>(row.userId)));
    }
    return cmd;
}

// 经 MessageStore accept 路径写新 ledger（Conversation 必要时 + ChatMessage +
// MessageDelivery；accept 事务原子提交，含 OutboxEvent）。抛异常由调用方转 quarantine。
void migrateRow(MySQLMessageStore& store, const OfflineRow& row, const ParsedRow& parsed,
                const SendMessageCommand& cmd)
{
    SessionIdentity sender(UserId(static_cast<uint64_t>(parsed.sender)), 0);
    ConversationId conv = store.getOrCreateConversation(sender, cmd);
    Message draft;
    draft.conversationId = conv;
    draft.senderId = sender.userId;
    draft.command = cmd;
    Message accepted = store.insertMessage(draft);
    Delivery delivery;
    delivery.messageId = accepted.id;
    delivery.conversationId = accepted.conversationId;
    delivery.sequence = accepted.sequence;
    delivery.recipient = UserId(static_cast<uint64_t>(row.userId));
    delivery.state = DeliveryState::Pending;
    // legacy：无 retention（旧表无到期语义），expiresAtMs 保持 0（insertDelivery 写 NULL）。
    store.insertDelivery(delivery);
}

// 单行处理：可解析 → 幂等检查 → 迁移；不可解析/写失败 → quarantine；绝不静默删。
// 错误分类（P3-10 M4，卡登记）：MessageStoreError kind=DependencyBusy（1205/1213/
// 断连/池超时）是瞬态依赖错误，不是行数据问题——重抛 fail-fast（checkpoint 未写，
// 重跑自动重试）；仅 NotFound（FK 1452）/解析类/Storage 等数据层分类走 quarantine。
void processRow(MySQL& m, const OfflineRow& row, const BackfillConfig& cfg,
                MySQLMessageStore& store, BackfillStats* stats)
{
    stats->sourceRows += 1;
    ParsedRow parsed = parsePayload(row.payload);
    if (!parsed.ok) {
        if (cfg.dryRun) {
            stats->quarantined += 1;
            return;
        }
        if (insertQuarantine(m, row.id, row.userId, parsed.reason, row.payload)) {
            stats->quarantined += 1;
        } else {
            stats->skippedIdempotent += 1;  // 已 quarantine 行重跑
        }
        return;
    }

    const SendMessageCommand cmd = buildCommand(row, parsed);
    uint64_t existingId = 0;
    uint64_t existingConv = 0;
    uint64_t existingSeq = 0;
    const bool exists = chatMessageExists(m, parsed.sender, cmd.clientMessageId.value(),
                                          &existingId, &existingConv, &existingSeq);
    if (exists) {
        if (!cfg.dryRun) {
            adoptExisting(m, existingId, cmd, row.userId);
        }
        stats->skippedIdempotent += 1;
        return;
    }

    if (cfg.dryRun) {
        stats->migrated += 1;
        return;
    }

    try {
        migrateRow(store, row, parsed, cmd);
        stats->migrated += 1;
    } catch (const MessageStoreError& e) {
        // 瞬态依赖错误（1205/1213/断连/池超时）fail-fast：重抛，本行不入
        // quarantine、checkpoint 未写（本批不落高水位），错误清除后重跑自动重试。
        if (e.kind() == StoreErrorKind::DependencyBusy) {
            throw;
        }
        // 数据层分类（NotFound FK 1452 / Storage 等）：quarantine 保留源行。
        const std::string reason = std::string("ledger write failed: ") + e.what();
        if (insertQuarantine(m, row.id, row.userId, reason, row.payload)) {
            stats->quarantined += 1;
        } else {
            stats->skippedIdempotent += 1;
        }
    } catch (const std::exception& e) {
        // 其它异常（非 MessageStoreError）：quarantine 保留源行（语义不变）。
        const std::string reason = std::string("ledger write failed: ") + e.what();
        if (insertQuarantine(m, row.id, row.userId, reason, row.payload)) {
            stats->quarantined += 1;
        } else {
            stats->skippedIdempotent += 1;
        }
    }
}

}  // namespace

const char* kCheckpointTable = "OfflineBackfillCheckpoint";
const char* kQuarantineTable = "OfflineBackfillQuarantine";

Runner::Runner(ConnectionPool& pool)
    : pool_(pool), faultHook_(nullptr)
{
}

Runner::Runner(ConnectionPool& pool, MySQLMessageStore::FaultHook* faultHook)
    : pool_(pool), faultHook_(faultHook)
{
}

BackfillStats Runner::run(const BackfillConfig& cfg)
{
    if (cfg.batchSize == 0) {
        throw std::invalid_argument("backfill batchSize must be >= 1");
    }
    BackfillStats stats;
    ConnectionPool::AcquireResult acq = pool_.acquire(5000);
    if (!acq.lease) {
        throw std::runtime_error("backfill: connection acquire failed");
    }
    MySQL& m = *acq.lease.get();

    if (!cfg.dryRun) {
        ensureTables(m);
    }
    uint64_t checkpointId = readCheckpoint(m);
    // hash 覆盖全部源行，与 checkpoint/批次无关（跨批/进度稳定比对）。
    stats.sourceHash = computeSourceHash(m);

    MySQLMessageStore store(pool_, kFanOutCap, faultHook_);
    uint32_t batchesDone = 0;
    while (true) {
        if (cfg.maxBatches != 0 && batchesDone >= cfg.maxBatches) {
            break;
        }
        std::vector<OfflineRow> batch = readBatch(m, checkpointId, cfg.batchSize);
        if (batch.empty()) {
            break;
        }
        for (size_t i = 0; i < batch.size(); ++i) {
            processRow(m, batch[i], cfg, store, &stats);
        }
        checkpointId = static_cast<uint64_t>(batch.back().id);
        if (!cfg.dryRun) {
            writeCheckpoint(m, checkpointId);
        }
        batchesDone += 1;
    }
    return stats;
}

uint64_t Runner::checkpoint() const
{
    ConnectionPool::AcquireResult acq = pool_.acquire(5000);
    if (!acq.lease) {
        return 0;
    }
    return readCheckpoint(*acq.lease.get());
}

std::vector<QuarantineEntry> Runner::quarantine() const
{
    std::vector<QuarantineEntry> out;
    ConnectionPool::AcquireResult acq = pool_.acquire(5000);
    if (!acq.lease) {
        return out;
    }
    MySQL& m = *acq.lease.get();
    MYSQL_RES* res = m.query(
        "SELECT offline_id, user_id, reason, CAST(payload AS BINARY) "
        "FROM OfflineBackfillQuarantine ORDER BY offline_id");
    if (!res) {
        return out;  // 表不存在（dry-run/未运行）→ 空
    }
    MySQLResultGuard guard(res);
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res))) {
        unsigned long* lens = mysql_fetch_lengths(res);
        QuarantineEntry e;
        e.offlineId = row[0] ? atoll(row[0]) : 0;
        e.userId = row[1] ? atoll(row[1]) : 0;
        e.reason = row[2] ? std::string(row[2]) : "";
        e.payload = (row[3] && lens) ? std::string(row[3], lens[3]) : "";
        out.push_back(std::move(e));
    }
    return out;
}

}  // namespace offline_backfill
