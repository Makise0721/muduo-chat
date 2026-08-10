#pragma once

#include "app/Command.hpp"
#include "app/Reply.hpp"
#include "app/UserRepository.hpp"

// 应用边界（P2 纵向切片入口）：网络层只转 codec 到 SessionContext/Command，
// 不接触 MySQL 类型；用例编排在本层。
class ChatApplication {
public:
    explicit ChatApplication(UserRepository* users);

    void handle(const SessionContext& ctx, const Command& cmd, Reply* reply);

private:
    void registerUser(const Command& cmd, Reply* reply);

    UserRepository* users_;
};
