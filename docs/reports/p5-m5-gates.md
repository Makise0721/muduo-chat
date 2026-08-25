# P5-05 M5 独立验收报告（2026-08-25）

结论：**M5 VERIFIED（四轴 H/M=0×4；五闸门 fresh 497/497×3 + Release；P5-03B 性能优化合入 +19.86%；全部数字五元组定位）**。基线提交 HEAD `f0101e2`（`experiment(io): record spike skip evidence`，git rev-parse HEAD 复核 = f0101e22df79dda6ebefabff33126805ebf3e2f5）。

环境：WSL2 Ubuntu 24.04 单机（i9-14900HX / kernel 6.6.87.2-microsoft-standard-WSL2），回环 127.0.0.1，MySQL/Redis/Kafka 本机回环真实依赖、零 skip。全部原始输出落 `/mnt/d/muduo-chat-build/`，详见「数字可追溯表」。本项目无生产流量，全部性能数字为**实验环境**测量，不外推。

## 正确性矩阵（五闸门 fresh，真实 MySQL/Redis/Kafka，无 skip）

| 维度 | 本卡 fresh 证据 |
|------|----------------|
| contract | Telemetry/MetricsSnapshot/PrometheusEndpoint（P5-00）与 Reliable/Presence/Redis/Gateway/Outbox 族随三树全量 fresh（各 497 用例含全部契约套件） |
| migration | SchemaMigration 套件随三树全量 fresh（0001–0003 未在 P5 改动；checksum 幂等复核随套件）；Debug focused 另含 SchemaMigrationTest 11/11 |
| direct/group | **P5-03B 已改服务器回复编码路径（ProtocolCodec.cpp encode seam + ChatService 接线）→ 本维度 fresh**：由「三树全量（Debug/ASan/TSan 各 497/497 各含全部用例）+ ASan focused ProtocolEncodeContract ×20 + TSan focused ProtocolEncodeContract/DomainCharacterization ×20」承载；ReliableProtocolGolden 字节 pin 与 MultiReactor 随全量 fresh |
| v1/v2 | ReliableProtocolGoldenTest 字节级 pin + DualProtocolCharacterizationTest 等价随三树全量 fresh |
| online/offline | 在线 DeliveryAttempt + 离线 Pending→新 Session claim：DeliveryAckProcess/ReconnectReplayProcess 随全量 fresh 全绿 |
| retry/reconnect | DeliveryRetry/ReconnectReplay/OutboxCrashRecovery fresh 全绿（三树全量内） |
| worker 矩阵 | 1/2/4/8 行为等价为 P3-11/P3-13 在案 fresh 证据（P3-13 worker 矩阵 14 份日志全 PASS），P5 未改 executor/worker 代码（P5-03B 仅回复序列化），沿 P4-07 口径登记归属不重复全矩阵；本卡三树全量未发现相关回归 |
| chaos 等价 | ClusterChaosProcess 随三树全量 Passed（Gate A r2 单轮 82.77s），until-fail ≥10 轮全绿的 P4-06/P4-07 在案口径延续 |
| 真实依赖 | MySQL/Redis/Kafka 全部真实零 skip |

### Debug focused 52/52 集合如实列出

Gate A 轮 focused 抽认共 52/52 全绿（75.99s）：SchemaMigrationTest 11 + MessageAcceptanceApplication 16 + ReliableProtocolGolden 14 + DualProtocolCharacterizationTest 2 + DeliveryRetryTest 8 + ReconnectReplayProcess 1。该集合**不含** ProtocolEncodeContract（encode seam 的 focused 覆盖由 ASan/TSan 轮承载，见上）。

## 工具矩阵（五闸门）

- Gate A Debug fresh：`m5-final-debug-20260825` 全量 **497/497**——r1 496/497（618.52s，唯一失败 #120 TcpServerTest.ForceCloseAllWithWorkersIsLoopAffine 单次瞬时失败）、r2 计数轮 **497/497**（615.97s）。flake 定性关闭：同用例隔离 `--repeat until-fail:20` **×20 全绿**（`diag-loopaffine-repeat.log`），定性为环境性 flake 非代码回归。
- Gate B 显式 ASan+UBSan：`m5-final-asan-20260825` 全量 **497/497**（722.02s），libasan/libubsan 加载与逐二进制插桩实证（`loaded.log`，如 ChatServer __asan×66/__ubsan×32）；**双处零报告**（fullctest 日志与 LastTest 快照 AddressSanitizer/UBSan 报告计数均为 0）；focused ×20 三目标 **640/640** 全绿（ReliableMessageMetricsTest 11 + ReliableProtocolGolden 14 + ProtocolEncodeContract 7 = 32 用例 ×20 轮）。
- Gate C 显式 TSan：`m5-final-tsan-20260825`（setarch x86_64 -R，WSL ASLR 关闭），libtsan.so.2 加载实证（`loaded.log`）。r1 计数轮 **497/497**（735.37s，exit 0）、**双处 0 WARNING**（fullctest 大小写敏感 WARNING=0 + LastTest 快照 WARNING=0；LastTest 中 2 条 mysql-cli `[Warning]` 为 dbmigrate 工具输出，跨前序闸门一致存在，非 TSan 报告）；focused ×20 三目标 **440/440** 全绿 0 WARNING（ReliableProtocolGolden 14 + ProtocolEncodeContract 7 + DomainCharacterization 1 = 22 用例 ×20 轮）。轮前 topic 卫生已执行并留痕：消费组 offset reset latest（v2 卫生脚本，`m5-final-tsan-20260825-topic-hygiene.log` 结尾 **HYGIENE-PASS**、remaining-laggy-rows=0），沿 P4-07 Gate C 台账口径预防默认 topic 积压重放诱发的分配器 churn 误报。
- Gate D Release tests-off：构建 exit 0（`CMAKE_BUILD_TYPE=Release`、`ENABLE_TESTS:BOOL=OFF` 实证；bin/ChatServer **21987504B**、ChatClient、backfill、chat-bench、dbmigrate、logger-bench + lib/libmymuduo.so、lib/libchatserver_core.a 齐全；bin 无 `*Test` 二进制，grep 计数 0）。
- 真实依赖无 skip：MySQL/Redis/Kafka 全部真实，三树全量无 skip。
- 静态闸门：`git diff --check`（本卡 scope）exit 0；schema checksum 复核随 SchemaMigration 套件 fresh；协议 golden 字节 pin 随全量 fresh。

## 性能矩阵（五元组定位逐项）

机器/环境统一标注：WSL2 Ubuntu 24.04 单机（i9-14900HX / kernel 6.6.87.2-microsoft-standard-WSL2）、回环、Debug 构建树、MySQL/Redis/Kafka 本机回环真实依赖。**全部为 synthetic benchmark 实验环境数字，不外推生产。**

| 数字源 | 内容 | 五元组定位 |
|---|---|---|
| P5-02 基线 | 9 场景 × 5 重复（connect 9383.00 / echo 21368.14 / slow-consumer 字节一致性 / reliable-direct 25.90 / reliable-group 16.00 / hot Conversation 51.90 / DB backpressure 锁窗信号 / Redis down ≈1.0× / Kafka pause 滞留收敛），均值 + 95% CI（t）+ CV | commit `350d4e2`（350d4e21d027336e7a561fe345cc500d76715aae，p5-baseline.md 环境表 / P5-02 卡 / P5-05 卡 Interface 冻结表 / r1-final.json 内嵌 commit 四方一致）；机器与环境见上；配置 = 方法学冻结（warmup 1×5s、≥5 重复、bench-result-v1）；原始产物 `/mnt/d/muduo-chat-build/p5-02-baseline-r1-final.json`（内嵌 commit 已复核）；统计脚本 `tests/scripts/p5_02_stats.py`；报告 [p5-baseline.md](performance/p5-baseline.md) |
| P5-03B 对比 | reliable-direct before 35.50 msg/s（CI 33.10..37.90，CV 0.055，n=5）→ after **42.55**（CI 41.03..44.07，CV 0.050，n=10 两批各 5 轮）= **+19.86%** ≥15% ✓、95% CI 不重叠 ✓、p99 582.19ms vs 623.07ms = **-6.56% 更优** ✓、p50 -16.7%、errors 全 0、正确性零回退 | commit `3fb46bb`（perf(message): eliminate serialization copies in protocol replies，已合入 main）；机器与环境同上（同负载同方法学）；原始产物 `/mnt/d/muduo-chat-build/p5-03b-after-final{,2}.json/.log` + before `p5-03b-before.json/.log`；统计口径同 stats.py；台账 [hotspot-ledger.md](performance/hotspot-ledger.md) P5-03B 行 |
| P5-03A/C FAIL 如实呈现 | A：批量 ACK 合并 after 23.90 vs before 27.00 = **-11.5% 无提升**、ACK p50/p95/p99 反超回退线（接线复杂度过高未接线）；C：processed 标记批量化 after 45.30 vs before 42.80 = **+5.84% <15%** 且 CI 重叠（接线落地 N→1 双证据成立，收益面不足非实现缺陷）；D：锁竞争缓解评估为高风险/范围失控不实施——失败结论入矩阵，不宣称收益 | A @ c3474a4 基线、产物 `/mnt/d/muduo-chat-build/p5-03a-{before,after}.json`；C @ 3fb46bb 基线、产物 `/mnt/d/muduo-chat-build/p5-03c-{before,after}.json` + 接线证据 `p5-03c-general.log`；D 无实现无采集（评估登记）；统计口径同上；台账 [hotspot-ledger.md](performance/hotspot-ledger.md) 对应行，commit/revert 逐项可追溯 |
| P5-04 spike 结论 | Buffer 内存池化进入条件失效：HEAD f4ae57f 重测 `vector<char>` emplace_back 分配计数 **92,406** vs @350d4e2 anchor 317,263 = **-70.9%**（P5-03B 序列化拷贝消除效应），分配 churn 不再为主要瓶颈；io_uring 跳过（kernel 6.6.87.2-microsoft-standard-WSL2 原始 syscall `io_uring_setup` EINVAL）；sendmmsg/recvmmsg 证据不足不开工（无 syscall 占比 profile）；MSG_ZEROCOPY 弱证据不开工——全候选有据跳过，不宣称未经测量的能力 | 分配计数重测 @ f4ae57f、gprof 产物 `/mnt/d/muduo-chat-build/p5-04-gprof-*`（profile.txt/run.json）；io_uring 探测程序 `/mnt/d/muduo-chat-build/p5-04-spike/iouring_probe.c`；bench before `p5-04-before.json`；台账 [hotspot-ledger.md](performance/hotspot-ledger.md)「P5-04 收口登记」 |

## 数据性质分类（三类分开标注）

- **synthetic benchmark**：bench 全部——P5-02 九场景、P5-03 各项 before/after 复测、P5-04 分配计数重测与 bench before echo 17788.42 msg/s（CV 0.0180）。
- **故障演练**：P5-01 promtool test rules 断言（12 条 firing+resolve）+ Kafka broker SIGSTOP 真实演练 ×3（firing→自动 resolve）+ tarball 等价运行 smoke SMOKE_ALL_PASS；P5-02 db-backpressure/redis-down/kafka-pause 场景注入；ClusterChaosProcess chaos 契约（在案 until-fail 口径）。
- **真实生产数据**：**无**。本项目无生产流量，全部数字标注「实验环境」，不外推；本报告无 SLA/生产承诺措辞（§9.9 停止规则）。

## 四轴独立验收

各轴由独立 luna_worker 基于 HEAD f0101e2 + 原始日志审查，互不替代；四轴 H/M 未决均为 0（L=4，均为表述精度项，落实位置见下表）。

| 轴 | H/M | 结论 |
|---|---|---|
| Standards | 0/0 | 合规。构建产物隔离（fresh 树 m5-final-*）、允许写入范围、`git diff --check` exit 0、状态唯一、日志留档齐全（configure/build/fullctest/lasttest/focused/hygiene/warningcheck 分文件）。 |
| Spec | 0/0 | 合规。at-least-once + MessageId 去重语义不变（ADR-0001/0002 冻结）；协议字段/migration 未改（ReliableProtocolGolden 字节 pin + SchemaMigration 套件随全量 fresh）；实验 SLO 措辞未升级为 SLA。 |
| Concurrency-Fault | 0/0 | 合规。ASan/UBSan 双处零报告、TSan 双处 0 WARNING、Gate A flake ×20 定性关闭留痕、topic 卫生前置执行留痕、chaos 契约延续。 |
| Migration-Security | 0/0 | 合规。migration 可重放幂等（checksum 幂等复核）、prepared statement 基础设施延续、敏感信息不入日志、schema checksum 随套件复核。 |

### L 项落实说明（L-1..L-4）

| 项 | 内容 | 本报告落实位置 |
|---|---|---|
| L-1 | P5-02 基线 commit 用证据值 **350d4e2**（不得写 c3474a4） | 性能矩阵 P5-02 行与可追溯表均写 350d4e2（四方一致已复核：p5-baseline.md 环境表 / P5-02 卡 / P5-05 卡 Interface 冻结表 / r1-final.json 内嵌 commit）；c3474a4 仅出现在 P5-03A/C 卡的各自基线行，与本报告 P5-02 基线无关 |
| L-2 | P5-03B 终态标签用卡面原词 | 本报告引用为「PASS/达标 + 已提交 main @ 3fb46bb」（性能矩阵 P5-03B 行），未改写标签措辞 |
| L-3 | direct/group 维度 fresh 归属精确表述 | 正确性矩阵 direct/group 行：三树全量承载 + ASan focused ProtocolEncodeContract ×20 + TSan focused ProtocolEncodeContract/DomainCharacterization ×20；Debug focused 52/52 集合如实单列且注明不含 ProtocolEncodeContract（encode seam 的 focused 覆盖不在 Debug 轮声称） |
| L-4 | Lows 第 8 项镜像拉取复查补登探测结论 | Lows 台账第 8 项：registry-1.docker.io/v2 返回 401（API 层可达）、docker pull 路径仍 BLOCKED（credential helper exec format error 未修复）→ tarball fallback 维持 |

## 数字可追溯表

| 类别 | commit | 命令 | 环境 | 原始输出路径（/mnt/d/muduo-chat-build/） |
|---|---|---|---|---|
| Gate A Debug fresh | f0101e2 | `cmake --fresh -S . -B m5-final-debug-20260825 -DCMAKE_BUILD_TYPE=Debug -DENABLE_TESTS=ON` + `ctest --test-dir <树> --output-on-failure`；flake 定性 `ctest -R ForceCloseAllWithWorkersIsLoopAffine --repeat until-fail:20` | WSL2 Ubuntu 24.04 + 本地 MySQL/Redis/Kafka | m5-final-debug-20260825/{configure,build,fullctest,fullctest-r2,focused,diag-loopaffine-repeat}.log |
| Gate B ASan+UBSan | f0101e2 | 同上（显式 `-fsanitize=address,undefined -fno-omit-frame-pointer` + 匹配链接 flags） | 同上（插桩树） | m5-final-asan-20260825-{configure,build,loaded,fullctest,lasttest-snapshot,focused-x20}.log |
| Gate C TSan | f0101e2 | topic 卫生 reset → `setarch x86_64 -R ctest --test-dir <m5-final-tsan-20260825> --output-on-failure`（-fsanitize=thread） | 同上（插桩树，WSL ASLR 关闭） | m5-final-tsan-20260825-topic-hygiene.log、m5-final-tsan-20260825-{configure,build,loaded,testlist,fullctest,lasttest-r1-copy,warningcheck,focused-x20}.log |
| Gate D Release tests-off | f0101e2 | `-DCMAKE_BUILD_TYPE=Release -DENABLE_TESTS=OFF` + build（GCC ICE j2→j1 重试成功） | WSL2 Ubuntu 24.04 | m5-final-release-20260825-{configure,build-j2}.log、bin/ 实物清单 |
| P5-02 基线 bench | 350d4e2 | `python3 tools/bench/run.py --build-dir p5-00b-final-debug-20260824 --reps 5 ...` + `tests/scripts/p5_02_stats.py` | 同上（Debug 树、回环、负载隔离在案） | p5-02-baseline-r1-final.json（合并产物，内嵌 commit 350d4e2）、p5-02-baseline-r1*.log/json、p5-02-gprof-profile.txt |
| P5-03B before/after | 3fb46bb | 同方法学复测（n=5 + n=10） | 同上 | p5-03b-before.json/.log、p5-03b-after-final.json/.log、p5-03b-after-final2.json/.log、p5-03b-fullctest.log、p5-03b-focused.log |
| P5-03A/C FAIL 产物 | c3474a4 / 3fb46bb 基线 | 同方法学复测 n=5 | 同上 | p5-03a-{before,after,fullctest,focused}.*、p5-03c-{before,after,general,fullctest,focused}.* |
| P5-04 spike | f4ae57f | gprof 重测 + io_uring 探测程序编译运行 | 同上 | p5-04-gprof-profile.txt、p5-04-gprof-run.{json,log}、p5-04-spike/iouring_probe.c、p5-04-before.json |

机器：WSL2 Ubuntu 24.04 单机（MySQL 127.0.0.1:3306、Redis 127.0.0.1:6379、Kafka 127.0.0.1:9092）。

## 完成判据逐条

计划 §9.7：

1. **依赖终态**：P5-00..P5-04 全部终态（00/01/02 VERIFIED、03 循环 = 1 PASS + 2 FAIL + 1 评估不实施、04 CLOSED 有据跳过）。达成。
2. **正确性矩阵**：五闸门 fresh 复验沿 P4-07 形态（Debug 497/497、ASan+UBSan 497/497 零报告、TSan 497/497 0 WARNING、Release tests-off 产物齐全）+ 契约/migration/direct/group/v1v2/online-offline/retry-reconnect/worker 抽认。达成。
3. **性能矩阵五元组**：P5-02 基线 + P5-03 各项前后对比 + P5-04 spike 结论，每项定位 commit/机器/配置/原始输出/统计脚本。达成。
4. **数据性质分类**：synthetic benchmark/故障演练/真实生产数据三类分开标注，无生产流量全部标注实验环境不外推。达成。
5. **四轴对抗审查 H/M 未决 = 0** 才标 M5 VERIFIED：四轴均为 H/M=0（L=4 已落实）。达成 → **M5 VERIFIED**。
6. **Lows 台账继承复核**：8 项逐项有复核结论（见下节）。达成。

架构演进方案 §9 M5 行硬条件：

- **每个数字可定位到 commit、环境、原始报告和统计脚本**：见数字可追溯表与性能矩阵。达成。
- **不宣称未经测量的能力**：P5-03A/C/D 失败结论如实入矩阵；P5-04 全候选跳过且不宣称 io_uring/zerocopy/池化收益；无 SLA/exactly-once/生产承诺措辞。达成。
- **路由四文档与 README 收口一致**：docs/README.md、implementation-sop.md（头部两行）、implementation-progress.md（P5-05 行 + M5 里程碑行）、本报告同步收口（progress/SOP/README 由本卡一并更新）。达成。

## Lows 台账继承复核（不影响 M5 VERIFIED，8 项逐项登记）

| # | 继承项 | 溯源位置 | 本卡状态结论 |
|---|---|---|---|
| 1 | db0 presence 键物理累积 | P4-06 环境复原项；P4-07 Lows 复核无回归（harness 断言 TTL != -1 防 EXPIRE 缺失） | 复核无回归：P5 全程未改 presence 路径，三树全量 Presence/Redis 契约 fresh 全绿 |
| 2 | expired 语义分裂 | M1 登记观察（P4-01 expired 区分 + P4-06 expired 语义登记延续） | 复核仍无需升级：本卡证据链中 Expired 行为符合 retention 语义，无语义升级触发条件 |
| 3 | Redis AUTH/TLS 延后 | P4-07 Lows（RESP 有界化已闭环；AUTH/TLS 因本机回环部署边界延后） | 复核部署边界未外扩：仍为本机回环 host 网络拓扑，延后状态维持 |
| 4 | 容器分区等价边界 | P4-07 Lows（in-process 无公开 harness seam，网络分区以可控注入模拟；Compose host 网络 + 宿主共享依赖等价覆盖） | 复核等价边界声明仍成立：P5 未引入容器级隔离分区需求，声明沿用 |
| 5 | TSan KafkaEventConsumer topic 卫生 | P4-07 Gate C 台账（默认 topic ~19.6k 积压重放诱发分配器 churn → 析构 T5→T5 同线程堆复用误报）；计划 §9.0 bench 前置含 topic 卫生 | **本轮已执行**：Gate C 轮前置 topic 卫生 v2（消费组 offset reset latest），日志结尾 HYGIENE-PASS、remaining-laggy-rows=0，reset 留痕 `m5-final-tsan-20260825-topic-hygiene.log`；TSan 全量 + focused 0 WARNING |
| 6 | legacy contract migration 延后项 | P3-10（contract migration 显式延后至观察窗口关闭；回滚窗口未关不 DROP） | 复核延后状态未变：P5 未触碰 legacy schema，0001–0003 未改动 |
| 7 | MessageAcceptance latency 与 DB pool wait 指标缺口 | P5-01 对抗审查 M-3 登记（dashboard 缺两 panel，根因 = P5-00 /metrics 字段名冻结未暴露二者） | 缺口现状如实登记：字段名冻结口径未变，本卡禁止实现代码改动故不补字段，留后续卡评估 |
| 8 | 镜像拉取受阻复查 | P5-01 环境决策节登记「P5-05 复查项」 | **本卡已执行复查**（编排者实测）：`registry-1.docker.io/v2` 返回 **401**（API 层可达）；`docker pull` 路径仍 **BLOCKED**（docker credential helper `exec format error` 未修复）→ compose 镜像路径验证不具备条件，**tarball fallback 维持**（prometheus/grafana tarball 等价运行为在案口径），登记为环境受限持续项；若 credential helper 或网络恢复可重开复查 |

## 提交链（e8ebf55..f0101e2，九提交）

| # | commit | 消息 |
|---|---|---|
| 1 | 598cd50 | feat(observability): add telemetry contract and recording adapter |
| 2 | 876997f | feat(observability): expose unified snapshot and prometheus endpoint |
| 3 | 350d4e2 | feat(observability): add experimental dashboards and alert rules |
| 4 | c3474a4 | perf(bench): freeze methodology and capture p5 baseline |
| 5 | 9882d00 | docs(perf): record reverted batch-ack candidate |
| 6 | 3fb46bb | perf(message): eliminate serialization copies in protocol replies |
| 7 | 51ea255 | docs(perf): record reverted outbox batch-mark candidate |
| 8 | f4ae57f | docs(perf): record hotspot optimization ledger |
| 9 | f0101e2 | experiment(io): record spike skip evidence |

## 结论

正确性/工具/性能三矩阵 fresh 全绿、真实依赖零 skip；四轴独立验收 H/M 均为 0（L=4 表述精度项已在正文落实）；完成判据逐条达成；所有数字可追溯五元组；Lows 台账 8 项逐项复核。**M5 VERIFIED（四轴 H/M=0×4；五闸门 fresh 497/497×3 + Release；P5-03B 性能优化合入 +19.86%；全部数字五元组定位）**，P5-00..P5-05 全部终态。提交消息 `docs: publish M5 evidence package`。
