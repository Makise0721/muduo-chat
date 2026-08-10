#pragma once

#include <cstdint>
#include <string>

// 领域值类型（CONTEXT.md：Command/Session）。
// 注意：不把线上 JSON 字段名（msgid/errno/id）直接充当领域模型。

struct SessionContext {
    int64_t connId = 0;
};

struct Command {
    enum class Type {
        Register,
        Login,
        Logout,
        OneChat,
        AddFriend,
        CreateGroup,
        AddGroup,
        GroupChat,
    };

    Type type = Type::Register;
    int64_t userId = 0;
    std::string name;
    std::string password;
    std::string raw;  // 原始协议 JSON；转发类消息按原样使用
};
