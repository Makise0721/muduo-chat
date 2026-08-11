# P2 对抗性审查循环（子代理驱动）

状态：VERIFIED（审查通过）
基线：main @ addfc23（P2-10 后）；提交：见文末

## 流程（用户指定：全流程子代理推进）

审查（子代理，四轴并行）→ 修复（子代理，最简）→ 再审查……直到审查通过。共 5 轮审查
（循环 1-4 + 最终确认）、4 批修复（R1/R2、R3/R4、R5、R6），验证（三树全量）全程子代理执行。

## 循环 1 发现（4 轴 30+ 项）→ R1/R2 修复

**严重正确性**（全部修复）：
- 单包崩溃 DoS：`{"msgid":1}` 缺字段抛异常穿透 → `handler()` 全 dispatch try/catch（catch...
  兜底），日志脱敏（不打印原文）
- 登录断线竞态锁死用户：login completion 未查连接状态把死连接插入 _userConnMap → 插入前
  查 `conn->connected()`
- completion 悬垂引用 UB：oneChat/addFriend/addGroup/groupChat completion 捕获 handler 局部
  `json&` → 按值捕获 payload（4 处 + reg 异步化全链核对）
- reg 仍在 loop 线程同步 DB（违反"所有阻塞 DB 路径在 executor"）→ 异步化 submit
- 离线消息长度无校验（VARCHAR(500) 截断/丢失）→ oneChat payload≤500 拒绝（在线/离线一致）

**规格/契约**（全部修复）：
- InMemoryFriendRepository::add 只校验 friendId → 双列校验（sender 不存在也 TargetNotFound）
- password>50 未防御 → isRepositoryInputValid 补校验（双 adapter 一致）
- Config int 溢出（workers=4294967296 → 0 worker 卡死）→ getIntMin1 加 INT_MAX 上限
- v1/v2 同端口无校验 → config error fail-fast；executor.workers>1 拒绝（P2-10 保序决策落地）
- 池初始化全失败静默继续 → main 空池 fail-fast exit(1)
- 注释字节损坏（chat-bench ? 字节 / MySQLFriendRepository.hpp 粘连）→ 重写

**测试**（R2）：ConfigTest 补 15 分支、Friend 双不存在用例、password 51/50 边界、
metrics_test.sh 断言解耦、assert_order count==1、RESOURCE_LOCK、fd_exhaustion pkill 端口限定、
chat_load.py 异常捕获。

**循环 1 收尾（主代理补 3 处）**：METRICS 单次 ostringstream 输出（防 Logger 交错）、
DRAINED 加 !forced 条件（A/B 场景互斥）、oneChat 校验改整条 payload 判定。

## 循环 2 发现 → R3/R4 修复

- **takeOffline 栈越界读**（payloadBuf[1024] 越界 assign）→ vector 动态缓冲 + TRUNCATED 重取
- **离线消息丢失竞态**（takeOffline 先取走、completion 早退丢消息）→ 早退恢复入队
  （restoreOfflineMessages，尽力而为，at-least-once 语义内）
- groupChat 无超长校验（静默丢消息且 ACK 0）→ 同款 payload≤500 拒绝
- worker task 无异常保护（reg 迁 worker 后 _app.handle 异常 → terminate）→ try/catch 兜底
- CLI threads 无 INT_MAX 上限（回绕海量线程）→ 报错拒绝
- 测试：metrics_test 去行首锚定（TSan flaky 根因）、domain 补 E1-E4（缺 id 静默/连接存活/
  600 字符单聊 errno=1）、config_fail 补空池 fail-fast、ConfigTest workers:2 断言补强、
  矩阵 B-04/B-13/B-24/B-25 同步

## 循环 3 发现 → R5 修复

- BlockingExecutor 注释与实现矛盾（异常后 completion 是否执行）→ 注释改准确 + 异常任务
  单测锁定（completion 仍执行、worker 存活）
- ConfigTest 补 CLI threads 溢出（2147483648）、domain 补群聊超长 G8
- 矩阵 B-17/B-19 同步（超长除外）、新增 B-27（离线早退恢复入队）、行号修正

## 循环 4/5（确认与闭合）

- 矩阵 B-17/B-18/B-19 行号精确化（430-488 区域）、G8 注释标签修正
- 四轴全部通过；技术债 10 项确认可接受记录（nowMs×3、errno switch×4、Middle Man、
  exit(-1)、atoi env、发送期断线损失窗口、B-18 文字、矩阵旧行号漂移、B-15、ChatService
  不可单测——均标记待ADR/P3）

## 验证矩阵（子代理执行，最终态）

- Debug/ASan/TSan **184/184**（180 + ConfigTest 11 → 13 新分支 + metrics + 异常用例 +
  shutdown/config/server-metrics + DomainCharacterization E1-E4/G8），TSan ×2 轮稳定，
  server-metrics 历史 flaky 根因（断言锚定）已消除；
- DomainCharacterization 含新增 E1-E4（静默/存活/超长）与 G8（群聊超长）；
- git diff --check 通过。

## Commit

`fix(server): adversarial review round 1 - crash, race, UB, blocking reg`
`fix(storage): adversarial review round 2 - overflow read, loss race, caps`
`test(app): adversarial review rounds 3-4 - exception, caps, doc sync`
`docs: adversarial review log and matrix sync`
