# P5-01 Dashboard 观测说明（experimental，无生产承诺）

本文档说明 `docker/grafana/dashboards/chat-overview.json` 每个 panel 的 PromQL 查询，
作为人工与脚本（prometheus 实测 /api/v1/query 断言）核对 dashboard 的落点。指标名来自
P5-00 /metrics 端点（`chatserver/main.cpp publishSnapshotToSink` 统一快照同源导出，
冻结字段名，见 docs/tasks/P5-00.md §问题与证据）。所有 SLO 均为 **experimental**，
无真实流量前不构成任何 SLA/生产承诺（docs/tasks/P5-01.md §9.9 停止规则）。

## 数据源与抓取

- prometheus scrape job `chat_metrics` → `127.0.0.1:9095`（/metrics），scrape_interval 5s。
- Grafana 数据源 `prometheus`（uid `prometheus`）→ `http://127.0.0.1:9090`。
- dashboard `Chat Overview`（uid `chat-overview`）经 provisioning 自动加载。

## Panel → PromQL 查询断言表

| # | Panel | PromQL | 指标字段（P5-00） | 断言 |
|---|-------|--------|------------------|------|
| 1 | Connection accept rate | `rate(accept_count[1m])` | `accept_count` | 查询返回 1 个时间序列，类型 gauge |
| 2 | Connection reject rate | `rate(accept_rate_reject[1m])`、`rate(accept_max_reject[1m])` | `accept_rate_reject`、`accept_max_reject` | 返回 2 序列 |
| 3 | ACK latency p50/p95/p99 | `reliable_ack_latency_p50_ms`、`reliable_ack_latency_p95_ms`、`reliable_ack_latency_p99_ms` | 同名 | 返回 3 序列 |
| 4 | Oldest pending age | `reliable_oldest_pending_age_ms` | 同名 | 返回 1 序列 |
| 5 | Outbox lag | `reliable_outbox_lag` | 同名 | 返回 1 序列 |
| 6 | Consumer lag | `consumer_lag` | 同名 | 返回 1 序列 |
| 7 | EventLoop lag | `loop_lag_ms` | 同名 | 返回 1 序列 |
| 8 | DB pool active | `pool_active`、`pool_idle` | 同名 | 返回 2 序列 |
| 9 | Executor queue / drops | `executor_queue`、`rate(executor_dropped[1m])` | `executor_queue`、`executor_dropped` | 返回 2 序列 |

## 断言说明

- 每条 PromQL 由 `tests/scripts/p5_01_rehearsal.py` 对真实 prometheus 实例查询
  `/api/v1/query`，断言序列名存在且数值类型正确（D6 查询断言）。
- 指标名冻结：本表 PromQL 引用字段在 P5-00 已冻结，dashboard 不更名。
- 无 SLA 措辞：所有 panel 仅展示实验观测，不设生产阈值承诺。

## 已知缺口登记（M-3 偏差，延后评估）

- dashboard 仅覆盖现有 **9 panel**（上表 #1–#9），**未覆盖**设计决定 D5 清单中的
  **MessageAcceptance latency** 与 **DB pool wait** 两信号：
  - 根因：P5-00 `/metrics` 字段名冻结（docs/tasks/P5-00.md §问题与证据）未暴露
    MessageAcceptance latency（msgid 11 accept 耗时）与 pool 获取等待（pool wait）
    两个字段；P5-01 禁止 C++ 生产代码改动，无法在卡内补齐。
  - 处理：本缺口登记在案，延后至 P5-02（或后续卡）评估是否需要新增对应指标字段并
    据此扩展 dashboard panel。
  - 覆盖范围以现有 9 panel 为准；本文件不夸大覆盖能力。
