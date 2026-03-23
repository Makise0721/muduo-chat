#include "ChatService.hpp"
#include <iostream>
#include <exception>
#include <string>
#include <vector>

namespace {
// 确保从 ConnectionPool 取出的连接一定会归还，避免连接池耗尽导致阻塞。
struct MySQLConnectionGuard {
    ConnectionPool& pool;
    std::shared_ptr<MySQL> mysql;

    MySQLConnectionGuard(ConnectionPool& p, std::shared_ptr<MySQL> c)
        : pool(p), mysql(std::move(c)) {}

    ~MySQLConnectionGuard() {
        pool.releaseConnection(mysql);
    }

    MySQL* operator->() { return mysql.get(); }
    const MySQL* operator->() const { return mysql.get(); }
};

// 确保 mysql_use_result() 产生的结果集会释放。
struct MySQLResultGuard {
    MYSQL_RES* res;
    explicit MySQLResultGuard(MYSQL_RES* r) : res(r) {}
    ~MySQLResultGuard() {
        if (res) {
            mysql_free_result(res);
        }
    }
};
} // namespace

ChatService::ChatService() {
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

void ChatService::login(const TcpConnectionPtr& conn, json& js, Timestamp time) {
    int id = js["id"].get<int>();
    std::string pwd = js["password"].get<std::string>();

    auto& connPool = ConnectionPool::getInstance();
    MySQLConnectionGuard mysql(connPool, connPool.getConnection());

    char sql[1024] = {0};
    sprintf(sql, "SELECT id, name, password, state FROM User WHERE id = %d AND password = '%s'", id, pwd.c_str());

    MYSQL_RES* resUser = mysql->query(sql);
    if (!resUser) {
        json response;
        response["msgid"] = LOGIN_MSG_ACK;
        response["errno"] = 1;
        response["errmsg"] = "userid or password is invalid!";
        conn->send(response.dump() + "\n");
        return;
    }
    MySQLResultGuard guardUser(resUser);

    MYSQL_ROW rowUser = mysql_fetch_row(resUser);
    if (!rowUser) {
        json response;
        response["msgid"] = LOGIN_MSG_ACK;
        response["errno"] = 1;
        response["errmsg"] = "userid or password is invalid!";
        conn->send(response.dump() + "\n");
        return;
    }

    std::string state = rowUser[3] ? rowUser[3] : "";
    if (state == "online") {
        json response;
        response["msgid"] = LOGIN_MSG_ACK;
        response["errno"] = 2;
        response["errmsg"] = "this account is using, input another!";
        conn->send(response.dump() + "\n");
        return;
    }

    {
        lock_guard<mutex> lock(_connMutex);
        _userConnMap.insert({id, conn});
    }

    sprintf(sql, "UPDATE User SET state = 'online' WHERE id = %d", id);
    mysql->update(sql);

    json response;
    response["msgid"] = LOGIN_MSG_ACK;
    response["errno"] = 0;
    response["id"] = id;
    response["name"] = rowUser[1] ? rowUser[1] : "";

    // 好友列表
    sprintf(sql, "SELECT friendid, name FROM Friend, User WHERE Friend.userid = %d AND Friend.friendid = User.id", id);
    MYSQL_RES* resFriends = mysql->query(sql);
    if (resFriends) {
        MySQLResultGuard guardFriends(resFriends);
        vector<string> vec;
        MYSQL_ROW rowFriend;
        while ((rowFriend = mysql_fetch_row(resFriends)) != nullptr) {
            if (rowFriend[1]) {
                vec.push_back(rowFriend[1]);
            }
        }
        response["friends"] = vec;
    }

    // 先回登录应答，客户端更容易按顺序处理
    conn->send(response.dump() + "\n");

    // 离线消息下发
    sprintf(sql, "SELECT message FROM OfflineMessage WHERE userid = %d", id);
    MYSQL_RES* resOffline = mysql->query(sql);
    if (resOffline) {
        MySQLResultGuard guardOffline(resOffline);
        MYSQL_ROW rowOff;
        while ((rowOff = mysql_fetch_row(resOffline)) != nullptr) {
            if (rowOff[0]) {
                conn->send(std::string(rowOff[0]) + "\n");
            }
        }
    }

    sprintf(sql, "DELETE FROM OfflineMessage WHERE userid = %d", id);
    mysql->update(sql);
}

void ChatService::reg(const TcpConnectionPtr& conn, json& js, Timestamp time) {
    std::string name = js["name"].get<std::string>();
    std::string pwd = js["password"].get<std::string>();

    auto& connPool = ConnectionPool::getInstance();
    MySQLConnectionGuard mysql(connPool, connPool.getConnection());

    char sql[1024] = {0};
    sprintf(sql, "SELECT id FROM User WHERE name = '%s'", name.c_str());

    MYSQL_RES* resName = mysql->query(sql);
    if (resName != nullptr) {
        MySQLResultGuard guardName(resName);
        MYSQL_ROW row = mysql_fetch_row(resName);
        if (row != nullptr) {
            json response;
            response["msgid"] = REG_MSG_ACK;
            response["errno"] = 1;
            response["errmsg"] = "this name is already exist!";
            conn->send(response.dump() + "\n");
            return;
        }
    }

    sprintf(sql, "INSERT INTO User(name, password) VALUES('%s', '%s')", name.c_str(), pwd.c_str());
    json response;
    response["msgid"] = REG_MSG_ACK;
    if (mysql->update(sql)) {
        response["errno"] = 0;
        response["id"] = mysql_insert_id(mysql->getConnection());
    } else {
        response["errno"] = 1;
        response["errmsg"] = "register failed!";
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
    
    auto& connPool = ConnectionPool::getInstance();
    MySQLConnectionGuard mysql(connPool, connPool.getConnection());
    char sql[1024] = {0};
    sprintf(sql, "UPDATE User SET state = 'offline' WHERE id = %d", userid);
    mysql->update(sql);

    json response;
    response["msgid"] = LOGINOUT_MSG;
    response["errno"] = 0;
    conn->send(response.dump() + "\n");
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
    MySQLConnectionGuard mysql(connPool, connPool.getConnection());
    
    char sql[1024] = {0};
    sprintf(sql, "INSERT INTO OfflineMessage VALUES(NULL, %d, '%s')", toid, js.dump().c_str());
    bool ok = mysql->update(sql);

    json response = js;
    response["errno"] = ok ? 0 : 1;
    conn->send(response.dump() + "\n");
}

void ChatService::addFriend(const TcpConnectionPtr& conn, json& js, Timestamp time) {
    int userid = js["id"].get<int>();
    int friendid = js["friendid"].get<int>();
    
    auto& connPool = ConnectionPool::getInstance();
    MySQLConnectionGuard mysql(connPool, connPool.getConnection());
    char sql[1024] = {0};
    sprintf(sql, "INSERT INTO Friend VALUES(%d, %d)", userid, friendid);
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
    MySQLConnectionGuard mysql(connPool, connPool.getConnection());
    char sql[1024] = {0};
    sprintf(sql, "INSERT INTO AllGroup(groupname, groupdesc) VALUES('%s', '%s')", groupname.c_str(), groupdesc.c_str());
    json response;
    response["msgid"] = CREATE_GROUP_MSG;
    response["id"] = userid;

    if (mysql->update(sql)) {
        int groupid = mysql_insert_id(mysql->getConnection());
        response["groupid"] = groupid;
        sprintf(sql, "INSERT INTO GroupUser VALUES(%d, %d, 'creator')", groupid, userid);
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
    MySQLConnectionGuard mysql(connPool, connPool.getConnection());
    char sql[1024] = {0};
    sprintf(sql, "INSERT INTO GroupUser VALUES(%d, %d, 'normal')", groupid, userid);
    bool ok = mysql->update(sql);

    json response = js;
    response["errno"] = ok ? 0 : 1;
    conn->send(response.dump() + "\n");
}

void ChatService::groupChat(const TcpConnectionPtr& conn, json& js, Timestamp time) {
    int userid = js["id"].get<int>();
    int groupid = js["groupid"].get<int>();
    
    auto& connPool = ConnectionPool::getInstance();
    MySQLConnectionGuard mysql(connPool, connPool.getConnection());
    char sql[1024] = {0};
    sprintf(sql, "SELECT userid FROM GroupUser WHERE groupid = %d AND userid != %d", groupid, userid);
    
    MYSQL_RES* res = mysql->query(sql);
    if (res != nullptr) {
        MySQLResultGuard guard(res);
        MYSQL_ROW row;
        while ((row = mysql_fetch_row(res)) != nullptr) {
            int toid = atoi(row[0]);
            lock_guard<mutex> lock(_connMutex);
            auto it = _userConnMap.find(toid);
            if (it != _userConnMap.end()) {
                it->second->send(js.dump() + "\n");
            } else {
                sprintf(sql, "INSERT INTO OfflineMessage VALUES(NULL, %d, '%s')", toid, js.dump().c_str());
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
        auto& connPool = ConnectionPool::getInstance();
        MySQLConnectionGuard mysql(connPool, connPool.getConnection());
        char sql[1024] = {0};
        sprintf(sql, "UPDATE User SET state = 'offline' WHERE id = %d", userid);
        mysql->update(sql);
    }
}

void ChatService::reset() {
    auto& connPool = ConnectionPool::getInstance();
    MySQLConnectionGuard mysql(connPool, connPool.getConnection());
    mysql->update("UPDATE User SET state = 'offline' WHERE state = 'online'");
}
