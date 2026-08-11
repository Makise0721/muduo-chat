#include "app/InMemoryFriendRepository.hpp"

AddFriendResult InMemoryFriendRepository::add(int64_t userId, int64_t friendId)
{
    AddFriendResult result;
    // 与 MySQL FK 双列校验一致：发送方或目标不存在 → TargetNotFound。
    if (!users_.userExists(userId) || !users_.userExists(friendId)) {
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
