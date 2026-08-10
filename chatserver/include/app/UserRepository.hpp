#pragma once

#include <cstdint>
#include <string>

// 用户数据访问 port（P2 应用边界）。
enum class UserError {
    NameExists,
    StorageFailure,
};

struct CreateUserResult {
    bool ok = false;
    int64_t id = 0;
    UserError error = UserError::StorageFailure;
};

class UserRepository {
public:
    virtual ~UserRepository() = default;

    // 注册新用户；name 重复返回 NameExists。
    virtual CreateUserResult create(const std::string& name, const std::string& password) = 0;
};
