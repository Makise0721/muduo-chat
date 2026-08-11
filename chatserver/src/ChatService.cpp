#include "ChatService.hpp"
#include "app/ChatApplication.hpp"
#include "db/MySQLGuards.hpp"
#include <iostream>
#include <exception>
#include <string>
#include <vector>

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
        cerr << "json parse failed: " << e.what() << ", len=" << msg.size() << endl;
        return;
    }

    try {
        if (!js.contains("msgid")) {
            cerr << "msg has no msgid" << endl;
            return;
        }

        int msgid = js["msgid"].get<int>();

        auto it = _msgHandlerMap.find(msgid);
        if (it == _msgHandlerMap.end()) {
            cerr << "msgid: " << msgid << " can not find handler!" << endl;
            return;
        }

        it->second(conn, js, time);
    } catch (const std::exception& e) {
        // 单包异常兜底：缺字段/类型错等一律不崩溃、不打印消息原文（可能含密码）。
        cerr << "handler exception: " << e.what() << endl;
    } catch (...) {
        cerr << "handler exception: unknown" << endl;
    }
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

// completion 早退（会话过期/连接断开）时离线消息已被 worker 取出却无法投递：
// 恢复入队避免永久丢失；executor 已 shutdown（RejectedShutdown）时丢弃可接受。
void restoreOfflineMessages(BlockingExecutor* executor, ChatApplication* app,
                            int64_t id, const std::shared_ptr<LoginDbResult>& result)
{
    if (result->offlineMessages.empty()) {
        return;
    }
    executor->submit(
        [app, id, result] {
            for (size_t i = 0; i < result->offlineMessages.size(); ++i) {
                app->storeOfflineMessage(id, result->offlineMessages[i]);
            }
        },
        [] {});
}

} // namespace

void ChatService::bindLoop(EventLoop* loop, int executorWorkers, int executorQueueCapacity)
{
    _loop = loop;
    // 单 worker：同一连接的串行依赖（如 addFriend 后立即重复 add）按提交顺序
    // 执行；多 worker 会乱序破坏业务语义（P2-10 性能评估后再分片扩并）。
    _executor.reset(new BlockingExecutor(loop, executorWorkers, executorQueueCapacity));
}

void ChatService::shutdownApp()
{
    if (_executor) {
        _executor->shutdown();
    }
}

int ChatService::executorQueueDepth() const
{
    return _executor ? _executor->queueDepth() : 0;
}

uint64_t ChatService::executorDroppedFull() const
{
    return _executor ? _executor->droppedFull() : 0;
}

uint64_t ChatService::executorDroppedShutdown() const
{
    return _executor ? _executor->droppedShutdown() : 0;
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
            // state 只读不改：单会话以 SessionRegistry（活动连接）为真相，
            // 断开残留的 DB online 由 completion 区分处理。
            if (result->auth.ok && _app.isSessionCurrent(id, gen)) {
                loadLoginExtras(&_app, result.get());
            }
        },
        [this, conn, id, gen, result] {
            // stale：会话已被登出/断开/新登录取代，不写状态、不响应。
            if (!_app.isSessionCurrent(id, gen)) {
                restoreOfflineMessages(_executor.get(), &_app, id, result);
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
            // 连接已断开：不建立会话（防断线竞态锁死用户），不响应。
            if (!conn->connected()) {
                restoreOfflineMessages(_executor.get(), &_app, id, result);
                return;
            }
            {
                // P3-05：连接绑定一个认证 Session（B-21 收紧）。bind 单锁原子判定：
                // UserBusy=同用户已有活动会话（B-08 保留），ConnectionBusy=该连接
                // 已绑定会话（同 User 或另一 User 的二次登录均被拒）。
                SessionRegistry::BindResult br = _sessions.bind(conn, id, gen);
                if (br == SessionRegistry::BindResult::UserBusy) {
                    response["errno"] = 2;
                    response["errmsg"] = "this account is using, input another!";
                    conn->send(response.dump() + "\n");
                    return;
                }
                if (br == SessionRegistry::BindResult::ConnectionBusy) {
                    response["errno"] = 2;
                    response["errmsg"] = "this connection has already logged in!";
                    conn->send(response.dump() + "\n");
                    return;
                }
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
    if (!_executor) {
        json response;
        response["msgid"] = REG_MSG_ACK;
        response["errno"] = 1;
        response["errmsg"] = "db unavailable!";
        conn->send(response.dump() + "\n");
        return;
    }

    Command cmd;
    cmd.type = Command::Type::Register;
    cmd.name = js["name"].get<std::string>();
    cmd.password = js["password"].get<std::string>();
    cmd.raw = js.dump();

    auto reply = std::make_shared<Reply>();
    _executor->submit(
        [this, cmd, reply] { _app.handle(SessionContext(), cmd, reply.get()); },
        [conn, reply] {
            json response;
            response["msgid"] = REG_MSG_ACK;
            response["errno"] = reply->errnoCode;
            if (reply->errnoCode == 0) {
                response["id"] = reply->userId;
            } else {
                response["errmsg"] = reply->errmsg;
            }
            conn->send(response.dump() + "\n");
        });
}

void ChatService::loginout(const TcpConnectionPtr& conn, json& js, Timestamp time) {
    int userid = js["id"].get<int>();
    // B-10 语义保留：登出按 payload id、未登录也成功（幂等）；经 registry
    // 释放保持双向一致性（同连接/同用户恰好一次）。
    _sessions.unbindUser(userid);

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
    // 消息超长（离线 payload 存 OfflineMessage.message VARCHAR(500)）：
    // 按整条序列化 payload 判定，在线/离线路径一致拒绝。
    if (js.dump().size() > 500) {
        json response = js;
        response["errno"] = 1;
        response["errmsg"] = "message too long";
        conn->send(response.dump() + "\n");
        return;
    }
    // P3-05 消息主体只来自 Session：未登录拒绝；payload id 与 Session user
    // 不符拒绝（不信任 payload；缺 id 字段时无比较对象，照常放行）。
    BoundSession session;
    if (!_sessions.lookupByConnection(conn, &session)) {
        json response = js;
        response["errno"] = 1;
        response["errmsg"] = "please login first!";
        conn->send(response.dump() + "\n");
        return;
    }
    if (js.contains("id") && js["id"].get<int>() != session.userId) {
        json response = js;
        response["errno"] = 1;
        response["errmsg"] = "invalid sender!";
        conn->send(response.dump() + "\n");
        return;
    }
    {
        TcpConnectionPtr target = _sessions.lookupByUser(toid);
        if (target) {
            // 目标在线：原样转发（内存操作，无 SQL），随后回发送者确认。
            target->send(js.dump() + "\n");
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
        [conn, payload, result] {
            json response = json::parse(payload);
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
    std::string payload = js.dump();
    _executor->submit(
        [this, userid, friendid, result] { *result = _app.addFriend(userid, friendid); },
        [conn, payload, result] {
            json response = json::parse(payload);
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
    std::string payload = js.dump();
    _executor->submit(
        [this, userid, groupid, result] { *result = _app.joinGroup(groupid, userid); },
        [conn, payload, result] {
            json response = json::parse(payload);
            response["errno"] = result->ok ? 0 : 1;
            conn->send(response.dump() + "\n");
        });
}

void ChatService::groupChat(const TcpConnectionPtr& conn, json& js, Timestamp time) {
    int groupid = js["groupid"].get<int>();
    // 消息超长（离线 payload 存 OfflineMessage.message VARCHAR(500)）：
    // 按整条序列化 payload 判定，在线/离线路径一致拒绝。
    if (js.dump().size() > 500) {
        json response = js;
        response["errno"] = 1;
        response["errmsg"] = "message too long";
        conn->send(response.dump() + "\n");
        return;
    }
    // P3-05 消息主体只来自 Session（见 oneChat 注释）。
    BoundSession session;
    if (!_sessions.lookupByConnection(conn, &session)) {
        json response = js;
        response["errno"] = 1;
        response["errmsg"] = "please login first!";
        conn->send(response.dump() + "\n");
        return;
    }
    if (js.contains("id") && js["id"].get<int>() != session.userId) {
        json response = js;
        response["errno"] = 1;
        response["errmsg"] = "invalid sender!";
        conn->send(response.dump() + "\n");
        return;
    }

    if (!_executor) {
        json response = js;
        response["errno"] = 1;
        conn->send(response.dump() + "\n");
        return;
    }
    auto members = std::make_shared<MembersResult>();
    std::string payload = js.dump();
    _executor->submit(
        [this, groupid, members] { *members = _app.groupMembers(groupid); },
        [this, conn, session, members, payload] {
            // 在线成员：单锁快照连接后原样转发（内存操作，无 SQL）。
            std::unordered_map<int64_t, TcpConnectionPtr> online =
                _sessions.snapshotConnections(members->userIds, session.userId);
            std::string msg = payload + "\n";
            for (auto& kv : online) {
                kv.second->send(msg);
            }
            // 离线成员：异步入队（每条一个任务，单 worker FIFO 保序）。
            for (size_t i = 0; i < members->userIds.size(); ++i) {
                int64_t toid = members->userIds[i];
                if (toid == session.userId || online.find(toid) != online.end()) {
                    continue;
                }
                _executor->submit(
                    [this, toid, payload] { _app.storeOfflineMessage(toid, payload); },
                    [] {});
            }
            // 给群聊发送者一个确认，避免客户端阻塞等待（B-19：查询失败也回 0）。
            json response = json::parse(payload);
            response["errno"] = 0;
            conn->send(response.dump() + "\n");
        });
}

void ChatService::clientCloseException(const TcpConnectionPtr& conn) {
    // P3-05：按连接解绑（幂等，恰好释放一次）；未绑定返回 0，不产生状态写入。
    int64_t userid = _sessions.unbind(conn);

    if (userid != 0) {
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
