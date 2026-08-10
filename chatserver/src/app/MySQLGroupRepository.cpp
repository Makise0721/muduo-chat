#include "app/MySQLGroupRepository.hpp"

#include "db/MySQLGuards.hpp"

#include <cstring>
#include <mysql/mysql.h>

namespace {

GroupError mapMySqlGroupError(unsigned int err)
{
    switch (err) {
        case 1062:  // ER_DUP_ENTRY
            return GroupError::Duplicate;
        case 1452:  // ER_NO_REFERENCED_ROW_2（FK 目标不存在）
            return GroupError::TargetNotFound;
        case 2006:  // CR_SERVER_GONE_ERROR
        case 2016:  // CR_SERVER_LOST
        case 1053:  // ER_SERVER_SHUTDOWN
        case 2003:  // CR_CONN_HOST_ERROR
            return GroupError::Disconnected;
        case 1205:  // ER_LOCK_WAIT_TIMEOUT
            return GroupError::Timeout;
        default:
            return GroupError::StorageFailure;
    }
}

} // namespace

GroupError mapGroupError(unsigned int err)
{
    return mapMySqlGroupError(err);
}

MySQLGroupRepository::MySQLGroupRepository(ConnectionPool& pool)
    : pool_(pool)
{
}

CreateGroupResult MySQLGroupRepository::create(int64_t ownerId, const std::string& name,
                                               const std::string& desc)
{
    CreateGroupResult result;
    ConnectionPool::AcquireResult acq = pool_.acquire(5000);
    if (!acq.lease) {
        switch (acq.error) {
            case ConnectionPool::PoolError::Timeout:
                result.error = GroupError::Timeout;
                break;
            default:
                result.error = GroupError::Disconnected;
                break;
        }
        return result;
    }
    MySQL* mysql = acq.lease.get();

    // 事务边界在 adapter 内：群表与 creator 成员原子提交。
    if (!mysql->update("START TRANSACTION")) {
        result.error = mapGroupError(mysql_errno(mysql->getConnection()));
        return result;
    }

    auto rollback = [mysql] { mysql->update("ROLLBACK"); };

    PreparedStatementGuard stmtGroup(
        mysql->prepareStatement("INSERT INTO AllGroup(groupname, groupdesc) VALUES(?, ?)"));
    if (!stmtGroup.stmt) {
        rollback();
        result.error = mapGroupError(mysql_errno(mysql->getConnection()));
        return result;
    }
    unsigned long nameLen = name.size();
    unsigned long descLen = desc.size();
    MYSQL_BIND binds[2];
    memset(binds, 0, sizeof(binds));
    binds[0].buffer_type = MYSQL_TYPE_STRING;
    binds[0].buffer = const_cast<char*>(name.data());
    binds[0].buffer_length = nameLen;
    binds[0].length = &nameLen;
    binds[1].buffer_type = MYSQL_TYPE_STRING;
    binds[1].buffer = const_cast<char*>(desc.data());
    binds[1].buffer_length = descLen;
    binds[1].length = &descLen;
    if (mysql_stmt_bind_param(stmtGroup.stmt, binds) != 0 ||
        mysql_stmt_execute(stmtGroup.stmt) != 0) {
        rollback();
        result.error = mapGroupError(mysql_stmt_errno(stmtGroup.stmt));
        return result;
    }
    int64_t groupId = mysql_insert_id(mysql->getConnection());

    PreparedStatementGuard stmtMember(
        mysql->prepareStatement("INSERT INTO GroupUser(groupid, userid, grouprole) VALUES(?, ?, 'creator')"));
    if (!stmtMember.stmt) {
        rollback();
        result.error = mapGroupError(mysql_errno(mysql->getConnection()));
        return result;
    }
    long long gid = groupId;
    long long uid = ownerId;
    MYSQL_BIND memberBinds[2];
    memset(memberBinds, 0, sizeof(memberBinds));
    memberBinds[0].buffer_type = MYSQL_TYPE_LONGLONG;
    memberBinds[0].buffer = &gid;
    memberBinds[1].buffer_type = MYSQL_TYPE_LONGLONG;
    memberBinds[1].buffer = &uid;
    if (mysql_stmt_bind_param(stmtMember.stmt, memberBinds) != 0 ||
        mysql_stmt_execute(stmtMember.stmt) != 0) {
        rollback();
        result.error = mapGroupError(mysql_stmt_errno(stmtMember.stmt));
        return result;
    }

    if (!mysql->update("COMMIT")) {
        rollback();
        result.error = mapGroupError(mysql_errno(mysql->getConnection()));
        return result;
    }
    result.ok = true;
    result.groupId = groupId;
    return result;
}

JoinGroupResult MySQLGroupRepository::join(int64_t groupId, int64_t userId)
{
    JoinGroupResult result;
    ConnectionPool::AcquireResult acq = pool_.acquire(5000);
    if (!acq.lease) {
        switch (acq.error) {
            case ConnectionPool::PoolError::Timeout:
                result.error = GroupError::Timeout;
                break;
            default:
                result.error = GroupError::Disconnected;
                break;
        }
        return result;
    }
    MySQL* mysql = acq.lease.get();
    PreparedStatementGuard stmt(
        mysql->prepareStatement("INSERT INTO GroupUser(groupid, userid, grouprole) VALUES(?, ?, 'normal')"));
    if (!stmt.stmt) {
        result.error = mapGroupError(mysql_errno(mysql->getConnection()));
        return result;
    }
    long long gid = groupId;
    long long uid = userId;
    MYSQL_BIND binds[2];
    memset(binds, 0, sizeof(binds));
    binds[0].buffer_type = MYSQL_TYPE_LONGLONG;
    binds[0].buffer = &gid;
    binds[1].buffer_type = MYSQL_TYPE_LONGLONG;
    binds[1].buffer = &uid;
    if (mysql_stmt_bind_param(stmt.stmt, binds) != 0 ||
        mysql_stmt_execute(stmt.stmt) != 0) {
        result.error = mapGroupError(mysql_stmt_errno(stmt.stmt));
        return result;
    }
    result.ok = true;
    return result;
}

MembersResult MySQLGroupRepository::members(int64_t groupId)
{
    MembersResult result;
    ConnectionPool::AcquireResult acq = pool_.acquire(5000);
    if (!acq.lease) {
        switch (acq.error) {
            case ConnectionPool::PoolError::Timeout:
                result.error = GroupError::Timeout;
                break;
            default:
                result.error = GroupError::Disconnected;
                break;
        }
        return result;
    }
    MySQL* mysql = acq.lease.get();
    PreparedStatementGuard stmt(
        mysql->prepareStatement("SELECT userid FROM GroupUser WHERE groupid = ?"));
    if (!stmt.stmt) {
        result.error = mapGroupError(mysql_errno(mysql->getConnection()));
        return result;
    }
    long long gid = groupId;
    MYSQL_BIND param;
    memset(&param, 0, sizeof(param));
    param.buffer_type = MYSQL_TYPE_LONGLONG;
    param.buffer = &gid;
    if (mysql_stmt_bind_param(stmt.stmt, &param) != 0 ||
        mysql_stmt_execute(stmt.stmt) != 0) {
        result.error = mapGroupError(mysql_stmt_errno(stmt.stmt));
        return result;
    }

    long long outUid = 0;
    bool isNull = false;
    MYSQL_BIND out;
    memset(&out, 0, sizeof(out));
    out.buffer_type = MYSQL_TYPE_LONGLONG;
    out.buffer = &outUid;
    out.is_null = &isNull;
    if (mysql_stmt_bind_result(stmt.stmt, &out) != 0 ||
        mysql_stmt_store_result(stmt.stmt) != 0) {
        result.error = mapGroupError(mysql_stmt_errno(stmt.stmt));
        return result;
    }
    int fetch;
    while ((fetch = mysql_stmt_fetch(stmt.stmt)) == 0) {
        result.userIds.push_back(static_cast<int64_t>(outUid));
    }
    if (fetch != MYSQL_NO_DATA) {
        result.error = mapGroupError(mysql_stmt_errno(stmt.stmt));
        return result;
    }
    result.ok = true;
    return result;
}
