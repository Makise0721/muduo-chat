#include "LegacyUserRepository.hpp"

#include "db/MySQLGuards.hpp"

#include <cstdio>

CreateUserResult LegacyUserRepository::create(const std::string& name, const std::string& password)
{
    auto& connPool = ConnectionPool::getInstance();
    MySQLConnectionGuard mysql(connPool, connPool.getConnection());

    std::string escapedName = escapeString(mysql->getConnection(), name);
    char sql[1024] = {0};
    snprintf(sql, sizeof(sql), "SELECT id FROM User WHERE name = '%s'", escapedName.c_str());

    MYSQL_RES* resName = mysql->query(sql);
    if (resName != nullptr) {
        MySQLResultGuard guardName(resName);
        MYSQL_ROW row = mysql_fetch_row(resName);
        if (row != nullptr) {
            CreateUserResult result;
            result.error = UserError::NameExists;
            return result;
        }
    }

    std::string escapedPwd = escapeString(mysql->getConnection(), password);
    snprintf(sql, sizeof(sql), "INSERT INTO User(name, password) VALUES('%s', '%s')",
             escapedName.c_str(), escapedPwd.c_str());

    CreateUserResult result;
    if (mysql->update(sql)) {
        result.ok = true;
        result.id = mysql_insert_id(mysql->getConnection());
    }
    return result;
}
