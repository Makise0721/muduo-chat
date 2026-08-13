#include "ChatService.hpp"
#include "app/ChatApplication.hpp"
#include "app/ProtocolCodec.hpp"
#include "db/MySQLGuards.hpp"
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <exception>
#include <string>
#include <vector>

namespace {

// 失败回复（决策表第 4/10 行）：v2 → msgid=13 + errno + errmsg（+client_message_id
// 可解析时回显）；legacy → 旧格式回显 errno=1 + errmsg。errmsg 一律由 errno 派生
// （protocolErrmsg），双通道字符串与 errno 保持一致。
void sendFailureReply(const TcpConnectionPtr& conn, const json& js,
                      const ParsedChatMessage& parsed, int errnoCode)
{
    const char* errmsg = protocolErrmsg(errnoCode);
    if (parsed.legacy) {
        json response = js;
        response["errno"] = 1;
        response["errmsg"] = errmsg;
        conn->send(response.dump() + "\n");
        return;
    }
    const std::string* cmid = parsed.hasClientMessageId ? &parsed.clientMessageId : nullptr;
    conn->send(buildErrorReply(ERROR_RESP_MSG, errnoCode, errmsg, cmid).dump() + "\n");
}

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
    _msgHandlerMap.insert({DELIVERY_ACK_MSG, bind(&ChatService::deliveryAck, this, placeholders::_1, placeholders::_2, placeholders::_3)});
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
    // P3-07：注册真实投递出口（wiring() 惰性构造时绑定；本调用先于首个
    // accept/claim，无竞态）。不同出口重复注册会保留原绑定并 fail-fast。
    if (!registerDeliverySink(&_deliverySink)) {
        std::cerr << "delivery sink registration failed" << std::endl;
        std::exit(EXIT_FAILURE);
    }
    // 单 worker：同一连接的串行依赖（如 addFriend 后立即重复 add）按提交顺序
    // 执行；多 worker 会乱序破坏业务语义（P2-10 性能评估后再分片扩并）。
    _executor.reset(new BlockingExecutor(loop, executorWorkers, executorQueueCapacity));
    // P3-08：server 就绪（pool 已 init、sink 已绑定）后启动可靠消息内部
    // scheduler（timer 驱动、幂等）；stop 在 shutdownApp（EXECUTOR_SHUTDOWN 后、
    // POOL_SHUTDOWN 前）有界 join。
    startReliableMessaging();
}

void ChatService::shutdownApp()
{
    if (_executor) {
        _executor->shutdown();
    }
    // P3-08：scheduler 线程先于 ConnectionPool 失效退出（有界 join；wiring 未
    // 构造时 no-op）。顺序：EXECUTOR_SHUTDOWN → MESSAGING_STOP → POOL_SHUTDOWN。
    std::cout << "MESSAGING_STOP" << std::endl;
    stopReliableMessaging(5000);
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
            // 防御性检查：连接已断开则不建会话、不响应。权威判定在 bind 的
            // 活跃集合锁内检查（close 回调先执行时 removeConnection 已生效，
            // bind 拒绝 ConnectionInactive；bind 先执行时 close 的
            // removeConnection+unbind 后到恰好释放——无窗口）。
            if (!conn->connected()) {
                restoreOfflineMessages(_executor.get(), &_app, id, result);
                return;
            }
            {
                // P3-05：连接绑定一个认证 Session（B-21 收紧）。bind 单锁原子判定：
                // ConnectionInactive=连接已从活跃集合移除（close 先于本 completion，
                // 不建会话）；UserBusy=同用户已有活动会话（B-08 保留），
                // ConnectionBusy=该连接已绑定会话（同 User 或另一 User 的二次登录均被拒）。
                SessionRegistry::BindResult br = _sessions.bind(conn, id, gen);
                if (br == SessionRegistry::BindResult::ConnectionInactive) {
                    // close 先于 bind：连接已死，不建会话、不响应（防断线竞态锁死用户）。
                    restoreOfflineMessages(_executor.get(), &_app, id, result);
                    return;
                }
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

            // 离线消息下发（补投后 OfflineMessage 已在 worker 清空；旧表补投
            // 保留至 P3-08 由新路径替换）
            for (size_t i = 0; i < result->offlineMessages.size(); ++i) {
                conn->send(result->offlineMessages[i] + "\n");
            }

            // P3-07：在 owner EventLoop 上先安装并 fence pressure callback，再
            // 提交新 Session claim；low-water 恢复也复核同一连接与 generation。
            _deliveryArmer.armSessionDelivery(
                conn, BoundSession(id, gen),
                [this, id, gen] {
                    if (!_executor) {
                        return false;
                    }
                    return _executor->submit(
                               [this, id, gen] {
                                   sessionAvailableDelivery(id, static_cast<uint64_t>(gen));
                               },
                               [] {}) == SubmitResult::Accepted;
                },
                [this, id, gen] {
                    if (!_executor) {
                        return false;
                    }
                    return _executor->submit(
                               [this, id, gen] {
                                   resumeDelivery(id, static_cast<uint64_t>(gen));
                               },
                               [] {}) == SubmitResult::Accepted;
                });
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

void ChatService::deliveryAck(const TcpConnectionPtr& conn, json& js, Timestamp time) {
    // P3-07：接收端按 MessageId 显式确认（spec §2.3）。ACK 主体只来自 Session
    // （P3-05），payload 不提供 UserId；重复/迟到/他人 ACK 由 DeliveryCoordinator
    // 幂等且不越权（spec §4 故障点 4/5）。无 ACK 回执（静默）。缺 message_id /
    // 类型错 → js.at 抛异常 → handler 层 B-25 静默。
    BoundSession session;
    if (!_sessions.lookupByConnection(conn, &session)) {
        return;  // 未登录：忽略（不越权）
    }
    if (!_executor) {
        return;
    }
    const uint64_t messageId = js.at("message_id").get<uint64_t>();
    // P3-08 冻结策略（docs/tasks/P3-08.md rejection carryover）：submit 被拒
    // （RejectedFull/RejectedShutdown）= 丢弃，等价 ACK 丢失——P3-08 ack-timeout
    // 对同一 MessageId 重投即恢复路径，客户端按 message_id 去重；不主动重试。
    _executor->submit(
        [session, messageId] {
            acknowledgeDelivery(session.userId, static_cast<uint64_t>(session.generation),
                                messageId);
        },
        [] {});
}

void ChatService::loginout(const TcpConnectionPtr& conn, json& js, Timestamp time) {
    int userid = js["id"].get<int>();
    // P3-07：登出前捕获本连接绑定 Session，提交 sessionClosed（名下 InFlight
    // 立即回 Pending，spec §3 断开节）。payload id 指向他人会话的病理路径不
    // 处理（记录于任务卡遗留）。
    BoundSession bs;
    _sessions.lookupByConnection(conn, &bs);
    if (bs.userId != 0) {
        _deliveryArmer.clearSessionDelivery(conn, bs);
    }
    // B-10 语义保留：登出按 payload id、未登录也成功（幂等）；经 registry
    // 释放保持双向一致性（同连接/同用户恰好一次）。
    _sessions.unbindUser(userid);

    // 会话代次递增：在途登录 completion 不再生效。
    if (bs.userId != 0) {
        _app.invalidateSessionAttempt(bs.userId, bs.generation);
    } else {
        _app.beginSessionAttempt(userid);
    }
    if (bs.userId != 0 && _executor) {
        // P3-08 冻结策略（docs/tasks/P3-08.md rejection carryover）：submit 被拒
        // （RejectedFull/RejectedShutdown）= 不新增立即动作——名下 InFlight 保持，
        // 由 scheduler 在 lease 到期后回退 Pending（等待重连 claim），与 P3-07
        // 已闭合的 sessionAvailable/pressure resume 拒绝策略对称。
        _executor->submit(
            [bs] {
                sessionClosedDelivery(bs.userId, static_cast<uint64_t>(bs.generation));
            },
            [] {});
    }
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
    // P3-06 codec：client_message_id 判定（缺 → legacy）+ content 16KB 上限
    // （B-13 500B 整包判定废止，决策表第 6 行）；105/107 为 accept 前拒绝。
    // 字段缺失/类型错误抛异常 → handler 层 B-25 静默（与旧行为一致）。
    ParsedChatMessage parsed;
    int codecErr = parseChatMessage(ChatCommandKind::Direct, js, &parsed);
    if (codecErr != 0) {
        sendFailureReply(conn, js, parsed, codecErr);
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
    if (!_executor) {
        json response = js;
        response["errno"] = 1;
        response["errmsg"] = "db unavailable!";
        conn->send(response.dump() + "\n");
        return;
    }

    // P3-06 durable accept：在线直写与 storeOffline 写路径退役（B-11/B-12/B-17
    // 让位 ledger + Delivery 状态机；takeOffline 读删流程保留至 P3-08）。
    // 预检与 accept 在 ProtocolCodec（worker 线程，ReliableMessaging 单一调用者）。
    auto view = std::make_shared<AcceptResultView>();
    _executor->submit(
        [this, session, parsed, view] {
            acceptChatCommand(&_app, session.userId, session.generation, parsed,
                              ChatCommandKind::Direct, view.get());
        },
        [conn, js, parsed, view] {
            if (view->ok) {
                if (parsed.legacy) {
                    // 决策表第 10 行：legacy 成功保持旧格式回显 errno=0。
                    json response = js;
                    response["errno"] = 0;
                    conn->send(response.dump() + "\n");
                } else {
                    // spec §2.2：五字段 MESSAGE_ACCEPTED，事务提交后发出。
                    conn->send(buildMessageAcceptedReply(MESSAGE_ACCEPTED_MSG,
                                                          view->clientMessageId,
                                                          view->messageId,
                                                          view->conversationId,
                                                          view->sequence,
                                                          view->duplicate).dump() + "\n");
                }
                return;
            }
            sendFailureReply(conn, js, parsed, view->errnoCode);
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
    // P3-06 codec（见 oneChat：legacy 判定 + 16KB 上限，105/107 accept 前拒绝）。
    ParsedChatMessage parsed;
    int codecErr = parseChatMessage(ChatCommandKind::Group, js, &parsed);
    if (codecErr != 0) {
        sendFailureReply(conn, js, parsed, codecErr);
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
        response["errmsg"] = "db unavailable!";
        conn->send(response.dump() + "\n");
        return;
    }

    // P3-06 durable accept：成员快照与资格校验在 executor 内（B-18/B-19 收紧：
    // 非成员→101、群不存在→106、查询失败→104）；在线转发/离线入队退役。
    auto view = std::make_shared<AcceptResultView>();
    _executor->submit(
        [this, session, parsed, view] {
            acceptChatCommand(&_app, session.userId, session.generation, parsed,
                              ChatCommandKind::Group, view.get());
        },
        [conn, js, parsed, view] {
            if (view->ok) {
                if (parsed.legacy) {
                    json response = js;
                    response["errno"] = 0;
                    conn->send(response.dump() + "\n");
                } else {
                    conn->send(buildMessageAcceptedReply(MESSAGE_ACCEPTED_MSG,
                                                          view->clientMessageId,
                                                          view->messageId,
                                                          view->conversationId,
                                                          view->sequence,
                                                          view->duplicate).dump() + "\n");
                }
                return;
            }
            sendFailureReply(conn, js, parsed, view->errnoCode);
        });
}

void ChatService::addConnection(const TcpConnectionPtr& conn) {
    _sessions.addConnection(conn);
}

void ChatService::clientCloseException(const TcpConnectionPtr& conn) {
    // P3-07：解绑前捕获 Session（userId+generation），提交 sessionClosed——
    // 名下 InFlight 立即回 Pending（spec §3 断开节；lease 释放）。提交在
    // executor 串行队列，与新 Session 的 login claim 交错安全（leaseOwner 按
    // SessionIdentity 精确匹配，新 owner 的在途不被旧 close 回滚）。
    BoundSession bs;
    _sessions.lookupByConnection(conn, &bs);
    if (bs.userId != 0) {
        _deliveryArmer.clearSessionDelivery(conn, bs);
    }
    // P3-05 对抗审查：先移出活跃连接集合（锁内，登录 completion 的 bind 将
    // 拒绝 ConnectionInactive），再按连接解绑（幂等，恰好释放一次）；
    // 未绑定返回 0，不产生状态写入。
    _sessions.removeConnection(conn);
    int64_t userid = _sessions.unbind(conn);

    if (bs.userId != 0 && _executor) {
        // P3-08 冻结策略（docs/tasks/P3-08.md rejection carryover）：submit 被拒
        // （RejectedFull/RejectedShutdown）= 不新增立即动作——名下 InFlight 保持，
        // 由 scheduler 在 lease 到期后回退 Pending（等待重连 claim），与 P3-07
        // 已闭合的 sessionAvailable/pressure resume 拒绝策略对称。
        _executor->submit(
            [bs] {
                sessionClosedDelivery(bs.userId, static_cast<uint64_t>(bs.generation));
            },
            [] {});
    }
    if (userid != 0) {
        // 仅使本连接捕获的会话代次失效；旧 close 不得推进新登录代次。
        if (bs.userId != 0) {
            _app.invalidateSessionAttempt(bs.userId, bs.generation);
        }
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
