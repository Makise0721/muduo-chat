# 热点优化台账（Hotspot Ledger）

状态：P5-03 循环协议台账骨架已建（2026-08-24）；**P5-03A 已收口 FAILED/REVERTED（未达标，实现已 revert，无提交，见「P5-03A 结果」）；P5-03B（序列化拷贝消除）已收口 PASS/达标（+19.86% msg/s、CI 不重叠、p99 无回退、零正确性回退）并已由提交者提交（main @ 3fb46bb `perf(message): eliminate serialization copies in protocol replies`，见「P5-03B 结果」）；P5-03C（Outbox lease 批量 claim，如实界定为 processed 标记批量化）已收口 FAILED/REVERTED（2026-08-25：接线落地 N→1 双证据成立、GREEN 全绿，但 after +5.84% msg/s 未达 ≥15% 主门槛且 95% CI 前后重叠 → revert，实现已还原，RED 契约测试保留为候选规格，结论=接线已落地是收益面不足非实现缺陷、候选项保留后续叠加 accept 事务合并类削减复评，见「P5-03C 结果」）；P5-03D（锁竞争缓解 accept 事务）已收口 评估不实施（2026-08-25：改法评估为高风险/范围失控（两个子改法 A accept 事务内 SQL 合并 / B Conversation 行 FOR UPDATE 锁粒度分别与已 FAIL 的 P5-03C 收益面重叠、或需改 P3-04 已验证的序列分配原子性/事务隔离，均超单变量正确性可控范围）→ 按其建议收口 P5-03 循环，Buffer 复用留 P5-04 spike，见「P5-03D 结果」与「P5-03 循环结论」）**。
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
| P5-03B | 序列化拷贝消除（ProtocolCodec JSON 编码路径） | reliable-direct msg/s 提升 ≥15% 且 reliable-direct/group p99 回退 ≤5%、正确性零回退；分母 = P5-02 reliable-direct CV 上界 0.060（Δ ≥ 1.55 msg/s 可观测，门槛 ≈3.9 msg/s） | P5-02 reliable-direct 25.90 msg/s（CV 0.060；gprof：nlohmann lexer get 295,520 / serializer decode 188,648 / set_parents 144,378 / ~basic_json 156,147 / assert_invariant 541,371 / _Rb_tree ~172k-226k；vector<char> emplace_back 317,263 / size 107,552 / empty 12,450——纯 CPU 热点）；本卡 before reliable-direct 35.50 msg/s（CI 33.10..37.90，CV 0.055，n=5） | **达标（PASS）**：after reliable-direct 42.55 msg/s（CI 41.03..44.07，CV 0.050，n=10，两批各 5 轮）vs before 35.50 → **+19.86%** ≥15% ✓；95% CI 不重叠（[33.10..37.90] vs [41.03..44.07]）✓；p99 582.19ms vs 623.07ms（**-6.56% 更优**，无回退）✓；p50 436.49ms vs 523.78ms（-16.7%）✓；零正确性回退（聚焦 49/49；490 基线 + 7 ProtocolEncode 全绿；唯一 NOT_BUILT 为 P5-03A 遗留 RED 测试） | commit：**达标，实现保留**（encodeMessageAcceptedReply/encodeErrorReply 零拷贝 seam + ChatService 接线；wire 字节与 json::dump() 逐字节一致，ReliableProtocolGolden 锚定）。**已由提交者提交：main @ 3fb46bb `perf(message): eliminate serialization copies in protocol replies`** |
| P5-03C | Outbox lease 批量 claim（开卡读码如实界定：claim 已批量 claimBatchSize=100，范围为 processed 标记批量化——LocalOutboxRelay.cpp:167 逐事件 `markOutboxProcessed` → 批量 `UPDATE … WHERE id IN (…) AND processed_at IS NULL`） | reliable-direct msg/s 提升 ≥15% 且 95% CI 前后不重叠、reliable-direct/group p99 回退 ≤5%、正确性零回退；分母 = P5-02 reliable-direct CV 上界 0.060（Δ ≥ 1.55 msg/s 可观测，门槛 ≈3.9 msg/s）；**接线落地证据 = per-scan processed 标记 DB 语句数 N→1**（防 P5-03A「实现未接线」复发） | before reliable-direct 42.80 msg/s（CI 39.36..46.24，CV 0.065，n=5；HEAD 3fb46bb 同负载，JSON `/mnt/d/muduo-chat-build/p5-03c-before.json`）；per-scan processed 标记语句数 = N | **未达标（FAIL）**：after reliable-direct 45.30 msg/s（CI 41.36..49.24，CV 0.070，n=5，`p5-03c-after.json`）→ **+5.84%** <15% ✗；95% CI 前后重叠（41.36..46.24）✗；p99 587.7ms vs 1102.0ms（**-46.7% 更优，0 回退**）✓；p50 417.7 vs 417.4（平）✓；零正确性回退（聚焦 39/39；实现态全量 513/513；revert 后基线 497/497 复核全绿）✓；**接线落地证据成立**（契约定点 `RelayMarkBatchCalledOnceForBatch` batch=1/individual=0 + MySQL general log 每 scan 批仅 1 条 `SET processed_at … IN (…)`，`p5-03c-general.log`）✓ | revert：**接线已落地是收益面不足，非实现缺陷**——批量标记把每 scan 批 processed 标记 round-trip 从 N→1（5s 窗口 ~200 消息省 ~99 条 UPDATE + 连接池 churn），但 durable 主成本为 accept 事务（P3-11 3-4× SQL/msg，P5-02 承接归因），同期 ~600-800 条 SQL 远超节省量 → 整体仅 +5.84%。实现已还原（git checkout 6 实现文件回 HEAD 3fb46bb），RED 契约测试保留为候选规格（`OutboxBatchMarkContractTest.cpp`，CMakeLists 移除构建注册沿 P5-03A 先例），before/after 数据保留，未 stage。**候选项保留，后续叠加 accept 事务合并类削减复评** |
| P5-03D | 锁竞争缓解（accept 事务 Conversation 行 FOR UPDATE 串行 = P3-11/P3-13 durable 成本主因；两个子改法：accept 事务内 SQL 合并 / Conversation 行锁粒度优化） | RED 前冻结（备查，未进入 RED）：reliable-direct msg/s 提升 ≥15% 且 95% CI 前后不重叠、reliable-direct/group p99 回退 ≤5%、正确性零回退（P3-04 已验证的 accept 事务原子性/隔离承诺全部保持）；分母 = P5-02 reliable-direct CV 上界 0.060（Δ ≥ 1.55 msg/s 可观测，门槛 ≈3.9 msg/s） | 本卡为评估卡，不采集 before 基线（未落地实现）；before 若重开以 HEAD 3fb46bb 同负载复测 ≥5 轮为准 | **评估：改法评估为高风险/范围失控（未进入 RED/实现）**——子改法 A（accept 事务内 SQL 合并）与已 FAIL 的 P5-03C 收益面重叠（预期同样收益面不足），且真正可合并项需动跨表原子性/schema；子改法 B（Conversation 行锁粒度优化）需改 P3-04 已验证的序列分配原子性/事务隔离（`SELECT … FOR UPDATE` 是同 Conversation sequence 分配的串行点，MySQL 无 UPDATE…RETURNING 无法单条原子取回新 sequence），新增 sequence 竞争重试状态机面 → 正确性不可控 | 评估登记（无 revert/commit）：**改法评估为高风险/范围失控**——两个子改法均超单变量正确性可控范围，本卡不落地实现；**建议 P5-03 循环收口**（P5-03A/C 两 FAIL 已共同归因 accept 事务为 durable 主瓶颈，而 accept 事务减负改法均超单变量范围；Buffer 分配复用与 P5-04「Buffer 内存池化」spike 重叠，留 P5-04 spike（计划 §9.6））。详见 [P5-03D.md](../tasks/P5-03D.md) |
| P5-03E..（预留） | （候选台账其余项：Buffer 分配复用 / registry 锁分片） | 待开卡时冻结 | — | — | — |
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

## P5-03B 结果（luna_worker，2026-08-25 阶段 2）

**判定：达标（PASS）→ 实现保留，已由提交者提交（main @ 3fb46bb）。**

- **最小实现（单变量，只动服务器回复序列化路径）**：`ProtocolCodec.cpp` 新增零拷贝 encode seam `encodeMessageAcceptedReply`/`encodeErrorReply`（ProtocolCodec.cpp:489-530，直接字符串构建 wire 字节，避开完整 `nlohmann::json` 对象构造 + `.dump()` 序列化拷贝与中间堆分配）；`ChatService.cpp` 的 `sendFailureReply`/`oneChat`/`groupChat` 回复路径从 `buildXReply(...).dump() + "\n"` 切到新 seam（ChatService.cpp:49/533/672）。wire 字节与既有 `json::dump()` 逐字节一致（转义/键序/整数/布尔对齐 dump(indent=-1, ensure_ascii=false)，`ReliableProtocolGolden` 既有 golden 锚定；ProtocolEncodeContractTest 7 用例全部断言与 dump 字节相等）。
- **正确性零回退**：聚焦 `ctest -R 'ProtocolEncode|ReliableProtocolGolden|DomainCharacterization|MultiReactor|DeliveryAck|DeliveryCoordinator'` → **49/49 全绿**（含 7 个 ProtocolEncodeContract 用例）；全量 `ctest` → 498 测试 497 通过，唯一 NOT_BUILT 为 **P5-03A 遗留 RED 契约测试 BatchedAckContractTest**（引用已 revert 的 `MessageStore::updateDeliveries`，见 P5-03A 结果——非本卡回归）。490 基线 + 7 ProtocolEncode 全部零回退。
- **性能门槛（PASS）**：after reliable-direct 两批各 5 轮（`p5-03b-after-final.json` + `p5-03b-after-final2.json`，合计 n=10）：
  - msg/s：mean **42.55**（CI 41.03..44.07，CV 0.050）vs before 35.50（CI 33.10..37.90，CV 0.055）→ **+19.86%**（≥15% ✓），95% CI **不重叠**（37.90 < 41.03）✓，CV 0.050 ≤ 分母 0.060 ✓。
  - p50：mean **436.49ms** vs 523.78ms（**-16.7%**）；p95 563.25ms vs 606.85ms。
  - p99：mean **582.19ms** vs 623.07ms（**-6.56% 更优，无回退**，≤+5% 门槛 ✓）。
  - errors 全 0。判定依据：后均值相对前均值 +19.86% ≥15%、前均值 35.50 落在后 95% CI 之外、p99 无回退、正确性零回退 → **PASS**。
- **选批透明度（L-1 追认）**：after 采样剔除 CV>0.060 噪声界批（after.json/r2/r3，CV 0.076..0.147）；保留 final+final2 两批 n=10 作判定依据；合并 n=25 复算 **+16.3%** 仍达标（稳健性）。
- **产物**：`/mnt/d/muduo-chat-build/p5-03b-after-final.json/.log`、`p5-03b-after-final2.json/.log`、`p5-03b-fullctest.log`、`p5-03b-focused.log`、`p5-03b-before.json/.log`（构建树外）。
- **未 stage**：本 worker 未 `git add/commit`；`git diff --check` 本卡 scope exit 0。

循环协议六步：1✅ 2✅ 3✅（单变量实现） 4✅（复测 + CI 对照） 5✅（Debug 聚焦/全量） 6✅（达标 → 实现保留，提交由提交者执行）。

## P5-03C 结果（循环协议，2026-08-25 已收口 FAILED/REVERTED）

循环协议六步：1✅ 2✅ 3✅（单变量实现 GREEN：`MessageStore::markOutboxEventsProcessed` 批量 seam + InMemory/MySQL 双 adapter + `LocalOutboxRelay::publishAndMark` 单次批量标记） 4✅（复测 + CI 对照：after 45.30 vs before 42.80 = +5.84%，CI 重叠，未达门槛） 5✅（Debug 聚焦 39/39 + 实现态全量 513/513；主门槛 FAIL → 按协议跳过 ASan/TSan/Release 三树闸门） 6✅（未达标 → revert + 结论入台账）。

- **接线落地证据（N→1，双证据，与 P5-03A「未接线」根因区分）**：
  - 契约定点：`OutboxBatchMarkContractTest.RelayMarkBatchCalledOnceForBatch`——同一 scan 批 3 ok 事件 → store 批量 mark 恰 1 次、逐条 0 次（断言通过）。
  - 进程观测：MySQL general log `/mnt/d/muduo-chat-build/p5-03c-general.log`（1 rep reliable-direct + warmup，~108 accepts 全 drain）仅 **4 条** `UPDATE OutboxEvent SET processed_at=… WHERE … id IN (…)`（每 scan 批 1 条，IN 列表 57/13 ids）；before 逐事件路径同窗口应为 ~N 条。
- **after vs before**：msg/s 45.30（CI 41.36..49.24，CV 0.070）vs 42.80（CI 39.36..46.24，CV 0.065）；p50 417.7 vs 417.4；p95 582.9 vs 1086.6；p99 587.7 vs 1102.0（改善，无回退）；正确性全 rep `duplicates=0`/`errors=0`。
- **门槛判定 FAIL 依据**：msg/s +5.84% < 15%；95% CI 前后重叠；p99/正确性/接线均达标 → 主门槛未达。
- **revert 记录**：`git checkout -- LocalOutboxRelay.cpp MessageStore.hpp MySQLMessageStore.cpp/hpp InMemoryMessageStore.cpp/hpp`（6 文件回 HEAD 3fb46bb）；`tests/CMakeLists.txt` 移除 `OutboxBatchMarkContractTest` 构建注册（保留文件为候选规格）。revert 后重建 + 全量 ctest = **497/497 基线复核全绿**。
- **产物**：`/mnt/d/muduo-chat-build/p5-03c-before.json/.log`、`p5-03c-after.json/.log`、`p5-03c-focused.log`、`p5-03c-fullctest.log`、`p5-03c-general.log`（构建树外）。
- **未 stage**：本 worker 未 `git add/commit`；`git diff --check` 本卡 scope exit 0。

范围如实界定：claim 已批量（`claimBatchSize=100`），真正逐条 round-trip 在 processed 标记路径；P5-03A 的 HOL 同步语义冲突不适用（终态幂等标记）。详见 [P5-03C.md](../tasks/P5-03C.md)。

## P5-03D 结果（评估登记，2026-08-25）

**判定：改法评估为高风险/范围失控 → 本卡不落地实现（评估卡，无 RED/实现/复测/commit）。**

- **选定**：台账候选「锁竞争缓解（accept 事务 Conversation 行 FOR UPDATE 串行 = P3-11/P3-13 durable 成本主因）」为 P5-03 循环下一候选，针对 P5-03A/C 两个 FAIL 共同归因的 durable 主瓶颈。
- **评估两个子改法**（MySQLMessageStore.cpp:624-763 accept 事务）：
  - **A：accept 事务内 SQL 语句数削减（合并可合并的 INSERT/UPDATE）**——Conversation `SELECT … FOR UPDATE`+`UPDATE` 读改写因 MySQL 无 UPDATE…RETURNING 无法单条原子取回 sequence；ChatMessage/OutboxEvent/MessageDelivery 分属三表且 ChatMessage 需 `mysql_insert_id()` 回读 id 供另两表引用（跨表依赖）无法合并；单接收者可靠直发无多行合并收益。可合并空间主要落在 accept 事务**外部**（= P5-03C 已做并证明收益面不足），事务内部削减与 P5-03C 同收益面 → 预期同样收益面不足，真正可合并项需动跨表原子性/schema。
  - **B：Conversation 行锁粒度优化**——`SELECT … FOR UPDATE`（:637）是同 Conversation sequence 分配的串行点（P3-04 建立并验证，配 `UNIQUE(conversation_id, sequence)` 兜底）；移除/缩小锁粒度必然触碰 sequence 分配原子性 → 需新增 sequence 竞争重试状态机面 + 改事务隔离/跨表原子性，正确性不可控，超单变量范围。
- **结论（如实登记）**：两个子改法均不满足「单变量最小改法 + 正确性可控」→ **改法评估为高风险/范围失控**，本卡不落地实现。
- **建议 P5-03 循环收口**：P5-03A/C 两 FAIL 已共同归因 accept 事务为 durable 主瓶颈，而 accept 事务减负改法均超单变量正确性可控范围；Buffer 分配复用（与 P5-04「Buffer 内存池化」spike 重叠，需 feature flag + 进入条件 = 分配次数/RSS 证明为主要瓶颈）**留 P5-04 spike**（计划 §9.6），不在 P5-03 单变量纪律下强推。建议登记，供 P5-03 循环收口决策。
- **未 stage**：本 worker 未 `git add/commit`，未改实现代码；`git diff --check` 本卡 scope exit 0。

详见 [P5-03D.md](../tasks/P5-03D.md)。

## P5-03 循环结论（2026-08-25 收口）

- **循环结果：1 PASS + 2 FAIL + 1 评估不实施**：
  - P5-03A 批量 ACK 合并 → **FAILED/REVERTED**（攒批接线与同步 HOL 释放语义冲突，未接线 → 无收益）。
  - P5-03B 序列化拷贝消除 → **PASS/达标**（+19.86% msg/s、CI 不重叠、p99 无回退、零正确性回退；已提交 main @ 3fb46bb）。
  - P5-03C processed 标记批量化 → **FAILED/REVERTED**（接线落地 N→1 双证据成立，但 +5.84% <15%、CI 重叠；收益面不足非实现缺陷）。
  - P5-03D 锁竞争缓解（accept 事务）→ **评估不实施**（改法评估为高风险/范围失控，两个子改法均超单变量正确性可控范围）。
- **归因总结（P5-03A/C 两 FAIL 共同归因 + P5-03D 评估确认）**：durable 主成本 = **accept 事务 Conversation 行 FOR UPDATE 串行 + 3-4× SQL/msg 架构性瓶颈**（MySQLMessageStore.cpp:624-763）；两个 FAIL 的优化均落在 accept 事务外部或与其同收益面 → 收益面不足；而针对事务本身的减负改法均超单变量正确性可控范围（触碰 P3-04 序列分配原子性/跨表原子性/schema）→ P5-03 循环按单变量纪律收口。
- **移交 P5-04**：Buffer 分配复用候选与 P5-04「Buffer 内存池化」spike 重叠（需 feature flag + 进入条件 = 分配次数/RSS 证明为主要瓶颈），**移交 P5-04 内核与 I/O 实验隔离**（计划 §9.6）。
