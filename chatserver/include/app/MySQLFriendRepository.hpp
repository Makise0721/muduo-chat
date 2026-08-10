#pragma once

#include "app/FriendRepository.hpp"
#include "db/ConnectionPool.hpp"

#include <string>

// 将 MySQL 错误码映射为好友/群组稳定领域错误（复用 mapMySqlError 分类，
// 另将 1452（FK 约束）映射为 TargetNotFound）。
FriendError mapFriendError(unsigned int err);

// 现网 MySQL adapter：prepared statement 防注入。
class MySQLFriendRepository : public FriendRepository {
public:
    explicit MySQLFriendRepository(ConnectionPool& pool);

    AddFriendResult add(int64_t userId, int64_t friendId) override;

private:
    ConnectionPool& pool_;
};
