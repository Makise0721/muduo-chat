#pragma once

#include "db/MySQL.hpp"
#include <string>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <memory>

class ConnectionPool {
public:
    static ConnectionPool& getInstance();
    std::shared_ptr<MySQL> getConnection();
    void releaseConnection(std::shared_ptr<MySQL> conn);
    void init(const std::string& host, const std::string& user, 
              const std::string& password, const std::string& dbname, 
              unsigned int port, int poolSize);
    ~ConnectionPool();

private:
    ConnectionPool();
    ConnectionPool(const ConnectionPool&) = delete;
    ConnectionPool& operator=(const ConnectionPool&) = delete;


    std::queue<std::shared_ptr<MySQL>> connections_;
    std::mutex mutex_;
    std::condition_variable cond_;
};
