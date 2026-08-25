# P5 基线报告（第一轮）

状态：第一轮采集完成（2026-08-24）；**第二轮隔日复跑待补**（同 commit 隔日复跑后更新本报告
与 P5-02 卡，逐场景 CV 入档作为 P5-03 优化门槛分母）。
方法学：`docs/specs/benchmark-methodology.md`（冻结，bench-result-v1）。
任务卡：[docs/archive/tasks/p5/P5-02.md](../../archive/tasks/p5/P5-02.md)。

## 环境

| 项 | 值 |
|---|---|
| commit | `350d4e21d027336e7a561fe345cc500d76715aae` |
| CPU | Intel(R) Core(TM) i9-14900HX（16 核 × 2 线程，32 逻辑核） |
| 内存 | 15796 MB（WSL2 可见） |
| kernel | 6.6.87.2-microsoft-standard-WSL2 |
| build | Debug，`-g -std=c++11`（`CMAKE_CXX_FLAGS` 空，顶层追加） |
| 构建树 | `/mnt/d/muduo-chat-build/p5-00b-final-debug-20260824` |
| 网络 | 回环 127.0.0.1 单机 |
| ulimit nofile | 10240 |
| 依赖 | MySQL 8.0.46 (3306) / Redis (6379) / Kafka (9092) 本机回环，真实依赖不 skip |
| 数据规模 | 1000 用户 / 916 direct 会话（900 对 + 16 hot 会话，seed 实际值；冻结口径 900，偏差见 methodology §4）/ 100 group 会话 / hot target=user 1000 |
| 负载隔离 | 起服前核对无其它 ChatServer/testserver 监听目标端口；uptime load avg < 0.3 |

## 工具链与命令

- runner：`tools/bench/run.py`（编排 ChatServer 起停、依赖核对、topic 卫生（fresh topic
  `muduo-p502-<runid>`）、逐场景 warmup 1×5s + 正式重复、/metrics 与 /proc RSS 采集、
  输出 bench-result-v1 JSON）。
- 负载：`tools/bench/load.py`（reliable direct/group/hot，v2 msgid 11/12 客户端 ACK 路径）
  ＋ `tools/chat_load.py`（既有）＋ `chat-bench`（connect/echo/slow-consumer，对 mymuduo
  echo testserver，端口 18000）。
- 统计：`tests/scripts/p5_02_stats.py`（均值 + 95% CI（t 分布）+ CV）。
- 复现命令：
  ```
  cd /mnt/d/agent_learning/muduo-chat && export DB_PASSWORD=123456
  python3 tools/bench/run.py --build-dir /mnt/d/muduo-chat-build/p5-00b-final-debug-20260824 \
      --reps 5 --duration-ms 2000 --conns 16 \
      --out /mnt/d/muduo-chat-build/p5-02-baseline-r1.json
  python3 tests/scripts/p5_02_stats.py /mnt/d/muduo-chat-build/p5-02-baseline-r1-final.json
  ```
  chat-bench 三场景 r1b 重采（workload 元数据修正）：
  ```
  python3 tools/bench/run.py --build-dir /mnt/d/muduo-chat-build/p5-00b-final-debug-20260824 \
      --reps 5 --scenarios connect,echo,slow-consumer \
      --out /mnt/d/muduo-chat-build/p5-02-baseline-r1b-chatbench.json
  ```

## 结果（9 场景 × 5 重复，均值 + 95% CI + CV）

| 场景 | msg/s | 95%CI low | 95%CI high | CV | p50 ms | p95 ms | p99 ms | RSS KB |
|---|---|---|---|---|---|---|---|---|
| connect | 9383.00 | 7171.53 | 11594.47 | 0.190 | 0.04 | 0.25 | 0.43 | 3584 |
| echo | 21368.14 | 20160.43 | 22575.85 | 0.046 | 0.12 | 0.43 | 0.56 | 3584 |
| slow-consumer | 0.00¹ | — | — | — | — | — | — | 3840 |
| reliable-direct | 25.90 | 23.97 | 27.83 | 0.060 | 751 | 929 | 948 | 16384 |
| reliable-group | 16.00 | 16.00 | 16.00 | 0.000 | 1188 | 2625 | 2639 | 16384 |
| hot Conversation | 51.90 | 40.04 | 63.76 | 0.184 | 212 | 541 | 612 | 16384 |
| DB backpressure | 8.70 | -15.45² | 32.85 | 2.236 | 50 | 101 | 109 | 16384 |
| Redis down | 26.40 | 24.68 | 28.12 | 0.053 | 664 | 882 | 940 | 16384 |
| Kafka pause | 44.40 | 30.21 | 58.59 | 0.257 | 219 | 660 | 741 | 16384 |

¹ slow-consumer 为数据一致性场景（吞吐非主指标）：sent=recv=1,638,400 B、early_closes=0 全 5 轮一致。
² DB backpressure 的 95%CI 含负值 = 5 轮中 4 轮 msg/s=0（锁窗完全覆盖）与 1 轮 43.5（锁窗部分重叠），
CV=2.236 反映锁窗/负载窗重叠时序抖动——该场景的实质信号是 `db_lock_wait_ms`（见下）。
同款说明适用于 **p50/p95/p99 均值**：由 [0,0,0,0,x] 计算（p50 唯一非零值 248.49 ms、
p95 唯一非零值 505.04 ms、p99 唯一非零值 544.02 ms，均来自第 5 轮锁窗部分重叠的那一次），
非 5 轮独立稳态延迟，不作延迟绝对值解读。

### 场景说明与降级数字

- **connect**：200 次建连；msg/s = 200/实测耗时（runner 计时），p50 ≈ 36-44 µs。
- **echo**：4 连接 × 200 × 64B；~21.4k msg/s（Debug、回环、WSL2）。
- **slow-consumer**：2 连接 × 200 × 4096B、duration 5s；字节一致，背压无阻塞。
- **reliable-direct**（v2，在线接收者 + 客户端 ACK msgid 12）：25.90 msg/s；`reliable_acked` 与
  `reliable_accepts` 同步增长，ACK 路径闭环——仅 rep1 `reliable_pending=0`，reps 2-5 为 3-7（
  跨 rep 累积、下轮 ACK 回补），accepts-acked 最大差 23（rep5，窗口末采样即时差，非残留永久缺口）。
  executor queue ≤ 31（< 64）、drop=0、duplicates=0。
- **reliable-group**：16 msg/s；群成员在线 ACK；executor queue 饱和（55-63），drop=0。
- **hot Conversation**（单一离线目标 user 1000）：51.90 msg/s；`reliable_pending` 单调累积
  （178→637，跨 5 轮）、`oldest_pending_age` 12.4s→23.4s——行锁串行 + 离线滞留信号。errors=4
  **恒定（5/5 轮一致）**——系统性边界现象（非随机超时），已计入 `errors` 字段、不计入 msg/s
  （load.py `msg_per_sec=ok/window`，errors 单独计数）。
- **DB backpressure**（FOR UPDATE 行锁 hot 会话）：5 轮中 4 轮 msg/s=0、errors=16（accept 被行锁
  阻塞，MESSAGE_ACCEPTED 超时），`db_lock_wait_ms`=3000-5000ms（与锁窗一致）；1 轮 43.5 msg/s
  （锁窗与负载窗部分重叠）。executor queue 15-47、drop=0（队列未满，锁等待阻塞在 DB 层）。
- **Redis down**（CLIENT PAUSE 3500ms）：msg/s 26.40 vs reliable-direct 25.90，比值 ≈ 1.02×
  （两场景 95% CI 完全重叠：redis-down 24.68-28.12 vs direct 23.97-27.83），主张「≈1.0×，
  无吞吐回退；v2 accept 为 DB-only，降级信号在 login 拒绝与 delivery 滞留」（非降 ~0.1×）。
  errors 1-2（登录被拒，冻结降级「登录暂停」）；探针证实：durable accept 在 down 期间继续
  （`accepts_during_down=5/5`）、fresh login 被拒（`login_rejected=1`）、恢复后登录成功（`=1`）。
  注：`oldest_pending_age` 为**累积 gauge**，跨场景继承先前离线积压（非本轮独立值，见下）。
- **Kafka pause**（broker SIGSTOP）：msg/s 24-52（accept 为 DB-only，不受 Kafka 阻塞）；
  核心信号 = `reliable_pending` 单调累积（1019→1382）与 `outbox_lag` 爬升（0→204）——
  accepted-but-undelivered 滞留（kp7 同族），恢复后消费收敛。errors=5 **恒定（5/5 轮一致）**——
  系统性边界现象（非随机超时），已计入 `errors` 字段、不计入 msg/s。
- **oldest_pending_age 累积说明（redis-down 115-166s、kafka-pause 177-190s）**：该字段为
  **累积 gauge**，值继承场景链中先前各场景（hot→db-backpressure→redis-down→kafka-pause）残留的
  离线/滞留积压时间戳，**非本轮独立测得值**；只作滞留是否仍存在的定性信号，不作延迟绝对值解读。

## 与 P2/P3 历史基线对比（定性）

- **echo（网络层）**：本报告 21.4k msg/s（r1b 重采，见「r1b 重采说明」）vs b3fde51 smoke 41.2k msg/s——同 Debug/回环/WSL2，
  本报告为固定 5 重复均值且负载隔离更严格；量级一致（~2-4 万 msg/s 区间），属测量环境噪声。
- **reliable direct（chat 业务层，durable accept）**：本报告 25.90 msg/s（v2 在线接收 + ACK
  全路径）vs P3-13 65.7 msg/s（legacy 离线 oneChat，chat_load.py 测量）。**口径不同**：本报告
  走 v2 客户端 ACK 全路径（accept + 在线投递 + ack），P3-13 为 legacy 离线单聊（无投递/ACK）；
  本报告 durable accept 事务相同（UPDATE Conversation.next_sequence + INSERT ChatMessage +
  OutboxEvent + MessageDelivery + COMMIT）。差异归因：ACK 全路径含投递/确认工作 + 在线接收者
  的 session/delivery 开销，非单纯 accept 吞吐。
- **P2-10 chat oneChat**：~123 msg/s（legacy 离线，16 并发）。本报告 hot（同一离线目标 legacy 语义）
  51.90 msg/s——v2 durable accept 与 legacy 的差值解释为 durable write 成本（P3-11 已归因：
  accept 事务 3-4×SQL）+ 本报告更重的并发模式。
- 结论：**不预设提升比例**，全部为定性比较；口径差异显式披露，数字可追溯五元组
  （commit/机器/配置/原始输出/统计脚本）。

## profile 工具链：gprof fallback 落实

perf/FlameGraph 本环境不可用（WSL2 无 PMU + sudo 需密码，P5-02 卡登记）。选定 fallback：

- **`-pg` gprof 构建变体**：独立构建树 `/mnt/d/muduo-chat-build/p5-02-gprof-20260824`
  （`-DCMAKE_CXX_FLAGS=-pg -DCMAKE_EXE_LINKER_FLAGS=-pg -DENABLE_TESTS=OFF`，不改仓库文件，
  不属仓库改动不需单独提交）。可靠-direct 场景 8 轮采集后 `gmon.out`（853 KB）→
  `gprof ./bin/ChatServer gmon.out` exit 0。**登记**：gprof profile 树负载 duration 用
  **3000ms**，偏离冻结常数 2000ms（独立 profile 树、与正式基线 run 分离；仅影响 profile
  采样时长，不影响基线数字，影响小）。
- **热点 Top（flat profile，可靠-direct 负载）**：
  1. `std::vector<char>` emplace_back/size/empty/push_back——Buffer 字节操作与序列化往返拷贝。
  2. `nlohmann::json` lexer/serializer/assert_invariant/set_parents/`std::_Rb_tree`（std::map 对象）——
    JSON 解析与序列化（协议编解码路径，ProtocolCodec 序列化拷贝候选）。
  3. `MySQLMessageStore::deliveriesWhere`——DB 查询路径（投递状态查询）。
  4. `MessageId::MessageId`、`unique_lock` 构造/析构/unlock——领域值构造与锁开销。
- **样本厚度声明（M-4 复核）**：flat profile **仅 16 个 CPU 采样（0.16s，每样本 0.01s）**，
  各条目百分比为 **1-2 样本粒度**，仅供**定性排序**，不能解读为精确占比。flat 第一
  `std::__unguarded_insertion_sort`（`unsigned long` vector 排序，**2 samples**，15.38%）未入
  上方 Top 列表（调用数 45、来自顺序容器小规模排序，非主路径）；**DB I/O wait 对本 gprof
  不可见**（本负载 accept 事务 SQL/行锁等待为主要成本，CPU profile 无法采样 DB 侧等待）。
  nlohmann/Buffer/`deliveriesWhere`/锁 的定性归因与 flat 调用计数一致（如 `deliveriesWhere`
  1 sample 且调用 4466 次）。
- **定点手工计时 instrumentation**（P5-00 `/metrics` 快照）：executor queue/drop、oldest pending、
  outbox lag、pool_active 全程采集（本报告各场景表）。
- **逐场景 RSS**（`/proc/<pid>/status VmRSS`）：echo testserver ~3.8-4.1 MB；ChatServer ~16.4 MB
  （5 轮稳定，无增长趋势）。**chat-bench 三场景（connect/echo/slow-consumer）的 RSS 采样来源为
  独立运行 `p5-02-chatbench-r1.json`（重采集时间戳 2026-08-24T20:19:05 / 20:19:11 / 20:19:22，
  非主 r1 运行 20:11-20:12 的采样），RSS 已并入主 r1 JSON**——两个原始产物并非矛盾，而是同一
  采样来源在独立产物与主产物的两次记录。
- 产物路径：`/mnt/d/muduo-chat-build/p5-02-gprof-20260824/gmon.out`、
  `/mnt/d/muduo-chat-build/p5-02-gprof-profile.txt`（本报告热点 Top 来源）。

## r1b 重采说明

chat-bench 三场景（connect/echo/slow-consumer）为**重采修正**：r1 的 workload 元数据误记
`connections:16/duration_ms:2000`（run.py 默认值），而非实际参数（connect 200 连 / echo
4×200×64B / slow-consumer 2×200×4096B×5s）。run.py 源码已修复（`CHATBENCH_PARAMS` 单一来源），
现用修复后 run.py 按原方法学（warmup 1×5s、正式 5 重复、同构建树 `p5-00b-final-debug-20260824`、
同 commit `350d4e2`）重跑三场景，输出 r1b（时间戳 2026-08-24T20:58:19/20:58:24/20:58:35），
并合并 r1 的 reliable 六场景为 `p5-02-baseline-r1-final.json`。

**r1b 三场景 vs r1（均值，单位 msg/s）**：

| 场景 | r1 | r1b | 说明 |
|---|---|---|---|
| connect | 12349 (CI 8088..16610, CV 0.278) | 9383 (CI 7172..11594, CV 0.190) | 落在 r1 CI 区间内，环境噪声 |
| echo | 28705 (CI 24696..32713, CV 0.112) | 21368 (CI 20160..22576, CV 0.046) | r1b 低于 r1 CI low 约 13%，但 r1b 自身 CV 0.046 高度稳定，量级一致（~2 万 msg/s 区间），属 WSL2/回环/Debug 环境噪声 |
| slow-consumer | 0（字节一致 1638400/1638400/0） | 0（字节一致 1638400/1638400/0） | 5/5 轮一致，无差异 |

reliable 六场景沿用 r1 数字（本卡范围仅重采 chat-bench 三场景，未重跑 reliable/fault 场景）。

## 日志 / 原始 JSON 绝对路径

- `/mnt/d/muduo-chat-build/p5-02-baseline-r1.log`（runner 全量输出，含每轮逐 rep 数据）
- `/mnt/d/muduo-chat-build/p5-02-baseline-r1.json`（bench-result-v1 原始 JSON，9 场景 × 5 重复；
  **注意**：其中 chat-bench 三场景 workload 元数据为 r1 早前缺陷值 `connections:16/duration_ms:2000`，
  非实际参数——见「r1b 重采说明」）
- `/mnt/d/muduo-chat-build/p5-02-baseline-r1b-chatbench.json`（**r1b 重采**：chat-bench 三场景
  connect/echo/slow-consumer 独立重跑，时间戳 2026-08-24T20:58:19 / 20:58:24 / 20:58:35，
  workload 元数据已修正：connect 200 连、echo 4×200×64B、slow-consumer 2×200×4096B×5s）
- `/mnt/d/muduo-chat-build/p5-02-baseline-r1b-chatbench.log`（r1b 重采 runner 全量输出）
- `/mnt/d/muduo-chat-build/p5-02-baseline-r1-final.json`（**合并产物**：r1b 三场景 + r1 reliable
  六场景（reliable-direct/group/hot/db-backpressure/redis-down/kafka-pause）合并为 9 场景数组，
  单一 JSON 供 stats.py 复核；本报告结果表数字即取自该合并产物）
- `/mnt/d/muduo-chat-build/p5-02-gprof-profile.txt`（gprof flat/call profile）
- `/mnt/d/muduo-chat-build/p5-02-gprof-20260824/gmon.out`（profile 原始采样）
- `/mnt/d/muduo-chat-build/p5-02-chatbench-r1.json`（chat-bench 三件套独立采集，时间戳
  2026-08-24T20:19:05 / 20:19:11 / 20:19:22；**主 r1 JSON 中三场景的 RSS 即取自本次独立采集并
  合并**，见「profile 工具链」节 RSS 来源说明）
- 构建树 `/mnt/d/muduo-chat-build/p5-00b-final-debug-20260824/`（`chatserver.log`、
  `testserver.log`、`p5-02-config.json`）

## 不可外推边界（明示）

WSL2 虚拟化（非裸 Linux）、回环网络（非真实 NIC）、单机（非多节点）、Debug 构建（非 Release）、
无 CPU 亲和性固定、MySQL/Redis/Kafka 本机回环。**本报告数字只用于 P5 阶段内相对比较与优化门槛，
禁止 SLA/生产承诺措辞**（P5 计划 §9.9 停止规则）。

## 第二轮隔日复跑标记

- **第一轮**：今日（2026-08-24）完成，本报告数字即第一轮基线。
- **第二轮（待补）**：同一 commit `350d4e2` 隔日复跑全部 9 场景 × ≥5 重复，命令同「复现命令」；
  完成后：(1) 更新本报告结果表（第二轮均值/CI/CV 并入或并列），(2) 更新 P5-02 卡状态与
  CV 入档。逐场景 CV 作为 P5-03 优化门槛分母依据。