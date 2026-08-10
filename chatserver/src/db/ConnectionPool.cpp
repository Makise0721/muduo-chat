#include "db/ConnectionPool.hpp"
#include <chrono>
#include <iostream>

namespace {

int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace

ConnectionLease::ConnectionLease(ConnectionPool* pool, std::shared_ptr<MySQL> conn)
    : pool_(pool), conn_(std::move(conn))
{
}

ConnectionLease::~ConnectionLease()
{
    if (pool_ && conn_) {
        pool_->releaseConnection(std::move(conn_));
    }
}

ConnectionLease::ConnectionLease(ConnectionLease&& other) noexcept
    : pool_(other.pool_), conn_(std::move(other.conn_))
{
    other.pool_ = nullptr;
}

ConnectionLease& ConnectionLease::operator=(ConnectionLease&& other) noexcept
{
    if (this != &other) {
        if (pool_ && conn_) {
            pool_->releaseConnection(std::move(conn_));
        }
        pool_ = other.pool_;
        conn_ = std::move(other.conn_);
        other.pool_ = nullptr;
    }
    return *this;
}

ConnectionPool::ConnectionPool()
{
}

ConnectionPool::~ConnectionPool()
{
    shutdown();
    while (!connections_.empty()) {
        connections_.pop();
    }
}

ConnectionPool& ConnectionPool::getInstance()
{
    static ConnectionPool instance;
    return instance;
}

void ConnectionPool::init(const std::string& host, const std::string& user,
                          const std::string& password, const std::string& dbname,
                          unsigned int port, int poolSize)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!connections_.empty() || poolSize_ > 0) {
        return;  // 已初始化
    }
    host_ = host;
    user_ = user;
    password_ = password;
    dbname_ = dbname;
    port_ = port;
    poolSize_ = poolSize;
    for (int i = 0; i < poolSize; ++i) {
        auto conn = createConnection();
        if (conn) {
            connections_.push(conn);
        } else {
            std::cerr << "Failed to create MySQL connection " << i << std::endl;
        }
    }
}

std::shared_ptr<MySQL> ConnectionPool::createConnection()
{
    auto conn = std::make_shared<MySQL>();
    if (conn->connect(host_, user_, password_, dbname_, port_)) {
        return conn;
    }
    return nullptr;
}

ConnectionPool::AcquireResult ConnectionPool::acquire(int64_t timeoutMs)
{
    std::unique_lock<std::mutex> lock(mutex_);
    if (closed_) {
        return {ConnectionLease(), PoolError::Shutdown};
    }
    if (connections_.empty()) {
        if (timeoutMs < 0) {
            cond_.wait(lock, [this] { return !connections_.empty() || closed_; });
        } else if (!cond_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                                   [this] { return !connections_.empty() || closed_; })) {
            return {ConnectionLease(), PoolError::Timeout};
        }
        if (closed_) {
            return {ConnectionLease(), PoolError::Shutdown};
        }
    }
    auto conn = connections_.front();
    connections_.pop();
    ++activeCount_;
    lock.unlock();

    if (mysql_ping(conn->getConnection()) != 0) {
        // 坏连接：丢弃并替换；替换失败则失败返回（连接不归还，容量 -1）。
        auto replacement = createConnection();
        if (!replacement) {
            {
                std::lock_guard<std::mutex> l(mutex_);
                if (activeCount_ > 0) {
                    --activeCount_;
                }
            }
            return {ConnectionLease(), PoolError::ConnectionFailed};
        }
        conn = replacement;
    }
    return {ConnectionLease(this, conn), PoolError::None};
}

void ConnectionPool::releaseConnection(std::shared_ptr<MySQL> conn)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (activeCount_ > 0) {
        --activeCount_;
    }
    if (closed_) {
        return;  // 池关闭后不再回收
    }
    connections_.push(std::move(conn));
    cond_.notify_one();
}

void ConnectionPool::shutdown()
{
    bool notify = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!closed_) {
            closed_ = true;
            notify = true;
        }
    }
    if (notify) {
        cond_.notify_all();
    }
    // 有界等待在途租约归还（5s 上限；超时后连接由持有方自行释放）。
    std::unique_lock<std::mutex> lock(mutex_);
    cond_.wait_for(lock, std::chrono::seconds(5), [this] { return activeCount_ == 0; });
}

ConnectionPool::Metrics ConnectionPool::metrics() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    Metrics m;
    m.total = poolSize_;
    m.idle = static_cast<int>(connections_.size());
    m.active = activeCount_;
    return m;
}
