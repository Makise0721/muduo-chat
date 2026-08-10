#pragma once

#include "db/ConnectionPool.hpp"
#include "db/MySQL.hpp"

#include <memory>
#include <string>
#include <vector>

// 确保从 ConnectionPool 取出的连接一定会归还，避免连接池耗尽导致阻塞。
struct MySQLConnectionGuard {
    ConnectionPool& pool;
    std::shared_ptr<MySQL> mysql;

    MySQLConnectionGuard(ConnectionPool& p, std::shared_ptr<MySQL> c)
        : pool(p), mysql(std::move(c)) {}

    ~MySQLConnectionGuard() {
        pool.releaseConnection(mysql);
    }

    MySQL* operator->() { return mysql.get(); }
    const MySQL* operator->() const { return mysql.get(); }
};

// 确保 mysql_use_result() 产生的结果集会释放。
struct MySQLResultGuard {
    MYSQL_RES* res;
    explicit MySQLResultGuard(MYSQL_RES* r) : res(r) {}
    ~MySQLResultGuard() {
        if (res) {
            mysql_free_result(res);
        }
    }
};

// 转义字符串用于 SQL 查询
inline std::string escapeString(MYSQL* mysql, const std::string& str) {
    if (!mysql || str.empty()) return str;
    std::vector<char> escaped(str.length() * 2 + 1);
    unsigned long len = mysql_real_escape_string(mysql, escaped.data(), str.c_str(), str.length());
    return std::string(escaped.data(), len);
}

// 预处理语句守护类
struct PreparedStatementGuard {
    MYSQL_STMT* stmt;
    explicit PreparedStatementGuard(MYSQL_STMT* s) : stmt(s) {}
    ~PreparedStatementGuard() {
        if (stmt) {
            mysql_stmt_close(stmt);
        }
    }
    MYSQL_STMT* operator->() { return stmt; }
};
