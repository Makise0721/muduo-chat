#include "app/MySQLUserRepository.hpp"
#include "app/InMemoryUserRepository.hpp"
#include "db/MySQLGuards.hpp"

#include <cstring>
#include <mysql/mysql.h>

namespace {

UserError mapPoolError(ConnectionPool::PoolError err)
{
    switch (err) {
        case ConnectionPool::PoolError::Timeout:
            return UserError::Timeout;
        case ConnectionPool::PoolError::Shutdown:
        case ConnectionPool::PoolError::ConnectionFailed:
        default:
            return UserError::Disconnected;
    }
}

} // namespace

UserError mapMySqlError(unsigned int err)
{
    switch (err) {
        case 1062:  // ER_DUP_ENTRY
            return UserError::NameExists;
        case 2006:  // CR_SERVER_GONE_ERROR
        case 2016:  // CR_SERVER_LOST
        case 1053:  // ER_SERVER_SHUTDOWN
        case 2003:  // CR_CONN_HOST_ERROR
            return UserError::Disconnected;
        case 1205:  // ER_LOCK_WAIT_TIMEOUT
            return UserError::Timeout;
        case 1406:  // ER_DATA_TOO_LONG
        case 1366:  // ER_TRUNCATED_WRONG_VALUE_FOR_FIELD
        case 1300:  // ER_INVALID_CHARACTER_STRING
            return UserError::StorageFailure;
        default:
            return UserError::StorageFailure;
    }
}

MySQLUserRepository::MySQLUserRepository(ConnectionPool& pool)
    : pool_(pool)
{
}

CreateUserResult MySQLUserRepository::create(const std::string& name, const std::string& password)
{
    CreateUserResult result;
    if (!isRepositoryInputValid(name, password)) {
        result.error = UserError::InvalidInput;
        return result;
    }

    ConnectionPool::AcquireResult acq = pool_.acquire(5000);
    if (!acq.lease) {
        result.error = mapPoolError(acq.error);
        return result;
    }
    MySQL* mysql = acq.lease.get();
    PreparedStatementGuard stmt(
        mysql->prepareStatement("INSERT INTO User(name, password) VALUES(?, ?)"));
    if (!stmt.stmt) {
        result.error = mapMySqlError(mysql_errno(mysql->getConnection()));
        return result;
    }

    unsigned long nameLen = name.size();
    unsigned long pwdLen = password.size();
    MYSQL_BIND binds[2];
    memset(binds, 0, sizeof(binds));
    binds[0].buffer_type = MYSQL_TYPE_STRING;
    binds[0].buffer = const_cast<char*>(name.data());
    binds[0].buffer_length = nameLen;
    binds[0].length = &nameLen;
    binds[1].buffer_type = MYSQL_TYPE_STRING;
    binds[1].buffer = const_cast<char*>(password.data());
    binds[1].buffer_length = pwdLen;
    binds[1].length = &pwdLen;

    if (mysql_stmt_bind_param(stmt.stmt, binds) != 0 ||
        mysql_stmt_execute(stmt.stmt) != 0) {
        result.error = mapMySqlError(mysql_stmt_errno(stmt.stmt));
        return result;
    }

    result.ok = true;
    result.id = mysql_insert_id(mysql->getConnection());
    return result;
}

AuthResult MySQLUserRepository::authenticate(int64_t id, const std::string& password)
{
    AuthResult result;
    ConnectionPool::AcquireResult acq = pool_.acquire(5000);
    if (!acq.lease) {
        result.error = mapPoolError(acq.error);
        return result;
    }
    MySQL* mysql = acq.lease.get();
    PreparedStatementGuard stmt(
        mysql->prepareStatement("SELECT id, name, state FROM User WHERE id = ? AND password = ?"));
    if (!stmt.stmt) {
        result.error = mapMySqlError(mysql_errno(mysql->getConnection()));
        return result;
    }

    unsigned long pwdLen = password.size();
    MYSQL_BIND params[2];
    memset(params, 0, sizeof(params));
    params[0].buffer_type = MYSQL_TYPE_LONGLONG;
    long long idValue = id;
    params[0].buffer = &idValue;
    params[1].buffer_type = MYSQL_TYPE_STRING;
    params[1].buffer = const_cast<char*>(password.data());
    params[1].buffer_length = pwdLen;
    params[1].length = &pwdLen;
    if (mysql_stmt_bind_param(stmt.stmt, params) != 0 ||
        mysql_stmt_execute(stmt.stmt) != 0) {
        result.error = mapMySqlError(mysql_stmt_errno(stmt.stmt));
        return result;
    }

    long long outId = 0;
    char nameBuf[64] = {0};
    unsigned long nameLen = 0;
    char stateBuf[16] = {0};
    unsigned long stateLen = 0;
    bool isNull[3] = {0};
    MYSQL_BIND out[3];
    memset(out, 0, sizeof(out));
    out[0].buffer_type = MYSQL_TYPE_LONGLONG;
    out[0].buffer = &outId;
    out[0].is_null = &isNull[0];
    out[1].buffer_type = MYSQL_TYPE_STRING;
    out[1].buffer = nameBuf;
    out[1].buffer_length = sizeof(nameBuf);
    out[1].length = &nameLen;
    out[1].is_null = &isNull[1];
    out[2].buffer_type = MYSQL_TYPE_STRING;
    out[2].buffer = stateBuf;
    out[2].buffer_length = sizeof(stateBuf);
    out[2].length = &stateLen;
    out[2].is_null = &isNull[2];
    if (mysql_stmt_bind_result(stmt.stmt, out) != 0 ||
        mysql_stmt_store_result(stmt.stmt) != 0 ||
        mysql_stmt_fetch(stmt.stmt) != 0) {
        result.error = mapMySqlError(mysql_stmt_errno(stmt.stmt));
        return result;
    }

    result.ok = true;
    result.id = static_cast<int64_t>(outId);
    result.name.assign(nameBuf, nameLen);
    result.state = (stateLen > 0 && std::string(stateBuf, stateLen) == "online")
                       ? UserState::Online
                       : UserState::Offline;
    return result;
}

bool MySQLUserRepository::updateState(int64_t id, UserState state)
{
    ConnectionPool::AcquireResult acq = pool_.acquire(5000);
    if (!acq.lease) {
        return false;
    }
    MySQL* mysql = acq.lease.get();
    PreparedStatementGuard stmt(
        mysql->prepareStatement("UPDATE User SET state = ? WHERE id = ?"));
    if (!stmt.stmt) {
        return false;
    }
    std::string stateStr = state == UserState::Online ? "online" : "offline";
    unsigned long stateLen = stateStr.size();
    long long idValue = id;
    MYSQL_BIND params[2];
    memset(params, 0, sizeof(params));
    params[0].buffer_type = MYSQL_TYPE_STRING;
    params[0].buffer = const_cast<char*>(stateStr.data());
    params[0].buffer_length = stateLen;
    params[0].length = &stateLen;
    params[1].buffer_type = MYSQL_TYPE_LONGLONG;
    params[1].buffer = &idValue;
    return mysql_stmt_bind_param(stmt.stmt, params) == 0 &&
           mysql_stmt_execute(stmt.stmt) == 0;
}
