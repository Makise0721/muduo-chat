# Chat Domain

聊天服务器领域：用户注册登录、好友、群组、单聊与群聊、离线消息投递。本词汇表是
`ChatService` 现状行为的规范术语来源；P2 各任务以它为准，不把线上 JSON 字段名
（`msgid`、`errno`、`id`）直接当作领域模型。

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

**Message**:
一条在用户之间传递的聊天内容（单聊或群聊）；服务器按原样转发或存储，不加工。
_Avoid_: 消息体、payload

**Delivery**:
一条 Message 的投递状态。现状中 Reply 的 `errno=0` 只承诺"服务器已接受"，不代表
对端已收到：目标在线时直接转发，离线时先入离线队列、待其登录后补投。
_Avoid_: 送达、投递成功

**Errno**:
Reply 的错误分类：0 成功；1 业务失败（认证失败、名称已存在、存储失败、关系冲突）；
2 会话冲突（重复登录）。
_Avoid_: 错误码、error code
