#include "app/InMemoryUserRepository.hpp"

#include <algorithm>

bool isRepositoryInputValid(const std::string& name, const std::string& password)
{
    if (name.empty() || password.empty() || name.size() > 50 || password.size() > 50) {
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
    if (idByName_.find(name) != idByName_.end()) {
        result.error = UserError::NameExists;
        return result;
    }
    usersById_.insert({nextId_, User{nextId_, name, password, UserState::Offline}});
    idByName_.insert({name, nextId_});
    result.ok = true;
    result.id = nextId_++;
    return result;
}

AuthResult InMemoryUserRepository::authenticate(int64_t id, const std::string& password)
{
    AuthResult result;
    auto it = usersById_.find(id);
    if (it == usersById_.end() || it->second.password != password) {
        return result;
    }
    result.ok = true;
    result.id = id;
    result.name = it->second.name;
    result.state = it->second.state;
    return result;
}

bool InMemoryUserRepository::updateState(int64_t id, UserState state)
{
    auto it = usersById_.find(id);
    if (it == usersById_.end()) {
        return false;
    }
    it->second.state = state;
    return true;
}

bool InMemoryUserRepository::userExists(int64_t id) const
{
    return usersById_.find(id) != usersById_.end();
}
