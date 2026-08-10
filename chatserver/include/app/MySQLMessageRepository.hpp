#pragma once

#include "app/MessageRepository.hpp"
#include "db/ConnectionPool.hpp"

#include <string>

// 现网 MySQL adapter：prepared statement 防注入。
class MySQLMessageRepository : public MessageRepository {
public:
    explicit MySQLMessageRepository(ConnectionPool& pool);

    StoreResult storeOffline(int64_t userId, const std::string& payload) override;
    std::vector<OfflineMessage> takeOffline(int64_t userId) override;

private:
    ConnectionPool& pool_;
};
