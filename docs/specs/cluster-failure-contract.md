# P4 集群故障与所有权契约（P4-00 核心产出）

状态：`定稿`（P4-00 冻结目标；[ADR-0002](../adr/0002-cluster-ownership-and-failure-contract.md) 已 `accepted`；P4-01 起约束实现）

基线：`main` @ `6bfbf2e`

关联：

- [ADR-0002](../adr/0002-cluster-ownership-and-failure-contract.md)：本规范的承诺来源
- [P4 实施计划](../plans/post-p2-implementation-plan.md) §8（P4-00 任务定义、P4 全队列）
- [架构演进方案](../architecture/evolution-plan.md) §7（Presence/fencing 与 Outbox/Kafka）
- [Cluster context map](../architecture/cluster-context-map.md)：术语与所有权边界
- [message-reliability.md](message-reliability.md) §4：单机故障点表（P3，本规范是其集群扩展，不推翻单机承诺）
- [CONTEXT.md](../../CONTEXT.md)：单机术语来源；集群词汇在本规范/ADR/context map 定义，CONTEXT.md 同步待单独授权

## 1. 目标承诺

本规范把 ADR-0002 的承诺展开为可测试的集群降级语义，是 P4 全部实现的不可回退约束：

| 承诺 | 内容 |
|------|------|
| 单真相源 | MySQL 继续是 Message 真相源；MessageAcceptance/DeliveryAcknowledgement 状态永远只在 durable ledger，Redis 或 broker 都不能替代 |
| 连接归属 Gateway | 每条 TCP 连接恰属一个 Gateway（GatewayId 标识）；ConnectionId 只在所属 Gateway 内唯一，跨节点寻址用 GatewayId+SessionEpoch |
| Session 跨 Gateway 唯一 | 同一 User 全集群至多一个活动 Session；登录总是产生新 monotonic SessionEpoch，旧 epoch 的 renew/release 全部被 fencing |
| Presence 只含路由 | Presence 条目 `user_id -> {gateway_id, connection_id, session_epoch}` + TTL，不含任何消息真相 |
| 降级唯一 | 每个依赖故障（§2 六行）对 accept/login/delivery 各有一个且只有一个预期降级结果；本表之外不存在"应当成功/应当送达"的分支 |
| 不承诺集群 exactly-once | at-least-once 语义随跨节点扩展不变；客户端按 MessageId 去重 |

失败判定（停止规则，计划 §10）：Redis/Kafka integration 被 skip、同一请求出现两个 MessageId、跨节点出现第二套消息状态机、无法解释的分区恢复行为，立即停止。

## 2. 故障点表

每一行有且只有一个预期结果（accept/login/delivery 各自唯一）；表内场景之外的组合不承诺结果。术语见 context map。

| 故障点 | accept（唯一预期结果） | login（唯一预期结果） | delivery（唯一预期结果） |
|--------|------------------------|------------------------|--------------------------|
| 快速重登（断开后立即在另一 Gateway 重登） | 不受影响：accept 只依赖 MySQL 事务与已认证 Session 身份；同 `(sender, ClientMessageId)` 幂等继续生效，不产生第二个 Message | 重登成功：新 SessionEpoch 单调递增；旧 epoch 的 PresenceLease 被 fencing，旧 lease 的 renew/release 全部被拒 | 已接受但未投递的 Delivery 按新 lease 定向到新 GatewayId+epoch；仍发往旧 epoch 的迟到连接包被丢弃；不重复创建 Delivery |
| 旧 Gateway 延迟 release（用户已迁移，旧 Gateway 的 sessionClosed 迟到执行） | 不受影响（同上行） | 迟到的 release 是 compare-and-delete：携带旧 epoch，与当前 lease 不符时不删除新 lease；新登录不受干扰 | 路由以当前有效 lease 为准；旧 epoch 收包丢弃；release 与 claim 竞争下由 CAS + TTL 收敛到新路由，最终状态唯一 |
| 网络分区（Gateway 与 Redis/Kafka 控制面分区；MySQL 保持可达） | 不受影响：accept 与 Redis/Kafka 无依赖，durable accept 继续写入 MySQL | 新登录暂停（无法 claim/renew PresenceLease）；已登录会话在自身 TTL 内继续维持 | 跨节点直投暂停（无法 locate）；同 Gateway 本地投递继续；Pending Delivery 持久在 MySQL 不丢，分区恢复后按当前 lease 重投 |
| Redis down（整个 Redis 不可用；MySQL/Kafka 可达） | 不受影响：真相源不依赖 Redis，durable accept 继续 | 新登录暂停；已有 lease 到期后不再 renew，该 User 在 Presence 路由上视为离线（locate 失败）；本地持有连接上的会话与本地投递不受影响 | 跨节点直投暂停；本地投递继续；Pending 持久不丢，Redis 恢复后 claim 并重投 |
| Gateway kill（-9，进程崩溃，无 sessionClosed 回调） | 其余 Gateway 的 accept 不受影响；被杀 Gateway 上未提交事务随进程回滚，无部分状态 | 被杀 Gateway 上的连接断开；用户重登到其他 Gateway 时用新 epoch claim，旧 lease 到期后（TTL）释放 | 该 Gateway 名下 InFlight 无 sessionClosed 清理 → 依赖 lease 到期重领（P3-08 boot id 已支撑跨实例重领）；已接受 Delivery 保持 Pending 或被存活 Gateway 重路由；恢复后已接受消息 ACK 或保持可查询状态 |
| broker 重放（重复 publish、rebalance 或 crash 后重放已发布事件） | 幂等：重复 publish/consume 不改变 Message/Delivery 状态（数据库状态是幂等处理点），不产生第二个 Message | 无影响：broker 不参与 login 控制面 | 消费者对已处理事件幂等（fencing + dedup），不重复创建状态；offset 提交晚于状态处理；任意重放次数状态一致，Conversation partition 内顺序不倒退 |

自检：每个故障点行内 accept/login/delivery 均无空白格、无"可能/应当"分支；"或"仅允许 at-least-once 终态（ACK 或保持可查询状态）；不存在 `success/delivered` 等未定义承诺词（MESSAGE_ACCEPTED、已接受等固定词除外，语义同 message-reliability.md §8）。

## 3. 与单机契约的关系

- 单机全部承诺（message-reliability.md §1：durable accept、幂等、at-least-once、局部顺序、群快照、Session identity、legacy 兼容）跨节点保持，不因多节点弱化。
- 群消息的 Delivery 行仍由 accept 事务固定接收者快照；跨节点只改变"送往哪个 Gateway 投递"，不改变 Delivery 状态机（Pending/InFlight/Acknowledged/Expired）与 ACK 语义。
- 单机局部顺序不变式（同一 `(recipient, conversation)` 至多一个未确认 sequence 在途）在跨节点投递下仍然成立：路由变化只换投递端点，不换顺序裁决（顺序由 MySQL 事务与 Delivery 状态机决定，计划 §5 P3-11）。
- P4 故障表是 P3 故障表的集群层扩展：P3 表的单机故障行（DB busy、ACK 丢失等）在集群下同样有效，本表只补充跨节点新增的六行。

## 4. 未定值冻结清单

| 项 | 冻结任务 | 说明 |
|----|----------|------|
| PresenceLease TTL / renew 窗口 | P4-01/P4-02 | 冻结前记录；须大于跨节点投递往返与网络分区恢复时长，值由 P4-06 chaos 验证 |
| GatewayId 编码与分配 | P4-01/P4-02 | 静态配置还是启动注册，在 P4-01 RED 前定案并记录 |
| Redis 版本/部署参数 | P4-02 | 按 SOP §8 开始任务时重新核对官方版本与文档 |
| Kafka 版本/重放参数 | P4-03 | 同上 |
| compose 拓扑端口/存储布局 | P4-00 | 见 [gateway-compose-topology.md](../architecture/gateway-compose-topology.md)，adapter 落地时按图核对 |

## 5. 审阅与验证

- **审阅流程**：P4-00 冻结目标经两轴（语义一致性/计划衔接）与四轴（Standards、Spec、Concurrency/Fault、Migration/Security）独立对抗审查 H/M=0 后，P4-01 方可开工。
- **验证命令**：`rg -n 'GatewayId|ConnectionId|SessionEpoch|PresenceLease|DeliveryRoute' CONTEXT.md docs`；逐故障点核对 §2 表无未定义分支。
- **自检**：每个故障点有唯一预期结果；全文本不存在 `success`/`delivered` 等未定义承诺词。
