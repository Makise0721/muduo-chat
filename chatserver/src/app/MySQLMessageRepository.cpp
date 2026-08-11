#include "app/MySQLMessageRepository.hpp"

#include "db/MySQLGuards.hpp"

#include <cstring>
#include <mysql/mysql.h>
#include <vector>

namespace {

MessageError mapMySqlMessageError(unsigned int err)
{
    switch (err) {
        case 2006:  // CR_SERVER_GONE_ERROR
        case 2016:  // CR_SERVER_LOST
        case 1053:  // ER_SERVER_SHUTDOWN
        case 2003:  // CR_CONN_HOST_ERROR
            return MessageError::Disconnected;
        case 1205:  // ER_LOCK_WAIT_TIMEOUT
            return MessageError::Timeout;
        default:
            return MessageError::StorageFailure;
    }
}

} // namespace

MySQLMessageRepository::MySQLMessageRepository(ConnectionPool& pool)
    : pool_(pool)
{
}

StoreResult MySQLMessageRepository::storeOffline(int64_t userId, const std::string& payload)
{
    StoreResult result;
    ConnectionPool::AcquireResult acq = pool_.acquire(5000);
    if (!acq.lease) {
        switch (acq.error) {
            case ConnectionPool::PoolError::Timeout:
                result.error = MessageError::Timeout;
                break;
            default:
                result.error = MessageError::Disconnected;
                break;
        }
        return result;
    }
    MySQL* mysql = acq.lease.get();
    PreparedStatementGuard stmt(
        mysql->prepareStatement("INSERT INTO OfflineMessage(userid, message) VALUES(?, ?)"));
    if (!stmt.stmt) {
        result.error = mapMySqlMessageError(mysql_errno(mysql->getConnection()));
        return result;
    }
    long long uid = userId;
    unsigned long payloadLen = payload.size();
    MYSQL_BIND params[2];
    memset(params, 0, sizeof(params));
    params[0].buffer_type = MYSQL_TYPE_LONGLONG;
    params[0].buffer = &uid;
    params[1].buffer_type = MYSQL_TYPE_STRING;
    params[1].buffer = const_cast<char*>(payload.data());
    params[1].buffer_length = payloadLen;
    params[1].length = &payloadLen;
    if (mysql_stmt_bind_param(stmt.stmt, params) != 0 ||
        mysql_stmt_execute(stmt.stmt) != 0) {
        result.error = mapMySqlMessageError(mysql_stmt_errno(stmt.stmt));
        return result;
    }
    result.ok = true;
    return result;
}

std::vector<OfflineMessage> MySQLMessageRepository::takeOffline(int64_t userId)
{
    std::vector<OfflineMessage> taken;
    ConnectionPool::AcquireResult acq = pool_.acquire(5000);
    if (!acq.lease) {
        return taken;
    }
    MySQL* mysql = acq.lease.get();
    PreparedStatementGuard stmt(
        mysql->prepareStatement(
            "SELECT id, message FROM OfflineMessage WHERE userid = ? ORDER BY id"));
    if (!stmt.stmt) {
        return taken;
    }
    long long uid = userId;
    MYSQL_BIND param;
    memset(&param, 0, sizeof(param));
    param.buffer_type = MYSQL_TYPE_LONGLONG;
    param.buffer = &uid;
    if (mysql_stmt_bind_param(stmt.stmt, &param) != 0 ||
        mysql_stmt_execute(stmt.stmt) != 0) {
        return taken;
    }

    long long outId = 0;
    std::vector<char> payloadBuf(1024, 0);
    unsigned long payloadLen = 0;
    bool isNull[2] = {0};
    MYSQL_BIND out[2];
    memset(out, 0, sizeof(out));
    out[0].buffer_type = MYSQL_TYPE_LONGLONG;
    out[0].buffer = &outId;
    out[0].is_null = &isNull[0];
    out[1].buffer_type = MYSQL_TYPE_STRING;
    out[1].buffer = &payloadBuf[0];
    out[1].buffer_length = payloadBuf.size();
    out[1].length = &payloadLen;
    out[1].is_null = &isNull[1];
    if (mysql_stmt_bind_result(stmt.stmt, out) != 0 ||
        mysql_stmt_store_result(stmt.stmt) != 0) {
        return taken;
    }
    int fetch;
    while ((fetch = mysql_stmt_fetch(stmt.stmt)) == 0 || fetch == MYSQL_DATA_TRUNCATED) {
        if (fetch == MYSQL_DATA_TRUNCATED) {
            // 行数据超出现有缓冲（如多字节字符超 1024 字节）：按真实长度扩容重取该列。
            payloadBuf.resize(payloadLen + 1);
            out[1].buffer = &payloadBuf[0];
            out[1].buffer_length = payloadBuf.size();
            if (mysql_stmt_fetch_column(stmt.stmt, &out[1], 1, 0) != 0) {
                return taken;
            }
        }
        OfflineMessage m;
        m.id = static_cast<int64_t>(outId);
        m.userId = userId;
        m.payload.assign(payloadBuf.data(), payloadLen);
        taken.push_back(std::move(m));
    }
    if (fetch != MYSQL_NO_DATA) {
        return taken;  // 读失败：不删队列（至少一次语义）
    }

    // 清空队列（与现状补投语义一致：读走后删除）。
    PreparedStatementGuard del(
        mysql->prepareStatement("DELETE FROM OfflineMessage WHERE userid = ?"));
    if (!del.stmt) {
        return taken;
    }
    MYSQL_BIND delParam;
    memset(&delParam, 0, sizeof(delParam));
    delParam.buffer_type = MYSQL_TYPE_LONGLONG;
    delParam.buffer = &uid;
    if (mysql_stmt_bind_param(del.stmt, &delParam) == 0) {
        mysql_stmt_execute(del.stmt);
    }
    return taken;
}
