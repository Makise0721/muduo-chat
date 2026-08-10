#include "db/ConnectionPool.hpp"
#include <iostream>

ConnectionPool::ConnectionPool() {
}

ConnectionPool::~ConnectionPool() {
    while (!connections_.empty()) {
        connections_.pop();
    }
}

ConnectionPool& ConnectionPool::getInstance() {
    static ConnectionPool instance;
    return instance;
}

void ConnectionPool::init(const std::string& host, const std::string& user, 
                         const std::string& password, const std::string& dbname, 
                         unsigned int port, int poolSize) {
    for (int i = 0; i < poolSize; ++i) {
        auto conn = std::make_shared<MySQL>();
        if (conn->connect(host, user, password, dbname, port)) {
            connections_.push(conn);
        } else {
            std::cerr << "Failed to create MySQL connection " << i << std::endl;
        }
    }
}

std::shared_ptr<MySQL> ConnectionPool::getConnection() {
    std::unique_lock<std::mutex> lock(mutex_);
    cond_.wait(lock, [this] { return !connections_.empty(); });
    
    auto conn = connections_.front();
    connections_.pop();
    return conn;
}

void ConnectionPool::releaseConnection(std::shared_ptr<MySQL> conn) {
    std::unique_lock<std::mutex> lock(mutex_);
    connections_.push(conn);
    cond_.notify_one();
}
