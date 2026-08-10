#pragma once

#include "app/GroupRepository.hpp"
#include "db/ConnectionPool.hpp"

#include <string>

GroupError mapGroupError(unsigned int err);

// 现网 MySQL adapter：建群在显式事务内（部分失败回滚，不留孤儿群）。
class MySQLGroupRepository : public GroupRepository {
public:
    explicit MySQLGroupRepository(ConnectionPool& pool);

    CreateGroupResult create(int64_t ownerId, const std::string& name,
                             const std::string& desc) override;
    JoinGroupResult join(int64_t groupId, int64_t userId) override;

private:
    ConnectionPool& pool_;
};
