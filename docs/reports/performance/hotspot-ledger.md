# 热点优化台账（Hotspot Ledger）

状态：P5-03 循环协议台账骨架已建（2026-08-24）；**P5-03A 已收口 FAILED/REVERTED（未达标，实现已 revert，无提交，见「P5-03A 结果」）**。
计划：`docs/plans/post-p2-implementation-plan.md` §9.5（P5-03 只修实测热点；循环协议六步固定；候选台账禁止无 profile 新增候选）。
基线报告：`docs/reports/performance/p5-baseline.md`（P5-02，9 场景 × 5 重复，均值 + 95% CI + CV）。
profile 产物：`/mnt/d/muduo-chat-build/p5-02-gprof-profile.txt`（gprof flat/call，16 采样/0.16s 定性；DB I/O wait 对 gprof 不可见）。

## 列说明

- 候选：计划 §9.5 候选台账项（禁止无 profile 新增）。
- 门槛：RED 前冻结的性能门槛（含分母 = P5-02 变异系数上界来源）。
- 基线数字：优化前原始测量（P5-02 基线或本卡前基线）。
- 结果：优化后测量 vs 门槛判定（达标/未达标）。
- 结论：commit 或 revert 结论，逐项可追溯。

## 项

| 子卡 | 候选 | 门槛（分母来源） | 基线数字 | 结果 | 结论（commit/revert） |
|---|---|---|---|---|---|
| P5-03A | 批量 ACK 合并（DELIVERY_ACK 攒批 UPDATE） | reliable-direct ACK 指标提升 ≥15% 且正确性零回退；分母 = P5-02 reliable-direct CV 上界 0.060 | P5-02 reliable-direct 25.90 msg/s、p50 751ms、CV 0.060（第一轮；第二轮隔日复跑未跑则沿用） | 未达标：after msg/s 23.90（CI 23.62..24.18，CV 0.009）vs before 27.00（CI 23.74..30.26）；ACK p50 872.4ms vs 746.6ms（+16.8% 更差）、p95 1018.5 vs 822.0（+23.9% 更差）、p99 1051.4 vs 828.8（+26.9% 更差）。零正确性回退（492/492 全绿）但无 ≥15% 提升，p50/p95/p99 反超 5% 回退线 | revert：**接线复杂度过高**（同步 HOL 释放语义下攒批延迟写与既有 DeliveryCoordinator 同步正确性冲突，见 P5-03A.md），仅实现批量方法未接线 → 热路径不变 → 性能门槛不达；实现已还原，RED 契约测试保留 |
| P5-03B..（预留） | （计划候选台账其余项：Outbox lease 批量 claim / Buffer 复用 / 锁竞争缓解） | 待开卡时冻结 | — | — | — |

## P5-03A 待执行清单（循环协议）

1. 正确性矩阵与 fault 套件先全绿（优化前基线，commit c3474a4）。
2. 跑优化前 reliable-direct ≥5 轮原始 JSON（`/mnt/d/muduo-chat-build/p5-03a-before-*.json`）。
3. 单变量最小实现（ACK 更新攒批）。
4. 同负载同方法学复测 ≥5 轮 + CI 对照。
5. Debug/ASan/TSan 全量 + focused repeat。
6. 达标 → 独立 revert 原子提交；未达标 → revert + 结论入本台账。

## P5-03A 结果（luna_worker，2026-08-24 阶段 2；2026-08-24 收口 FAILED/REVERTED）

**判定：未达标（FAIL）→ 实现已 revert，无提交，RED 契约测试保留。**

- **正确性**：优化后 `ctest --test-dir /mnt/d/muduo-chat-build/p5-03a-debug --output-on-failure` → **492/492 全绿**（490 基线 + BatchedAckContract 双 adapter 2 用例）。聚焦（BatchedAck|DeliveryCoordinator|DeliveryRetry|DeliveryAck|OutboxConsumer|GatewayDelivery）43/43 全绿。**零回退**。
- **性能门槛（FAIL）**：优化后 reliable-direct 5 轮（`/mnt/d/muduo-chat-build/p5-03a-after.json` + `p5-03a-after.log`）：
  - msg/s：mean **23.90**（CI 23.62..24.18，CV 0.009）vs before 27.00（CI 23.74..30.26）→ **-11.5%**（无提升）。
  - ACK p50：mean **872.4ms**（CI 840.8..904.1）vs before 746.6ms → **+16.8% 更差**（门槛要求 ≤639ms，未达）。
  - ACK p95：mean **1018.5ms** vs before 822.0 → **+23.9% 更差**（>5% 回退线，违反）。
  - ACK p99：mean **1051.4ms** vs before 828.8 → **+26.9% 更差**（>5% 回退线，违反）。
  - 零 errors/duplicates；CI 前后不重叠（after 整体低于/劣于 before）。无 ≥15% 提升，p50/p95/p99 反超回退线 → **FAIL**。
- **为什么未达（接线复杂度过高，循环协议第 3/4 步登记）**：`DeliveryCoordinator::acknowledge` 的 ACK 依赖**同步**持久化到 store 才 `claimFor` 放行同 Conversation 下一 sequence（HOL），既有 DeliveryCoordinatorTest 逐个断言 ACK 后立即放行。攒批延迟写（flush 窗口/阈值）会推迟 store 反映 Acknowledged → claimFor 读旧态 → HOL 不释放、已 ACK 消息被重投/重复。要在攒批下保持同步 HOL，需在 Coordinator 引入写透本地 acknowledged 集合并改 claimFor 读取（新增状态机面、改动面失控、490 基线回归风险高）。单变量纪律下判定接线复杂度过高 → **仅实现批量方法 `MessageStore::updateDeliveries`（InMemory/MySQL 双 adapter，通过 RED 契约 6 语义）未接线热路径** → 热路径仍逐条 `updateDelivery` → 无性能收益。
- **revert**：实现文件（`MessageStore.hpp`/`InMemoryMessageStore.hpp/.cpp`/`MySQLMessageStore.hpp/.cpp` 的 `updateDeliveries`）已 `git checkout` 还原；热路径从未改动。**RED 契约测试保留**（`tests/unit/BatchedAckContract.hpp`/`BatchedAckContractTest.cpp`，含阶段 2 修正的秒对齐时间戳 + getOrCreateConversation 建 Conversation 行），作为下次候选的契约规格。
- **产物**：`/mnt/d/muduo-chat-build/p5-03a-after.json`、`p5-03a-after.log`、`p5-03a-focused.log`、`p5-03a-fullctest.log`、`p5-03a-red-build.log`、`p5-03a-before.json`、`p5-03a-before.log`（构建树外）。
- **未 stage**：本 worker 未 `git add/commit`。
- **遗留测试清理（2026-08-25）**：P5-03B 全量 497/498 唯一非绿 `BatchedAckContractTest_NOT_BUILT`（RED 契约注册为构建目标但引用已 revert 的 `updateDeliveries`）→ 已从 `tests/CMakeLists.txt` 构建集移除目标注册（add_executable/foreach/discover_gtest 三处，其它目标不动）；`tests/unit/BatchedAckContractTest.cpp` + `BatchedAckContract.hpp` 保留为批量 ACK 候选规格（头部注释补「非构建目标」一行）。重新 configure + `ctest -N` 确认目标不在列表，NOT_BUILT 污染回归清除。
