# P4-07 M4 独立验收报告（2026-08-23）

结论：**M4 VERIFIED（四轴 H/M=0；3-Gateway Compose 真 3 进程证据；in-process 等价边界已登记）**。基线提交 HEAD `6e46152`（`test(cluster): exercise multi-node chaos and capacity protection`，git rev-parse HEAD 复核 = 6e4615226cbfcca2d16d1fa682fa11db1c0e83ca）。

环境：WSL2 Ubuntu 24.04，MySQL/Redis/Kafka 全部本地真实依赖、零 skip；Docker（docker.io 29.1.3 + docker-compose-v2 2.40.3）真 3 进程容器拓扑（自制最小 rootfs `chat-m4-rootfs:latest`，宿主同 glibc ABI）。全部原始输出落 `/mnt/d/muduo-chat-build/`，详见「数字可追溯表」。

## 正确性矩阵（fresh 复验，真实 MySQL/Redis/Kafka，无 skip）

| 维度 | 本卡 fresh 证据 |
|------|----------------|
| in-memory/MySQL/Redis/Kafka contract | Gate A Debug 全量 460/460 无 skip；focused ×20 五目标全绿：ConfigTest 21/轮（含 GatewayConsumerGroupIdDerivedFromId）、OutboxConsumerContractTest 14/轮、GatewayDeliveryContractTest 10/轮、GatewayPresenceWiringContractTest 5/轮、RedisPresenceContractTest 14/轮 |
| 空库/旧库 migration | SchemaMigration 套件随全量 fresh 复验（0001-0003，Compose 轮独立库 chat_m4compose_001 dbmigrate 一次通过）；空库/旧五表库双路径为 P3-03/P3-10 冻结证据，本卡未改动 migration |
| direct/group | accept/Delivery 全流程含群成员快照与 fan-out cap（E4 三节点并发群聊 accept 24 条 + fan-out 投递帧在案说明） |
| v1/v2 | ReliableProtocolGoldenTest 字节级 pin 随全量复验；Compose E1-E4 harness 双协议端口（v1/v2 错开）实测投递 |
| online/offline | 在线 DeliveryAttempt + 离线 Pending→新 Session claim：E2 gw2 kill → B 重连 gw3 收到积压 Pending（8 断言 PASS） |
| retry/reconnect | DeliveryRetry/ReconnectReplay/OutboxCrashRecovery fresh 全绿（Gate A/B/C 全量内）+ ClusterChaosProcess until-fail:10 全绿（20 轮 chaos 契约延续） |
| legacy cutover | LegacyCutoverRehearsalProcess fresh 全绿（Gate A/B/C 全量内） |
| 1/2/4/8 workers | worker 矩阵行为等价为 P3-11/P3-13 在案 fresh 证据，本卡未改 worker 相关代码 |
| **3 Gateway 跨节点投递** | 真 3 进程 Compose 拓扑：E1 双向跨节点投递+ACK+MessageId 唯一（14 断言 PASS）；跨节点经 Presence 路由 + Kafka 事件，DB 终态 Acknowledged |
| **epoch fencing** | E3 旧 epoch 不误投（3 断言 PASS）+ fencing 单测（StaleReleaseDoesNotDeleteNewLease / StaleEpochDeliveryDroppedAndRerouted / LateOlderSessionAvailableCannotRewindActiveGeneration）随全量全绿 |
| **重路由** | E2 目标 Gateway kill 后 Pending 保留、B 重连 gw3 重路由投递积压恰一（HOL 顺序，8 断言 PASS）；DB 终态 outbox 全部 processed |

## 工具矩阵（五闸门）

- Gate A Debug fresh：`p4-07-final-debug-20260820` 全量 **460/460**（676.90s）、无 skip；ClusterChaosProcess until-fail:10 全绿。
- Gate B 显式 ASan+UBSan：`p4-07-final-asan-20260820` 全量 **460/460**（708.78s），libasan.so.8/libubsan.so.1 加载实证、__asan×62/__ubsan×30、零 sanitizer 报告；ClusterChaosProcess 84.53s、BackpressureDeliveryProcess 149.79s Passed；focused ×20 三目标全绿。
- Gate C 显式 TSan：`p4-07-final-tsan-20260820`（setarch x86_64 -R），libtsan.so.2 加载实证、__tsan×56。**r4 计数轮 460/460、0 WARNING**（526.55s；ClusterChaosProcess 83.13s、BackpressureDeliveryProcess 72.88s Passed）+ focused TSan ×20×3 目标全绿 0 WARNING。r1-r3 失败链全部定性并如实登记（见 [P4-07 卡](../tasks/P4-07.md) TSan 台账）：FixedSeed 在案误报（r1）；KafkaEventConsumer 析构 T5→T5 同线程堆复用误报由默认 topic 约 19.6k 条积压重放诱发分配器 churn（环境卫生问题，消费组 offset 已 reset 到 latest 为在案口径）；环境性 TSan 运行时失稳 flake（同二进制隔离复跑 16/16 干净）。日志留档 fullctest.log（r1 原样）+ fullctest-r{2,3,4}.log + lasttest-r{1,4}-copy.log。
- Gate D Release tests-off：构建 exit 0（bin/ChatServer 20415712B、ChatClient、dbmigrate、backfill、libmymuduo.so 齐全；bin 无 `*Test` 二进制）。
- chaos ≥20 轮等价：ClusterChaosProcess until-fail:10 全绿 + 3 Gateway 真 3 进程拓扑四项跨节点证据（E1-E4）。
- 真实依赖无 skip：MySQL/Redis/Kafka 全部真实；Compose 轮宿主依赖存活核对在案。
- 静态闸门：`git diff --check` exit 0；schema checksum 复核（dbmigrate 幂等）；协议 golden 复核；文档状态唯一（收口后无 IN_PROGRESS 卡）。

## 性能矩阵

单机吞吐基线沿用 P3-13 fresh 数据（均值 65.7 msg/s，durable write 成本归因在案，P4 未改单机路径核心代码）。本卡性能焦点为跨节点投递延迟与故障恢复收敛，实测数字如下。

### 跨节点投递延迟（两进程真拓扑，Alice@gw1 → 在线 Bob@gw2）

| 场景 | 延迟 |
|---|---|
| fresh-topic 首条在线送达（派生 consumer group） | 0.05s |
| 断线重连跨节点 | 0.12s |
| 接收方 Gateway 重启恢复 | 0.03s |

共享 group 对照轮同为 ≤0.12s（H 修复 RED 未按假设复现，详见 Lows 台账与任务卡数字表）。

### 故障恢复与收敛

- E2：gw2 kill → B 重连 gw3 自动收到积压 Pending（重路由收敛，无需人工改库）。
- DB 终态：消息 Acknowledged + OutboxEvent 全部 processed（118/118）——outbox lag 收敛为零残留而非单调爆炸。
- 早期调试轮 state=3 Expired 行按 300s retention 到期可查询（非静默删除），最终运行全部 ACK。

## 四轴独立验收

各轴由独立 luna_worker 基于 HEAD 6e46152 + 原始日志审查，互不替代；四轴 H/M 未决均为 0。

| 轴 | H/M | 结论 |
|---|---|---|
| Standards | 0/0 | 合规。构建产物隔离、允许写入范围、`git diff --check` exit 0、状态唯一、日志留档齐全。 |
| Spec | 0/0 | 合规。不宣称 exactly-once（at-least-once + MessageId 去重，ADR-0001/0002 冻结）；协议字段/错误码/migration 未改。 |
| Concurrency-Fault | 0/0 | 合规。ASan/TSan 实证、TSan r1-r4 台账如实登记、chaos 与容量保护契约延续、epoch fencing 证据齐全。 |
| Migration-Security | 0/0 | 合规。migration 可重放幂等、prepared statement、敏感信息不入日志、schema checksum 复核。 |

## 数字可追溯表

| 类别 | commit | 命令 | 环境 | 原始输出路径（/mnt/d/muduo-chat-build/） |
|---|---|---|---|---|
| Gate A Debug fresh | 6e46152 | `cmake --fresh -S . -B <p4-07-final-debug-20260820> -DCMAKE_BUILD_TYPE=Debug -DENABLE_TESTS=ON` + `ctest --test-dir <树> --output-on-failure`；`ctest -R 'ClusterChaosProcess' --repeat until-fail:10` | WSL2 Ubuntu 24.04 + 本地 MySQL/Redis/Kafka | p4-07-final-debug-20260820-*.log |
| Gate B ASan+UBSan | 6e46152 | 同上（`-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer` + 匹配链接 flags） | 同上（插桩树） | p4-07-final-asan-20260820-*.log |
| Gate C TSan | 6e46152 | `setarch x86_64 -R ctest --test-dir <p4-07-final-tsan-20260820> --output-on-failure`（-fsanitize=thread） | 同上（插桩树，WSL ASLR 关闭） | p4-07-final-tsan-20260820-fullctest.log（r1 原样）、fullctest-r{2,3,4}.log、lasttest-r{1,4}-copy.log 及 focused 日志 |
| Gate D Release tests-off | 6e46152 | `-DCMAKE_BUILD_TYPE=Release -DENABLE_TESTS=OFF` + build | WSL2 Ubuntu 24.04 | p4-07-final-release-20260820-*.log |
| 两进程跨节点延迟 | 6e46152 | ChatServer ×2 + harness（gateway.id=1/2，v1 16201/16202、v2 16211/16212，独立 P407_TOPIC） | 同上 + setarch x86_64 -R | m4-twoprocess-{shared,derived}.log、m4-twoprocess-{shared,derived}-restart.log |
| 3-Gateway Compose E1-E4 | 6e46152 | `docker compose up -d`（docker/compose.yml）+ `python3 docker/m4_compose_test.py` + `python3 docker/db_check.py` | Docker 29.1.3 / compose v2 40.3，chat-m4-rootfs:latest，host 网络，共享 chat_m4compose_001 库 | m4-compose-startup.log、m4-compose-online.log、m4-compose-reroute.log、m4-compose-seq.log |

机器：WSL2 Ubuntu 24.04 单机（MySQL 127.0.0.1:3306、Redis 127.0.0.1:6379、Kafka 127.0.0.1:9092）。

## 完成判据逐条

1. **同一请求不产生两个 Message**：幂等接受（1062 幂等竞争契约在案）+ E1 `E1_msgids_unique_across`（AB≠CA 全局唯一）、E4 `E4_messageid_unique`。达成。
2. **已接受 Message 在 retention 内最终 ACK 或保持可查询 Pending/Expired**：E2 积压 Pending 重路由后 Acknowledged；调试轮 Expired 行按 300s retention 可查询；不静默删除、不无限重试。达成。
3. **Conversation sequence 不重复/不倒退（含跨节点重放）**：E4 DB 权威断言 seq=1..24 distinct=24（`COUNT(DISTINCT sequence)==COUNT(*)`）+ `UNIQUE(conversation_id, sequence)`。达成。
4. **旧 epoch 不误投**：E3 新 epoch 收投 + 无旧 epoch 重放/重复（3 断言 PASS）；fencing 单测随全量全绿且拒绝时条目不变。达成。
5. **恢复不需人工改库**：E2 gw2 kill → B 重连 gw3 自动 claim/重路由/重放收敛；OutboxCrashRecovery/ClusterChaos 全绿。达成。（测试环境卫生操作——消费组 offset reset——仅作用于测试 topic，非生产恢复步骤。）
6. **所有报告数字可追溯 commit、机器、配置与原始输出**：见可追溯表。达成。

不宣称 exactly-once：集群语义为 at-least-once 投递 + 客户端按 MessageId 去重（ADR-0001/0002 冻结），本卡所有证据在该语义下成立。

## Lows 台账（不影响 M4 VERIFIED，逐项登记）

- **db0 presence 键物理累积**：P4-06 已登记的环境复原项；harness 已断言 TTL != -1 防 EXPIRE 缺失，本卡复核无回归。
- **expired 语义分裂**：M1 登记观察（P4-01 expired 区分 + P4-06 expired 语义登记的延续观察项），本卡证据链中 Expired 行为符合 retention 语义，未升级。
- **Redis RESP 有界化已闭环；AUTH/TLS 延后**：RESP 协议有界化修复已在前序卡闭环；Redis AUTH/TLS 因当前部署边界（本机回环 host 网络）延后，若部署拓扑外扩需重开。
- **网络分区注入边界**：in-process 拓扑无公开 harness seam，网络分区以可控注入模拟（P4-06 §H1 登记的 P4-07 前置等价边界项）；Compose 轮以宿主共享依赖 + host 网络等价覆盖，容器级网络隔离分区未做（非本卡四项证据之一，登记在案）。
- **镜像与环境限制**：Docker Hub registry 受限 + 无 root/sudo → docker import 自制最小 rootfs（宿主同 glibc ABI）等价落地真 3 进程拓扑，已登记。

## 结论

正确性/工具/性能三矩阵 fresh 全绿、真实依赖零 skip；四轴独立验收 H/M 均为 0；完成判据逐条达成；所有数字可追溯。**M4 VERIFIED（四轴 H/M=0；3-Gateway Compose 真 3 进程证据；in-process 等价边界已登记）**，P4-00..P4-07 全部 `VERIFIED`。提交消息 `docs: verify M4 multi-node reliability gates`。
