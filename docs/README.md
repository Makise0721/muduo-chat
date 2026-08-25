# muduo-chat 文档导航

当前状态：P1/P2 已完成并通过对应验收；P3-00..P3-13 全部 VERIFIED；M3 已 VERIFIED（[M3 报告](reports/p3-m3-gates.md)）；[P4-00](tasks/P4-00.md)、[P4-01 PresenceDirectory interface 与 in-memory contract](tasks/P4-01.md)、[P4-02 Redis fencing adapter](tasks/P4-02.md)、[P4-03 OutboxPublisher port 与 Kafka adapter](tasks/P4-03.md)、[P4-04 幂等 Delivery consumer](tasks/P4-04.md)、[P4-05 Gateway 定向投递与 epoch 校验](tasks/P4-05.md)、[P4-06 三节点 chaos 与容量保护](tasks/P4-06.md)、[P4-07 M4 独立验收](tasks/P4-07.md) 全部 VERIFIED；M4 已 VERIFIED（入口 [M4 报告](reports/p4-m4-gates.md)）；[P5-00](archive/tasks/p5/P5-00.md)、[P5-01](archive/tasks/p5/P5-01.md)、[P5-02](archive/tasks/p5/P5-02.md)、P5-03A..D、P5-04、[P5-05 M5 证据包](archive/tasks/p5/P5-05.md) 全部终态（03A/C FAILED/REVERTED、03B PASS/已提交 3fb46bb、03D 评估不实施、04 CLOSED 有据跳过，其余 VERIFIED）；M5 已 VERIFIED（入口 [M5 报告](reports/p5-m5-gates.md)）。

## 从这里开始

1. [当前实施计划](plans/post-p2-implementation-plan.md)：P3-00..P3-13，以及 P4/P5 的进入闸门。
2. [实施 SOP](process/implementation-sop.md)：单任务 RED→GREEN、WSL2 验证、提交和停止规则。
3. [进度索引](process/implementation-progress.md)：已完成任务与可定位证据。
4. [架构演进方案](architecture/evolution-plan.md)：长期架构目标、模块 interface 和里程碑。
5. [领域词汇](../CONTEXT.md) 与 [ADR](adr/)：术语和难以逆转的设计决定。

## 目录职责

| 目录 | 内容 | 是否作为当前指令 |
|---|---|---|
| `architecture/` | 长期目标架构与设计约束 | 是，但具体执行顺序以 `plans/` 为准 |
| `plans/` | 当前里程碑之后的可执行任务拆分 | 是 |
| `process/` | SOP、状态与验证证据索引 | 是 |
| `specs/` | 当前已观察到的协议/领域行为 | 是；目标行为变化需同步 ADR/计划 |
| `adr/` | 难以逆转且存在真实权衡的决定 | `accepted` 才能约束实现；`proposed` 只是候选 |
| `reviews/` | 固定基线的完成性或对抗审查 | 只作为审查证据 |
| `reports/` | benchmark、里程碑验收与原始环境说明 | 只作为测量证据 |
| `tasks/` | 当前正在执行的单张任务卡 | 无任务时可以不存在或为空 |
| `archive/` | 已完成任务卡等历史证据 | 否；不要从这里启动新任务 |

## 维护规则

- `docs/` 根目录只保留本导航；新文件必须进入职责明确的子目录。
- 当前只保留一份后续实施计划。旧计划完成或被取代后从工作树删除，历史版本由 Git 保留。
- 一个任务一个任务卡；任务结束后先留在 `tasks/`，里程碑独立验收完成后再移入 `archive/tasks/<phase>/`。
- 任务卡记录实际 RED/GREEN 命令和提交事实，不把未来目标写成已经完成。
- review 与 report 不混入 task；benchmark 必须记录 commit、环境、参数和不可外推边界。
- 移动文档后必须运行 Markdown 相对链接检查、`rg` 旧路径扫描和 `git diff --check`。

## 历史材料

P0、P1、P1R、P2、P5 的任务卡位于 [archive/tasks](archive/tasks/)。归档文件保留执行时的原始路径和状态措辞，因此其中的 `docs/tasks/...` 等文本是历史事实，不是当前导航链接。
