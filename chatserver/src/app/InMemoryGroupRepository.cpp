#include "app/InMemoryGroupRepository.hpp"

CreateGroupResult InMemoryGroupRepository::create(int64_t ownerId, const std::string& name,
                                                  const std::string& desc)
{
    CreateGroupResult result;
    // 事务等价：owner 校验先于任何写入（InMemory 无部分失败路径）。
    if (!users_.userExists(ownerId)) {
        result.error = GroupError::TargetNotFound;
        return result;
    }
    Group g;
    g.id = nextId_;
    g.name = name;
    g.desc = desc;
    g.members.insert(ownerId);
    groups_.insert({nextId_, std::move(g)});
    result.ok = true;
    result.groupId = nextId_++;
    return result;
}

JoinGroupResult InMemoryGroupRepository::join(int64_t groupId, int64_t userId)
{
    JoinGroupResult result;
    auto it = groups_.find(groupId);
    if (it == groups_.end()) {
        result.error = GroupError::TargetNotFound;
        return result;
    }
    if (!users_.userExists(userId)) {
        result.error = GroupError::TargetNotFound;
        return result;
    }
    if (!it->second.members.insert(userId).second) {
        result.error = GroupError::Duplicate;
        return result;
    }
    result.ok = true;
    return result;
}
