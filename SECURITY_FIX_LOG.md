# 安全修复与功能增强日志

## 修复日期
2026年3月24日

## 变更概览
| 文件 | 状态 | 主要变更 |
|------|------|----------|
| `chatserver/src/MySQL.cpp` | 已暂存 | 修复结果集处理方式 |
| `chatserver/src/ChatService.cpp` | 未暂存 | 修复SQL注入漏洞、增强好友列表功能 |

## 详细变更说明

### 1. `chatserver/src/MySQL.cpp`
- **变更**: 第41行 `mysql_use_result(conn_)` → `mysql_store_result(conn_)`
- **影响**: 修复结果集处理方式，确保所有查询结果都能被完整获取和处理

### 2. `chatserver/src/ChatService.cpp`

#### 2.1 安全性修复（SQL注入防护）
- **新增函数** `escapeString()` (第33-42行):
  ```cpp
  std::string escapeString(MYSQL* mysql, const std::string& str) {
      if (!mysql || str.empty()) return str;
      char* escaped = new char[str.length() * 2 + 1];
      unsigned long len = mysql_real_escape_string(mysql, escaped, str.c_str(), str.length());
      std::string result(escaped, len);
      delete[] escaped;
      return result;
  }
  ```

- **替换所有`sprintf`为`snprintf`** (共15处):
  - 防止缓冲区溢出风险
  - 添加缓冲区大小限制参数 `sizeof(sql)`

- **转义所有用户输入**:
  - 密码字段: `login()` L96, `reg()` L208
  - 用户名: `reg()` L188
  - 群组名和描述: `createGroup()` L292
  - 离线消息JSON: `oneChat()` L259, `groupChat()` L348

#### 2.2 功能增强（好友列表）
- **扩展SQL查询** (第145行):
  ```cpp
  // 原: SELECT friendid, name FROM ...
  // 新: SELECT friendid, name, state FROM ...
  ```

- **新增`friendDetails`字段** (第154-160行):
  - 包含好友ID、名称、在线状态的完整对象数组
  - 保持向后兼容的`friends`字段（纯名称数组）

- **响应结构示例**:
  ```json
  {
    "friends": ["Alice", "Bob"],
    "friendDetails": [
      {"friendid": 2, "name": "Alice", "state": "online"},
      {"friendid": 3, "name": "Bob", "state": "offline"}
    ]
  }
  ```

#### 2.3 受影响的方法清单
| 方法 | 修改内容 |
|------|----------|
| `login()` | 密码转义、SQL格式化、好友列表增强 |
| `reg()` | 用户名和密码转义、SQL格式化 |
| `loginout()` | SQL格式化 |
| `oneChat()` | 离线消息转义、SQL格式化 |
| `addFriend()` | SQL格式化 |
| `createGroup()` | 群组名/描述转义、SQL格式化 |
| `addGroup()` | SQL格式化 |
| `groupChat()` | SQL格式化、离线消息转义 |
| `clientCloseException()` | SQL格式化 |

## 安全性影响评估
1. **SQL注入风险大幅降低**: 所有动态字符串输入均经过`mysql_real_escape_string()`转义
2. **缓冲区溢出防护**: 使用`snprintf`替代`sprintf`，明确指定缓冲区大小
3. **离线消息安全性**: JSON字符串在存储前转义，避免特殊字符破坏SQL语句结构

## 兼容性说明
- **向后兼容**: `friends`字段保持不变，客户端无需立即修改
- **渐进增强**: 新增`friendDetails`字段，客户端可按需使用
- **数据库兼容**: 需要确保`User`表包含`state`字段用于好友在线状态查询

## Git变更状态
```
位于分支 main
您的分支与上游分支 'origin/main' 一致。

要提交的变更：
  （使用 "git restore --staged <文件>..." 以取消暂存）
    修改：     chatserver/src/MySQL.cpp

尚未暂存以备提交的变更：
  （使用 "git add <文件>..." 更新要提交的内容）
  （使用 "git restore <文件>..." 丢弃工作区的改动）
    修改：     chatserver/src/ChatService.cpp
```

## 建议后续操作
1. **测试验证**: 运行完整的功能测试，确保SQL注入防护有效
2. **客户端适配**: 如需要好友在线状态展示，可开始使用`friendDetails`字段
3. **长期优化**: 考虑迁移到MySQL预处理语句(`mysql_stmt_prepare`)以进一步提高安全性

> **编译状态**: 修改后的代码已通过语法检查（`g++`无报错）