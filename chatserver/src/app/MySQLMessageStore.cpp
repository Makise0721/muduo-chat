#include "app/MySQLMessageStore.hpp"

#include "db/MySQLGuards.hpp"

#include "json.hpp"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>

namespace {

const int64_t kAcquireTimeoutMs = 5000;

// READ COMMITTED：并发唯一键竞争（1062）后，事务内重读必须看到已提交原行
// （REPEATABLE READ 的读快照在 1062 前已建立，看不到后来者的提交）。
const char* kBeginSql = "SET TRANSACTION ISOLATION LEVEL READ COMMITTED";

const char* kEventTypeAccepted = "MessageAccepted";
const char* kKindDirect = "DIRECT";
const char* kKindGroup = "GROUP";

// 绑定辅助：buffer 必须由调用方持有到 execute 完成（MySQLUserRepository 先例）。
void bindInt(MYSQL_BIND& b, int32_t* p)
{
    memset(&b, 0, sizeof(b));
    b.buffer_type = MYSQL_TYPE_LONG;
    b.buffer = p;
}

void bindU64(MYSQL_BIND& b, uint64_t* p)
{
    memset(&b, 0, sizeof(b));
    b.buffer_type = MYSQL_TYPE_LONGLONG;
    b.is_unsigned = 1;
    b.buffer = p;
}

void bindI64(MYSQL_BIND& b, int64_t* p)
{
    memset(&b, 0, sizeof(b));
    b.buffer_type = MYSQL_TYPE_LONGLONG;
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

void bindNull(MYSQL_BIND& b)
{
    memset(&b, 0, sizeof(b));
    b.buffer_type = MYSQL_TYPE_NULL;
}

void throwStoreError(unsigned int err, const std::string& what)
{
    throw MessageStoreError(mapStoreError(err), what);
}

// 执行带参 prepared statement（无结果集）：0=成功，1062=唯一键冲突（调用方
// 按幂等/竞争语义处理，不抛），其余错误抛映射后的 MessageStoreError。
unsigned int execStmt(MySQL& m, const char* sql, MYSQL_BIND* binds, unsigned nBinds)
{
    MYSQL_STMT* stmt = m.prepareStatement(sql);
    if (!stmt) {
        throwStoreError(mysql_errno(m.getConnection()), std::string("prepare failed: ") + sql);
    }
    unsigned int err = 0;
    {
        PreparedStatementGuard guard(stmt);
        if (binds != nullptr && mysql_stmt_bind_param(stmt, binds) != 0) {
            err = mysql_stmt_errno(stmt);
        } else if (mysql_stmt_execute(stmt) != 0) {
            err = mysql_stmt_errno(stmt);
        }
    }
    if (err == 0 || err == 1062) {
        return err;
    }
    throwStoreError(err, std::string("stmt failed: ") + sql);
    return err;
}

// 单行单 u64 查询；无行返回 false。
bool queryU64(MySQL& m, const char* sql, MYSQL_BIND* params, unsigned nParams, uint64_t& out)
{
    MYSQL_STMT* stmt = m.prepareStatement(sql);
    if (!stmt) {
        throwStoreError(mysql_errno(m.getConnection()), std::string("prepare failed: ") + sql);
    }
    bool found = false;
    unsigned int err = 0;
    {
        PreparedStatementGuard guard(stmt);
        if (params != nullptr && mysql_stmt_bind_param(stmt, params) != 0) {
            err = mysql_stmt_errno(stmt);
        } else if (mysql_stmt_execute(stmt) != 0) {
            err = mysql_stmt_errno(stmt);
        } else {
            MYSQL_BIND outBind;
            memset(&outBind, 0, sizeof(outBind));
            outBind.buffer_type = MYSQL_TYPE_LONGLONG;
            outBind.is_unsigned = 1;
            outBind.buffer = &out;
            if (mysql_stmt_bind_result(stmt, &outBind) != 0 || mysql_stmt_store_result(stmt) != 0) {
                err = mysql_stmt_errno(stmt);
            } else {
                int rc = mysql_stmt_fetch(stmt);
                if (rc == 0) {
                    found = true;
                } else if (rc != MYSQL_NO_DATA) {
                    err = mysql_stmt_errno(stmt);
                }
            }
        }
    }
    if (err != 0) {
        throwStoreError(err, std::string("query failed: ") + sql);
    }
    return found;
}

// 结果集 BLOB 列二段读取：先探针取实际长度，再 fetch_column 取全量。
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
        throw MessageStoreError(StoreErrorKind::Storage, "fetch_column failed");
    }
    return out;
}

// 命令快照：kind/direct_recipient|group_id/members（有序，保 samePayload 逐元素
// 比较语义）写入 OutboxEvent.payload；content/client_message_id 走列原样存储。
std::string encodeCommandSnapshot(const SendMessageCommand& cmd)
{
    nlohmann::json j;
    if (cmd.kind == SendMessageCommand::Kind::Direct) {
        j["kind"] = kKindDirect;
        j["direct_recipient"] = cmd.directRecipient.value;
    } else {
        j["kind"] = kKindGroup;
        j["group_id"] = cmd.groupId.value;
        nlohmann::json members = nlohmann::json::array();
        for (size_t i = 0; i < cmd.members.size(); ++i) {
            members.push_back(cmd.members[i].value);
        }
        j["members"] = members;
    }
    return j.dump();
}

SendMessageCommand decodeCommandSnapshot(const std::string& payloadJson,
                                         const std::string& content,
                                         const std::string& clientMessageId)
{
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(payloadJson);
    } catch (...) {
        throw MessageStoreError(StoreErrorKind::Storage, "invalid outbox payload");
    }
    SendMessageCommand cmd;
    cmd.clientMessageId = ClientMessageId(clientMessageId);
    cmd.content = content;
    try {
        const std::string kind = j.at("kind").get<std::string>();
        if (kind == kKindDirect) {
            cmd.kind = SendMessageCommand::Kind::Direct;
            cmd.directRecipient = UserId{j.at("direct_recipient").get<uint64_t>()};
        } else if (kind == kKindGroup) {
            cmd.kind = SendMessageCommand::Kind::Group;
            cmd.groupId = GroupId{j.at("group_id").get<uint64_t>()};
            const nlohmann::json& members = j.at("members");
            for (size_t i = 0; i < members.size(); ++i) {
                cmd.members.push_back(UserId{members[i].get<uint64_t>()});
            }
        } else {
            throw MessageStoreError(StoreErrorKind::Storage, "unknown payload kind");
        }
    } catch (const MessageStoreError&) {
        throw;
    } catch (...) {
        throw MessageStoreError(StoreErrorKind::Storage, "malformed outbox payload");
    }
    return cmd;
}

// P3-08：lease_owner 列编 "uid:gen[:bootid]"——boot id（进程实例标识）保证
// 跨进程重启后 owner 必不同（generation 重置可能使 "uid:gen" 与新 owner 相同，
// 否则 P3-07 的 generation fencing 失效，HOL 被卡到 lease 到期）。VARCHAR(64)
// 足够容纳（uid 为 int32，gen/bootid 为 uint64 十进制，最坏约 52 字符）。
std::string encodeLeaseOwner(const SessionIdentity& owner, uint64_t bootId)
{
    if (owner.userId.value == 0) {
        return std::string();
    }
    return std::to_string(owner.userId.value) + ":" + std::to_string(owner.generation) + ":" +
           std::to_string(bootId);
}

// 旧格式 "uid:gen"（无 boot id，启动前遗留行）按 bootId=0 解码；任何该进程
// 新 claim 的 boot id 均非 0，故遗留行必然不等，会被立即回收（安全方向）。
void decodeLeaseOwner(const char* s, unsigned long len, SessionIdentity* owner, uint64_t* bootId)
{
    if (len == 0) {
        *owner = SessionIdentity();
        *bootId = 0;
        return;
    }
    // "uid:gen[:bootid]"
    size_t colon1 = 0;
    while (colon1 < len && s[colon1] != ':') {
        ++colon1;
    }
    if (colon1 == 0 || colon1 + 1 >= len) {
        throw MessageStoreError(StoreErrorKind::Storage, "malformed lease owner");
    }
    size_t colon2 = len;
    for (size_t i = colon1 + 1; i < len; ++i) {
        if (s[i] == ':') {
            colon2 = i;
            break;
        }
    }
    const uint64_t uid =
        static_cast<uint64_t>(strtoull(std::string(s, colon1).c_str(), nullptr, 10));
    const uint64_t gen = static_cast<uint64_t>(
        strtoull(std::string(s + colon1 + 1, colon2 - colon1 - 1).c_str(), nullptr, 10));
    const uint64_t boot = (colon2 < len)
                              ? static_cast<uint64_t>(
                                    strtoull(std::string(s + colon2 + 1, len - colon2 - 1).c_str(),
                                             nullptr, 10))
                              : 0;
    *owner = SessionIdentity(UserId{uid}, gen);
    *bootId = boot;
}

int32_t encodeState(DeliveryState state)
{
    switch (state) {
        case DeliveryState::Pending:
            return 0;
        case DeliveryState::InFlight:
            return 1;
        case DeliveryState::Acknowledged:
            return 2;
        case DeliveryState::Expired:
            return 3;
    }
    return 0;  // 防御：未来新增枚举值时回退 Pending
}

DeliveryState decodeState(int32_t state)
{
    switch (state) {
        case 0:
            return DeliveryState::Pending;
        case 1:
            return DeliveryState::InFlight;
        case 2:
            return DeliveryState::Acknowledged;
        case 3:
            return DeliveryState::Expired;
        default:
            throw MessageStoreError(StoreErrorKind::Storage, "unknown delivery state");
    }
}

int64_t secsToMs(double secs)
{
    return static_cast<int64_t>(secs * 1000.0);
}

// RAII accept 事务：acquire 连接（带 deadline）→ READ COMMITTED → START
// TRANSACTION；析构时未提交则 ROLLBACK（错误路径/线程退出兜底）；commit() 失败
// 时显式回滚（尽力而为，ROLLBACK 再失败记录日志、析构兜底再试），回滚成功后
// 标记关闭、析构不重复回滚——保证归还池的连接无未提交事务。
class TransactionGuard {
public:
    explicit TransactionGuard(ConnectionPool& pool,
                              MySQLMessageStore::FaultHook* faultHook = nullptr)
        : faultHook_(faultHook)
    {
        ConnectionPool::AcquireResult acq = pool.acquire(kAcquireTimeoutMs);
        if (!acq.lease) {
            throw MessageStoreError(StoreErrorKind::DependencyBusy,
                                    "connection acquire failed");
        }
        lease_ = std::move(acq.lease);
        MySQL* mysql = lease_.get();
        if (!mysql->update(kBeginSql)) {
            throw MessageStoreError(StoreErrorKind::DependencyBusy,
                                    "set transaction isolation failed");
        }
        if (!mysql->update("START TRANSACTION")) {
            throwStoreError(mysql_errno(mysql->getConnection()), "start transaction failed");
        }
        open_ = true;
    }

    ~TransactionGuard()
    {
        if (open_ && lease_) {
            lease_->update("ROLLBACK");
        }
    }

    void commit()
    {
        if (!open_) {
            return;
        }
        bool failed = false;
        unsigned int err = 0;
        if (faultHook_ != nullptr && faultHook_->failCommit()) {
            // 故障注入：跳过真实 COMMIT，模拟其失败（如 1205 lock wait timeout）。
            failed = true;
            err = 1205;
        } else if (!lease_->update("COMMIT")) {
            failed = true;
            err = mysql_errno(lease_->getConnection());
        }
        if (failed) {
            // COMMIT 失败时事务仍开放：显式回滚（尽力而为）后重抛。绝不让带
            // 未提交事务的连接归还池——START TRANSACTION 隐式提交的自愈脆弱，
            // 会把失败 accept 的部分数据复活成已提交。
            open_ = true;
            if (!lease_->update("ROLLBACK")) {
                std::cerr << "rollback after failed commit also failed, mysql_errno="
                          << mysql_errno(lease_->getConnection()) << std::endl;
            } else {
                open_ = false;  // 回滚成功：析构不再重复回滚
            }
            throwStoreError(err, "commit failed");
        }
    }

    bool open() const { return open_; }
    MySQL& mysql() { return *lease_.get(); }

private:
    ConnectionLease lease_;
    bool open_ = false;
    MySQLMessageStore::FaultHook* faultHook_ = nullptr;
};

// 线程本地 accept 事务上下文：ReliableMessaging 由单一调用者串行驱动（P3-02
// 文档化），但并发测试以每线程一个 ReliableMessaging 共享同一 adapter——连接
// 租约与事务状态因此按线程隔离；线程退出时 guard 析构 ROLLBACK 兜底。
struct AcceptTx {
    std::unique_ptr<TransactionGuard> guard;
    uint64_t expectedDeliveries = 0;  // getOrCreateConversation 由命令快照确定
    uint64_t insertedDeliveries = 0;
};
thread_local AcceptTx g_tx;

} // namespace

StoreErrorKind mapStoreError(unsigned int err)
{
    switch (err) {
        case 1205:  // ER_LOCK_WAIT_TIMEOUT
        case 1213:  // ER_LOCK_DEADLOCK
        case 2006:  // CR_SERVER_GONE_ERROR
        case 2013:  // CR_SERVER_LOST
        case 1053:  // ER_SERVER_SHUTDOWN
        case 2003:  // CR_CONN_HOST_ERROR
            return StoreErrorKind::DependencyBusy;
        case 1452:  // ER_NO_REFERENCED_ROW_2
            return StoreErrorKind::NotFound;
        case 1406:  // ER_DATA_TOO_LONG
        case 1366:  // ER_TRUNCATED_WRONG_VALUE_FOR_FIELD
        case 1300:  // ER_INVALID_CHARACTER_STRING
        case 1062:  // ER_DUP_ENTRY——期望处先按 1062 处理，落到这里按 Storage
        default:
            return StoreErrorKind::Storage;
    }
}

MySQLMessageStore::MySQLMessageStore(ConnectionPool& pool, uint64_t fanOutCap,
                                     FaultHook* faultHook)
    : pool_(pool), fanOutCap_(fanOutCap), faultHook_(faultHook)
{
    if (fanOutCap_ == 0) {
        throw std::invalid_argument("fanOutCap must be >= 1");
    }
}

void MySQLMessageStore::fire(Step step)
{
    if (faultHook_ != nullptr) {
        faultHook_->onStep(step);
    }
}

void MySQLMessageStore::beginTx()
{
    if (g_tx.guard) {
        return;  // 已在事务中（防御）
    }
    g_tx.guard.reset(new TransactionGuard(pool_, faultHook_));
}

void MySQLMessageStore::commitTx()
{
    if (!g_tx.guard) {
        return;
    }
    g_tx.guard->commit();
    fire(Step::Commit);
    g_tx.guard.reset();
    g_tx.expectedDeliveries = 0;
    g_tx.insertedDeliveries = 0;
}

void MySQLMessageStore::rollbackTx()
{
    if (g_tx.guard) {
        g_tx.guard.reset();  // 析构 ROLLBACK（open 时）；保留计数器（同一 accept 内重试）
    }
}

void MySQLMessageStore::rollbackAndClear()
{
    rollbackTx();
    g_tx.expectedDeliveries = 0;
    g_tx.insertedDeliveries = 0;
}

// 边界：非 accept 操作先提交遗留 accept 事务（防御；0 成员快照现已内联提交）。
void MySQLMessageStore::finishPending()
{
    if (g_tx.guard) {
        commitTx();
    }
}

void MySQLMessageStore::ensureTx()
{
    if (!g_tx.guard) {
        beginTx();  // 防御：正常路径事务已由 getOrCreateConversation 打开
    }
}

uint64_t MySQLMessageStore::findDirectConversation(MySQL& m, int32_t low, int32_t high)
{
    uint64_t id = 0;
    int32_t pLow = low;
    int32_t pHigh = high;
    MYSQL_BIND binds[2];
    bindInt(binds[0], &pLow);
    bindInt(binds[1], &pHigh);
    if (!queryU64(m, "SELECT conversation_id FROM DirectConversation "
                     "WHERE user_low_id=? AND user_high_id=?",
                  binds, 2, id)) {
        return 0;
    }
    return id;
}

uint64_t MySQLMessageStore::findGroupConversation(MySQL& m, int32_t groupId)
{
    uint64_t id = 0;
    int32_t pGroup = groupId;
    MYSQL_BIND bind;
    bindInt(bind, &pGroup);
    if (!queryU64(m, "SELECT conversation_id FROM GroupConversation WHERE group_id=?",
                  &bind, 1, id)) {
        return 0;
    }
    return id;
}

uint64_t MySQLMessageStore::createDirectConversation(MySQL& m, int32_t low, int32_t high)
{
    fire(Step::CreateConversation);
    (void)execStmt(m, "INSERT INTO Conversation(kind) VALUES('DIRECT')", nullptr, 0);
    uint64_t id = static_cast<uint64_t>(mysql_insert_id(m.getConnection()));
    fire(Step::CreateConversationLink);
    uint64_t pId = id;
    int32_t pLow = low;
    int32_t pHigh = high;
    MYSQL_BIND binds[3];
    bindU64(binds[0], &pId);
    bindInt(binds[1], &pLow);
    bindInt(binds[2], &pHigh);
    unsigned int err = execStmt(m,
        "INSERT INTO DirectConversation(conversation_id, user_low_id, user_high_id) VALUES(?,?,?)",
        binds, 3);
    if (err == 1062) {
        // 并发创建竞争：唯一键赢家已提交。回滚本事务（丢弃孤儿 Conversation 行），
        // 新事务内重读已提交对话（RC 隔离保证可见）。
        rollbackTx();
        beginTx();
        fire(Step::RecoverConversation);
        MySQL& m2 = g_tx.guard->mysql();
        uint64_t existing = findDirectConversation(m2, low, high);
        if (existing == 0) {
            throw MessageStoreError(StoreErrorKind::Storage,
                                    "conversation lost after unique race");
        }
        return existing;
    }
    return id;
}

uint64_t MySQLMessageStore::createGroupConversation(MySQL& m, int32_t groupId)
{
    fire(Step::CreateConversation);
    (void)execStmt(m, "INSERT INTO Conversation(kind) VALUES('GROUP')", nullptr, 0);
    uint64_t id = static_cast<uint64_t>(mysql_insert_id(m.getConnection()));
    fire(Step::CreateConversationLink);
    uint64_t pId = id;
    int32_t pGroup = groupId;
    MYSQL_BIND binds[2];
    bindU64(binds[0], &pId);
    bindInt(binds[1], &pGroup);
    unsigned int err = execStmt(m,
        "INSERT INTO GroupConversation(conversation_id, group_id) VALUES(?,?)",
        binds, 2);
    if (err == 1062) {
        rollbackTx();
        beginTx();
        fire(Step::RecoverConversation);
        MySQL& m2 = g_tx.guard->mysql();
        uint64_t existing = findGroupConversation(m2, groupId);
        if (existing == 0) {
            throw MessageStoreError(StoreErrorKind::Storage,
                                    "conversation lost after unique race");
        }
        return existing;
    }
    return id;
}

std::shared_ptr<const Message> MySQLMessageStore::findAccepted(
    const ClientMessageId& clientMessageId, UserId sender)
{
    finishPending();
    ConnectionPool::AcquireResult acq = pool_.acquire(kAcquireTimeoutMs);
    if (!acq.lease) {
        throw MessageStoreError(StoreErrorKind::DependencyBusy, "connection acquire failed");
    }
    int32_t pSender = static_cast<int32_t>(sender.value);
    unsigned long cmidLen = 0;
    const std::string& cmid = clientMessageId.value();
    MYSQL_BIND binds[2];
    bindInt(binds[0], &pSender);
    bindString(binds[1], &cmid, &cmidLen);
    return loadMessage(*acq.lease.get(), "m.sender_id=? AND m.client_message_id=?", binds, 2);
}

ConversationId MySQLMessageStore::getOrCreateConversation(const SessionIdentity& sender,
                                                          const SendMessageCommand& cmd)
{
    // fan-out cap 与接受事务绑定（docs/tasks/P3-04.md RED 节冻结：100）：事务外
    // 拒绝，不写任何行；同 key 重试稳定返回同一错误（不会变成已接受）。
    if (cmd.kind == SendMessageCommand::Kind::Group && cmd.members.size() > fanOutCap_) {
        throw MessageStoreError(StoreErrorKind::TooManyRecipients, "fan-out exceeds cap");
    }
    try {
        finishPending();  // 防御：遗留 accept 事务在此边界提交（0 成员快照已内联提交）
        beginTx();
        g_tx.expectedDeliveries = (cmd.kind == SendMessageCommand::Kind::Direct)
                                      ? 1
                                      : static_cast<uint64_t>(cmd.members.size());
        g_tx.insertedDeliveries = 0;

        MySQL& m = g_tx.guard->mysql();
        uint64_t conversationId = 0;
        if (cmd.kind == SendMessageCommand::Kind::Direct) {
            // direct 双人 low/high 归一化由 adapter 保证（P3-03 登记）。
            const uint64_t a = sender.userId.value;
            const uint64_t b = cmd.directRecipient.value;
            const int32_t low = static_cast<int32_t>(a < b ? a : b);
            const int32_t high = static_cast<int32_t>(a < b ? b : a);
            fire(Step::FindConversation);
            conversationId = findDirectConversation(m, low, high);
            if (conversationId == 0) {
                conversationId = createDirectConversation(m, low, high);
            }
        } else {
            const int32_t groupId = static_cast<int32_t>(cmd.groupId.value);
            fire(Step::FindConversation);
            conversationId = findGroupConversation(m, groupId);
            if (conversationId == 0) {
                conversationId = createGroupConversation(m, groupId);
            }
        }
        return ConversationId{conversationId};
    } catch (...) {
        rollbackAndClear();
        throw;
    }
}

Message MySQLMessageStore::insertMessage(const Message& draft)
{
    try {
        ensureTx();
        MySQL& m = g_tx.guard->mysql();

        // 锁 Conversation 行：同对话 accept 的串行点（sequence 分配原子化，
        // 配合 UNIQUE(conversation_id, sequence) 兜底）。
        fire(Step::LockConversation);
        uint64_t pConv = draft.conversationId.value;
        MYSQL_BIND lockBind;
        bindU64(lockBind, &pConv);
        uint64_t nextSequence = 0;
        if (!queryU64(m, "SELECT next_sequence FROM Conversation WHERE id=? FOR UPDATE",
                      &lockBind, 1, nextSequence)) {
            throw MessageStoreError(StoreErrorKind::Storage, "conversation row missing");
        }
        nextSequence += 1;

        fire(Step::AdvanceSequence);
        uint64_t pSeq = nextSequence;
        MYSQL_BIND upBinds[2];
        bindU64(upBinds[0], &pSeq);
        bindU64(upBinds[1], &pConv);
        (void)execStmt(m, "UPDATE Conversation SET next_sequence=? WHERE id=?", upBinds, 2);

        // INSERT ChatMessage：(sender_id, client_message_id) 唯一键是幂等权威检查。
        fire(Step::InsertMessage);
        Message accepted = draft;
        accepted.sequence = ConversationSequence{nextSequence};
        {
            uint64_t pConvId = draft.conversationId.value;
            int32_t pSender = static_cast<int32_t>(draft.senderId.value);
            const std::string& pCmid = draft.command.clientMessageId.value();
            const std::string& pContent = draft.command.content;
            unsigned long cmidLen = 0;
            unsigned long contentLen = 0;
            MYSQL_BIND insBinds[5];
            bindU64(insBinds[0], &pConvId);
            bindInt(insBinds[1], &pSender);
            bindString(insBinds[2], &pCmid, &cmidLen);
            bindU64(insBinds[3], &pSeq);
            bindBlob(insBinds[4], &pContent, &contentLen);
            unsigned int err = execStmt(m,
                "INSERT INTO ChatMessage(conversation_id, sender_id, client_message_id, "
                "sequence, content) VALUES(?,?,?,?,?)",
                insBinds, 5);
            if (err == 1062) {
                // 并发竞争：同 (sender, client_message_id) 已被其它事务提交。
                // 回滚本事务（不消费 sequence），新事务内读已提交原行比较 payload；
                // 相同返回原结果，不同 → IdempotencyConflict（不得悄悄当重复成功）。
                rollbackTx();
                beginTx();
                MySQL& m2 = g_tx.guard->mysql();
                fire(Step::RecoverMessage);
                int32_t pSender2 = static_cast<int32_t>(draft.senderId.value);
                const std::string& pCmid2 = draft.command.clientMessageId.value();
                unsigned long cmidLen2 = 0;
                MYSQL_BIND keyBinds[2];
                bindInt(keyBinds[0], &pSender2);
                bindString(keyBinds[1], &pCmid2, &cmidLen2);
                std::shared_ptr<const Message> original =
                    loadMessage(m2, "m.sender_id=? AND m.client_message_id=?", keyBinds, 2);
                if (!original) {
                    throw MessageStoreError(StoreErrorKind::Storage,
                                            "duplicate key without committed original");
                }
                if (!samePayload(original->command, draft.command)) {
                    throw MessageStoreError(StoreErrorKind::IdempotencyConflict,
                                            "same key with different payload");
                }
                return *original;
            }
        }
        accepted.id = MessageId{static_cast<uint64_t>(mysql_insert_id(m.getConnection()))};

        fire(Step::InsertOutboxEvent);
        {
            uint64_t pMid = accepted.id.value;
            const std::string payload = encodeCommandSnapshot(draft.command);
            unsigned long payloadLen = 0;
            MYSQL_BIND outBinds[2];
            bindU64(outBinds[0], &pMid);
            bindBlob(outBinds[1], &payload, &payloadLen);
            (void)execStmt(m,
                "INSERT INTO OutboxEvent(aggregate_message_id, event_type, payload) "
                "VALUES(?, 'MessageAccepted', ?)",
                outBinds, 2);
        }
        // 0 成员快照（空群）：无 MessageDelivery 可插 → 无 insertDelivery 提交
        // 触发点；在此立即 COMMIT（accept 返回前持久化；空事务提交无害）。
        // P3-06 拒空群登记不变，本分支仅防御性保边界。
        if (g_tx.expectedDeliveries == 0) {
            commitTx();
        }
        return accepted;
    } catch (...) {
        rollbackAndClear();
        throw;
    }
}

void MySQLMessageStore::insertDelivery(const Delivery& delivery)
{
    try {
        ensureTx();
        MySQL& m = g_tx.guard->mysql();
        fire(Step::InsertDelivery);
        uint64_t pMid = delivery.messageId.value;
        int32_t pRecipient = static_cast<int32_t>(delivery.recipient.value);
        // P3-08：expires_at 必须随行持久化（accept 注入的 retention deadline）。
        // 旧实现写死 NULL，MySQL 侧 expireDeliveries 的 `expires_at IS NOT NULL`
        // 永不命中，Expired 转移与 retention cleanup 在真实库上静默失效。
        int64_t expiresSecs = delivery.expiresAtMs / 1000;
        unsigned long expiresLen = 0;
        MYSQL_BIND binds[3];
        bindU64(binds[0], &pMid);
        bindInt(binds[1], &pRecipient);
        if (delivery.expiresAtMs == 0) {
            bindNull(binds[2]);
        } else {
            bindI64(binds[2], &expiresSecs);
        }
        unsigned int err = execStmt(m,
            "INSERT INTO MessageDelivery(message_id, recipient_id, state, attempt_count, "
            "next_attempt_at, lease_owner, lease_until, last_sent_at, acknowledged_at, "
            "expires_at) VALUES(?,?,0,0,NULL,NULL,NULL,NULL,NULL,FROM_UNIXTIME(?))",
            binds, 3);
        if (err == 1062) {
            // 同 (message_id, recipient_id) 已存在：并发重复 accept 或同 key 竞争
            // 恢复路径的原消息投递——幂等 no-op（message_id 全局唯一，PK 冲突
            // 不可能是不同消息）。
        }
        g_tx.insertedDeliveries += 1;
        if (g_tx.insertedDeliveries == g_tx.expectedDeliveries) {
            commitTx();  // 提交先于 accept 返回（Durable acceptance 承诺）
        }
    } catch (...) {
        rollbackAndClear();
        throw;
    }
}

void MySQLMessageStore::updateDelivery(const Delivery& delivery)
{
    finishPending();
    ConnectionPool::AcquireResult acq = pool_.acquire(kAcquireTimeoutMs);
    if (!acq.lease) {
        throw MessageStoreError(StoreErrorKind::DependencyBusy, "connection acquire failed");
    }
    MySQL* mysql = acq.lease.get();
    int32_t pState = encodeState(delivery.state);
    int32_t pAttempts = static_cast<int32_t>(delivery.attemptCount);
    const std::string owner = encodeLeaseOwner(delivery.leaseOwner, delivery.leaseBootId);
    int64_t leaseSecs = delivery.leaseUntilMs / 1000;
    int64_t ackedSecs = delivery.acknowledgedAtMs / 1000;
    int64_t expiresSecs = delivery.expiresAtMs / 1000;
    // P3-08：ACK timeout 扫描字段（spec §3/§4）。
    int64_t lastSentSecs = delivery.lastSentAtMs / 1000;
    // 补缺轮 M2：next_attempt_at 必须向上取整（ceil），绝不早于写入时刻。因
    // DATETIME(0) 秒精度，floor(ms/1000) 会使到期判定 `next_attempt_at <=
    // floor(now/1000)` 提前至多 999ms（不安全方向：在排程时刻前重投）。仅此列
    // 取 ceil；lease_until 等其它列保持 floor（既有 round-trip 语义不动）。
    int64_t nextAttemptSecs = (delivery.nextAttemptAtMs + 999) / 1000;
    uint64_t pMid = delivery.messageId.value;
    int32_t pRecipient = static_cast<int32_t>(delivery.recipient.value);
    unsigned long ownerLen = 0;
    MYSQL_BIND binds[10];
    bindInt(binds[0], &pState);
    bindInt(binds[1], &pAttempts);
    if (delivery.leaseOwner.userId.value == 0) {
        bindNull(binds[2]);
    } else {
        bindString(binds[2], &owner, &ownerLen);
    }
    if (delivery.leaseUntilMs == 0) {
        bindNull(binds[3]);
    } else {
        bindI64(binds[3], &leaseSecs);
    }
    if (delivery.acknowledgedAtMs == 0) {
        bindNull(binds[4]);
    } else {
        bindI64(binds[4], &ackedSecs);
    }
    if (delivery.expiresAtMs == 0) {
        bindNull(binds[5]);
    } else {
        bindI64(binds[5], &expiresSecs);
    }
    if (delivery.lastSentAtMs == 0) {
        bindNull(binds[6]);
    } else {
        bindI64(binds[6], &lastSentSecs);
    }
    if (delivery.nextAttemptAtMs == 0) {
        bindNull(binds[7]);
    } else {
        bindI64(binds[7], &nextAttemptSecs);
    }
    bindU64(binds[8], &pMid);
    bindInt(binds[9], &pRecipient);
    // DATETIME(0) 秒精度（任务卡登记：契约时间值均为秒的倍数，round-trip 精确；
    // P3-08 需要亚秒精度时另提 migration）。
    (void)execStmt(*mysql,
        "UPDATE MessageDelivery SET state=?, attempt_count=?, lease_owner=?, "
        "lease_until=FROM_UNIXTIME(?), acknowledged_at=FROM_UNIXTIME(?), "
        "expires_at=FROM_UNIXTIME(?), last_sent_at=FROM_UNIXTIME(?), "
        "next_attempt_at=FROM_UNIXTIME(?) WHERE message_id=? AND recipient_id=?",
        binds, 10);
}

std::shared_ptr<const Message> MySQLMessageStore::loadMessage(MySQL& m, const std::string& where,
                                                              MYSQL_BIND* params,
                                                              unsigned nParams)
{
    const std::string sql =
        "SELECT m.id, m.conversation_id, m.sender_id, m.sequence, m.client_message_id, "
        "m.content, o.payload FROM ChatMessage m JOIN OutboxEvent o "
        "ON o.aggregate_message_id = m.id AND o.event_type = 'MessageAccepted' WHERE " + where +
        " LIMIT 1";
    MYSQL_STMT* stmt = m.prepareStatement(sql.c_str());
    if (!stmt) {
        throwStoreError(mysql_errno(m.getConnection()), "prepare message load failed");
    }
    uint64_t id = 0;
    uint64_t conversationId = 0;
    uint64_t sequence = 0;
    int32_t senderId = 0;
    char cmidBuf[65] = {0};
    unsigned long cmidLen = 0;
    char contentProbe = 0;
    unsigned long contentLen = 0;
    char payloadProbe = 0;
    unsigned long payloadLen = 0;
    bool nulls[7] = {0, 0, 0, 0, 0, 0, 0};
    bool found = false;
    std::string content;
    std::string payload;
    {
        PreparedStatementGuard guard(stmt);
        if (params != nullptr && mysql_stmt_bind_param(stmt, params) != 0) {
            throwStoreError(mysql_stmt_errno(stmt), "bind message load params");
        }
        if (mysql_stmt_execute(stmt) != 0) {
            throwStoreError(mysql_stmt_errno(stmt), "execute message load");
        }
        MYSQL_BIND out[7];
        memset(out, 0, sizeof(out));
        out[0].buffer_type = MYSQL_TYPE_LONGLONG;
        out[0].is_unsigned = 1;
        out[0].buffer = &id;
        out[0].is_null = &nulls[0];
        out[1].buffer_type = MYSQL_TYPE_LONGLONG;
        out[1].is_unsigned = 1;
        out[1].buffer = &conversationId;
        out[1].is_null = &nulls[1];
        out[2].buffer_type = MYSQL_TYPE_LONG;
        out[2].buffer = &senderId;
        out[2].is_null = &nulls[2];
        out[3].buffer_type = MYSQL_TYPE_LONGLONG;
        out[3].is_unsigned = 1;
        out[3].buffer = &sequence;
        out[3].is_null = &nulls[3];
        out[4].buffer_type = MYSQL_TYPE_STRING;
        out[4].buffer = cmidBuf;
        out[4].buffer_length = sizeof(cmidBuf);
        out[4].length = &cmidLen;
        out[4].is_null = &nulls[4];
        out[5].buffer_type = MYSQL_TYPE_BLOB;
        out[5].buffer = &contentProbe;
        out[5].buffer_length = 1;
        out[5].length = &contentLen;
        out[5].is_null = &nulls[5];
        out[6].buffer_type = MYSQL_TYPE_BLOB;
        out[6].buffer = &payloadProbe;
        out[6].buffer_length = 1;
        out[6].length = &payloadLen;
        out[6].is_null = &nulls[6];
        if (mysql_stmt_bind_result(stmt, out) != 0 || mysql_stmt_store_result(stmt) != 0) {
            throwStoreError(mysql_stmt_errno(stmt),
                            "store message load result (" + std::to_string(mysql_stmt_errno(stmt)) + ")");
        }
        int rc = mysql_stmt_fetch(stmt);
        if (rc == 0 || rc == MYSQL_DATA_TRUNCATED) {
            found = true;
        } else if (rc != MYSQL_NO_DATA) {
            throwStoreError(mysql_stmt_errno(stmt), "fetch message load result");
        }
        if (found) {
            content = fetchBlob(stmt, 5, contentLen);
            payload = fetchBlob(stmt, 6, payloadLen);
        }
    }
    if (!found) {
        return std::shared_ptr<const Message>();
    }
    if (nulls[5] || nulls[6] || payload.empty()) {
        throw MessageStoreError(StoreErrorKind::Storage, "message without outbox snapshot");
    }
    std::shared_ptr<Message> outMsg(new Message());
    outMsg->id = MessageId{id};
    outMsg->conversationId = ConversationId{conversationId};
    outMsg->senderId = UserId{static_cast<uint64_t>(senderId)};
    outMsg->sequence = ConversationSequence{sequence};
    outMsg->command = decodeCommandSnapshot(payload, content, std::string(cmidBuf, cmidLen));
    return outMsg;
}

std::vector<Delivery> MySQLMessageStore::deliveriesWhere(MySQL& m, const std::string& where,
                                                         MYSQL_BIND* params, unsigned nParams)
{
    const std::string sql =
        "SELECT d.message_id, d.recipient_id, d.state, d.attempt_count, d.lease_owner, "
        "UNIX_TIMESTAMP(d.lease_until), UNIX_TIMESTAMP(d.acknowledged_at), "
        "UNIX_TIMESTAMP(d.expires_at), UNIX_TIMESTAMP(d.last_sent_at), "
        "UNIX_TIMESTAMP(d.next_attempt_at), m.conversation_id, m.sequence "
        "FROM MessageDelivery d JOIN ChatMessage m ON m.id = d.message_id WHERE " + where;
    MYSQL_STMT* stmt = m.prepareStatement(sql.c_str());
    if (!stmt) {
        throwStoreError(mysql_errno(m.getConnection()), "prepare deliveries load failed");
    }
    std::vector<Delivery> out;
    {
        PreparedStatementGuard guard(stmt);
        if (params != nullptr && mysql_stmt_bind_param(stmt, params) != 0) {
            throwStoreError(mysql_stmt_errno(stmt), "bind deliveries load params");
        }
        if (mysql_stmt_execute(stmt) != 0) {
            throwStoreError(mysql_stmt_errno(stmt), "execute deliveries load");
        }
        uint64_t messageId = 0;
        int32_t recipientId = 0;
        int32_t state = 0;
        int32_t attemptCount = 0;
        char ownerBuf[128] = {0};
        unsigned long ownerLen = 0;
        double leaseSecs = 0;
        double ackedSecs = 0;
        double expiresSecs = 0;
        double lastSentSecs = 0;
        double nextAttemptSecs = 0;
        uint64_t conversationId = 0;
        uint64_t sequence = 0;
        bool nulls[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        MYSQL_BIND bindOut[12];
        memset(bindOut, 0, sizeof(bindOut));
        bindOut[0].buffer_type = MYSQL_TYPE_LONGLONG;
        bindOut[0].is_unsigned = 1;
        bindOut[0].buffer = &messageId;
        bindOut[0].is_null = &nulls[0];
        bindOut[1].buffer_type = MYSQL_TYPE_LONG;
        bindOut[1].buffer = &recipientId;
        bindOut[1].is_null = &nulls[1];
        bindOut[2].buffer_type = MYSQL_TYPE_LONG;
        bindOut[2].buffer = &state;
        bindOut[2].is_null = &nulls[2];
        bindOut[3].buffer_type = MYSQL_TYPE_LONG;
        bindOut[3].buffer = &attemptCount;
        bindOut[3].is_null = &nulls[3];
        bindOut[4].buffer_type = MYSQL_TYPE_STRING;
        bindOut[4].buffer = ownerBuf;
        bindOut[4].buffer_length = sizeof(ownerBuf);
        bindOut[4].length = &ownerLen;
        bindOut[4].is_null = &nulls[4];
        bindOut[5].buffer_type = MYSQL_TYPE_DOUBLE;
        bindOut[5].buffer = &leaseSecs;
        bindOut[5].is_null = &nulls[5];
        bindOut[6].buffer_type = MYSQL_TYPE_DOUBLE;
        bindOut[6].buffer = &ackedSecs;
        bindOut[6].is_null = &nulls[6];
        bindOut[7].buffer_type = MYSQL_TYPE_DOUBLE;
        bindOut[7].buffer = &expiresSecs;
        bindOut[7].is_null = &nulls[7];
        bindOut[8].buffer_type = MYSQL_TYPE_DOUBLE;
        bindOut[8].buffer = &lastSentSecs;
        bindOut[8].is_null = &nulls[8];
        bindOut[9].buffer_type = MYSQL_TYPE_DOUBLE;
        bindOut[9].buffer = &nextAttemptSecs;
        bindOut[9].is_null = &nulls[9];
        bindOut[10].buffer_type = MYSQL_TYPE_LONGLONG;
        bindOut[10].is_unsigned = 1;
        bindOut[10].buffer = &conversationId;
        bindOut[10].is_null = &nulls[10];
        bindOut[11].buffer_type = MYSQL_TYPE_LONGLONG;
        bindOut[11].is_unsigned = 1;
        bindOut[11].buffer = &sequence;
        bindOut[11].is_null = &nulls[11];
        if (mysql_stmt_bind_result(stmt, bindOut) != 0 || mysql_stmt_store_result(stmt) != 0) {
            throwStoreError(mysql_stmt_errno(stmt), "store deliveries load result");
        }
        while (true) {
            int rc = mysql_stmt_fetch(stmt);
            if (rc == MYSQL_NO_DATA) {
                break;
            }
            if (rc != 0) {
                throwStoreError(mysql_stmt_errno(stmt), "fetch deliveries load result");
            }
            Delivery d;
            d.messageId = MessageId{messageId};
            d.recipient = UserId{static_cast<uint64_t>(recipientId)};
            d.conversationId = ConversationId{conversationId};
            d.sequence = ConversationSequence{sequence};
            d.state = decodeState(state);
            d.attemptCount = static_cast<uint32_t>(attemptCount);
            decodeLeaseOwner(ownerBuf, ownerLen, &d.leaseOwner, &d.leaseBootId);
            d.leaseUntilMs = nulls[5] ? 0 : secsToMs(leaseSecs);
            d.acknowledgedAtMs = nulls[6] ? 0 : secsToMs(ackedSecs);
            d.expiresAtMs = nulls[7] ? 0 : secsToMs(expiresSecs);
            d.lastSentAtMs = nulls[8] ? 0 : secsToMs(lastSentSecs);
            d.nextAttemptAtMs = nulls[9] ? 0 : secsToMs(nextAttemptSecs);
            out.push_back(d);
        }
    }
    return out;
}

std::vector<Delivery> MySQLMessageStore::deliveriesByRecipient(UserId recipient)
{
    finishPending();
    ConnectionPool::AcquireResult acq = pool_.acquire(kAcquireTimeoutMs);
    if (!acq.lease) {
        throw MessageStoreError(StoreErrorKind::DependencyBusy, "connection acquire failed");
    }
    int32_t pRecipient = static_cast<int32_t>(recipient.value);
    MYSQL_BIND binds[1];
    bindInt(binds[0], &pRecipient);
    return deliveriesWhere(*acq.lease.get(), "d.recipient_id=?", binds, 1);
}

std::vector<Delivery> MySQLMessageStore::deliveriesByMessage(MessageId messageId)
{
    finishPending();
    ConnectionPool::AcquireResult acq = pool_.acquire(kAcquireTimeoutMs);
    if (!acq.lease) {
        throw MessageStoreError(StoreErrorKind::DependencyBusy, "connection acquire failed");
    }
    uint64_t pMid = messageId.value;
    MYSQL_BIND binds[1];
    bindU64(binds[0], &pMid);
    return deliveriesWhere(*acq.lease.get(), "d.message_id=?", binds, 1);
}

std::shared_ptr<const Message> MySQLMessageStore::findMessage(MessageId messageId)
{
    finishPending();
    ConnectionPool::AcquireResult acq = pool_.acquire(kAcquireTimeoutMs);
    if (!acq.lease) {
        throw MessageStoreError(StoreErrorKind::DependencyBusy, "connection acquire failed");
    }
    uint64_t pMid = messageId.value;
    MYSQL_BIND binds[1];
    bindU64(binds[0], &pMid);
    return loadMessage(*acq.lease.get(), "m.id=?", binds, 1);
}

// 带参 UPDATE/DELETE（LIMIT 有界），返回 affected rows；失败抛映射后的
// MessageStoreError（与 execStmt 同风格，但需暴露影响行数）。
static uint64_t execAffected(MySQL& m, const char* sql, MYSQL_BIND* binds, unsigned nBinds)
{
    MYSQL_STMT* stmt = m.prepareStatement(sql);
    if (!stmt) {
        throwStoreError(mysql_errno(m.getConnection()), std::string("prepare failed: ") + sql);
    }
    uint64_t affected = 0;
    {
        PreparedStatementGuard guard(stmt);
        if (binds != nullptr && mysql_stmt_bind_param(stmt, binds) != 0) {
            throwStoreError(mysql_stmt_errno(stmt), std::string("bind failed: ") + sql);
        }
        if (mysql_stmt_execute(stmt) != 0) {
            throwStoreError(mysql_stmt_errno(stmt), std::string("stmt failed: ") + sql);
        }
        affected = static_cast<uint64_t>(mysql_stmt_affected_rows(stmt));
    }
    return affected;
}

std::vector<Delivery> MySQLMessageStore::deliveriesDueForRetry(int64_t nowMs, uint64_t limit)
{
    finishPending();
    ConnectionPool::AcquireResult acq = pool_.acquire(kAcquireTimeoutMs);
    if (!acq.lease) {
        throw MessageStoreError(StoreErrorKind::DependencyBusy, "connection acquire failed");
    }
    // InFlight(1) 且 next_attempt_at <= now：一次有界查询（LIMIT）返回全部到期
    // 重投候选，避免按活动会话逐人扫描（无界）。列顺序与 deliveriesWhere 一致。
    int64_t nowSecs = nowMs / 1000;
    uint64_t pLimit = limit;
    MYSQL_BIND binds[2];
    bindI64(binds[0], &nowSecs);
    bindU64(binds[1], &pLimit);
    return deliveriesWhere(*acq.lease.get(),
        "d.state=1 AND d.next_attempt_at IS NOT NULL AND d.next_attempt_at <= "
        "FROM_UNIXTIME(?) LIMIT ?",
        binds, 2);
}

// F1：DATETIME(0) 秒精度持久化（M2 ceil 写 + due 判定 floor(now_sec)>=ceil）。
// scheduler 唤醒对齐到秒边界，避免提前 1..999ms 空转（回退 ackTimeoutMs 轮询）。
uint32_t MySQLMessageStore::timeGranularityMs()
{
    return 1000;
}

uint32_t MySQLMessageStore::expireDeliveries(int64_t nowMs, uint64_t limit)
{
    finishPending();
    ConnectionPool::AcquireResult acq = pool_.acquire(kAcquireTimeoutMs);
    if (!acq.lease) {
        throw MessageStoreError(StoreErrorKind::DependencyBusy, "connection acquire failed");
    }
    MySQL* mysql = acq.lease.get();
    int64_t nowSecs = nowMs / 1000;
    uint64_t pLimit = limit;
    MYSQL_BIND binds[2];
    bindI64(binds[0], &nowSecs);
    bindU64(binds[1], &pLimit);
    // 未确认（Pending=0/InFlight=1）且已过 expires_at → state=3（Expired），
    // 释放 lease/重投排程；不删除（spec §3 Expired 可查询，audited cleanup 清理）。
    return static_cast<uint32_t>(execAffected(*mysql,
        "UPDATE MessageDelivery SET state=3, lease_owner=NULL, lease_until=NULL, "
        "next_attempt_at=NULL, last_sent_at=NULL "
        "WHERE state IN (0,1) AND expires_at IS NOT NULL AND expires_at < FROM_UNIXTIME(?) "
        "LIMIT ?",
        binds, 2));
}

uint32_t MySQLMessageStore::cleanupDeliveries(int64_t ackedBeforeMs, int64_t expiredBeforeMs,
                                              uint64_t limit)
{
    finishPending();
    ConnectionPool::AcquireResult acq = pool_.acquire(kAcquireTimeoutMs);
    if (!acq.lease) {
        throw MessageStoreError(StoreErrorKind::DependencyBusy, "connection acquire failed");
    }
    MySQL* mysql = acq.lease.get();
    uint32_t removed = 0;
    // acked_retention：Acknowledged(2) 且 acknowledged_at < ceil(截止/1000)。
    // 持久化 acknowledged_at = floor(ackMs/1000)；旧 `<= floor(cutoff/1000)` 会把
    // ackMs∈(floor(cutoff/1000)*1000, cutoff] 的行（秒截断后等于 floor 值）提前
    // 至多 999ms 删除（不安全方向）。改 `< ceil`：绝不早删，至多晚 <1s。
    {
        int64_t ackedSecs = (ackedBeforeMs + 999) / 1000;
        uint64_t pLimit = limit;
        MYSQL_BIND binds[2];
        bindI64(binds[0], &ackedSecs);
        bindU64(binds[1], &pLimit);
        removed += static_cast<uint32_t>(execAffected(*mysql,
            "DELETE FROM MessageDelivery WHERE state=2 AND acknowledged_at IS NOT NULL "
            "AND acknowledged_at < FROM_UNIXTIME(?) LIMIT ?",
            binds, 2));
    }
    // expired_retention：Expired(3) 且 expires_at < ceil(截止/1000)。与 acked 分支
    // 同理（旧 `<= floor` 早删至多 999ms）：改 `< ceil`，绝不早删，至多晚 <1s。
    {
        int64_t expiredSecs = (expiredBeforeMs + 999) / 1000;
        uint64_t pLimit = limit;
        MYSQL_BIND binds[2];
        bindI64(binds[0], &expiredSecs);
        bindU64(binds[1], &pLimit);
        removed += static_cast<uint32_t>(execAffected(*mysql,
            "DELETE FROM MessageDelivery WHERE state=3 AND expires_at IS NOT NULL "
            "AND expires_at < FROM_UNIXTIME(?) LIMIT ?",
            binds, 2));
    }
    return removed;
}
