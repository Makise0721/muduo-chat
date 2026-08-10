# P2-10 M2 独立验收与性能矩阵报告（2026-08-10）

环境：WSL2 Ubuntu（kernel 6.6.87.2），Intel i9-14900HX（32 逻辑核），Debug 构建，
MySQL 本地（127.0.0.1:3306，pool=5）。负载：tools/chat_load.py（16 并发 ×
reg→login→离线单聊循环，15s）；chat-bench connect 200 连接 3s；chat-bench
slow-consumer 4 连接 × 1.28MB 突发 2s。METRICS 快照在负载第 5s 经 SIGUSR1 抓取。

## chat 业务负载（离线单聊，每消息 = executor 任务 + DB 写 + ACK）

| loops | msg/s | p50 ms | p95 ms | p99 ms | executor_queue | executor_drop_full | pool_active |
|-------|-------|--------|--------|--------|----------------|--------------------|-------------|
| 1     | 101.8 | 137.9  | 152.5  | 269.6  | 15             | 0                  | 1           |
| 2     | 118.5 | 130.6  | 148.4  | 186.8  | 15             | 0                  | 1           |
| 4     | 123.8 | 125.0  | 150.6  | 196.4  | 15             | 0                  | 1           |
| 8     | 122.8 | 124.7  | 146.9  | 216.3  | 15             | 0                  | 1           |

## connect 连接建立延迟（200 连接同时发起）

| loops | p50 us | p95 us | p99 us |
|-------|--------|--------|--------|
| 1     | 53     | 253    | 410    |
| 2     | 40     | 143    | 483    |
| 4     | 33     | 126    | 224    |
| 8     | 33     | 119    | 326    |

## slow-consumer 背压（64B × 20000/连接 突发，5ms/包消费）

全档一致：sent=80000，recv=0（非法 JSON 被丢弃属预期），early_closes=4，
发送端不阻塞（读速 ≥ 发送速）——EventLoop 对不可解析输入无背压阻塞。

## 分析

- **吞吐饱和 ~123 msg/s（1→2 +17%，2→4 +4%，4→8 持平）**；executor_queue 恒 15
  积压、drop=0、pool_active=1：瓶颈是 **executor 单 worker 串行 DB 写延迟**（~8ms/写），
  不是 I/O loop 数。p50~125-150ms 与 16 并发 × 串行 8ms 排队延迟吻合。
- connect 尾延迟随 loops 下降（p99 410→326us），I/O 层扩展正常（与 P2-08 报告一致）。
- **executor 决策：保持单 worker**。扩并需按用户分片才能保住同连接串行依赖
  （P2-06：多 worker 破坏 addFriend 后立即重复 add 语义）；单 worker 上限由 DB 写
  延迟主导，属架构工作（分片队列），留 P3 语义工作后评估。本报告记录上限，不隐含
  生产容量。
- EventLoop lag：无独立探针，由 chat p99（~200-270us 网络侧贡献小）与 connect p99
  间接表征，未观察到异常滞后。

## 工具修复（本任务发现）

- chat-bench slow-consumer：阻塞 recv 无视 duration 上限（server 无响应时死等）→
  加 SO_RCVTIMEO 100ms + EAGAIN/EWOULDBLOCK continue（回到 deadline 检查）。
- METRICS 机制：SIGUSR1 → `METRICS pool_total=.. pool_idle=.. pool_active=..
  executor_queue=.. executor_drop_full=.. executor_drop_shutdown=..`（metrics_test 进程
  测试覆盖格式）。

## M2 独立验收矩阵（全绿）

- in-memory unit + MySQL integration + v1/v2 process：Debug/ASan/TSan 全量 182/182
  （180 + BlockingExecutor metrics 2 新增）；
- 1/2/4/8 loops：MultiReactorTest（4 loops）+ MultiReactorProcess（2/4 loops）+
  本报告性能矩阵；
- 慢 DB / DB down：ConnectionPool 真 KILL 断线替换 + acquire 超时路径
  （P2-04 已有测试，本任务无回归）；
- Release 构建：`-DCMAKE_BUILD_TYPE=Release -DENABLE_TESTS=OFF` 全新构建通过；
- 所有阻塞 DB 路径在 executor（P2-07 完成定义 + P2-08 前置检查）；应用测试不依赖
  网络；adapter contract 双跑（P2-02/P2-05/P2-06/P2-07 已有）。
