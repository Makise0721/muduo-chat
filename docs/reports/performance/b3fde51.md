# Performance Report b3fde51

状态：WSL2 smoke 基线（非正式 benchmark）

环境（SOP §9.5：正式数字只来自固定环境重复测量；本报告全部标记 WSL2）：

| 项目 | 值 |
|---|---|
| commit | b3fde51 |
| build | Debug，`-g -std=c++11` |
| CPU | Intel(R) Core(TM) i9-14900HX（32 线程） |
| kernel | 6.6.87.2-microsoft-standard-WSL2 |
| ulimit nofile | 10240 |
| 负载 | testserver（mymuduo echo，端口 8000）本地回环 |
| 工具 | chat-bench（tools/chat-bench，本仓库） |

## 结果（单次 smoke，2026-08-09）

### connect（8 连接）
- 成功 8/8，失败 0
- 建连延迟：p50 = 1064.7 µs，max = 1912.5 µs

### echo（4 连接 × 200 消息，payload 64B）
- 800/800 消息往返成功
- 延迟：p50 = 80.5 µs，p95 = 139.2 µs，p99 = 318.7 µs，p999 = 2542.3 µs
- 吞吐：41230 msg/s，21.1 Mbps

### slow-consumer（2 连接 × 200 × 4096B，duration 5000ms）
- 发送/接收 1,638,400 字节一致，early_closes = 0
- 说明：5ms 节流读取，全部在 duration 内完成

## 结论与边界

- 本报告为 smoke 基线，验证工具链与 schema 可用；**不可外推为裸 Linux 生产性能**
  （WSL2 回环 + Debug 构建 + 未固定 CPU 亲和性）。
- 正式基线需在固定 Linux 主机重复测量；当前方法见 [架构演进方案 §8](../../architecture/evolution-plan.md#8-可观测性与性能方法)。
