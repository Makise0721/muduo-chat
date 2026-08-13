#include "app/ChatApplication.hpp"

namespace {

const char* kErrMsgNameExists = "this name is already exist!";
const char* kErrMsgRegisterFailed = "register failed!";

// User.name 为 VARCHAR(50)；空值与超长均为非法输入。
bool validRegistrationInput(const Command& cmd)
{
    return !cmd.name.empty() && !cmd.password.empty() && cmd.name.size() <= 50;
}

} // namespace

ChatApplication::ChatApplication(UserRepository* users, FriendRepository* friends,
                                 GroupRepository* groups, MessageRepository* messages)
    : users_(users), friends_(friends), groups_(groups), messages_(messages)
{
}

int64_t ChatApplication::beginSessionAttempt(int64_t userId)
{
    std::lock_guard<std::mutex> lock(sessionMutex_);
    return ++generations_[userId];
}

bool ChatApplication::isSessionCurrent(int64_t userId, int64_t generation) const
{
    std::lock_guard<std::mutex> lock(sessionMutex_);
    auto it = generations_.find(userId);
    return it != generations_.end() && it->second == generation;
}

bool ChatApplication::invalidateSessionAttempt(int64_t userId, int64_t generation)
{
    std::lock_guard<std::mutex> lock(sessionMutex_);
    std::map<int64_t, int64_t>::iterator it = generations_.find(userId);
    if (it == generations_.end() || it->second != generation) {
        return false;
    }
    ++it->second;
    return true;
}

AuthResult ChatApplication::authenticate(int64_t userId, const std::string& password)
{
    return users_->authenticate(userId, password);
}

bool ChatApplication::updateUserState(int64_t userId, UserState state)
{
    return users_->updateState(userId, state);
}

AddFriendResult ChatApplication::addFriend(int64_t userId, int64_t friendId)
{
    return friends_->add(userId, friendId);
}

CreateGroupResult ChatApplication::createGroup(int64_t ownerId, const std::string& name,
                                               const std::string& desc)
{
    return groups_->create(ownerId, name, desc);
}

JoinGroupResult ChatApplication::joinGroup(int64_t groupId, int64_t userId)
{
    return groups_->join(groupId, userId);
}

MembersResult ChatApplication::groupMembers(int64_t groupId)
{
    return groups_->members(groupId);
}

StoreResult ChatApplication::storeOfflineMessage(int64_t userId, const std::string& payload)
{
    return messages_->storeOffline(userId, payload);
}

std::vector<OfflineMessage> ChatApplication::takeOfflineMessages(int64_t userId)
{
    return messages_->takeOffline(userId);
}

void ChatApplication::handle(const SessionContext&, const Command& cmd, Reply* reply)
{
    reply->type = cmd.type;
    switch (cmd.type) {
        case Command::Type::Register:
            registerUser(cmd, reply);
            break;
        default:
            reply->errnoCode = 1;
            reply->errmsg = "not supported yet";
    }
}

void ChatApplication::registerUser(const Command& cmd, Reply* reply)
{
    if (!validRegistrationInput(cmd)) {
        reply->errnoCode = 1;
        reply->errmsg = "invalid input!";
        return;
    }
    CreateUserResult result = users_->create(cmd.name, cmd.password);
    if (result.ok) {
        reply->errnoCode = 0;
        reply->userId = result.id;
    } else if (result.error == UserError::NameExists) {
        reply->errnoCode = 1;
        reply->errmsg = kErrMsgNameExists;
    } else if (result.error == UserError::InvalidInput) {
        reply->errnoCode = 1;
        reply->errmsg = "invalid input!";
    } else {
        reply->errnoCode = 1;
        reply->errmsg = kErrMsgRegisterFailed;
    }
}
