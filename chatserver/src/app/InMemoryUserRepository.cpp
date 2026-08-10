#include "app/InMemoryUserRepository.hpp"

CreateUserResult InMemoryUserRepository::create(const std::string& name, const std::string& password)
{
    CreateUserResult result;
    if (users_.find(name) != users_.end()) {
        result.error = UserError::NameExists;
        return result;
    }
    users_.insert({name, User{nextId_, password}});
    result.ok = true;
    result.id = nextId_++;
    return result;
}
