#include "ChatService.hpp"
#include <iostream>
#include <string>
#include <vector>

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
    json js = json::parse(msg);
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
    string pwd = js["password"];
    
    auto& connPool = ConnectionPool::getInstance();
    auto mysql = connPool.getConnection();
    
    char sql[1024] = {0};
    sprintf(sql, "SELECT id, name, password, state FROM User WHERE id = %d AND password = '%s'", id, pwd.c_str());
    
    MYSQL_RES* res = mysql->query(sql);
    if (res != nullptr) {
        MYSQL_ROW row = mysql_fetch_row(res);
        if (row != nullptr) {
            string state = row[3];
            if (state == "online") {
                json response;
                response["msgid"] = LOGIN_MSG_ACK;
                response["errno"] = 2;
                response["errmsg"] = "this account is using, input another!";
                conn->send(response.dump());
            } else {
                {
                    lock_guard<mutex> lock(_connMutex);
                    _userConnMap.insert({id, conn});
                }
                
                mysql_free_result(res);
                sprintf(sql, "UPDATE User SET state = 'online' WHERE id = %d", id);
                mysql->update(sql);
                
                json response;
                response["msgid"] = LOGIN_MSG_ACK;
                response["errno"] = 0;
                response["id"] = id;
                response["name"] = row[1];
                
                sprintf(sql, "SELECT friendid, name FROM Friend, User WHERE Friend.userid = %d AND Friend.friendid = User.id", id);
                res = mysql->query(sql);
                if (res != nullptr) {
                    vector<string> vec;
                    while ((row = mysql_fetch_row(res)) != nullptr) {
                        vec.push_back(row[1]);
                    }
                    response["friends"] = vec;
                    mysql_free_result(res);
                }
                
                conn->send(response.dump());
            }
        } else {
            json response;
            response["msgid"] = LOGIN_MSG_ACK;
            response["errno"] = 1;
            response["errmsg"] = "userid or password is invalid!";
            conn->send(response.dump());
        }
    }
}

void ChatService::reg(const TcpConnectionPtr& conn, json& js, Timestamp time) {
    string name = js["name"];
    string pwd = js["password"];
    
    auto& connPool = ConnectionPool::getInstance();
    auto mysql = connPool.getConnection();
    
    char sql[1024] = {0};
    sprintf(sql, "SELECT id FROM User WHERE name = '%s'", name.c_str());
    
    MYSQL_RES* res = mysql->query(sql);
    if (res != nullptr) {
        MYSQL_ROW row = mysql_fetch_row(res);
        if (row != nullptr) {
            json response;
            response["msgid"] = REG_MSG_ACK;
            response["errno"] = 1;
            response["errmsg"] = "this name is already exist!";
            conn->send(response.dump());
            mysql_free_result(res);
            return;
        }
        mysql_free_result(res);
    }
    
    sprintf(sql, "INSERT INTO User(name, password) VALUES('%s', '%s')", name.c_str(), pwd.c_str());
    if (mysql->update(sql)) {
        json response;
        response["msgid"] = REG_MSG_ACK;
        response["errno"] = 0;
        response["id"] = mysql_insert_id(mysql->getConnection());
        conn->send(response.dump());
    } else {
        json response;
        response["msgid"] = REG_MSG_ACK;
        response["errno"] = 1;
        response["errmsg"] = "register failed!";
        conn->send(response.dump());
    }
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
    auto mysql = connPool.getConnection();
    char sql[1024] = {0};
    sprintf(sql, "UPDATE User SET state = 'offline' WHERE id = %d", userid);
    mysql->update(sql);
}

void ChatService::oneChat(const TcpConnectionPtr& conn, json& js, Timestamp time) {
    int toid = js["toid"].get<int>();
    {
        lock_guard<mutex> lock(_connMutex);
        auto it = _userConnMap.find(toid);
        if (it != _userConnMap.end()) {
            it->second->send(js.dump());
            return;
        }
    }
    
    auto& connPool = ConnectionPool::getInstance();
    auto mysql = connPool.getConnection();
    
    char sql[1024] = {0};
    sprintf(sql, "INSERT INTO OfflineMessage VALUES(NULL, %d, '%s')", toid, js.dump().c_str());
    mysql->update(sql);
}

void ChatService::addFriend(const TcpConnectionPtr& conn, json& js, Timestamp time) {
    int userid = js["id"].get<int>();
    int friendid = js["friendid"].get<int>();
    
    auto& connPool = ConnectionPool::getInstance();
    auto mysql = connPool.getConnection();
    char sql[1024] = {0};
    sprintf(sql, "INSERT INTO Friend VALUES(%d, %d)", userid, friendid);
    mysql->update(sql);
}

void ChatService::createGroup(const TcpConnectionPtr& conn, json& js, Timestamp time) {
    int userid = js["id"].get<int>();
    string groupname = js["groupname"];
    string groupdesc = js["groupdesc"];
    
    auto& connPool = ConnectionPool::getInstance();
    auto mysql = connPool.getConnection();
    char sql[1024] = {0};
    sprintf(sql, "INSERT INTO AllGroup(groupname, groupdesc) VALUES('%s', '%s')", groupname.c_str(), groupdesc.c_str());
    if (mysql->update(sql)) {
        int groupid = mysql_insert_id(mysql->getConnection());
        sprintf(sql, "INSERT INTO GroupUser VALUES(%d, %d, 'creator')", groupid, userid);
        mysql->update(sql);
    }
}

void ChatService::addGroup(const TcpConnectionPtr& conn, json& js, Timestamp time) {
    int userid = js["id"].get<int>();
    int groupid = js["groupid"].get<int>();
    
    auto& connPool = ConnectionPool::getInstance();
    auto mysql = connPool.getConnection();
    char sql[1024] = {0};
    sprintf(sql, "INSERT INTO GroupUser VALUES(%d, %d, 'normal')", groupid, userid);
    mysql->update(sql);
}

void ChatService::groupChat(const TcpConnectionPtr& conn, json& js, Timestamp time) {
    int userid = js["id"].get<int>();
    int groupid = js["groupid"].get<int>();
    
    auto& connPool = ConnectionPool::getInstance();
    auto mysql = connPool.getConnection();
    char sql[1024] = {0};
    sprintf(sql, "SELECT userid FROM GroupUser WHERE groupid = %d AND userid != %d", groupid, userid);
    
    MYSQL_RES* res = mysql->query(sql);
    if (res != nullptr) {
        MYSQL_ROW row;
        while ((row = mysql_fetch_row(res)) != nullptr) {
            int toid = atoi(row[0]);
            lock_guard<mutex> lock(_connMutex);
            auto it = _userConnMap.find(toid);
            if (it != _userConnMap.end()) {
                it->second->send(js.dump());
            } else {
                sprintf(sql, "INSERT INTO OfflineMessage VALUES(NULL, %d, '%s')", toid, js.dump().c_str());
                mysql->update(sql);
            }
        }
        mysql_free_result(res);
    }
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
        auto mysql = connPool.getConnection();
        char sql[1024] = {0};
        sprintf(sql, "UPDATE User SET state = 'offline' WHERE id = %d", userid);
        mysql->update(sql);
    }
}

void ChatService::reset() {
    auto& connPool = ConnectionPool::getInstance();
    auto mysql = connPool.getConnection();
    mysql->update("UPDATE User SET state = 'offline' WHERE state = 'online'");
}
