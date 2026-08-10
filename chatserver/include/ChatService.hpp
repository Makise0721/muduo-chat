#pragma once

#include <unordered_map>
#include <functional>
#include <memory>
#include <mutex>
#include "../mymuduo/TcpConnection.h"
#include "../mymuduo/Buffer.h"
#include "../mymuduo/Timestamp.h"
#include "../thirdparty/json.hpp"
#include "db/MySQL.hpp"
#include "db/ConnectionPool.hpp"
#include "app/ChatApplication.hpp"
#include "app/BlockingExecutor.hpp"
#include "app/MySQLUserRepository.hpp"
#include "app/MySQLFriendRepository.hpp"
#include "app/MySQLGroupRepository.hpp"
#include "app/MySQLMessageRepository.hpp"

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

    // P2-05：EventLoop 绑定与阻塞执行器（main 在服务器启动前调用；
    // 退出前调用 shutdownApp 使 worker 有界退出）。
    // P2-09：executor 容量来自配置（workers/queueCapacity，>=1）。
    void bindLoop(EventLoop* loop, int executorWorkers, int executorQueueCapacity);
    void shutdownApp();

    // P2-10：executor 运行期指标（未绑定/空 executor 时返回 0）。
    int executorQueueDepth() const;
    uint64_t executorDroppedFull() const;
    uint64_t executorDroppedShutdown() const;

private:
    ChatService();
    ChatService(const ChatService&) = delete;
    ChatService& operator=(const ChatService&) = delete;

    unordered_map<int, MsgHandler> _msgHandlerMap;
    unordered_map<int, TcpConnectionPtr> _userConnMap;
    mutex _connMutex;
    MySQLUserRepository _mysqlUsers;
    MySQLFriendRepository _mysqlFriends;
    MySQLGroupRepository _mysqlGroups;
    MySQLMessageRepository _mysqlMessages;
    ChatApplication _app;
    EventLoop* _loop = nullptr;
    std::unique_ptr<BlockingExecutor> _executor;
};
