#pragma once

#include "app/UserRepository.hpp"

#include <map>
#include <string>

// 内存 adapter：单元测试与开发期使用，无数据库依赖。
class InMemoryUserRepository : public UserRepository {
public:
    CreateUserResult create(const std::string& name, const std::string& password) override;
    AuthResult authenticate(int64_t id, const std::string& password) override;
    bool updateState(int64_t id, UserState state) override;

private:
    struct User {
        int64_t id;
        std::string name;
        std::string password;
        UserState state;
    };
    std::map<int64_t, User> usersById_;
    std::map<std::string, int64_t> idByName_;
    int64_t nextId_ = 1;
};

// 与 MySQL adapter 一致的防御式输入检查（契约：NUL 与超长 → InvalidInput）。
bool isRepositoryInputValid(const std::string& name, const std::string& password);
