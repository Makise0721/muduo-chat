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

    // Prepared statement support
    MYSQL_STMT* prepareStatement(const std::string& sql);
    bool bindParam(MYSQL_STMT* stmt, int paramIndex, const std::string& value);
    bool bindParam(MYSQL_STMT* stmt, int paramIndex, int value);
    bool execute(MYSQL_STMT* stmt);
    MYSQL_RES* getResult(MYSQL_STMT* stmt);
    void closeStatement(MYSQL_STMT* stmt);

private:
    MYSQL* conn_;
};
