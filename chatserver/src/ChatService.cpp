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
    : _mysqlUsers(ConnectionPool::getInstance()), _app(&_mysqlUsers) {
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

// 在 worker 线程执行：认证通过后查询好友列表与离线消息（SQL 与 P2-05 前一致）。
void loadLoginExtras(LoginDbResult* r)
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

    snprintf(sql, sizeof(sql), "SELECT message FROM OfflineMessage WHERE userid = %d",
             static_cast<int>(r->auth.id));
    MYSQL_RES* resOffline = mysql->query(sql);
    if (resOffline) {
        MySQLResultGuard guardOffline(resOffline);
        MYSQL_ROW rowOff;
        while ((rowOff = mysql_fetch_row(resOffline)) != nullptr) {
            if (rowOff[0]) {
                r->offlineMessages.push_back(rowOff[0]);
            }
        }
    }

    snprintf(sql, sizeof(sql), "DELETE FROM OfflineMessage WHERE userid = %d",
             static_cast<int>(r->auth.id));
    mysql->update(sql);
}

} // namespace

void ChatService::bindLoop(EventLoop* loop)
{
    _loop = loop;
    _executor.reset(new BlockingExecutor(loop, 2, 64));
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
                loadLoginExtras(result.get());
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
            it->second->send(js.dump() + "\n");
            json response = js;
            response["errno"] = 0;
            conn->send(response.dump() + "\n");
            return;
        }
    }
    
    auto& connPool = ConnectionPool::getInstance();
    ConnectionPool::AcquireResult acq = connPool.acquire(5000);
    if (!acq.lease) {
        json response = js;
        response["errno"] = 1;
        conn->send(response.dump() + "\n");
        return;
    }
    MySQL* mysql = acq.lease.get();
    
    char sql[1024] = {0};
    std::string escapedMsg = escapeString(mysql->getConnection(), js.dump());
    snprintf(sql, sizeof(sql), "INSERT INTO OfflineMessage VALUES(NULL, %d, '%s')", toid, escapedMsg.c_str());
    bool ok = mysql->update(sql);

    json response = js;
    response["errno"] = ok ? 0 : 1;
    conn->send(response.dump() + "\n");
}

void ChatService::addFriend(const TcpConnectionPtr& conn, json& js, Timestamp time) {
    int userid = js["id"].get<int>();
    int friendid = js["friendid"].get<int>();
    
    auto& connPool = ConnectionPool::getInstance();
    ConnectionPool::AcquireResult acq = connPool.acquire(5000);
    if (!acq.lease) {
        json response = js;
        response["errno"] = 1;
        conn->send(response.dump() + "\n");
        return;
    }
    MySQL* mysql = acq.lease.get();
    char sql[1024] = {0};
    snprintf(sql, sizeof(sql), "INSERT INTO Friend VALUES(%d, %d)", userid, friendid);
    bool ok = mysql->update(sql);

    json response = js;
    response["errno"] = ok ? 0 : 1;
    conn->send(response.dump() + "\n");
}

void ChatService::createGroup(const TcpConnectionPtr& conn, json& js, Timestamp time) {
    int userid = js["id"].get<int>();
    string groupname = js["groupname"];
    string groupdesc = js["groupdesc"];
    
    auto& connPool = ConnectionPool::getInstance();
    ConnectionPool::AcquireResult acq = connPool.acquire(5000);
    if (!acq.lease) {
        json response;
        response["msgid"] = CREATE_GROUP_MSG;
        response["id"] = userid;
        response["errno"] = 1;
        conn->send(response.dump() + "\n");
        return;
    }
    MySQL* mysql = acq.lease.get();
    char sql[1024] = {0};
    std::string escapedGroupname = escapeString(mysql->getConnection(), groupname);
    std::string escapedGroupdesc = escapeString(mysql->getConnection(), groupdesc);
    snprintf(sql, sizeof(sql), "INSERT INTO AllGroup(groupname, groupdesc) VALUES('%s', '%s')", escapedGroupname.c_str(), escapedGroupdesc.c_str());
    json response;
    response["msgid"] = CREATE_GROUP_MSG;
    response["id"] = userid;

    if (mysql->update(sql)) {
        int groupid = mysql_insert_id(mysql->getConnection());
        response["groupid"] = groupid;
        snprintf(sql, sizeof(sql), "INSERT INTO GroupUser VALUES(%d, %d, 'creator')", groupid, userid);
        bool ok = mysql->update(sql);
        response["errno"] = ok ? 0 : 1;
    } else {
        response["errno"] = 1;
    }
    conn->send(response.dump() + "\n");
}

void ChatService::addGroup(const TcpConnectionPtr& conn, json& js, Timestamp time) {
    int userid = js["id"].get<int>();
    int groupid = js["groupid"].get<int>();
    
    auto& connPool = ConnectionPool::getInstance();
    ConnectionPool::AcquireResult acq = connPool.acquire(5000);
    if (!acq.lease) {
        json response = js;
        response["errno"] = 1;
        conn->send(response.dump() + "\n");
        return;
    }
    MySQL* mysql = acq.lease.get();
    char sql[1024] = {0};
    snprintf(sql, sizeof(sql), "INSERT INTO GroupUser VALUES(%d, %d, 'normal')", groupid, userid);
    bool ok = mysql->update(sql);

    json response = js;
    response["errno"] = ok ? 0 : 1;
    conn->send(response.dump() + "\n");
}

void ChatService::groupChat(const TcpConnectionPtr& conn, json& js, Timestamp time) {
    int userid = js["id"].get<int>();
    int groupid = js["groupid"].get<int>();

    auto& connPool = ConnectionPool::getInstance();
    ConnectionPool::AcquireResult acq = connPool.acquire(5000);
    if (!acq.lease) {
        json response = js;
        response["errno"] = 1;
        conn->send(response.dump() + "\n");
        return;
    }
    MySQL* mysql = acq.lease.get();
    char sql[1024] = {0};
    snprintf(sql, sizeof(sql), "SELECT userid FROM GroupUser WHERE groupid = %d AND userid != %d", groupid, userid);

    MYSQL_RES* res = mysql->query(sql);
    if (res != nullptr) {
        MySQLResultGuard guard(res);
        std::vector<int> toids;
        MYSQL_ROW row;
        while ((row = mysql_fetch_row(res)) != nullptr) {
            toids.push_back(atoi(row[0]));
        }

        // 收集在线用户的连接指针和ID集合
        std::vector<std::pair<int, TcpConnectionPtr>> onlineUsers;
        std::unordered_set<int> onlineIds;
        {
            lock_guard<mutex> lock(_connMutex);
            for (int toid : toids) {
                auto it = _userConnMap.find(toid);
                if (it != _userConnMap.end()) {
                    onlineUsers.emplace_back(toid, it->second);
                    onlineIds.insert(toid);
                }
            }
        }

        // 发送消息给在线用户
        std::string msg = js.dump() + "\n";
        for (auto& userPair : onlineUsers) {
            userPair.second->send(msg);
        }

        // 处理离线用户
        std::string escapedMsg = escapeString(mysql->getConnection(), js.dump());
        for (int toid : toids) {
            if (onlineIds.find(toid) == onlineIds.end()) {
                snprintf(sql, sizeof(sql), "INSERT INTO OfflineMessage VALUES(NULL, %d, '%s')", toid, escapedMsg.c_str());
                mysql->update(sql);
            }
        }
    }

    // 给群聊发送者一个确认，避免客户端阻塞等待
    json response = js;
    response["errno"] = 0;
    conn->send(response.dump() + "\n");
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
