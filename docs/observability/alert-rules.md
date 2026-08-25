# P5-01 告警规则说明（experimental SLO，无生产承诺）

本文档记录 `docker/prometheus/rules/{delivery,capacity,loop}.yml` 每条告警的触发/清除
语义与阈值初值来源。所有阈值均为 **experimental 初值**（D4），来自 P4 故障收敛实测
（docs/reports/p4-m4-gates.md 故障恢复与收敛节；docs/tasks/P4-07.md 容量保护/风暴
恢复契约）。P5-02 固定 benchmark 基线产出后允许一次复核修订（docs/archive/tasks/p5/P5-01.md §D4）。

**无 SLA/生产承诺**：本仓库在无真实流量前只称「实验 SLO」，任何阈值不构成生产承诺。

## 阈值初值与来源

| 规则 | 表达式 | for | experimental 来源 |
|------|--------|-----|-------------------|
| P5_DELIVERY_OLDEST_PENDING_STALLED | `reliable_oldest_pending_age_ms > 60000` | 30s | P4 收敛实测：Pending 重路由/收敛秒级上限；取 60s（收敛上限 × 容差），D4 冻结 |
| P5_DELIVERY_OUTBOX_LAG_HIGH | `reliable_outbox_lag > 60` | 30s | P4 E2/Kafka pause 收敛：outbox lag 收敛为零非单调爆炸；取 60（收敛窗口），D4 冻结 |
| P5_DELIVERY_CONSUMER_LAG_HIGH | `consumer_lag > 60` | 30s | P4 Kafka 消费停滞收敛：consumer_lag 为 offset 差（滞后消息数，非秒）；取 60 消息数，D4 冻结 |
| P5_DELIVERY_FENCING_CONFLICTS | `rate(fencing_conflicts[1m]) > 0` | 60s | P4 epoch fencing：冲突为异常事件，rate>0 持续 60s 即告警，D4 冻结 |
| P5_DELIVERY_ACK_LATENCY_SPIKED | `reliable_ack_latency_p99_ms > 3000` | 30s | P4 跨节点投递延迟 ≤0.12s + ack_timeout 30s 生产默认；取 p99>3s 为实验初值 |
| P5_CAPACITY_ACCEPT_RATE_REJECTED | `rate(accept_rate_reject[1m]) > 0` | 30s | P4 容量保护：rate-limit 拒绝为过载信号，rate>0 持续 30s 即告警，D4 冻结 |
| P5_CAPACITY_ACCEPT_MAX_REJECTED | `rate(accept_max_reject[1m]) > 0` | 30s | P4 容量保护：max-connections 拒绝为过载信号，rate>0 持续 30s 即告警 |
| P5_CAPACITY_OUTSTANDING_BYTES_HIGH | `outstanding_bytes > 10485760` | 30s | P4 backpressure：outstanding 预算；取 10MiB 为实验初值，P5-02 复核 |
| P5_CAPACITY_EXECUTOR_DROP_FULL | `rate(executor_dropped[1m]) > 0` | 30s | P4 风暴保护：executor drop 为容量耗尽信号，rate>0 持续 30s 即告警，D4 冻结 |
| P5_CAPACITY_EXECUTOR_QUEUE_HIGH | `executor_queue > 56` | 30s | P4 风暴保护：queue_capacity 默认 64；取 56（88% 高水位）为实验初值 |
| P5_CAPACITY_POOL_ACTIVE_HIGH | `pool_active / (pool_total + 1) > 0.8` | 30s | P4 依赖超时：DB 连接池活跃比 >0.8 持续 30s 即告警 |
| P5_LOOP_LAG_HIGH | `loop_lag_ms > 2000` | 10s | D4 冻结：loop 卡顿阈值 2000ms 持续 10s（P5-00 M-1 稳态时钟采样量级） |

## 触发/清除语义

- 每条规则 `expr` 满足并持续 `for` 时长 → 触发（firing）。experimental_slo=true 标签标识。
- `expr` 不再满足（回落阈值下）且维持 → 自动清除（resolve）。无人工干预、无 AlertManager
  投递（本卡范围外，docs/archive/tasks/p5/P5-01.md §非目标）。
- 触发/清除证据：promtool unit test（tests/scripts/p5_01_test_rules.yml 逐条 firing +
  resolve 断言）与真实演练（tests/scripts/p5_01_rehearsal.py ×3 轮）。
- 真实演练覆盖（Kafka broker SIGSTOP，docs/archive/tasks/p5/P5-01.md GREEN 记录）：`P5_DELIVERY_OUTBOX_LAG_HIGH`
  与 `P5_DELIVERY_OLDEST_PENDING_STALLED` 在 pause 窗口内 firing → CONT 恢复 → 收敛 → 自动 resolve；
  稳态对照轮无 P5_* firing。其余规则（capacity/loop 域）由 promtool unit test 逐条 firing+resolve
  断言覆盖（构造输入序列）。

## 变更纪律（D7 冻结）

任何阈值改动必须三处同步：`docker/prometheus/rules/*.yml` + 本文件 + 断言脚本
（tests/scripts/p5_01_test_rules.yml / p5_01_rehearsal.py）。本卡初值即冻结，改动记
revision 于 docs/archive/tasks/p5/P5-01.md。
