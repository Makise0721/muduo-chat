#pragma once

#include "db/MySQL.hpp"
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <string>

class ConnectionPool;

// RAII 连接租约：析构自动归还池。仅可移动；空 Lease 表示获取失败。
class ConnectionLease {
public:
    ConnectionLease() = default;
    ConnectionLease(ConnectionPool* pool, std::shared_ptr<MySQL> conn);
    ~ConnectionLease();
    ConnectionLease(const ConnectionLease&) = delete;
    ConnectionLease& operator=(const ConnectionLease&) = delete;
    ConnectionLease(ConnectionLease&& other) noexcept;
    ConnectionLease& operator=(ConnectionLease&& other) noexcept;

    explicit operator bool() const { return conn_ != nullptr; }
    MySQL* get() const { return conn_.get(); }
    MySQL* operator->() const { return conn_.get(); }

private:
    ConnectionPool* pool_ = nullptr;
    std::shared_ptr<MySQL> conn_;
};

class ConnectionPool {
public:
    enum class PoolError {
        None,
        Timeout,
        Shutdown,
        ConnectionFailed,
    };

    struct AcquireResult {
        AcquireResult() = default;
        AcquireResult(ConnectionLease l, PoolError e)
            : lease(std::move(l)), error(e) {}

        ConnectionLease lease;
        PoolError error = PoolError::None;
    };

    struct Metrics {
        int total = 0;
        int idle = 0;
        int active = 0;
    };

    static ConnectionPool& getInstance();

    // 获取一个连接租约；timeoutMs 内无空闲连接返回 Timeout，
    // 池已关闭返回 Shutdown，坏连接无法替换返回 ConnectionFailed。
    // timeoutMs < 0 表示无限等待（不推荐，接口默认不允许无限等待）。
    AcquireResult acquire(int64_t timeoutMs);

    // 幂等：拒绝新获取（等待者被唤醒返回 Shutdown），有界等待在途租约归还（5s）。
    void shutdown();

    Metrics metrics() const;

    void init(const std::string& host, const std::string& user,
              const std::string& password, const std::string& dbname,
              unsigned int port, int poolSize);
    ~ConnectionPool();

private:
    friend class ConnectionLease;
    void releaseConnection(std::shared_ptr<MySQL> conn);
    std::shared_ptr<MySQL> createConnection();

    ConnectionPool();
    ConnectionPool(const ConnectionPool&) = delete;
    ConnectionPool& operator=(const ConnectionPool&) = delete;

    std::string host_;
    std::string user_;
    std::string password_;
    std::string dbname_;
    unsigned int port_ = 3306;
    int poolSize_ = 0;

    std::queue<std::shared_ptr<MySQL>> connections_;
    int activeCount_ = 0;
    bool closed_ = false;
    mutable std::mutex mutex_;
    std::condition_variable cond_;
};
