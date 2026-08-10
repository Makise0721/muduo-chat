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

ChatApplication::ChatApplication(UserRepository* users)
    : users_(users)
{
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
    } else {
        reply->errnoCode = 1;
        reply->errmsg = kErrMsgRegisterFailed;
    }
}
