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
| B-04 | 注册成功 | REG_MSG_ACK errno=0 + 自增 id；不自动登录；空名/空密码/超长名（>50 字符）→ errno=1 "invalid input!"；password>50 → InvalidInput（P2 修复落地） | ChatService.cpp:252-282；app/ChatApplication.cpp:89-110 | 稳定 |
| B-05 | 注册重名 | errno=1 "this name is already exist!" | ChatService.cpp:208-220 | 稳定 |
| B-06 | 登录成功 | errno=0 + id + name + friends（名字数组）+ friendDetails（friendid/name/state 数组） | ChatService.cpp:149-175 | 待ADR（friends/friendDetails 冗余双字段） |
| B-07 | 登录失败（id/密码不匹配） | errno=1 "userid or password is invalid!" | ChatService.cpp:121-129 | 稳定 |
| B-08 | 重复登录 | errno=2 "this account is using, input another!"（单会话约束） | ChatService.cpp:132-138 | 稳定 |
| B-09 | 登录补投离线消息 | 先回 LOGIN_MSG_ACK，再逐条补投原始 Command（无 errno），随后清空离线队列 | ChatService.cpp:242-248（取队列 114-117） | 稳定 |
| B-10 | 登出 | LOGINOUT_MSG errno=0；未登录也成功（幂等）；不清空离线队列 | ChatService.cpp:236-256 | 稳定 |
| B-11 | 单聊：目标在线 | **P3-06 迁移**：在线直写退役——目标不再即时收到原样转发（投递由 Delivery 状态机接管，属 P3-07）；发送者收 MESSAGE_ACCEPTED（v2，msgid=11 五字段）/旧格式回显 errno=0（legacy，决策表第 10 行） | ChatService.cpp:345-408（accept 路径） | 稳定（P3-06 迁移替代原行为） |
| B-12 | 单聊：目标离线 | **P3-06 迁移**：storeOfflineMessage 写路径退役——不再写 OfflineMessage，离线/在线统一 durable accept（同一 ledger）；发送者收 MESSAGE_ACCEPTED/旧格式回显 errno=0（=已接受，非已投递）；登录补投暂无新 Delivery（P3-07） | ChatService.cpp:345-408（accept 路径） | 稳定（P3-06 迁移替代原行为） |
| B-13 | 单聊：超长消息 | **P3-06 收紧落地**：content（UTF-8 字节）>16KB 拒绝（决策表第 6 行冻结）；v2 → errno=105 + errmsg "content too long"（msgid=13），legacy → 旧格式回显 errno=1；旧 500 字节整包判定废止 | app/ProtocolCodec.cpp:148-169（codec 105） | 稳定（P3-06 收紧替代原行为） |
| B-14 | 加好友 | 有向边插入成功 errno=0；重复添加主键冲突 errno=1；不校验目标用户存在，也不校验调用方身份 | ChatService.cpp:285-298 | 待ADR（无身份/存在性校验） |
| B-15 | 建群 | 建群成功 → groupid + errno=0，建群者以 creator 入群；建群失败 → errno=1 | ChatService.cpp:300-325 | 稳定 |
| B-16 | 入群 | 成功 errno=0；重复加入主键冲突 errno=1；不校验群存在 | ChatService.cpp:327-340 | 稳定 |
| B-17 | 群聊 | **P3-06 迁移**：在线转发/离线入队退役——accept 事务内快照成员（决策表第 8 行，保序、含发送者）；发送者收 MESSAGE_ACCEPTED/旧格式回显；投递由 Delivery 状态机接管（P3-07） | ChatService.cpp:485-543（accept 路径） | 稳定（P3-06 迁移替代原行为） |
| B-18 | 群聊：非成员可发 | **P3-06 收紧落地**：发送者必须是群成员，非成员 → 101（NotConversationMember，msgid=13）/legacy → 旧格式回显 errno=1 | app/ProtocolCodec.cpp:202-212（成员资格预检） | 稳定（P3-06 收紧替代原行为） |
| B-19 | 群聊：确认无条件成功 | **P3-06 收紧落地**：成员查询失败/DB 故障 → 104（DependencyBusy），不再无条件 errno=0 | app/ProtocolCodec.cpp:194-196（查询失败→104） | 稳定（P3-06 收紧替代原行为） |
| B-20 | 断开 | 在线用户断开 → state=offline；未登录连接断开无数据库操作 | ChatService.cpp:511-524 | 稳定 |
| B-21 | 连接绑定一个认证会话 | 登录成功后连接绑定一个 Session（User+generation）；同一连接二次登录（同 User 或另一 User）→ errno=2 拒绝；单聊/群聊发送者身份取自 Session，payload id 与 Session user 不符 → errno=1 "invalid sender!" 拒绝（B-21 收紧落地，P3-05；对照 message-reliability.md §6） | ChatService.cpp:216（bind）；loginout 294；oneChat 327-343；groupChat 459-471；clientCloseException 513；app/SessionRegistry | 稳定（P3-05 收紧替代原行为） |
| B-22 | msgid 数值编码 | 协议 msgid 表：1=LOGIN、2=LOGIN_MSG_ACK、3=LOGINOUT（登出请求与响应同值）、4=REG、5=REG_MSG_ACK、6=ONE_CHAT、7=ADD_FRIEND、8=CREATE_GROUP、9=ADD_GROUP、10=GROUP_CHAT；ACK 值与请求值不同，客户端必须硬编码。**P3-06 golden 冻结**：11=MESSAGE_ACCEPTED、12=DELIVERY_ACK（handler 属 P3-07）、13=ERROR_RESP（spec §2.5，数值不再变更） | chatserver/include/ChatService.hpp:25-41 | 待ADR |
| B-23 | 帧上限不对称 | v2 默认 1MiB 帧上限，超限 → ProtocolError → forceClose；v1 无行大小上限 | mymuduo/StreamCodec.cc:40-42；LegacyJsonLineCodec | 待ADR |
| B-24 | v2 端口配置化 | v1 取 argv/配置覆盖，v2 监听端口/ip 由配置 v2.port/v2.ip 控制（P2-09 落地，缺省 7000/127.0.0.1） | chatserver/main.cpp:168-171 | 稳定 |
| B-25 | 字段类型错误/缺字段 | handler 全异常捕获 → 静默不响应（与 B-01/B-02/B-03 静默约定一致），连接保持 | ChatService.cpp:58-63 | 稳定 |
| B-26 | 启动不重置在线状态 | 服务器启动不调用 reset()；崩溃/强杀残留的 state=online 永久锁号（后续登录 errno=2），直到手动 reset | chatserver/main.cpp:51-106（无 reset 调用）；ChatService.cpp:418-422 | 待ADR（P2 启动重置或心跳） |
| B-27 | 登录早退：离线消息恢复入队 | 登录 completion 早退（会话过期/连接断开）时，已取出的离线消息恢复入队，至少一次投递（executor 已 shutdown 时丢弃） | ChatService.cpp:120-135（调用于 197/210） | 稳定 |
| B-28 | 幂等接受与稳定错误（P3-06） | ONE_CHAT/GROUP_CHAT 携带 client_message_id（ASCII 1..64，缺失→legacy 判定）；同 (sender, cmid) 重试返回原 identity（duplicate=true，不产生第二行）；同 key 不同 payload → 103；目标用户/群不存在 → 106；非法 cmid → 107；错误响应 msgid=13 + errno + errmsg（+cmid 回显）；errno 101..107 与 errmsg 字符串见 spec §2.4（golden 冻结） | app/ProtocolCodec.cpp（codec 148-169、accept 171-246、errmsg 248-268） | 稳定（P3-06 落地） |
| — | 矩阵边界 | 本矩阵=现状（P2-00 锁定 + P3-05/P3-06 已落地收紧/迁移，类别列标注）；P3 目标语义（幂等接受、可靠投递、ACK）见 [message-reliability.md](message-reliability.md)；对 待ADR 行不改类别 | — | — |
