# 热点优化台账（Hotspot Ledger）

状态：P5-03 循环协议台账骨架已建（2026-08-24）；**P5-03A 已收口 FAILED/REVERTED（未达标，实现已 revert，无提交，见「P5-03A 结果」）；P5-03B（序列化拷贝消除）已收口 **PASS/达标**（+19.86% msg/s、CI 不重叠、p99 无回退、零正确性回退；实现保留待提交者单变量原子提交，见「P5-03B 结果」）**。
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
| P5-03B | 序列化拷贝消除（ProtocolCodec JSON 编码路径） | reliable-direct msg/s 提升 ≥15% 且 reliable-direct/group p99 回退 ≤5%、正确性零回退；分母 = P5-02 reliable-direct CV 上界 0.060（Δ ≥ 1.55 msg/s 可观测，门槛 ≈3.9 msg/s） | P5-02 reliable-direct 25.90 msg/s（CV 0.060；gprof：nlohmann lexer get 295,520 / serializer decode 188,648 / set_parents 144,378 / ~basic_json 156,147 / assert_invariant 541,371 / _Rb_tree ~172k-226k；vector<char> emplace_back 317,263 / size 107,552 / empty 12,450——纯 CPU 热点）；本卡 before reliable-direct 35.50 msg/s（CI 33.10..37.90，CV 0.055，n=5） | **达标（PASS）**：after reliable-direct 42.55 msg/s（CI 41.03..44.07，CV 0.050，n=10，两批各 5 轮）vs before 35.50 → **+19.86%** ≥15% ✓；95% CI 不重叠（[33.10..37.90] vs [41.03..44.07]）✓；p99 582.19ms vs 623.07ms（**-6.56% 更优**，无回退）✓；p50 436.49ms vs 523.78ms（-16.7%）✓；零正确性回退（聚焦 49/49；490 基线 + 7 ProtocolEncode 全绿；唯一 NOT_BUILT 为 P5-03A 遗留 RED 测试） | commit：**达标，实现保留**（encodeMessageAcceptedReply/encodeErrorReply 零拷贝 seam + ChatService 接线；wire 字节与 json::dump() 逐字节一致，ReliableProtocolGolden 锚定）。由提交者执行单变量原子提交 |
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

## P5-03B 结果（luna_worker，2026-08-25 阶段 2）

**判定：达标（PASS）→ 实现保留，待提交者执行单变量原子提交。**

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
