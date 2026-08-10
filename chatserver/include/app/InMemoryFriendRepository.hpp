#pragma once

#include "app/FriendRepository.hpp"
#include "app/InMemoryUserRepository.hpp"

#include <set>
#include <utility>

// 内存 adapter：单元测试与开发期使用，无数据库依赖。
// 目标用户存在性由注入的 InMemoryUserRepository 判定。
class InMemoryFriendRepository : public FriendRepository {
public:
    explicit InMemoryFriendRepository(InMemoryUserRepository& users)
        : users_(users) {}

    AddFriendResult add(int64_t userId, int64_t friendId) override;

private:
    InMemoryUserRepository& users_;
    std::set<std::pair<int64_t, int64_t>> edges_;
};
