# P3-13 M3 独立验收与性能矩阵报告（2026-08-15）

结论：**M3 VERIFIED（四轴 H/M=0）**。基线提交 HEAD `80a08e7`（`test(message): exercise the reliable delivery fault matrix`，git rev-parse HEAD 复核）；工作树 C++/schema/协议零改动，本卡只含报告、状态与必要文档。

环境：WSL2 Ubuntu 24.04（kernel 6.6.87.2，Intel i9-14900HX 32 逻辑核），MySQL 8.0.46 本地 127.0.0.1:3306（pool=5）。全部原始输出落 `/mnt/d/muduo-chat-build/`，详见「数字可追溯表」。

## 正确性矩阵（全部 fresh 复验，真实 MySQL，无 skip）

| 维度 | 本卡 fresh 证据 |
|------|----------------|
| in-memory/MySQL contract | `ReliableMessagingContractTest` InMemory/MySQL 双 adapter 契约双跑全绿；FixedSeedRandomOps 固定种子 500 轮不破坏不变量 |
| 空库/旧库 migration | `SchemaMigrationTest` 11/11；空库 vs 旧五表库升级 structure dump 一致；`ReliableMessageSchemaTest` 契约列/键/FK 断言；legacy backfill 计数/hash 守恒、重复运行不增加 Message |
| direct/group | Direct 与 Group 的 accept/Delivery 全流程 process 证据；群成员快照与 fan-out cap=100 拒绝 |
| v1/v2 | `ReliableProtocolGoldenTest` 14 用例 + protocol golden fixture 字节级 pin；DualProtocol 等价 process（DomainCharacterization） |
| online/offline | 在线 DeliveryAttempt + 离线 Pending→新 Session claim；登录补投；legacy implicit-ack 与 v2 ACK 双路径 |
| retry/reconnect | `DeliveryRetryTest` 7、`ReconnectReplayProcess`、`OutboxCrashRecoveryProcess` fresh 全绿；ACK 丢失重投同 MessageId、attempt/retry 计数一致 |
| legacy cutover | `LegacyCutoverRehearsalProcess`（schema checksum + 旧二进制回滚 `rb_old_*` 真实执行非跳过）fresh 全绿 |
| 1/2/4/8 workers | repeat 套件行为等价（P3-11 23/23）+ 本卡 worker 矩阵 fresh（2/4/8 × DomainCharacterization/MultiReactor ×2 + DeliveryAck w2/w4，14 份日志全 PASS/exit 0） |

## 工具矩阵

- 全新 Debug：fresh 树全目标 CTest 全绿、无 skip（计数与 `ctest -N` 一致）。
- 显式 ASan+UBSan：fresh 插桩树全量零报告（libasan/libubsan 加载、`__asan`/`__ubsan` 命中、UBSAN halt_on_error）。
- 显式 TSan：fresh 树 `setarch x86_64 -R ctest` 0 WARNING、无真实 skip；FixedSeedRandomOps 同线程堆复用误报按历史诊断复跑收敛（P3-04/05/11 在案同类，未转 H）。
- Release tests-off：fresh 生产构建 exit 0（GCC 13.3 瞬态 ICE 经 `--parallel 1` 重试收敛；bin/ChatServer、ChatClient、dbmigrate、backfill、lib/libmymuduo.so 齐全，无 `*Test` 二进制）。
- 并发/fault 重复：SessionSerialExecutor 高压 executor30（Debug 树）无断言失败 + focused TSan ×20 0 WARNING；`ReliableMessageFaultProcess` 10 kill 点 ×10 轮全绿，故障后由指标定位且恢复符合 ADR/spec §4。
- process/真实依赖不得 skip：MySQL contract/integration 用真实测试库；migration 空库与已发布 schema 双路径；schema checksum、cutover 演练真实执行。
- 静态闸门：`git diff --check` exit 0（无尾随空白）；schema checksum 复核；protocol golden 复核；文档状态唯一（仅本卡活动）。

## 性能矩阵

复用 P2 workload（`tools/chat_load.py`，16 并发 reg→login→离线单聊循环，15s），executor.workers=1 queue_capacity=64 pool_size=5。每轮重建专用库 + dbmigrate；SIGUSR1 @7s 抓 METRICS。原始输出 `/mnt/d/muduo-chat-build/p3-13-bench-reliable-direct-r{1..4}.log`（含模式注记）。

| round | msg/s | ok | p50 ms | p95 ms | p99 ms | errors | METRICS @7s（pool_active / queue / drop / outbox_lag / duplicates） |
|---|---|---|---|---|---|---|---|
| r1 | 49.8 | 772 | 294.23 | 404.39 | 540.64 | 0 | pool_active=1 queue=15 drop=0/0 outbox_lag=162 duplicates=0 |
| r2 | 46.8 | 728 | 313.76 | 471.02 | 766.23 | 0 | pool_active=1 queue=15 drop=0/0 outbox_lag=168 duplicates=0 |
| r3 | 84.2 | 1302 | 172.20 | 281.59 | 334.64 | 0 | pool_active=1 queue=15 drop=0/0 outbox_lag=233 duplicates=0 |
| r4 | 81.8 | 1263 | 169.99 | 290.66 | 574.81 | 0 | pool_active=1 queue=15 drop=0/0 outbox_lag=340 duplicates=0 |

均值 65.7 msg/s、中位 65.8、区间 46.8-84.2；p50 170-314 ms、p95 282-471 ms、p99 335-766 ms。errors=0、executor drop=0/0、reliable_duplicates=0/conflicts=0、reliable_outbox_poison=0、oldest_pending_age ≈ 6.4-6.7 s（离线目标未 claim，Pending 保持可查询）。

### 比较与 durable write 成本

- P2-10 ~123 msg/s（单条 `INSERT OfflineMessage`，单 worker 串行 DB 写约 8 ms）；P3-11 w=1 均值 67.5（51.8-99.7）。
- 本卡 fresh 均值 65.7、区间 46.8-84.2：与 P3-11 w=1 区间完全重叠、均值持平（65.7 vs 67.5），约为 P2 的 0.53×。
- 归因（MySQLMessageStore.cpp:648/668/709/748）：accept 事务 = UPDATE Conversation.next_sequence + INSERT ChatMessage + INSERT OutboxEvent + INSERT MessageDelivery + COMMIT，每消息 MySQL 写工作约 P2 的 3-4×；外加 outbox relay 周期 claim/扫描（outbox_lag 随轮递增 162→340 = relay batch 100/5s 落后于接受速率）、16 条 conversation 同一目标用户行锁、executor 单 worker 串行。METRICS pool_active=1、queue=15（< 容量 64）证明瓶颈在 DB 串行写侧而非队列丢弃。与 P2 比较的 durable write 成本已如实解释，无未解释回退。

### 默认 worker 维持 1

workers=1 跑出 46.8-84.2（均值 65.7），落在 P3-11 w=1 区间（51.8-99.7）内；worker 矩阵 2/4/8 等价复核成立。无支持上调默认值的可复现显著性证据 → 维持 1，不改默认。

## 四轴独立验收

各轴由独立 `luna_worker` 基于 HEAD 80a08e7 + 原始日志与代码审查，互不替代；四轴 H/M 未决均为 0。

| 轴 | H/M | 结论 |
|---|---|---|
| Standards | 0/0 | 合规。行号 449→453 修正；worker 矩阵 fresh 补齐；允许写入范围/构建产物隔离/`git diff --check` 通过。 |
| Spec | 0/0 | 合规。spec 承诺逐条对照 ADR-0001/schema.md 一致；不宣称 exactly-once；工具局限如实披露。 |
| Concurrency/Fault | 0/0 | 合规。fault 原始证据、计数守恒、TSan 实证、有界队列/lane/Recorder；executor 高压措辞已澄清。 |
| Migration/Security | 0/0 | 合规。migration 可重放、无静默丢数据、prepared statement、敏感信息不入日志、schema checksum。 |

## 数字可追溯表

| 类别 | commit | 命令 | 环境 | 原始输出路径（/mnt/d/muduo-chat-build/） |
|---|---|---|---|---|
| 正确性 fresh | 80a08e7 | `ctest --test-dir <p3-13-final-debug-20260814> --output-on-failure` | WSL2 Ubuntu 24.04 + MySQL 8.0.46 本地 | p3-13-final-debug-20260814-* |
| worker 矩阵 fresh | 80a08e7 | `ctest -R 'DomainCharacterization\|MultiReactor\|DeliveryAck'` × 2/4/8 workers | 同上（Debug 树） | p3-13-worker-matrix-w{2,4,8}-{DomainCharacterization,MultiReactor}-p{1,2}.log；p3-13-worker-matrix-DeliveryAck-w{2,4}.log |
| ASan+UBSan | 80a08e7 | `ctest --test-dir <p3-13-final-asan-*> --output-on-failure` | 同上（插桩树） | p3-13-final-asan-* |
| TSan | 80a08e7 | `setarch x86_64 -R ctest --test-dir <p3-13-final-tsan-*> --output-on-failure` | 同上（插桩树） | p3-13-final-tsan-* |
| Release tests-off | 80a08e7 | `cmake -S … -B p3-13-final-release-20260815 -DCMAKE_BUILD_TYPE=Release -DENABLE_TESTS=OFF`；`cmake --build … --parallel 1` | WSL2 Ubuntu 24.04 | p3-13-final-release-20260815-{configure,build,build-j1,build-j1b}.log |
| 性能矩阵 | 80a08e7 | `tools/chat_load.py 127.0.0.1 <v1port> 16 15000` + SIGUSR1 METRICS | 同上（Debug 树，workers=1 queue=64 pool=5） | p3-13-bench-reliable-direct-r{1..4}.log / -server.log；p3-13-bench-direct.sh；库 chat_p313_bench_direct_r{1..4} |
| fault 矩阵 | 80a08e7 | `ctest -R ReliableMessageFaultProcess --repeat until-fail:10` | 同上（Debug） | p3-12 在案日志 + 本卡复验（P3-12 闸门记录） |

## 完成判据逐条

1. **同一请求不产生两个 Message**：幂等接受（同 key 重试返回原 MessageId/原 sequence）；reliable_duplicates=0/conflicts=0、1062 幂等竞争、backfill 重复运行不增加 Message。达成。
2. **已接受 Message 在 retention 内最终 ACK 或保持可查询 Pending/Expired**：DeliveryRetryTest/Retention、outbox relay、oldest_pending_age ≈ 6.4-6.7 s Pending 可查询；不静默删除、不无限重试。达成。
3. **Conversation sequence 不重复/不倒退**：`UNIQUE(conversation_id, sequence)` 实证 + 契约断言。达成。
4. **不宣称 exactly-once**：at-least-once + 客户端按 MessageId 去重；outbox docstring「exactly once」措辞已登记延后（台账）。达成。
5. **所有报告数字可追溯 commit、机器、配置与原始输出**：见可追溯表。达成。

## Lows 台账

已修复 2 项（本卡收口）：Standards L1 行号 449→453；Concurrency L2 executor 高压措辞澄清（executor30 为 Debug 树，TSan 维度由 focused-sessionserial-x20 承担）。另 Standards L2 worker 矩阵 fresh 缺失已于 2026-08-15 补跑补齐（14 份日志全绿）。

延后登记 5 项（不影响 M3 VERIFIED）：schema.md:30「本表不迁移数据」措辞（contract migration 阶段）；outbox_crash_recovery_test.py docstring「exactly once」措辞；shutdown_order_test.sh 未 pin OUTBOX_STOP 标记；ChatService.cpp:133-137 Friend 查询 snprintf %d（风格项，无注入路径）；p3-13 fresh 轮 harness.out 未保留（实践小缺口）。明细见 [P3-13 卡](../tasks/P3-13.md)。

## 结论

正确性/工具/性能三矩阵 fresh 全绿、真实依赖零 skip；四轴独立验收 H/M 均为 0；完成判据 5 条全部达成；所有数字可追溯。**M3 VERIFIED（四轴 H/M=0）**，P3-00..P3-13 全部 `VERIFIED`。提交消息 `docs: verify M3 reliable messaging gates`。
