# P5-02 Benchmark 方法学（冻结）

状态：FROZEN（P5-02 开卡冻结，D1；GREEN 采集首轮基线后按卡复核）
提交：`perf(bench): freeze methodology and capture p5 baseline`
数据规模任何改动 = 方法学变更（P5 计划 §9.8 冻结项），需重新采集基线并登记。

## 1. 目的与边界

本方法学用于 P5 阶段固定 benchmark 基线（P5-02）与后续优化门槛（P5-03
分母）的测量。所有数字都是 **实验环境** 数字：WSL2、回环、单机、Debug 构建、
无 CPU 亲和性固定。**不可外推为裸 Linux 生产性能**；后续一切性能主张必须引用
P5-02 基线产物（本卡），并满足五元组定位（commit/机器/配置/原始输出/统计脚本）。

## 2. 硬件与环境（实测记录）

| 项 | 值 | 实测方式 |
|---|---|---|
| CPU | Intel(R) Core(TM) i9-14900HX | `lscpu` / `/proc/cpuinfo` |
| 逻辑核 | 32（16 核 × 2 线程；Socket 1） | `lscpu` CPU(s)=32，Thread(s) per core=2 |
| 内存 | 15796 MB（WSL2 可见） | `free -m` |
| 内核 | 6.6.87.2-microsoft-standard-WSL2 | `uname -r` |
| 虚拟化 | WSL2 全虚拟化（Hypervisor: Microsoft） | `lscpu` |
| 网络模型 | 回环 127.0.0.1，本地单机 | — |
| ulimit nofile | 10240 | `ulimit -n` |
| 负载隔离 | 依赖存活核对 + 无其它负载检查（uptime load avg 记录） | runner 前置 |

## 3. 编译 flags

- 正式基线构建：**Debug**，`-g -std=c++11`（顶层 CMakeLists 固定；`CMAKE_CXX_FLAGS`
  空，由顶层追加 `-g -std=c++11`）。独立 binary tree（非源码树）。
- Release 对照：可选记录 `-DCMAKE_BUILD_TYPE=Release -DENABLE_TESTS=OFF`（明示
  对照口径，不替代 Debug 基线）。
- 构建树固定：`/mnt/d/muduo-chat-build/<p5-02-bench-...>`；`cmake --build` 增量
  复核 `ChatServer`/`chat-bench`/`dbmigrate` 与当前 HEAD 一致。

## 4. 数据规模（冻结）

- 预建用户数：**1000 用户**（注册用 chat_load 预建或既有 sql 种子；id 1..1000）。
- Conversation 分布：**direct 900 + group 100**。
  - direct：900 条 `DirectConversation`（`UNIQUE(user_low_id,user_high_id)` 唯一对）。
  - **规模偏差登记（冻结值 900，seed 实际 916）**：runner `seed_data` 在 900 对之外额外预建
    **16 条 hot 直连会话**（Conversation id 2001..2016，sender 1..16 × hot_target，供 DB
    backpressure 用 FOR UPDATE 行锁精确阻塞 accept）。故实际 `DirectConversation` 计数为
    **916 = 900 对 + 16 hot 会话**，非冻结值 900。本偏差开卡登记于方法学（不改变冻结口径，
    影响仅 DB backpressure 场景的锁目标集，已在报告披露）。
  - group：100 个 `AllGroup` + `GroupConversation`（`UNIQUE(group_id)`）+ 成员表。
- **hot Conversation = 单一离线目标**（P3-11 行锁串行验证）：固定 target ID 写入
  runner 配置（默认 target user id 在 runner 内固定，注册为已存在用户；负载全部
  指向该单一离线目标）。
- 规模常数任何改动 = 方法学变更（§9.8 冻结项），重新采集基线。

## 5. 场景矩阵（D2 冻结，9 场景）

每场景记录字段（D3）：msg/s、端到端 p50/p95/p99、DB lock wait、executor queue
depth/drop、oldest pending age、outbox lag、RSS（`/proc/<pid>/status VmRSS`）。

| # | 场景 | 工具/注入 | 说明 |
|---|---|---|---|
| 1 | connect | chat-bench | 建连延迟，mymuduo echo 服务器（端口固定） |
| 2 | echo | chat-bench | 回环吞吐，payload 64B |
| 3 | slow-consumer | chat-bench | 背压，payload 4096B，duration 5s |
| 4 | reliable direct | `tools/bench/load.py` | v2 oneChat（msgid 11/12 客户端 ACK 路径） |
| 5 | reliable group | `tools/bench/load.py` | v2 groupChat（msgid 11/12） |
| 6 | hot Conversation | `tools/bench/load.py` | 单一离线目标（行锁串行） |
| 7 | DB backpressure | FOR UPDATE 行锁注入 | 外部事务对预建 hot 会话（Conversation id 2001..2016）持 FOR UPDATE 行锁，阻塞 hot accept 事务（取 next_sequence 同行锁，P3-11 行锁串行） |
| 8 | Redis down | `redis-cli CLIENT PAUSE` | 降级路径（登录暂停、durable accept 继续） |
| 9 | Kafka pause | broker `SIGSTOP` | outbox publish 停滞（kp7 同族） |

## 6. 验证参数（冻结）

- warmup：每场景正式测量前 **1 轮 × 5s**（丢弃采样）。
- 正式重复：**≥5 次**（同 commit 隔日两轮验证即 ≥5 样本）。
- 每重复采集窗口：chat-bench 三件套按既有 `--duration-ms`/`--messages`；reliable
  场景按 `load.py` duration（默认 2000ms，开卡固定，写入 runner）。
- 每重复单独输出原始采样（不聚合跨重复的延迟样本）。

## 7. 统计口径（冻结）

- 每场景：均值 + **95% CI**（正态近似 t 分布）：
  `CI = mean ± t_{0.975,n-1} · (s/√n)`，n = 重复次数，s = 样本标准差。
- **变异系数 CV = σ/μ** 逐场景入档，作为 P5-03 优化门槛的分母依据（噪声上界）。
- CI 区间 = 完成定义「CI 区间」交付物。

## 8. 结果 schema 版本：bench-result-v1（冻结）

schema_version=`bench-result-v1`。字段集冻结于本方法学与 runner 输出：

| 字段 | 类型 | 说明 |
|---|---|---|
| schema_version | string | 固定 `bench-result-v1` |
| commit | string | git rev-parse HEAD |
| timestamp | string | ISO-8601 采集时刻 |
| host | object | `{kernel, cpu_model, cpu_count, ulimit_nofile, mem_mb}` |
| build | object | `{commit, cxx_flags, build_type}` |
| scenario | string | 场景名（上表 9 场景） |
| workload | object | 每场景参数（conns/duration_ms/messages/payload 等） |
| warmup | object | `{rounds, duration_ms}` |
| repetitions | array | ≥5 个 per-rep 指标对象（含 msg/s、p50/p95/p99、DB lock wait、executor queue/drop、oldest pending、outbox lag、RSS） |
| stats | object | `{mean, ci95_low, ci95_high, cv}` 逐指标 |

per-rep 指标对象固定字段：

| 字段 | 说明 |
|---|---|
| msg_per_sec | 场景吞吐（msg/s） |
| p50_ms / p95_ms / p99_ms | 端到端延迟分位 |
| db_lock_wait_ms | DB lock wait（FOR UPDATE 行锁注入窗口实测，innodb_trx trx_state='LOCK WAIT'） |
| executor_queue_depth / executor_dropped | 来自 /metrics 或 SIGUSR1 快照 |
| oldest_pending_age_ms | reliable_oldest_pending_age_ms（/metrics） |
| outbox_lag | reliable_outbox_lag（/metrics） |
| rss_kb | `/proc/<server_pid>/status VmRSS` |

JSON schema 文件：`tools/bench/bench-result-v1.schema.json`。

## 9. 工具链与命令

- runner：`tools/bench/run.py`（编排 ChatServer 起停、依赖存活核对、topic 卫生、
  逐场景 warmup + 重复、调用 chat-bench/load.py/故障注入、采集 /metrics 与 /proc
  RSS、输出原始 JSON 序列）。
- 负载：`tools/bench/load.py`（reliable direct/group/hot，v2 msgid 11/12 路径）＋
  `tools/chat_load.py`（既有，legacy 对照可选）＋ `chat-bench`（connect/echo/
  slow-consumer）。
- 统计：`tests/scripts/p5_02_stats.py`（读原始 JSON 序列，输出均值 + 95% CI + CV）。
- 每场景可复现命令 + 原始 JSON + CI 区间（见 `docs/reports/performance/p5-baseline.md`）。

### 依赖存活核对与 topic 卫生（runner 前置，固化）

- MySQL 3306 / Redis 6379 / Kafka 9092 存活核对（真实依赖，不得 skip）。
- **topic 卫生**：使用 fresh topic（每次 run 创建独立 topic，如
  `muduo-p502-<runid>`）或 consumer group offset=latest，避免 P4-07 台账登记的
  积压重放 churn 污染测量。
- 负载隔离检查：runner 启动前核对无其它 ChatServer/testserver 监听目标端口。

## 10. 不可外推边界（明示）

- WSL2 虚拟化（非裸 Linux）；回环网络（非真实 NIC）；单机（非多节点）。
- Debug 构建（非 Release）；无 CPU 亲和性固定（OS 调度抖动计入 CV）。
- MySQL/Redis/Kafka 本机回环，非生产部署形态。
- 本方法学数字只用于 P5 阶段内相对比较与优化门槛；禁止 SLA/生产承诺措辞
  （P5 计划 §9.9 停止规则）。

## 11. profile 工具链（D-fallback，冻结）

perf/FlameGraph 本环境不可用（WSL2 无 PMU + sudo 需密码，P5-02 卡登记）。选定
fallback 组合：

1. `-pg` gprof 构建变体（首选；gprof 2.42 已装）；触及构建系统则单独提交登记，
   否则仅命令行加 `-pg` 的独立构建树（不改仓库文件）。
2. 定点手工计时 instrumentation（P5-00 /metrics 与 SIGUSR1 快照：DB lock wait、
   executor queue/drop、oldest pending、outbox lag）。
3. 逐场景 RSS（`/proc/<pid>/status VmRSS`）。

profile 产物与本次基线 commit 对齐（D5），热点 Top 列表带产物链接。

## 12. 第二轮隔日复跑

同 commit 隔日两轮（今日完成第一轮；第二轮隔日补跑后更新
`docs/reports/performance/p5-baseline.md` 与 P5-02 卡，逐场景 CV 入档作为 P5-03
门槛分母）。