#pragma once

#include "app/UserRepository.hpp"
#include "db/ConnectionPool.hpp"

#include <string>

// 将 MySQL 错误码映射为稳定领域错误（断线/超时/唯一键冲突/数据超限）。
// 独立函数便于单元测试错误分类。
UserError mapMySqlError(unsigned int err);

// 现网 MySQL adapter：prepared statement 防注入，错误映射稳定领域错误。
class MySQLUserRepository : public UserRepository {
public:
    explicit MySQLUserRepository(ConnectionPool& pool);

    CreateUserResult create(const std::string& name, const std::string& password) override;
    AuthResult authenticate(int64_t id, const std::string& password) override;
    bool updateState(int64_t id, UserState state) override;

private:
    ConnectionPool& pool_;
};
