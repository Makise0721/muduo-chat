# 数据库 Schema 规范（0001 baseline + 0002 expand + 0003 consumer dead-letter）

状态：`定稿`（P3-03 冻结；与 sql/migrations/0001_baseline.sql、0002_expand.sql 逐条一致，漂移由 SchemaMigrationTest.DdlDriftBetweenChatSqlAndBaseline 与 ReliableMessageSchemaTest.SchemaContractAssertsColumnsIndexesAndForeignKeys 固化）

基线：`main` @ `1a2562f`（P3-03 任务卡基线）

关联：

- [message-reliability.md](message-reliability.md) §1 承诺与计划 §6 目标 schema 契约：本文件的六表约束是其物理落地，约束不可削弱
- `sql/chat.sql`：已发布"五表库"DDL 源（旧快照构建与 0001 漂移契约依赖它，本文件不改它）
- 迁移 runner 约定见 `chatserver/src/db/SchemaMigration.cpp`：每条语句以 `;` 结尾、字符串字面量无 `;`、`CREATE TABLE IF NOT EXISTS`、utf8

## 1. 总则

- 全部表 `ENGINE=InnoDB DEFAULT CHARSET=utf8`（0001 风格）；FK 全部 `ON DELETE CASCADE`。
- 全部 `CREATE TABLE IF NOT EXISTS`：中断重跑幂等；旧库已有表时为 no-op。
- 迁移文件按 `NNNN_*.sql` 版本排序执行，`schema_migrations` 记录 version/checksum/applied_at（P3-01）。
- FK 引用列类型同型：引用 `User.id`/`AllGroup.id`（INT）的列用 `INT`；引用 `Conversation.id`/`ChatMessage.id`（BIGINT UNSIGNED）的列用 `BIGINT UNSIGNED`。

## 2. 0001 baseline：旧五表（不变、不删）

与 `sql/chat.sql` 的 CREATE TABLE 语句逐字节一致（漂移契约，DdlDriftBetweenChatSqlAndBaseline）：

| 表 | 说明 |
|----|------|
| `User` | `id INT PK AUTO_INCREMENT`、`name VARCHAR(50) NOT NULL UNIQUE`、`password VARCHAR(50) NOT NULL`、`state ENUM('online','offline') DEFAULT 'offline'` |
| `Friend` | `PRIMARY KEY(userid, friendid)`，双 FK → `User(id)` |
| `AllGroup` | `id INT PK AUTO_INCREMENT`、`groupname VARCHAR(50) NOT NULL`、`groupdesc VARCHAR(200) DEFAULT ''` |
| `GroupUser` | `PRIMARY KEY(groupid, userid)`，FK → `AllGroup(id)`/`User(id)` |
| `OfflineMessage` | `id INT PK AUTO_INCREMENT`、`userid INT NOT NULL`、`message VARCHAR(500) NOT NULL`，FK → `User(id)`；**保留原样**（新消息走 ChatMessage/MessageDelivery，本表不迁移数据） |

## 3. 0002 expand：可靠消息六表

### 3.1 Conversation

| 列 | 类型 | 键/约束 |
|----|------|---------|
| `id` | `BIGINT UNSIGNED` | PK，AUTO_INCREMENT |
| `kind` | `ENUM('DIRECT','GROUP')` | NOT NULL |
| `next_sequence` | `BIGINT UNSIGNED` | NOT NULL DEFAULT 0 |

### 3.2 DirectConversation

| 列 | 类型 | 键/约束 |
|----|------|---------|
| `conversation_id` | `BIGINT UNSIGNED` | PK，FK → `Conversation(id)` |
| `user_low_id` | `INT` | `UNIQUE(user_low_id, user_high_id)`，FK → `User(id)` |
| `user_high_id` | `INT` | 同上 UNIQUE 第二列，FK → `User(id)`（MySQL 自动补前缀索引） |

### 3.3 GroupConversation

| 列 | 类型 | 键/约束 |
|----|------|---------|
| `conversation_id` | `BIGINT UNSIGNED` | PK，FK → `Conversation(id)` |
| `group_id` | `INT` | `UNIQUE(group_id)`，FK → `AllGroup(id)` |

### 3.4 ChatMessage

| 列 | 类型 | 键/约束 |
|----|------|---------|
| `id` | `BIGINT UNSIGNED` | PK，AUTO_INCREMENT |
| `conversation_id` | `BIGINT UNSIGNED` | `UNIQUE(conversation_id, sequence)`，FK → `Conversation(id)` |
| `sender_id` | `INT` | `UNIQUE(sender_id, client_message_id)`，FK → `User(id)` |
| `client_message_id` | `VARCHAR(64) CHARACTER SET ascii COLLATE ascii_bin` | NOT NULL（DB 层 ascii 字符集防非 ASCII + `ascii_bin` 大小写敏感；1..64 长度上界由领域层 `ClientMessageId` 校验 P3-02 承担；严格 sql_mode 下超长报 1406，非严格部署截断——见 §4 已知限制） |
| `sequence` | `BIGINT UNSIGNED` | NOT NULL（Conversation 内局部顺序） |
| `content` | `MEDIUMBLOB` | NOT NULL（上限 16777215 字节） |
| `created_at` | `DATETIME` | NOT NULL DEFAULT CURRENT_TIMESTAMP |

### 3.5 MessageDelivery

| 列 | 类型 | 键/约束 |
|----|------|---------|
| `message_id` | `BIGINT UNSIGNED` | PK `(message_id, recipient_id)`，FK → `ChatMessage(id)` |
| `recipient_id` | `INT` | PK 第二列，FK → `User(id)` |
| `state` | `TINYINT` | NOT NULL DEFAULT 0（编码由 adapter 隐藏，领域不泄漏） |
| `attempt_count` | `INT` | NOT NULL DEFAULT 0 |
| `next_attempt_at` | `DATETIME` | NULL |
| `lease_owner` | `VARCHAR(64)` | NULL |
| `lease_until` | `DATETIME` | NULL |
| `last_sent_at` | `DATETIME` | NULL |
| `acknowledged_at` | `DATETIME` | NULL |
| `expires_at` | `DATETIME` | NULL |

索引：`INDEX(recipient_id, state, next_attempt_at)`（投递扫描；FK 复用该索引前缀，不产生重复索引）。

### 3.6 OutboxEvent

| 列 | 类型 | 键/约束 |
|----|------|---------|
| `id` | `BIGINT UNSIGNED` | PK，AUTO_INCREMENT |
| `aggregate_message_id` | `BIGINT UNSIGNED` | `UNIQUE(event_type, aggregate_message_id)`，FK → `ChatMessage(id)` |
| `event_type` | `VARCHAR(64)` | NOT NULL（P4 可加事件类型，不必改 ENUM） |
| `payload` | `MEDIUMBLOB` | NOT NULL |
| `available_at` | `DATETIME` | NOT NULL DEFAULT CURRENT_TIMESTAMP |
| `lease_owner` | `VARCHAR(64)` | NULL |
| `lease_until` | `DATETIME` | NULL |
| `attempt_count` | `INT` | NOT NULL DEFAULT 0 |
| `processed_at` | `DATETIME` | NULL |

### 3.7 KafkaDeadLetter

消费侧 dead-letter 表（P4-04 D3：键 = Kafka record 定位 topic/partition/offset + 原始 bytes；与发布侧 outbox poison 机制不复用）。仅 `CREATE TABLE IF NOT EXISTS` 追加，不改、不删 0001/0002 既有表。

| 列 | 类型 | 键/约束 |
|----|------|---------|
| `id` | `BIGINT UNSIGNED` | PK，AUTO_INCREMENT |
| `topic` | `VARCHAR(191)` | NOT NULL |
| `partition_id` | `INT` | NOT NULL |
| `kafka_offset` | `BIGINT UNSIGNED` | NOT NULL，`UNIQUE(topic, partition_id, kafka_offset)`（幂等落库） |
| `message_id` | `BIGINT UNSIGNED` | NULL（信封 message_id；不可解析时 0） |
| `conversation_id` | `BIGINT UNSIGNED` | NULL |
| `sequence` | `BIGINT UNSIGNED` | NULL |
| `event_type` | `VARCHAR(64)` | NULL |
| `reason` | `VARCHAR(32)` | NOT NULL：`poison_payload`\|`unknown_event_type`\|`sequence_regression`\|`sequence_conflict`\|`message_missing`（消费处置管线冻结词汇） |
| `raw_value` | `MEDIUMBLOB` | NOT NULL（原始 record bytes，信封原文） |
| `created_at` | `DATETIME` | NOT NULL DEFAULT CURRENT_TIMESTAMP |

## 4. 约束清单（契约不可削弱）

| 约束 | 位置 | 语义 |
|------|------|------|
| `UNIQUE(sender_id, client_message_id)` | ChatMessage | 幂等接受：同键重试不产生第二行 |
| `UNIQUE(conversation_id, sequence)` | ChatMessage | 局部顺序：Conversation 内 sequence 唯一 |
| `UNIQUE(event_type, aggregate_message_id)` | OutboxEvent | outbox 事件幂等 |
| `UNIQUE(user_low_id, user_high_id)` | DirectConversation | UNIQUE 物理约束；low/high 归一化（low<high）由 adapter 保证（P3-04） |
| `UNIQUE(group_id)` | GroupConversation | 群对话一表一 |
| `PRIMARY KEY(message_id, recipient_id)` | MessageDelivery | 每接收者至多一行 Delivery |
| `INDEX(recipient_id, state, next_attempt_at)` | MessageDelivery | 待投递扫描 |
| `UNIQUE(topic, partition_id, kafka_offset)` | KafkaDeadLetter | 消费侧 dead-letter 幂等落库：kill-前已落库的事件重放时不双插（P4-04） |
| FK ×10（见 §3 各表） | 六表 | 同型引用 + `ON DELETE CASCADE` |

已知限制：`client_message_id` 超长（>64 字节）只在 DB 严格 sql_mode（`STRICT_TRANS_TABLES`）下被拒绝（error 1406）；非严格部署会静默截断——ASCII 1..64 上界由领域层 `ClientMessageId` 校验（P3-02）兜底，不依赖 DB 约束。

## 5. 变更

- P4-04：新增 §3.7 KafkaDeadLetter 表与 §4 对应约束行（消费侧 dead-letter，additive 追加，不改/不删 0001/0002 既有表）。对应 `0003_consumer_dead_letter.sql`（P4-04 提交）。
- P3-03：新增 §3 六表与 §4 约束清单；§2 旧五表概述。对应 `0002_expand.sql`（P3-03 提交）。
