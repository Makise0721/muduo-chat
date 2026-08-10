#include "app/InMemoryFriendRepository.hpp"

AddFriendResult InMemoryFriendRepository::add(int64_t userId, int64_t friendId)
{
    AddFriendResult result;
    if (!users_.userExists(friendId)) {
        result.error = FriendError::TargetNotFound;
        return result;
    }
    if (!edges_.insert({userId, friendId}).second) {
        result.error = FriendError::Duplicate;
        return result;
    }
    result.ok = true;
    return result;
}
