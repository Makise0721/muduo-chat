#pragma once

#include <unordered_map>
#include <functional>
#include <mutex>
#include "../mymuduo/TcpConnection.h"
#include "../mymuduo/Buffer.h"
#include "../mymuduo/Timestamp.h"
#include "../thirdparty/json.hpp"
#include "db/MySQL.hpp"
#include "db/ConnectionPool.hpp"

using json = nlohmann::json;
using namespace std;

using MsgHandler = function<void(const TcpConnectionPtr&, json&, Timestamp)>;

enum EnMsgType {
    LOGIN_MSG = 1,
    LOGIN_MSG_ACK,
    LOGINOUT_MSG,
    REG_MSG,
    REG_MSG_ACK,
    ONE_CHAT_MSG,
    ADD_FRIEND_MSG,
    CREATE_GROUP_MSG,
    ADD_GROUP_MSG,
    GROUP_CHAT_MSG,
};

class ChatService {
public:
    static ChatService* instance();
    void handler(const TcpConnectionPtr& conn, string& msg, Timestamp time);
    void login(const TcpConnectionPtr& conn, json& js, Timestamp time);
    void reg(const TcpConnectionPtr& conn, json& js, Timestamp time);
    void loginout(const TcpConnectionPtr& conn, json& js, Timestamp time);
    void oneChat(const TcpConnectionPtr& conn, json& js, Timestamp time);
    void addFriend(const TcpConnectionPtr& conn, json& js, Timestamp time);
    void createGroup(const TcpConnectionPtr& conn, json& js, Timestamp time);
    void addGroup(const TcpConnectionPtr& conn, json& js, Timestamp time);
    void groupChat(const TcpConnectionPtr& conn, json& js, Timestamp time);
    void clientCloseException(const TcpConnectionPtr& conn);
    void reset();

private:
    ChatService();
    ChatService(const ChatService&) = delete;
    ChatService& operator=(const ChatService&) = delete;

    unordered_map<int, MsgHandler> _msgHandlerMap;
    unordered_map<int, TcpConnectionPtr> _userConnMap;
    mutex _connMutex;
};
