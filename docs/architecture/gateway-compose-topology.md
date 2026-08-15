# 3 Gateway Docker Compose 拓扑（文档契约）

状态：`定稿`（P4-00 冻结目标；[ADR-0002](../adr/0002-cluster-ownership-and-failure-contract.md) `accepted`）

基线：`main` @ `6bfbf2e`

关联：[P4 实施计划](../plans/post-p2-implementation-plan.md) §8、[cluster context map](cluster-context-map.md)、[集群故障与所有权契约](../specs/cluster-failure-contract.md)

本文件只定义组件/端口/网络/存储布局，不写 adapter 代码（Redis/Kafka adapter 分别在 P4-02/P4-03）。Redis 与 Kafka 的镜像版本是**占位**：P4-02/P4-03 开始任务时按 SOP §8 重新核对官方版本与文档后替换。MySQL 沿用项目既有单真相源角色。

## 1. 组件与角色

| 组件 | 角色 | 实例 | 说明 |
|------|------|------|------|
| gateway | ChatServer（现有生产二进制） | g1..g3 | 三个独立 Gateway 进程；各自持有连接、执行 accept、按 Presence 路由投递 |
| mysql | Message 真相源 | 1 | 唯一权威；全部 accept/ACK/Delivery 状态写它；沿用现有 schema（migration 体系） |
| redis | Presence 目录（路由+epoch） | 1 | 只存 `user -> {gw, conn, epoch}` TTL 条目，不存消息真相（P4-02 adapter） |
| kafka | Outbox 事件总线（可重放） | 1 | MessageAccepted 事件，key=ConversationId（P4-03 adapter）；消费者幂等 |

## 2. 端口布局

| 服务 | 端口 | 用途 |
|------|------|------|
| gateway g1/g2/g3 | 6011/6012/6013 | v1 JSON-Line 客户端端口（模拟现有 v1=6000 语义，三实例错开避免冲突） |
| gateway g1/g2/g3 | 7011/7012/7013 | v2 BinaryFrame 客户端端口（现有 v2=7000 语义） |
| gateway（后续，非本卡实现） | 8011..8013（预留） | 跨 Gateway v2 TCP RPC（仅 P4-05 确认拆进程时才启用） |
| mysql | 3306 | 项目既有连接 |
| redis | 6379 | Presence（占位端口） |
| kafka | 9092 | Outbox 事件（占位端口） |

## 3. 网络

- 单一 compose bridge 网络 `chatnet`（隔离，不映射宿主 3306/6379/9092，仅 Gateway 客户端端口对宿主开放）。
- 三个 Gateway 互为对等：任何 Gateway 都可能成为任意 Session 的路由目标，无主从。
- 分区故障（spec §2"网络分区"行）即指 Gateway 与 redis/kafka 同网络但分区、或宿主与 compose 网络隔离场景；MySQL 网络独立于 broker 网络，允许单侧故障。

## 4. 存储布局

| 数据 | 位置 | 生命周期 |
|------|------|----------|
| Message/Delivery/Outbox 持久化 | mysql 命名卷 `mysql-data` | 持久（真相源，chaos 后不丢） |
| Presence 条目 | redis 内存（TTL） | 可丢失；丢失只降级路由，不丢消息 |
| Kafka 事件 | kafka 数据卷 `kafka-data` | 可重放（retention 内）；丢失由数据库状态幂等兜底 |
| 日志/指标 | gateway 各自 stdout + 卷 `gateway-logs` | 每实例独立，供 P4-06 定位 |

## 5. 布局不变式（供 P4-06 chaos 核对）

- 任意 Gateway kill（含 `-9`）：其余两个 Gateway 的 accept 继续；被杀实例的 Delivery 依赖 lease 到期重领，不依赖人工干预。
- redis down：新登录与跨节点直投暂停，durable accept 继续；恢复后自动恢复，无需改库。
- kafka 单 broker（开发拓扑）：consumer 重放幂等；P4-06 的 broker 故障测试以 pause/restart 代替多副本切换（单机开发环境），主从切换留真实部署验证。
- 三实例共享同一 MySQL schema 与 migration 版本；不引入每实例私有 schema。
