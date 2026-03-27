#include "db/MySQL.hpp"
#include <iostream>
#include <cstring>

MySQL::MySQL() {
    conn_ = mysql_init(nullptr);
    if (conn_ == nullptr) {
        std::cerr << "MySQL init failed" << std::endl;
    }
}

MySQL::~MySQL() {
    if (conn_ != nullptr) {
        mysql_close(conn_);
    }
}

bool MySQL::connect(const std::string& host, const std::string& user, 
                    const std::string& password, const std::string& dbname, 
                    unsigned int port) {
    MYSQL* p = mysql_real_connect(conn_, host.c_str(), user.c_str(), 
                                   password.c_str(), dbname.c_str(), 
                                   port, nullptr, 0);
    if (p == nullptr) {
        std::cerr << "MySQL connect failed: " << mysql_error(conn_) << std::endl;
        return false;
    }
    mysql_set_character_set(conn_, "utf8");
    return true;
}

bool MySQL::update(const std::string& sql) {
    if (mysql_query(conn_, sql.c_str())) {
        std::cerr << "MySQL update failed: " << mysql_error(conn_) << std::endl;
        return false;
    }
    return true;
}

MYSQL_RES* MySQL::query(const std::string& sql) {
    if (mysql_query(conn_, sql.c_str())) {
        std::cerr << "MySQL query failed: " << mysql_error(conn_) << std::endl;
        return nullptr;
    }
    return mysql_store_result(conn_);
}

MYSQL_STMT* MySQL::prepareStatement(const std::string& sql) {
    MYSQL_STMT* stmt = mysql_stmt_init(conn_);
    if (!stmt) {
        std::cerr << "mysql_stmt_init failed: " << mysql_error(conn_) << std::endl;
        return nullptr;
    }
    if (mysql_stmt_prepare(stmt, sql.c_str(), sql.length())) {
        std::cerr << "mysql_stmt_prepare failed: " << mysql_stmt_error(stmt) << std::endl;
        mysql_stmt_close(stmt);
        return nullptr;
    }
    return stmt;
}

bool MySQL::bindParam(MYSQL_STMT* stmt, int paramIndex, const std::string& value) {
    MYSQL_BIND bind;
    memset(&bind, 0, sizeof(bind));
    bind.buffer_type = MYSQL_TYPE_STRING;
    bind.buffer = const_cast<char*>(value.c_str());
    bind.buffer_length = value.length();
    bind.length = &bind.buffer_length;
    return mysql_stmt_bind_param(stmt, &bind) == 0;
}

bool MySQL::bindParam(MYSQL_STMT* stmt, int paramIndex, int value) {
    MYSQL_BIND bind;
    memset(&bind, 0, sizeof(bind));
    bind.buffer_type = MYSQL_TYPE_LONG;
    bind.buffer = &value;
    bind.buffer_length = sizeof(value);
    return mysql_stmt_bind_param(stmt, &bind) == 0;
}

bool MySQL::execute(MYSQL_STMT* stmt) {
    return mysql_stmt_execute(stmt) == 0;
}

MYSQL_RES* MySQL::getResult(MYSQL_STMT* stmt) {
    if (mysql_stmt_store_result(stmt)) {
        return nullptr;
    }
    return mysql_stmt_result_metadata(stmt);
}

void MySQL::closeStatement(MYSQL_STMT* stmt) {
    if (stmt) {
        mysql_stmt_close(stmt);
    }
}
