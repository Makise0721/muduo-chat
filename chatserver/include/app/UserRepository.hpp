#pragma once

#include <cstdint>
#include <string>

// 用户数据访问 port（P2 应用边界）。
enum class UserError {
    NameExists,
    StorageFailure,
    Disconnected,
    Timeout,
    InvalidInput,
};

enum class UserState {
    Offline,
    Online,
};

struct CreateUserResult {
    bool ok = false;
    int64_t id = 0;
    UserError error = UserError::StorageFailure;
};

struct AuthResult {
    bool ok = false;
    int64_t id = 0;
    std::string name;
    UserState state = UserState::Offline;
    UserError error = UserError::StorageFailure;
};

class UserRepository {
public:
    virtual ~UserRepository() = default;

    // 注册新用户；name 重复返回 NameExists。
    virtual CreateUserResult create(const std::string& name, const std::string& password) = 0;

    // 按 id+password 认证；ok 时返回当前 state（单会话判断用）。
    virtual AuthResult authenticate(int64_t id, const std::string& password) = 0;

    // 更新在线状态（login/loginout/断开）。
    virtual bool updateState(int64_t id, UserState state) = 0;
};
