#pragma once

#include <cstdint>

// 好友数据访问 port（P2 应用边界）。
enum class FriendError {
    None,
    Duplicate,      // 该有向边已存在
    TargetNotFound, // 目标用户不存在（FK 约束）
    StorageFailure,
    Disconnected,
    Timeout,
};

struct AddFriendResult {
    bool ok = false;
    FriendError error = FriendError::StorageFailure;
};

class FriendRepository {
public:
    virtual ~FriendRepository() = default;

    // 添加一条有向好友边（A -> B）；重复返回 Duplicate。
    virtual AddFriendResult add(int64_t userId, int64_t friendId) = 0;
};
