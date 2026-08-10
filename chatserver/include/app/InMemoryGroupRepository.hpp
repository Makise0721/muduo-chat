#pragma once

#include "app/GroupRepository.hpp"
#include "app/InMemoryUserRepository.hpp"

#include <map>
#include <set>
#include <string>

// 内存 adapter：单元测试与开发期使用，无数据库依赖。
class InMemoryGroupRepository : public GroupRepository {
public:
    explicit InMemoryGroupRepository(InMemoryUserRepository& users)
        : users_(users) {}

    CreateGroupResult create(int64_t ownerId, const std::string& name,
                             const std::string& desc) override;
    JoinGroupResult join(int64_t groupId, int64_t userId) override;
    MembersResult members(int64_t groupId) override;

private:
    struct Group {
        int64_t id;
        std::string name;
        std::string desc;
        std::set<int64_t> members;
    };

    InMemoryUserRepository& users_;
    std::map<int64_t, Group> groups_;
    int64_t nextId_ = 1;
};
