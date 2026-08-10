# 行为矩阵：聊天用例 characterization（P2-00）

锁定 `ChatService` 与协议层在 P2-00 时的现状行为。每行是"当前代码实际做什么"，
与 `CONTEXT.md` 词汇表配套：词汇定义领域概念，本矩阵定义概念之间的可观察行为。

验证载体：
- 进程矩阵 `tests/scripts/domain_characterization_test.sh`（真实服务器 + 真实 MySQL，
  v1/v2 双连接逐用例断言一致），CTest 名 `DomainCharacterization`；
- codec 对称性 `tests/unit/DualProtocolCharacterizationTest.cpp`（v1/v2 同一 payload
  round-trip 还原为同一 JSON）。

类别：稳定 = P2 不应改变的现状；待ADR = 现状与 P2 目标语义冲突、需要决策后再改。

| ID | 用例 | 可观察行为（现状） | 代码定位 | 类别 |
|----|------|--------------------|----------|------|
| B-01 | 协议防御：非法 JSON | 静默丢弃，无 Reply，连接保持 | chatserver/src/ChatService.cpp:74-81 | 稳定 |
| B-02 | 协议防御：无 msgid | 静默丢弃，无 Reply | ChatService.cpp:83-86 | 稳定 |
| B-03 | 协议防御：未知 msgid | 静默丢弃，无 Reply | ChatService.cpp:90-94 | 稳定 |
| B-04 | 注册成功 | REG_MSG_ACK errno=0 + 自增 id；不自动登录 | ChatService.cpp:197-234 | 稳定 |
| B-05 | 注册重名 | errno=1 "this name is already exist!" | ChatService.cpp:208-220 | 稳定 |
| B-06 | 登录成功 | errno=0 + id + name + friends（名字数组）+ friendDetails（friendid/name/state 数组） | ChatService.cpp:149-175 | 待ADR（friends/friendDetails 冗余双字段） |
| B-07 | 登录失败（id/密码不匹配） | errno=1 "userid or password is invalid!" | ChatService.cpp:121-129 | 稳定 |
| B-08 | 重复登录 | errno=2 "this account is using, input another!"（单会话约束） | ChatService.cpp:132-138 | 稳定 |
| B-09 | 登录补投离线消息 | 先回 LOGIN_MSG_ACK，再逐条补投原始 Command（无 errno），随后清空离线队列 | ChatService.cpp:177-194 | 稳定 |
| B-10 | 登出 | LOGINOUT_MSG errno=0；未登录也成功（幂等）；不清空离线队列 | ChatService.cpp:236-256 | 稳定 |
| B-11 | 单聊：目标在线 | 目标收到原始 Command（无 errno 字段）；发送者收到副本 + errno=0 | ChatService.cpp:258-270 | 稳定 |
| B-12 | 单聊：目标离线 | 整条 Command 入离线队列；发送者收到 errno=0（=已接受，非已投递） | ChatService.cpp:272-282 | 稳定 |
| B-13 | 单聊：超长消息 | 离线消息列 message 为 VARCHAR(500)；超限行为取决于 MySQL sql_mode（当前实例宽松模式：截断存储、发送者 errno=0；严格模式报错） | sql/chat.sql OfflineMessage；ChatService.cpp:277-282 | 待ADR（超限语义未定，不锁测试） |
| B-14 | 加好友 | 有向边插入成功 errno=0；重复添加主键冲突 errno=1；不校验目标用户存在，也不校验调用方身份 | ChatService.cpp:285-298 | 待ADR（无身份/存在性校验） |
| B-15 | 建群 | 建群成功 → groupid + errno=0，建群者以 creator 入群；建群失败 → errno=1 | ChatService.cpp:300-325 | 稳定 |
| B-16 | 入群 | 成功 errno=0；重复加入主键冲突 errno=1；不校验群存在 | ChatService.cpp:327-340 | 稳定 |
| B-17 | 群聊 | 在线成员收到原始 Command；离线成员入队；发送者收到 errno=0 | ChatService.cpp:342-394 | 稳定 |
| B-18 | 群聊：非成员可发 | SQL 只排除发送者自身、不校验发送者是否成员，非成员发群聊仍收到 errno=0 | ChatService.cpp:349 | 待ADR |
| B-19 | 群聊：确认无条件成功 | 成员查询失败（res==nullptr）也回 errno=0 | ChatService.cpp:390-393 | 待ADR |
| B-20 | 断开 | 在线用户断开 → state=offline；未登录连接断开无数据库操作 | ChatService.cpp:396-416 | 稳定 |
| B-21 | 连接可持有多个会话 | 同一连接先后登录不同 User 均成功，Session 不绑定连接 | ChatService.cpp:141-144 | 待ADR |
| B-22 | msgid 数值编码 | 协议 msgid 表：1=LOGIN、2=LOGIN_MSG_ACK、3=LOGINOUT（登出请求与响应同值）、4=REG、5=REG_MSG_ACK、6=ONE_CHAT、7=ADD_FRIEND、8=CREATE_GROUP、9=ADD_GROUP、10=GROUP_CHAT；ACK 值与请求值不同，客户端必须硬编码 | chatserver/include/ChatService.hpp:18-29 | 待ADR |
| B-23 | 帧上限不对称 | v2 默认 1MiB 帧上限，超限 → ProtocolError → forceClose；v1 无行大小上限 | mymuduo/StreamCodec.cc:40-42；LegacyJsonLineCodec | 待ADR |
| B-24 | v2 端口硬编码 | v1 取 argv 端口，v2 固定监听 ip:7000 | chatserver/main.cpp:73-75 | 待ADR（P2-09 配置化） |
| B-25 | 字段类型错误 | 登录/登出等带非数字 id 的 Command 在 `get<int>()` 抛未捕获异常 → 进程终止（terminate + core dump），不回复 | chatserver/src/ChatService.cpp:100-101 | 待ADR（P2 需防御式解析；不锁进测试） |
| B-26 | 启动不重置在线状态 | 服务器启动不调用 reset()；崩溃/强杀残留的 state=online 永久锁号（后续登录 errno=2），直到手动 reset | chatserver/main.cpp:51-106（无 reset 调用）；ChatService.cpp:418-422 | 待ADR（P2 启动重置或心跳） |
