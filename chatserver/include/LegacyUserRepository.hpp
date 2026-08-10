#pragma once

#include "app/UserRepository.hpp"

#include <string>

// 现网 MySQL 存储的注册用例实现（SQL 语义与 P2-00 前 ChatService::reg 一致）。
class LegacyUserRepository : public UserRepository {
public:
    CreateUserResult create(const std::string& name, const std::string& password) override;
};
