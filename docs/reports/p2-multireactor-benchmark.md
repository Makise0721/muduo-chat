# P2-08 多 Reactor 初始性能报告（2026-08-10）

环境：WSL2 Ubuntu（kernel 6.6.87.2），Intel i9-14900HX（32 逻辑核），Debug 构建，
MySQL 本地。工具：tools/chat-bench `connect` 场景（200 连接，3s 窗口）。

| I/O loops | connections_ok | 连接建立延迟 p50 | p95 | p99 |
|-----------|---------------|------------------|-----|-----|
| 1         | 200/200       | 25 µs            | 2685 µs | 2808 µs |
| 2         | 200/200       | 27 µs            | 833 µs  | 1110 µs |
| 4         | 200/200       | 16 µs            | 88 µs   | 159 µs |

观察（初始报告，不预设线性加速）：

- 连接建立延迟尾延迟（p95/p99）随 I/O 线程数增加显著下降（1→4 loops 约
  17–31 倍）；p50 基本持平（accept 路径本身轻）。
- 单 worker BlockingExecutor（P2-06 保序决策）使业务异步任务串行，本报告
  未测量业务吞吐上限；P2-10 将补完整矩阵（吞吐、消息延迟、DB pool wait、
  executor queue/drop、1/2/4/8 loops）。
- 语义注意：多 Reactor 下同一连接的 ACK 与消息转发出自不同 I/O loop，
  到达顺序不保证（DomainCharacterization 单 loop 矩阵保持有序语义；
  多 loop 客户端须按字段区分）。
