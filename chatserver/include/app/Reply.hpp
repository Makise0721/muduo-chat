#pragma once

#include "app/Command.hpp"

#include <cstdint>
#include <string>

// 领域 Reply（CONTEXT.md：Errno 0 成功 / 1 业务失败 / 2 会话冲突）。
struct Reply {
    Command::Type type = Command::Type::Register;
    int errnoCode = 1;
    std::string errmsg;
    int64_t userId = 0;
    std::string name;
};
