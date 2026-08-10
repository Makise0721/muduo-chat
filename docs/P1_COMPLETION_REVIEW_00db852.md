# P1 完成性审查报告

审查日期：2026-08-10

审查范围：`git diff 1b94d43...00db852`

- merge-base：`1b94d436b8410412a7c0eb5a04966039ab4d3b87`
- 被审查 HEAD：`00db852`
- 审查对象：P0/P1 的 23 个提交、相关设计/任务卡、生产代码与测试
- 审查方法：按 Standards 与 Spec 两条轴独立审查，并在 WSL2 Ubuntu 的全新构建目录中复验

## 1. 结论

**P1 暂不通过完成性验收，应重新打开为 `P1_REMEDIATION`。**

当前代码可以完成 Release 生产构建，现有 Debug、ASan+UBSan、TSan 测试均为 78/78 通过。这证明当前测试覆盖到的路径没有复现失败，但不能证明 P1 的全部设计契约已经实现。审查仍发现信号处理阻塞、跨线程发送无界排队、跨 EventLoop 直接关闭连接、过载保护被缩减、编码上限缺失、定时器相位漂移与结构化日志未完成等问题。

| 验收维度 | 结果 | 说明 |
|---|---|---|
| Release 生产构建 | PASS | `ENABLE_TESTS=OFF` 全新构建成功 |
| Debug CTest | PASS | 78/78 |
| ASan+UBSan CTest | PASS | 78/78 |
| TSan CTest | PASS | 78/78 |
| Standards | FAIL | 5 个硬性问题、1 个设计气味、1 个陈旧文档问题 |
| Spec | FAIL | 3 个 High、3 个 Medium |
| P1 完成性 | REOPEN | 先完成 P1R，再进入 P2 |

## 2. Standards 轴

以下结果只判断是否遵守仓库已写明的开发标准、任务卡和修改边界，不与 Spec 轴合并排序。

### S-01 P0：信号处理器可能阻塞

`chatserver/main.cpp` 的 signal handler 向 `gSignalFds[1]` 写入，但初始化时只将 `gSignalFds[0]` 设为 `O_NONBLOCK`。信号突发填满 socket buffer 后，handler 可能阻塞。该实现不满足 `docs/tasks/P1-05.md` 声明的“nonblocking self-pipe / async-safe handler”。

完成要求：读写端都必须非阻塞并设置 close-on-exec；handler 只执行可丢弃的非阻塞通知，`EAGAIN` 不得重试或阻塞；增加进程级信号风暴测试。

### S-02 P1：P1-07 缺少可定位的 RED 证据

`docs/tasks/P1-07.md` 只记录修复后的绿色结果，没有记录每个目标失败测试的命令、退出码和失败原因。它不满足 `docs/IMPLEMENTATION_SOP.md` 的 RED gate，因此“五轮审查通过”不能替代测试先失败的证据。

### S-03 P1：多个任务越过自己的允许写入边界

- P1-05 修改了任务卡未授权的 `ChatServer.hpp`、`TcpConnection.h`。
- P1-06 修改了任务卡未授权的 `BackpressureTest`。
- P1-07 修改了任务卡未授权的 `EVOLUTION_PLAN.md`、`TcpConnection.h`。

这些改动不一定都应撤销，但必须在整改任务中重新归属，说明依赖关系并按单一任务重新验证，不能继续把原任务卡当成可审计的原子提交记录。

### S-04 P2：任务状态互相矛盾

`docs/tasks/P1-06A.md` 仍为 `IN_PROGRESS`，而 `docs/IMPLEMENTATION_PROGRESS.md` 已将其记为 `VERIFIED`，并继续启动、完成了 P1-07。这违反“任一时刻最多一个任务为 `IN_PROGRESS`”的 SOP。

### S-05 P2：仓库自己的 diff gate 当前失败

`git diff --check 1b94d43...HEAD` 报告 `docs/EVOLUTION_PLAN.md` 与 `docs/IMPLEMENTATION_SOP.md` 的行尾空白。即使它们是 Markdown 强制换行，也与仓库当前要求的无条件 `git diff --check` 闸门冲突。应统一改为无行尾空白的 Markdown，或显式修改并解释 gate；不能同时声称该 gate 已通过。

### S-06 P3：`ChatServer` 出现浅层委托接口

新增的三个方法只把调用转发给 `TcpServer`，没有隐藏额外复杂性。这是 Middle Man 设计气味。它不是本轮最高优先级缺陷，但在 P2 建立 `ChatApplication` 边界时应避免继续扩大这层接口。

### S-07 P2：`CLAUDE.md` 的构建与测试说明已过期

该文档仍描述源码树内 `build` 流程，且没有反映已有 CTest/Sanitizer 闸门，与当前 out-of-source 构建约束不一致。

## 3. Spec 轴

以下结果只判断实现是否满足 `EVOLUTION_PLAN.md` 和 P1 任务卡所承诺的行为，不与 Standards 轴合并排序。

### F-01 High：`SendResult` 不能让生产者可靠停止，跨线程排队仍无界

设计要求达到 pause 水位时返回 `Backpressured`，并让上层停止继续生产。当前实现存在两条偏差：

1. loop 线程内的一次发送跨过 pause 水位后仍可能返回 `Accepted`。
2. 非 loop 线程只检查单消息大小，随后把消息排入 `EventLoop::pendingFunctors_` 并立即返回 `Accepted`；这些尚未编码、尚未进入 output buffer 的数据不受 hard limit 约束。

此外，hard limit 只在 append 前检查当前 output buffer，没有按“当前字节 + 本次帧字节”做原子准入，因此单帧可越界。P1-07 通过修改设计文档接受 overshoot，属于用文档迁就实现，而不是满足原来的有界内存目标。

### F-02 High：强制关闭违反 connection 所属 EventLoop 的线程亲和性

`TcpServer::forceCloseAllConnections()` 在 base loop 中遍历连接后直接调用 `TcpConnection::forceClose()`；后者直接操作 Channel、回调和 fd 状态。启用 worker loops 后，这些连接属于其他 EventLoop，当前路径会跨线程操作 loop-affine 对象。

现有测试没有覆盖 `threadNum > 0` 下的强制 drain/close，因此 TSan 全绿不能关闭此问题。

### F-03 High：P1-05 将既定过载保护范围降级

上位设计要求覆盖最大连接数、accept 速率、任务队列长度、EMFILE idle-fd 处理和 reconnect storm。当前实现只有最大连接数，以及 EMFILE 后暂停 100ms 再重试：

- 没有 idle-fd 恢复流程。
- 没有 accept rate limiter。
- 没有任务队列容量保护。
- 没有真实 fd exhaustion 与 reconnect storm 测试。

P1-05 将 accept rate 写成非目标，不能单方面覆盖既有 P1 完成定义。

### F-04 Medium：v2 encode 可生成违反协议上限的帧

`StreamCodec::encode` 没有拒绝超过 16 MiB 的 body，也没有校验 header 的 body length 与实际 body 一致。当前 `void encode(...)` 接口也没有向调用方表达失败的通道。

### F-05 Medium：重复定时器丢失原计划相位

定时器迟到时按 `now + interval` 重置，会让延迟累积成长期漂移；回调耗时也会进一步改变后续相位。这不满足设计中重复定时器保留 planned phase 的要求。

### F-06 Medium：P1-06 尚不是完整的异步结构化日志

队列只保存 `(level, string)`，输出中没有结构化 context；消费端逐条写并使用 flush 型输出，没有批量/double-buffer 设计；也没有同步与异步模式的对比 benchmark。当前实现验证了队列与级别行为，但没有完成 P1-06 的整体契约。

## 4. 补充代码核对

这些问题用于补全下一轮整改范围，不改变上述两条审查轴的原始计数。

### A-01 默认 `TimerId` 可触发空指针

`TimerQueue::cancel(TimerId id)` 在使用 `id.timer->expiration()` 前没有处理空 timer。公开 API 若收到默认构造的 `TimerId{}` 会崩溃。整改时应明确“无效/已取消 TimerId 为幂等 no-op”的接口契约，并先加入回归测试。

### A-02 协议错误只 half-close 写端

`ChatServer` 在 codec 报协议错误时调用 `shutdown()`；该操作只关闭写方向并继续读。恶意客户端可以继续占用连接并发送非法数据。协议错误应走 loop-affine 的强制关闭路径，并有进程级/`socketpair` 测试证明连接最终释放。

### A-03 测试没有统一链接生产库目标

多个测试目标直接重新编译生产 `.cc`，而生产库与测试目标的 C++ 标准/编译选项并不完全相同。这会产生“测试通过的对象代码不等于最终链接进 ChatServer 的对象代码”的验证偏差。应让契约测试优先链接真实 `mymuduo` target，仅对需要替身的边界使用显式 adapter。

## 5. 本轮复验记录

全部命令均从 Windows 调用 WSL2 Ubuntu，并使用 `/tmp` 下的全新构建目录，没有复用编码代理的既有 build tree。

| 配置 | 构建目录 | 结果 |
|---|---|---|
| Debug | `/tmp/muduo-chat-review-00db852-debug` | build 成功；CTest 78/78 |
| ASan+UBSan | `/tmp/muduo-chat-review-00db852-asan` | build 成功；CTest 78/78 |
| TSan | `/tmp/muduo-chat-review-00db852-tsan` | build 成功；CTest 78/78 |
| Release, tests off | `/tmp/muduo-chat-review-00db852-prod` | `ChatClient`、`chat-bench`、`libmymuduo.so`、`ChatServer` 构建成功 |

本轮没有把 MySQL 真实业务流程、fd exhaustion、信号风暴、worker-loop drain、reconnect storm 或慢 DB 路径称为已验证；这些路径需要 P1R/P2 的新测试证据。

## 6. 审查汇总

- Standards：7 项；最严重问题是 signal handler 的写端可能阻塞。
- Spec：6 项；最严重问题是发送准入不真正有界、跨 EventLoop 强制关闭和过载保护范围降级。
- 接受条件：完成 `POST_P1_IMPLEMENTATION_PLAN.md` 的 P1R-00 至 P1R-09，并重新执行独立完成性审查。
