#pragma once

#include "app/Command.hpp"
#include "app/FriendRepository.hpp"
#include "app/GroupRepository.hpp"
#include "app/MessageRepository.hpp"
#include "app/Reply.hpp"
#include "app/UserRepository.hpp"

#include <cstdint>
#include <map>
#include <mutex>
#include <vector>

// 应用边界（P2 纵向切片入口）：网络层只转 codec 到 SessionContext/Command，
// 不接触 MySQL 类型；用例编排在本层。
class ChatApplication {
public:
    ChatApplication(UserRepository* users, FriendRepository* friends,
                    GroupRepository* groups, MessageRepository* messages);

    void handle(const SessionContext& ctx, const Command& cmd, Reply* reply);

    // 会话代次：登录尝试/登出/断开时必须调用（代次递增），异步 completion
    // 回调回来用 isSessionCurrent 校验，防止旧 completion 写入重连后的会话。
    int64_t beginSessionAttempt(int64_t userId);
    bool isSessionCurrent(int64_t userId, int64_t generation) const;

    // Invalidate only the session attempt represented by generation. A delayed
    // close from an older session must not advance a newer login generation.
    bool invalidateSessionAttempt(int64_t userId, int64_t generation);

    // 供阻塞 executor 的 worker 线程调用的用例操作（内部走 repository）。
    AuthResult authenticate(int64_t userId, const std::string& password);
    bool updateUserState(int64_t userId, UserState state);
    AddFriendResult addFriend(int64_t userId, int64_t friendId);
    CreateGroupResult createGroup(int64_t ownerId, const std::string& name,
                                  const std::string& desc);
    JoinGroupResult joinGroup(int64_t groupId, int64_t userId);
    MembersResult groupMembers(int64_t groupId);
    StoreResult storeOfflineMessage(int64_t userId, const std::string& payload);
    std::vector<OfflineMessage> takeOfflineMessages(int64_t userId);

private:
    void registerUser(const Command& cmd, Reply* reply);

    UserRepository* users_;
    FriendRepository* friends_;
    GroupRepository* groups_;
    MessageRepository* messages_;
    mutable std::mutex sessionMutex_;
    std::map<int64_t, int64_t> generations_;
};
