#pragma once

#include <cstdint>
#include <string>
#include <vector>

// 群组数据访问 port（P2 应用边界）。
enum class GroupError {
    None,
    Duplicate,      // 重复加入
    TargetNotFound, // 群/用户不存在（FK 约束）
    StorageFailure,
    Disconnected,
    Timeout,
};

struct CreateGroupResult {
    bool ok = false;
    int64_t groupId = 0;
    GroupError error = GroupError::StorageFailure;
};

struct JoinGroupResult {
    bool ok = false;
    GroupError error = GroupError::StorageFailure;
};

struct MembersResult {
    bool ok = false;
    std::vector<int64_t> userIds;
    GroupError error = GroupError::StorageFailure;
};

class GroupRepository {
public:
    virtual ~GroupRepository() = default;

    // 建群：群表与 creator 成员在同一事务内（部分失败回滚，不留孤儿群）。
    virtual CreateGroupResult create(int64_t ownerId, const std::string& name,
                                     const std::string& desc) = 0;

    // 加入群（role=normal）；重复返回 Duplicate，群/用户不存在返回 TargetNotFound。
    virtual JoinGroupResult join(int64_t groupId, int64_t userId) = 0;

    // 群成员 id 列表（发送者过滤由调用方负责）。
    virtual MembersResult members(int64_t groupId) = 0;
};
