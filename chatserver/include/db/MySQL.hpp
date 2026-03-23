#pragma once

#include <mysql/mysql.h>
#include <string>
#include <vector>
#include <memory>

class MySQL {
public:
    MySQL();
    ~MySQL();

    bool connect(const std::string& host, const std::string& user, 
                 const std::string& password, const std::string& dbname, 
                 unsigned int port = 3306);
    bool update(const std::string& sql);
    MYSQL_RES* query(const std::string& sql);
    MYSQL* getConnection() { return conn_; }

private:
    MYSQL* conn_;
};
