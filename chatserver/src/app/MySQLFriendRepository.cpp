#include "app/MySQLFriendRepository.hpp"

#include "db/MySQLGuards.hpp"

#include <cstring>
#include <mysql/mysql.h>

namespace {

FriendError mapMySqlFriendError(unsigned int err)
{
    switch (err) {
        case 1062:  // ER_DUP_ENTRY
            return FriendError::Duplicate;
        case 1452:  // ER_NO_REFERENCED_ROW_2（FK 目标不存在）
            return FriendError::TargetNotFound;
        case 2006:  // CR_SERVER_GONE_ERROR
        case 2016:  // CR_SERVER_LOST
        case 1053:  // ER_SERVER_SHUTDOWN
        case 2003:  // CR_CONN_HOST_ERROR
            return FriendError::Disconnected;
        case 1205:  // ER_LOCK_WAIT_TIMEOUT
            return FriendError::Timeout;
        default:
            return FriendError::StorageFailure;
    }
}

} // namespace

FriendError mapFriendError(unsigned int err)
{
    return mapMySqlFriendError(err);
}

MySQLFriendRepository::MySQLFriendRepository(ConnectionPool& pool)
    : pool_(pool)
{
}

AddFriendResult MySQLFriendRepository::add(int64_t userId, int64_t friendId)
{
    AddFriendResult result;
    ConnectionPool::AcquireResult acq = pool_.acquire(5000);
    if (!acq.lease) {
        switch (acq.error) {
            case ConnectionPool::PoolError::Timeout:
                result.error = FriendError::Timeout;
                break;
            case ConnectionPool::PoolError::Shutdown:
            case ConnectionPool::PoolError::ConnectionFailed:
            default:
                result.error = FriendError::Disconnected;
                break;
        }
        return result;
    }
    MySQL* mysql = acq.lease.get();
    PreparedStatementGuard stmt(
        mysql->prepareStatement("INSERT INTO Friend(userid, friendid) VALUES(?, ?)"));
    if (!stmt.stmt) {
        result.error = mapFriendError(mysql_errno(mysql->getConnection()));
        return result;
    }
    long long uid = userId;
    long long fid = friendId;
    MYSQL_BIND params[2];
    memset(params, 0, sizeof(params));
    params[0].buffer_type = MYSQL_TYPE_LONGLONG;
    params[0].buffer = &uid;
    params[1].buffer_type = MYSQL_TYPE_LONGLONG;
    params[1].buffer = &fid;
    if (mysql_stmt_bind_param(stmt.stmt, params) != 0 ||
        mysql_stmt_execute(stmt.stmt) != 0) {
        result.error = mapFriendError(mysql_stmt_errno(stmt.stmt));
        return result;
    }
    result.ok = true;
    return result;
}
