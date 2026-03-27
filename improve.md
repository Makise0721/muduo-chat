# ChatService 改进审查日志

日期：2026-03-24  
范围：`chatserver/src/ChatService.cpp`（关联参考：`chatserver/src/MySQL.cpp`）

## 结论

本次改动方向正确，代码质量有明显提升，尤其在 SQL 注入防护和资源释放可靠性方面。  
整体可评价为：**安全性提升明显、稳定性提升明显、协议兼容性基本保持**。

## 本次已完成的核心改进

### 1) 新增 SQL 字符串转义工具，降低注入风险

在匿名命名空间新增 `escapeString(MYSQL*, const std::string&)`，通过 `mysql_real_escape_string` 对用户输入进行转义。  
该能力已用于以下高风险输入路径：

- 登录密码：`login()` 中 `password`
- 注册信息：`reg()` 中 `name/password`
- 离线消息入库：`oneChat()`、`groupChat()`
- 建群信息：`createGroup()` 中 `groupname/groupdesc`

**收益**：避免原先直接拼接 `'%s'` 导致的单引号逃逸与注入风险。

### 2) 将 `sprintf` 全面替换为 `snprintf`

关键 SQL 构造点改为 `snprintf(sql, sizeof(sql), ...)`。  
**收益**：减少缓冲区溢出风险，提升字符串拼接的健壮性。

### 3) 保持并强化 RAII 资源管理策略

`MySQLConnectionGuard` 与 `MySQLResultGuard` 继续保留并生效：

- 连接自动归还连接池，避免连接泄漏
- `MYSQL_RES*` 自动释放，避免结果集泄漏

**收益**：异常或早返回路径下依然能正确释放资源。

### 4) 登录回包信息增强（向后兼容）

登录查询朋友时由 `friendid,name` 扩展到 `friendid,name,state`，并新增 `friendDetails` 数组字段（包含 id、名称、在线状态）。同时保留原 `friends` 名称列表。

**收益**：前端可直接渲染好友在线态，且旧客户端仍可读 `friends` 字段。

## 回归检查（逻辑层）

- JSON 解析失败与非法 `msgid` 均有防御性返回
- 登录后先返回 ACK 再发送离线消息，时序合理
- 私聊/群聊离线存储逻辑保持一致，发送方均能收到回执
- `clientCloseException()` 和 `reset()` 仍能保证用户状态回收

## 仍建议关注的剩余风险

### 1) 仍是拼接 SQL（非预处理语句）

虽然已做转义，但本质仍为字符串拼接。长期建议改为 prepared statement（参数绑定）进一步收敛风险与边界问题。

### 2) `escapeString` 的内存管理可再优化

当前使用 `new[]/delete[]` 手动管理，逻辑正确，但建议后续改为 `std::string` 预分配或 `std::vector<char>`，减少手动内存管理负担。

### 3) 群聊循环中锁粒度仍较细

`groupChat()` 在每个成员循环中获取一次 `_connMutex`。  
当前可用，但高并发群聊下可能放大锁竞争，可考虑后续优化为“先快照在线连接，再批量发送”。

## 建议测试清单

- 输入包含 `'`, `"`, `\\`, emoji 的账号/密码/群名/消息，确认无 SQL 异常
- 私聊与群聊消息包含 JSON 特殊字符时，离线存储与重放正常
- 同账号重复登录拦截仍有效
- 登录响应中 `friends` 与 `friendDetails` 字段均正确
- 长时间压测后连接池未耗尽、MySQL 结果集无泄漏迹象

## 总评

这次改动是一次**有效且有针对性的安全与稳定性加固**。
若后续再补齐“预处理语句 + 并发锁粒度优化”，`ChatService` 的可维护性和抗风险能力会进一步提升。

---

# ChatService 改进审查日志（续）

日期：2026-03-27
范围：`chatserver/src/ChatService.cpp`、`chatserver/src/MySQL.cpp`、`chatserver/include/db/MySQL.hpp`

## 结论

本次改动**逐项落实了上一轮审查的建议**，重点优化内存管理、锁竞争与预处理语句基础设施。
整体可评价为：**代码健壮性提升、并发性能优化、安全基础加固**。

## 本次已完成的核心改进

### 1) `escapeString` 内存管理优化

将原先手动 `new[]/delete[]` 管理改为 `std::vector<char>` 自动管理，消除手动内存管理负担，降低资源泄漏风险。
**收益**：代码更简洁、更安全，符合现代 C++ 最佳实践。

### 2) `groupChat` 锁粒度优化

重构 `groupChat()` 中的锁竞争热点，从“循环内每次加锁”改为“先快照在线连接，再批量发送”。
- 收集所有群组成员 ID 向量
- 单次加锁复制在线用户的 `TcpConnectionPtr` 与 ID 集合
- 锁外发送在线消息、处理离线存储
- 使用 `std::unordered_set<int>` 快速判断在线状态

**收益**：高并发群聊场景下，锁竞争显著降低，吞吐量预期提升。

### 3) 预处理语句（Prepared Statement）基础设施

在 `MySQL` 类中新增预处理语句支持，为后续彻底替换拼接 SQL 提供基础。

- **接口扩展**（`MySQL.hpp`）：
  `prepareStatement()`、`bindParam()`（支持 `std::string` 与 `int`）、`execute()`、`getResult()`、`closeStatement()`
- **实现**（`MySQL.cpp`）：
  封装 `mysql_stmt_init/prepare/bind_param/execute` 等 C API，自动管理语句生命周期
- **RAII 包装**（`ChatService.cpp`）：
  新增 `PreparedStatementGuard`，确保语句句柄自动关闭

**收益**：为下一步全面迁移到参数化查询打下基础，从根本上消除 SQL 注入风险。

## 回归检查（逻辑层）

- `escapeString` 功能保持不变，仅内部实现从 `new[]/delete[]` 改为 `std::vector<char>`
- `groupChat` 的消息投递逻辑（在线直发 + 离线入库）完全等价，仅锁范围变化
- 预处理语句接口仅新增，未改动现有 `query()`/`update()` 调用，不影响当前功能
- 编译通过（C++11），无警告/错误

## 仍建议关注的剩余风险

### 1) 实际业务 SQL 仍未使用预处理语句

基础设施已就绪，但 `login`、`reg`、`oneChat` 等关键路径仍为拼接 SQL。建议逐步替换为参数化查询。

### 2) 预处理语句绑定接口尚不完善

目前仅支持 `std::string` 与 `int` 两种类型，实际业务可能需要 `unsigned int`、`long long`、`float` 等更多类型。可进一步扩展 `bindParam` 重载。

### 3) 连接池中预处理语句的复用

若每个请求都创建/销毁预处理语句，可能带来额外开销。可考虑在连接池层面缓存常用语句。

## 建议测试清单

- 群聊消息在成员数量多（如 100+）时，锁优化后性能对比
- 输入超长字符串（接近 1MB）时，`std::vector<char>` 转义是否正常
- 预处理语句接口的基本功能验证：`prepareStatement` + `bindParam` + `execute` 组合
- 压测中观察连接池使用率、MySQL 服务端 prepared statement 计数

## 总评

这次改动是一次**高质量的技术债偿还**，不仅解决了已知问题，还为未来安全升级铺平了道路。
下一步只需将业务 SQL 逐步迁移到预处理语句，即可完成从“转义防护”到“根本免疫”的进化。
