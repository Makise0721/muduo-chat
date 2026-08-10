#pragma once

#include "app/Command.hpp"
#include "app/Reply.hpp"
#include "app/UserRepository.hpp"

#include <cstdint>
#include <map>
#include <mutex>

// 应用边界（P2 纵向切片入口）：网络层只转 codec 到 SessionContext/Command，
// 不接触 MySQL 类型；用例编排在本层。
class ChatApplication {
public:
    explicit ChatApplication(UserRepository* users);

    void handle(const SessionContext& ctx, const Command& cmd, Reply* reply);

    // 会话代次：登录尝试/登出/断开时必须调用（代次递增），异步 completion
    // 回调回来用 isSessionCurrent 校验，防止旧 completion 写入重连后的会话。
    int64_t beginSessionAttempt(int64_t userId);
    bool isSessionCurrent(int64_t userId, int64_t generation) const;

    // 供阻塞 executor 的 worker 线程调用的认证/状态操作（内部走 repository）。
    AuthResult authenticate(int64_t userId, const std::string& password);
    bool updateUserState(int64_t userId, UserState state);

private:
    void registerUser(const Command& cmd, Reply* reply);

    UserRepository* users_;
    mutable std::mutex sessionMutex_;
    std::map<int64_t, int64_t> generations_;
};
