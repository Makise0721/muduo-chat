#include "app/InMemoryUserRepository.hpp"

#include <algorithm>

bool isRepositoryInputValid(const std::string& name, const std::string& password)
{
    if (name.empty() || password.empty() || name.size() > 50) {
        return false;
    }
    // NUL 与 MySQL 字符串列语义冲突（存储截断），防御式拒绝。
    return std::find(name.begin(), name.end(), '\0') == name.end() &&
           std::find(password.begin(), password.end(), '\0') == password.end();
}

CreateUserResult InMemoryUserRepository::create(const std::string& name, const std::string& password)
{
    CreateUserResult result;
    if (!isRepositoryInputValid(name, password)) {
        result.error = UserError::InvalidInput;
        return result;
    }
    if (users_.find(name) != users_.end()) {
        result.error = UserError::NameExists;
        return result;
    }
    users_.insert({name, User{nextId_, password}});
    result.ok = true;
    result.id = nextId_++;
    return result;
}
