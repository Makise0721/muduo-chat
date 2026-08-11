# Chat Domain

聊天服务器领域：用户注册登录、好友、群组、单聊与群聊、离线消息投递。本词汇表是
`ChatService` 现状与可靠消息演进的规范术语来源；P2/P3 各任务以它为准，不把线上
JSON 字段名（`msgid`、`errno`、`id`）直接当作领域模型。

## Language

**Command**:
一条从客户端发往服务器的完整请求，由线上协议字段 `msgid` 区分类型：注册、登录、
登出、单聊、加好友、建群、入群、群聊。
_Avoid_: 消息请求、封包

**Reply**:
服务器针对一条 Command 发出的单条响应，现网带 `errno`（与部分 `msgid`）表达结果。
_Avoid_: 返回包、应答消息

**Session**:
一个已登录用户的在线连接。登录成功时建立，登出或连接断开时销毁；同一 User 同时
至多有一个 Session，重复登录被拒绝（单会话约束）。
_Avoid_: 连接（连接在登录前已存在）、在线状态（它是 Session 的持久化影子）

**User**:
已注册用户，由 id 标识，name 全局唯一，password 与 state（online/offline）随行存储。
_Avoid_: account、账户

**Friendship**:
一条有向边（A → B）：A 主动添加 B 后，A 的好友列表包含 B；关系不自动反向。
_Avoid_: 好友关系（暗示双向）

**Group**:
一个群组，由 id 标识，含 groupname 与 groupdesc；成员关系记录在成员表中，建群者
为 creator，后加入者为 normal。
_Avoid_: 聊天室、频道

**GroupRole**:
群组成员角色：creator（建群者）或 normal（后加入者）。现状只有标记，无权限语义。
_Avoid_: admin、管理员

**Conversation**:
一组按局部顺序排列的 Message；目标是固定的两名 User 或一个 Group。
_Avoid_: 全局消息流、聊天室

**ClientMessageId**:
发送端为一次消息意图生成的稳定幂等标识；同一 User 重试时保持不变。大小写敏感：
'abc' 与 'ABC' 是不同 ClientMessageId。
_Avoid_: request id、sequence

**MessageId**:
服务器为一条已接受 Message 分配的永久标识；所有重复投递共享同一 MessageId。
_Avoid_: ClientMessageId、离线消息 id

**ConversationSequence**:
Message 在一个 Conversation 内的单调位置；不同 Conversation 的值不可比较。
_Avoid_: 全局序号、MessageId

**Message**:
服务器接受到一个 Conversation 中的持久聊天记录，包含发送者、内容和 MessageId；
它不是协议原文，也不是一次网络发送。
_Avoid_: 消息体、payload、Command

**MessageAcceptance**:
服务器确认 Message 及其接收者 Delivery 已被持久记录；不表示任何接收端已经收到。
_Avoid_: 发送成功、送达

**Delivery**:
一条已接受 Message 对一个接收者的待履行投递义务；可经历多次 DeliveryAttempt，
直到被确认或按保留策略过期。
_Avoid_: 送达、投递成功

**DeliveryAttempt**:
服务器把同一 Message 尝试发送给接收者的一次动作；TCP 写入成功仍只是一种尝试。
_Avoid_: Delivery、重发消息

**DeliveryAcknowledgement**:
接收端客户端针对 MessageId 发出的显式确认；重复确认不改变结果。
_Avoid_: TCP ACK、写完成回调

**OutboxEvent**:
与 MessageAcceptance 同时形成、用于驱动后续投递副作用的持久通知。
_Avoid_: Message、日志事件

**Errno**:
Reply 的错误分类：0 成功；1 业务失败（认证失败、名称已存在、存储失败、关系冲突）；
2 会话冲突（重复登录）。
_Avoid_: 错误码、error code
