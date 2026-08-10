#include "ChatService.hpp"
#include "app/ChatApplication.hpp"
#include "db/MySQLGuards.hpp"
#include <iostream>
#include <exception>
#include <string>
#include <vector>
#include <unordered_set>

namespace {
} // namespace

ChatService::ChatService()
    : _mysqlUsers(ConnectionPool::getInstance()),
      _mysqlFriends(ConnectionPool::getInstance()),
      _mysqlGroups(ConnectionPool::getInstance()),
      _mysqlMessages(ConnectionPool::getInstance()),
      _app(&_mysqlUsers, &_mysqlFriends, &_mysqlGroups, &_mysqlMessages) {
    _msgHandlerMap.insert({LOGIN_MSG, bind(&ChatService::login, this, placeholders::_1, placeholders::_2, placeholders::_3)});
    _msgHandlerMap.insert({REG_MSG, bind(&ChatService::reg, this, placeholders::_1, placeholders::_2, placeholders::_3)});
    _msgHandlerMap.insert({LOGINOUT_MSG, bind(&ChatService::loginout, this, placeholders::_1, placeholders::_2, placeholders::_3)});
    _msgHandlerMap.insert({ONE_CHAT_MSG, bind(&ChatService::oneChat, this, placeholders::_1, placeholders::_2, placeholders::_3)});
    _msgHandlerMap.insert({ADD_FRIEND_MSG, bind(&ChatService::addFriend, this, placeholders::_1, placeholders::_2, placeholders::_3)});
    _msgHandlerMap.insert({CREATE_GROUP_MSG, bind(&ChatService::createGroup, this, placeholders::_1, placeholders::_2, placeholders::_3)});
    _msgHandlerMap.insert({ADD_GROUP_MSG, bind(&ChatService::addGroup, this, placeholders::_1, placeholders::_2, placeholders::_3)});
    _msgHandlerMap.insert({GROUP_CHAT_MSG, bind(&ChatService::groupChat, this, placeholders::_1, placeholders::_2, placeholders::_3)});
}

ChatService* ChatService::instance() {
    static ChatService service;
    return &service;
}

void ChatService::handler(const TcpConnectionPtr& conn, string& msg, Timestamp time) {
    json js;
    try {
        js = json::parse(msg);
    } catch (const std::exception& e) {
        cerr << "json parse failed: " << e.what() << ", msg=" << msg << endl;
        return;
    }

    if (!js.contains("msgid")) {
        cerr << "msg has no msgid, msg=" << msg << endl;
        return;
    }

    int msgid = js["msgid"].get<int>();
    
    auto it = _msgHandlerMap.find(msgid);
    if (it == _msgHandlerMap.end()) {
        cerr << "msgid: " << msgid << " can not find handler!" << endl;
        return;
    }
    
    it->second(conn, js, time);
}

namespace {

// P2-05 登录用例的 worker 线程 DB 结果（认证 + 好友 + 离线消息一次取齐）。
struct FriendDetail {
    int friendid;
    std::string name;
    std::string state;
};

struct LoginDbResult {
    AuthResult auth;
    std::vector<std::string> friendNames;
    std::vector<FriendDetail> friendDetails;
    std::vector<std::string> offlineMessages;
};

// 在 worker 线程执行：认证通过后查询好友列表与离线消息。
// 好友列表查询为 P2-06 后遗留的原生 SQL；离线消息经 MessageRepository 读取
// （补投后队列清空）。
void loadLoginExtras(ChatApplication* app, LoginDbResult* r)
{
    auto& connPool = ConnectionPool::getInstance();
    ConnectionPool::AcquireResult acq = connPool.acquire(5000);
    if (!acq.lease) {
        return;
    }
    MySQL* mysql = acq.lease.get();
    char sql[1024] = {0};
    snprintf(sql, sizeof(sql),
             "SELECT friendid, name, state FROM Friend, User "
             "WHERE Friend.userid = %d AND Friend.friendid = User.id",
             static_cast<int>(r->auth.id));
    MYSQL_RES* resFriends = mysql->query(sql);
    if (resFriends) {
        MySQLResultGuard guardFriends(resFriends);
        MYSQL_ROW rowFriend;
        while ((rowFriend = mysql_fetch_row(resFriends)) != nullptr) {
            if (rowFriend[1]) {
                r->friendNames.push_back(rowFriend[1]);
            }
            FriendDetail d;
            d.friendid = rowFriend[0] ? atoi(rowFriend[0]) : 0;
            d.name = rowFriend[1] ? rowFriend[1] : "";
            d.state = rowFriend[2] ? rowFriend[2] : "";
            r->friendDetails.push_back(d);
        }
    }

    std::vector<OfflineMessage> offline = app->takeOfflineMessages(r->auth.id);
    for (size_t i = 0; i < offline.size(); ++i) {
        r->offlineMessages.push_back(offline[i].payload);
    }
}

} // namespace

void ChatService::bindLoop(EventLoop* loop)
{
    _loop = loop;
    // 单 worker：同一连接的串行依赖（如 addFriend 后立即重复 add）按提交顺序
    // 执行；多 worker 会乱序破坏业务语义（P2-10 性能评估后再分片扩并）。
    _executor.reset(new BlockingExecutor(loop, 1, 64));
}

void ChatService::shutdownApp()
{
    if (_executor) {
        _executor->shutdown();
    }
}

void ChatService::login(const TcpConnectionPtr& conn, json& js, Timestamp time) {
    int id = js["id"].get<int>();
    std::string pwd = js["password"].get<std::string>();

    if (!_executor) {
        json response;
        response["msgid"] = LOGIN_MSG_ACK;
        response["errno"] = 1;
        response["errmsg"] = "db unavailable!";
        conn->send(response.dump() + "\n");
        return;
    }

    // 会话代次：登录尝试登记，旧异步 completion 不得覆盖重连后的会话。
    int64_t gen = _app.beginSessionAttempt(id);
    auto result = std::make_shared<LoginDbResult>();
    _executor->submit(
        [this, id, pwd, gen, result] {
            result->auth = _app.authenticate(id, pwd);
            // state 只读不改：单会话以 _userConnMap（活动连接）为真相，
            // 断开残留的 DB online 由 completion 区分处理。
            if (result->auth.ok && _app.isSessionCurrent(id, gen)) {
                loadLoginExtras(&_app, result.get());
            }
        },
        [this, conn, id, gen, result] {
            // stale：会话已被登出/断开/新登录取代，不写状态、不响应。
            if (!_app.isSessionCurrent(id, gen)) {
                return;
            }
            json response;
            response["msgid"] = LOGIN_MSG_ACK;
            if (!result->auth.ok) {
                response["errno"] = 1;
                response["errmsg"] = "userid or password is invalid!";
                conn->send(response.dump() + "\n");
                return;
            }
            {
                lock_guard<mutex> lock(_connMutex);
                if (_userConnMap.find(id) != _userConnMap.end()) {
                    // 真重复登录：该用户已有活动会话。
                    response["errno"] = 2;
                    response["errmsg"] = "this account is using, input another!";
                    conn->send(response.dump() + "\n");
                    return;
                }
                _userConnMap.insert({id, conn});
            }
            // DB 状态影子异步落库（登录成功后必须是 online）。
            _executor->submit(
                [this, id] { _app.updateUserState(id, UserState::Online); },
                [] {});
            response["errno"] = 0;
            response["id"] = id;
            response["name"] = result->auth.name;
            response["friends"] = result->friendNames;
            json friendDetails = json::array();
            for (size_t i = 0; i < result->friendDetails.size(); ++i) {
                json friendInfo;
                friendInfo["friendid"] = result->friendDetails[i].friendid;
                friendInfo["name"] = result->friendDetails[i].name;
                friendInfo["state"] = result->friendDetails[i].state;
                friendDetails.push_back(friendInfo);
            }
            response["friendDetails"] = friendDetails;

            // 先回登录应答，客户端更容易按顺序处理
            conn->send(response.dump() + "\n");

            // 离线消息下发（补投后 OfflineMessage 已在 worker 清空）
            for (size_t i = 0; i < result->offlineMessages.size(); ++i) {
                conn->send(result->offlineMessages[i] + "\n");
            }
        });
}

void ChatService::reg(const TcpConnectionPtr& conn, json& js, Timestamp time) {
    Command cmd;
    cmd.type = Command::Type::Register;
    cmd.name = js["name"].get<std::string>();
    cmd.password = js["password"].get<std::string>();
    cmd.raw = js.dump();

    SessionContext ctx;
    Reply reply;
    _app.handle(ctx, cmd, &reply);

    json response;
    response["msgid"] = REG_MSG_ACK;
    response["errno"] = reply.errnoCode;
    if (reply.errnoCode == 0) {
        response["id"] = reply.userId;
    } else {
        response["errmsg"] = reply.errmsg;
    }
    conn->send(response.dump() + "\n");
}

void ChatService::loginout(const TcpConnectionPtr& conn, json& js, Timestamp time) {
    int userid = js["id"].get<int>();
    {
        lock_guard<mutex> lock(_connMutex);
        auto it = _userConnMap.find(userid);
        if (it != _userConnMap.end()) {
            _userConnMap.erase(it);
        }
    }

    // 会话代次递增：在途登录 completion 不再生效。
    _app.beginSessionAttempt(userid);
    if (_executor) {
        _executor->submit(
            [this, userid] { _app.updateUserState(userid, UserState::Offline); },
            [conn, userid] {
                json response;
                response["msgid"] = LOGINOUT_MSG;
                response["errno"] = 0;
                conn->send(response.dump() + "\n");
            });
    } else {
        json response;
        response["msgid"] = LOGINOUT_MSG;
        response["errno"] = 1;
        response["errmsg"] = "db unavailable!";
        conn->send(response.dump() + "\n");
    }
}

void ChatService::oneChat(const TcpConnectionPtr& conn, json& js, Timestamp time) {
    int toid = js["toid"].get<int>();
    {
        lock_guard<mutex> lock(_connMutex);
        auto it = _userConnMap.find(toid);
        if (it != _userConnMap.end()) {
            // 目标在线：原样转发（内存操作，无 SQL），随后回发送者确认。
            it->second->send(js.dump() + "\n");
            json response = js;
            response["errno"] = 0;
            conn->send(response.dump() + "\n");
            return;
        }
    }

    // 目标离线：异步入队（Reply errno=0 表示"服务器已接受"，非"对端已收到"）。
    if (!_executor) {
        json response = js;
        response["errno"] = 1;
        conn->send(response.dump() + "\n");
        return;
    }
    std::string payload = js.dump();
    auto result = std::make_shared<StoreResult>();
    _executor->submit(
        [this, toid, payload, result] { *result = _app.storeOfflineMessage(toid, payload); },
        [conn, js, result] {
            json response = js;
            response["errno"] = result->ok ? 0 : 1;
            conn->send(response.dump() + "\n");
        });
}

void ChatService::addFriend(const TcpConnectionPtr& conn, json& js, Timestamp time) {
    int userid = js["id"].get<int>();
    int friendid = js["friendid"].get<int>();

    if (!_executor) {
        json response = js;
        response["errno"] = 1;
        conn->send(response.dump() + "\n");
        return;
    }
    auto result = std::make_shared<AddFriendResult>();
    _executor->submit(
        [this, userid, friendid, result] { *result = _app.addFriend(userid, friendid); },
        [conn, js, result] {
            json response = js;
            response["errno"] = result->ok ? 0 : 1;
            conn->send(response.dump() + "\n");
        });
}

void ChatService::createGroup(const TcpConnectionPtr& conn, json& js, Timestamp time) {
    int userid = js["id"].get<int>();
    string groupname = js["groupname"];
    string groupdesc = js["groupdesc"];

    if (!_executor) {
        json response;
        response["msgid"] = CREATE_GROUP_MSG;
        response["id"] = userid;
        response["errno"] = 1;
        conn->send(response.dump() + "\n");
        return;
    }
    auto result = std::make_shared<CreateGroupResult>();
    _executor->submit(
        [this, userid, groupname, groupdesc, result] {
            *result = _app.createGroup(userid, groupname, groupdesc);
        },
        [conn, userid, result] {
            json response;
            response["msgid"] = CREATE_GROUP_MSG;
            response["id"] = userid;
            if (result->ok) {
                response["groupid"] = result->groupId;
                response["errno"] = 0;
            } else {
                response["errno"] = 1;
            }
            conn->send(response.dump() + "\n");
        });
}

void ChatService::addGroup(const TcpConnectionPtr& conn, json& js, Timestamp time) {
    int userid = js["id"].get<int>();
    int groupid = js["groupid"].get<int>();

    if (!_executor) {
        json response = js;
        response["errno"] = 1;
        conn->send(response.dump() + "\n");
        return;
    }
    auto result = std::make_shared<JoinGroupResult>();
    _executor->submit(
        [this, userid, groupid, result] { *result = _app.joinGroup(groupid, userid); },
        [conn, js, result] {
            json response = js;
            response["errno"] = result->ok ? 0 : 1;
            conn->send(response.dump() + "\n");
        });
}

void ChatService::groupChat(const TcpConnectionPtr& conn, json& js, Timestamp time) {
    int userid = js["id"].get<int>();
    int groupid = js["groupid"].get<int>();

    if (!_executor) {
        json response = js;
        response["errno"] = 1;
        conn->send(response.dump() + "\n");
        return;
    }
    auto members = std::make_shared<MembersResult>();
    _executor->submit(
        [this, groupid, members] { *members = _app.groupMembers(groupid); },
        [this, conn, js, userid, members] {
            std::string payload = js.dump();
            // 在线成员：锁内拷贝连接后原样转发（内存操作，无 SQL）。
            std::vector<TcpConnectionPtr> onlineUsers;
            std::unordered_set<int64_t> onlineIds;
            {
                lock_guard<mutex> lock(_connMutex);
                for (int64_t toid : members->userIds) {
                    if (toid == userid) {
                        continue;  // 发送者不接收
                    }
                    auto it = _userConnMap.find(toid);
                    if (it != _userConnMap.end()) {
                        onlineUsers.push_back(it->second);
                        onlineIds.insert(toid);
                    }
                }
            }
            std::string msg = payload + "\n";
            for (const TcpConnectionPtr& target : onlineUsers) {
                target->send(msg);
            }
            // 离线成员：异步入队（每条一个任务，单 worker FIFO 保序）。
            for (int64_t toid : members->userIds) {
                if (toid == userid || onlineIds.find(toid) != onlineIds.end()) {
                    continue;
                }
                _executor->submit(
                    [this, toid, payload] { _app.storeOfflineMessage(toid, payload); },
                    [] {});
            }
            // 给群聊发送者一个确认，避免客户端阻塞等待（B-19：查询失败也回 0）。
            json response = js;
            response["errno"] = 0;
            conn->send(response.dump() + "\n");
        });
}

void ChatService::clientCloseException(const TcpConnectionPtr& conn) {
    int userid = -1;
    {
        lock_guard<mutex> lock(_connMutex);
        for (auto it = _userConnMap.begin(); it != _userConnMap.end(); ++it) {
            if (it->second == conn) {
                userid = it->first;
                _userConnMap.erase(it);
                break;
            }
        }
    }
    
    if (userid != -1) {
        // 会话代次递增：在途登录 completion 不再生效。
        _app.beginSessionAttempt(userid);
        if (_executor) {
            _executor->submit(
                [this, userid] { _app.updateUserState(userid, UserState::Offline); },
                [] {});
        }
    }
}

void ChatService::reset() {
    auto& connPool = ConnectionPool::getInstance();
    ConnectionPool::AcquireResult acq = connPool.acquire(5000);
    if (!acq.lease) {
        return;
    }
    MySQL* mysql = acq.lease.get();
    mysql->update("UPDATE User SET state = 'offline' WHERE state = 'online'");
}
