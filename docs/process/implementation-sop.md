# muduo-chat 实施标准与执行手册

状态：当前可执行

当前活动任务：无（P3-11 已 VERIFIED）

下一任务：[P3-12 建立可靠消息指标与故障矩阵](../plans/post-p2-implementation-plan.md#p3-12-建立可靠消息指标与故障矩阵)

## 1. 文档与事实优先级

发生冲突时按以下顺序处理：

1. 当前代码、Git diff、测试输出和数据库实际 schema。
2. `accepted` ADR、正式协议和已执行 migration。
3. [当前实施计划](../plans/post-p2-implementation-plan.md)。
4. 本 SOP 与 [进度索引](implementation-progress.md)。
5. [架构演进方案](../architecture/evolution-plan.md)。
6. `archive/` 中的历史任务卡和旧 review/report。

文档中的目标不是完成事实。只有当前 checkout、命令输出、diff 和提交记录均可定位时，任务才能标为 `VERIFIED`。

## 2. Windows 与 WSL2 分工

Windows PowerShell 负责：

- Git status/log/diff、文件与换行事实。
- 文档链接、`git diff --check` 和最终变更范围审查。
- 调用 `wsl.exe -d Ubuntu -- ...` 启动 Linux 验证。

WSL2 Ubuntu 负责：

- epoll/eventfd/timerfd/socketpair 等 Linux 路径。
- CMake build、CTest、MySQL integration、process tests 和 benchmark。
- ASan+UBSan、TSan 与 Linux 资源限制测试。

不得把 Windows 未执行路径称为 Linux 已验证，也不得把被 skip 的 MySQL/进程测试算作通过。

## 3. 工作树与构建目录

- 禁止在源码树内构建；每个任务使用独立 binary tree。
- Fresh review 优先使用 `/tmp/muduo-chat-build/<task>-<config>`。
- 需要跨 WSL 重启复用时可使用 WSL 用户目录下的专用 build 目录，但证据中必须记录绝对路径。
- configure/build 前后都检查 Windows `git status --short`；生成物不得写回源码树。
- WSL 与 Windows 的换行视图不同，以 Windows Git diff 为工作树事实；不要为消除 WSL CRLF 噪声批量改源码。

## 4. 单任务状态机

允许状态：

`PLANNED → IN_PROGRESS → RED → GREEN → VERIFIED`

`BLOCKED` 只表示依赖或授权确实阻止继续，不表示任务困难。任一时刻最多一张任务卡为 `IN_PROGRESS/RED/GREEN`。

开始任务前必须确认：

- 上游任务 `VERIFIED`。
- 当前 HEAD 与计划基线一致，或差异已经写入任务卡。
- 工作树中的既有修改归属明确。
- 允许写入文件和非目标明确。
- schema、系统安装、网络下载或外部依赖变更已经获得所需授权。

## 5. RED→GREEN 执行循环

1. 在 `docs/tasks/<TASK-ID>.md` 创建唯一活动任务卡。
2. 通过公开 interface 写最小失败测试。
3. 运行聚焦测试，记录命令、退出码和目标失败原因。
4. 如果测试意外通过，停止：行为已经存在或测试没有覆盖目标。
5. 只实现让目标测试通过的最小代码，不顺手重构相邻模块。
6. 运行聚焦 GREEN、Debug 全量和适用 sanitizer/process/integration tests。
7. 审查 diff、文档、协议、schema 和 benchmark 变化。
8. 更新任务卡和进度索引，再形成一个原子提交。

测试与调用方应穿过同一个 interface；不要读取私有容器、依赖固定 sleep 或复制生产实现到测试中。

## 6. 任务卡模板

```md
# <TASK-ID> <标题>

状态：PLANNED
基线：main @ <commit>
依赖：<task/status>
允许写入：<paths>

## 问题与证据

## Interface / 可观察行为

## 非目标

## RED
- 测试：
- 命令：
- 预期失败：
- 实际失败与退出码：

## 最小实现

## 验证
- 聚焦：
- Debug：
- ASan+UBSan：
- TSan：
- integration/process：

## Diff 审查

## 完成定义

## Commit
```

## 7. 标准验证命令

以下命令中的 `<task>`、`<regex>` 必须替换为当前任务值。默认串行 CTest，避免 MySQL schema 和固定端口互相干扰。

### Debug

```powershell
wsl.exe -d Ubuntu -- bash -lc "set -euo pipefail; cmake -S /mnt/d/agent_learning/muduo-chat -B /tmp/muduo-chat-build/<task>-debug -DCMAKE_BUILD_TYPE=Debug -DENABLE_TESTS=ON; cmake --build /tmp/muduo-chat-build/<task>-debug -j2; ctest --test-dir /tmp/muduo-chat-build/<task>-debug --output-on-failure"
```

### 聚焦测试

```powershell
wsl.exe -d Ubuntu -- ctest --test-dir /tmp/muduo-chat-build/<task>-debug -R '<regex>' --output-on-failure
```

### ASan + UBSan

```powershell
wsl.exe -d Ubuntu -- bash -lc "set -euo pipefail; cmake -S /mnt/d/agent_learning/muduo-chat -B /tmp/muduo-chat-build/<task>-asan -DCMAKE_BUILD_TYPE=Debug -DENABLE_TESTS=ON -DENABLE_ASAN=ON -DENABLE_UBSAN=ON; cmake --build /tmp/muduo-chat-build/<task>-asan -j2; ctest --test-dir /tmp/muduo-chat-build/<task>-asan --output-on-failure"
```

### TSan

```powershell
wsl.exe -d Ubuntu -- bash -lc "set -euo pipefail; cmake -S /mnt/d/agent_learning/muduo-chat -B /tmp/muduo-chat-build/<task>-tsan -DCMAKE_BUILD_TYPE=Debug -DENABLE_TESTS=ON -DENABLE_TSAN=ON; cmake --build /tmp/muduo-chat-build/<task>-tsan -j2; setarch x86_64 -R ctest --test-dir /tmp/muduo-chat-build/<task>-tsan --output-on-failure"
```

TSan suppression 必须是仓库已审查文件；不得用 suppression 隐藏本项目竞态。

### Release 生产构建

```powershell
wsl.exe -d Ubuntu -- bash -lc "set -euo pipefail; cmake -S /mnt/d/agent_learning/muduo-chat -B /tmp/muduo-chat-build/<task>-release -DCMAKE_BUILD_TYPE=Release -DENABLE_TESTS=OFF; cmake --build /tmp/muduo-chat-build/<task>-release -j2"
```

### Windows 最终审查

```powershell
git status --short
git diff --check
git diff --stat
git diff --name-status
git diff
```

新文件不包含在普通 `git diff` 中，必须同时查看 `git status --short` 并单独检查其行尾、链接和内容。

## 8. 数据库、协议与外部依赖

- MySQL contract/integration 必须使用真实测试库且不得 skip。
- migration 必须从空库和当前已发布 schema 两条路径验证；破坏性 contract migration 与 expand/backfill 分开。
- 所有用户输入使用 prepared statement；连接获取必须有 deadline。
- 协议字段或错误语义变化先更新 accepted ADR/golden tests，再改实现。
- Redis、Kafka 等新依赖开始任务时重新核对官方版本与文档；没有 M3/M4 前置证据不得抢跑。
- 配置和测试日志不得记录数据库密码、token 或原始敏感 payload。

## 9. 完成闸门

任务只有同时满足以下条件才能 `VERIFIED`：

- RED 的失败原因与目标一致，GREEN 由最小实现获得。
- 聚焦与全量回归通过；适用 sanitizer/integration/process 路径通过。
- 并发测试不靠偶然时序，关键竞态有重复运行证据。
- 文档、协议、schema、配置示例与代码同步。
- `git diff --check` 通过，diff 只包含任务允许范围。
- benchmark 没有未解释回退；数字带 commit、环境和原始参数。
- 任务卡、进度索引和实际提交一致。

## 10. 停止条件

出现以下任一情况立即停止当前任务并保留证据：

- RED 意外通过或失败原因不对。
- 需要越过允许写入范围或改变已 accepted 语义。
- sanitizer、真实 MySQL、process/fault test 失败或被 skip。
- migration 不能从旧 schema 重放，或需要静默丢数据。
- 性能结果与假设相反且无法解释。
- 需要未经授权的下载、系统变更或外部状态写入。

完成的任务卡在里程碑独立验收后移到 `docs/archive/tasks/<phase>/`；归档文件不再作为当前执行指令。
