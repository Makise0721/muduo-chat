#pragma once

#include "app/Command.hpp"
#include "app/FriendRepository.hpp"
#include "app/GroupRepository.hpp"
#include "app/Reply.hpp"
#include "app/SessionRegistry.hpp"
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
                    GroupRepository* groups);

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

    // P3-11 M2：Offline 状态写入守卫。跨代 lane 并行下（keyed executor 多
    // worker），旧代次 lane 的 Offline 写入可能在 DB 层面覆盖新代次已在线用户
    // （旧单 worker 全局 FIFO 曾保证顺序）。以 SessionRegistry 为权威：仍有
    // 活动会话（lookupByUser 非空）则跳过写 Offline。registry 线程安全，可在
    // executor worker 线程调用。返回是否实际写入 Offline。
    bool updateUserStateOfflineUnlessActive(SessionRegistry& sessions, int64_t userId);
    AddFriendResult addFriend(int64_t userId, int64_t friendId);
    CreateGroupResult createGroup(int64_t ownerId, const std::string& name,
                                  const std::string& desc);
    JoinGroupResult joinGroup(int64_t groupId, int64_t userId);
    MembersResult groupMembers(int64_t groupId);

private:
    void registerUser(const Command& cmd, Reply* reply);

    UserRepository* users_;
    FriendRepository* friends_;
    GroupRepository* groups_;
    mutable std::mutex sessionMutex_;
    std::map<int64_t, int64_t> generations_;
};
