#include "db/MySQL.hpp"
#include <iostream>

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
