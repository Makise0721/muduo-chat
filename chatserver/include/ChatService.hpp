#pragma once

#include <unordered_map>
#include <functional>
#include <memory>
#include "../mymuduo/TcpConnection.h"
#include "../mymuduo/Buffer.h"
#include "../mymuduo/Timestamp.h"
#include "../thirdparty/json.hpp"
#include "db/MySQL.hpp"
#include "db/ConnectionPool.hpp"
#include "app/ChatApplication.hpp"
#include "app/SessionSerialExecutor.hpp"
#include "app/SessionRegistry.hpp"
#include "app/SessionDeliverySink.hpp"
#include "app/MySQLUserRepository.hpp"
#include "app/MySQLFriendRepository.hpp"
#include "app/MySQLGroupRepository.hpp"

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
    // P3-06 协议 golden 一次冻结（docs/tasks/P3-06.md 决策表 1/2/3 行；B-22
    // 不占 1..10）：11/12/13 数值落地后不再变更。DELIVERY_ACK handler 属 P3-07。
    MESSAGE_ACCEPTED_MSG = 11,
    DELIVERY_ACK_MSG = 12,
    ERROR_RESP_MSG = 13,
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
    // P3-07：接收端按 MessageId 显式确认（msgid=12，spec §2.3）；UserId 取
    // Session，不由 payload 提供；静默处理（无 ACK 回执），缺字段 B-25 静默。
    void deliveryAck(const TcpConnectionPtr& conn, json& js, Timestamp time);
    void clientCloseException(const TcpConnectionPtr& conn);
    void reset();

    // P3-05 对抗审查：连接建立时登记活跃（onConnection 建立分支），
    // close 回调的 clientCloseException 中随 unbind 一起移除。登录 completion
    // 的 bind 在锁内校验活跃集合，close 先到则拒绝（ConnectionInactive），
    // 消除 bind 死连接导致的会话泄漏/锁死用户竞态。
    void addConnection(const TcpConnectionPtr& conn);

    // P2-05：EventLoop 绑定与阻塞执行器（main 在服务器启动前调用；
    // 退出前调用 shutdownApp 使 worker 有界退出）。
    // P2-09：executor 容量来自配置（workers/queueCapacity，>=1）。
    // P3-11：executor 替换为 keyed SessionSerialExecutor；lane 上限生产默认
    // = 卡冻结值（SessionSerialExecutor::kDefaultLaneCapacity）。
    void bindLoop(EventLoop* loop, int executorWorkers, int executorQueueCapacity,
                  int executorLaneCapacity = SessionSerialExecutor::kDefaultLaneCapacity);
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
    SessionRegistry _sessions;
    MySQLUserRepository _mysqlUsers;
    MySQLFriendRepository _mysqlFriends;
    MySQLGroupRepository _mysqlGroups;
    ChatApplication _app;
    // P3-07：真实投递出口（bindLoop 时 registerDeliverySink；声明顺序在
    // _sessions 之后，&_sessions 有效）。
    SessionDeliverySink _deliverySink{&_sessions};
    SessionDeliveryArmer _deliveryArmer{&_sessions};
    EventLoop* _loop = nullptr;
    std::unique_ptr<SessionSerialExecutor> _executor;
};
