#pragma once

#include "app/MessageRepository.hpp"

#include <deque>
#include <map>

// 内存 adapter：单元测试与开发期使用，无数据库依赖。
class InMemoryMessageRepository : public MessageRepository {
public:
    StoreResult storeOffline(int64_t userId, const std::string& payload) override;
    std::vector<OfflineMessage> takeOffline(int64_t userId) override;

private:
    std::map<int64_t, std::deque<OfflineMessage>> queues_;
    int64_t nextId_ = 1;
};
