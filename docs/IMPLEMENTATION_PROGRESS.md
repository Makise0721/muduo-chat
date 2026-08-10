# IMPLEMENTATION_PROGRESS

状态索引：只有经过验证的完成项才能标记完成，且必须附命令与证据。

| 任务 | 状态 | 证据（命令/结果） |
|---|---|---|
| P0-00 构建产物隔离 | VERIFIED | `cmake --build /tmp/muduo-chat-build/p0-00` exit 0；产物在 binary tree（bin/ChatServer、bin/ChatClient、lib/libmymuduo.so）；增量 build exit 0；`git diff --check` exit 0；已提交 63a76ed |
| P0-01 CMake/CTest 测试骨架 | VERIFIED | `ctest --test-dir /tmp/muduo-chat-build/p0-01` 6/6 通过；`ctest -N` = 6 tests；`-DENABLE_TESTS=OFF` build exit 0；无需 MySQL；已提交 3274bca |
| P0-02A Buffer contract | VERIFIED | `ctest -R BufferTest` 11/11 通过；全量 11/11；生产源码零改动；已提交 18a3b20 |
| P0-02B EventLoop contract | VERIFIED | `ctest -R EventLoopTest` 4/4 通过；全量 15/15；生产源码零改动；已提交 88a74c6 |
| P0-02C EventLoopThreadPool contract | VERIFIED | `ctest -R EventLoopThreadPoolTest` 3/3 通过；全量 18/18；生产源码零改动；已提交 a9d8f76 |
| P0-02D TcpConnection characterization | VERIFIED | `ctest -R TcpConnectionTest` 3/3 通过；全量 21/21；生产源码零改动；已提交 12a0ef1 |
| P0-03 跨线程 send/close 生命周期修复 | VERIFIED | ASan 聚焦 6/6 ×5 轮稳定；ASan 全量 24/24；Debug 全量 24/24；10000 交错无报告；已提交 1d3fd84；TSan 结论见任务卡（3 项失败归因在案 Logger/fd 竞态） |
| P0-03A EventLoop loop-start quit 竞态修复 | VERIFIED | 32s 超时消除（-v 日志证据链）；Debug/ASan 28/28（重复注册修复后唯一计数）；TSan 遗留 fd 理论竞态在案；已提交 3bf4b84 |
| P0-04 可复现 benchmark 基线 | VERIFIED | `ctest -R BenchStatsTest` 4/4；全量 28/28（唯一计数）；OFF 构建 exit 0；三场景 smoke 通过（connect 8/8、echo 800/800、slow-consumer 1.6MB 一致）；已提交 b9880e4 + b3fde51（审查修复）；performance-reports/b3fde51.md |

## 对抗性审查记录（2026-08-09，code-review skill 双轴子代理）

发现并修复：
- `tests/CMakeLists.txt` gtest_discover_tests(BenchStatsTest) 重复注册 → CTest 虚增 4 个测试（此前"32/32"计数含重复）；已删重复（4e6b8d7），唯一计数 28
- chat-bench slow-consumer 把字节塞入 messages_*/connections_failed（schema 语义漂移）；新增 bytes_sent/bytes_received/early_closes 独立字段 + `--duration-ms` 真正生效（b3fde51）
- docs/performance-reports/<commit>.md 缺失；已生成首份（b3fde51.md）

记录在案（不修复/待处理）：
- 63a76ed 混入 EVOLUTION_PLAN.md/IMPLEMENTATION_SOP.md（P0-00 允许写集合外，历史提交不回写）
- b91c9c2 无任务卡（TSan harness 清理，事后记录于此）
- 8KB payload 断言未覆盖 partial-write 路径（判断项；UAF 检测与 payload 大小无关）
- libFuzzer 未跑（WSL 无 Clang，CI 前置项；P1-01 以随机输入风暴替代）

| P1-01 StreamCodec 与 v2 framing | VERIFIED | `ctest -R StreamCodecTest` 15/15；Debug 全量 43/43；ASan 43/43；golden bytes 固化；10000 轮随机风暴无报告；已提交 cb8ba00 |
| P1-02 双协议迁移 | VERIFIED | `ctest -R 'LegacyJsonLineCodec|BinaryFrameCodec|OutputEncoder'` 13/13；Debug 全量 56/56；ASan 56/56；进程级双端口 smoke OK（注册/登录等价）；ChatService 零改动；已提交 6334546 |
| P1-03 TimerQueue | VERIFIED | `ctest -R TimerQueueTest` 8/8；Debug 全量 64/64；ASan 64/64；无漂移断言通过；测试不读私有容器；已提交 c4be285 |
| P1-04 背压状态机 | VERIFIED | `ctest -R BackpressureTest` 4/4 ×5 轮；Debug 全量 68/68；ASan 68/68（含 LSan）；slow-consumer smoke 通过；SendResult 四态 + stall 断开断言；已提交 51f9b66 |
| P1-05 过载保护与优雅退出 | VERIFIED | `ctest -R TcpServerTest` 1/1；Debug 69/69；ASan 69/69；进程级 SIGINT/SIGTERM 双路径 smoke（DRAINED 快路径 + 5s 硬截止 pending=1 打印）；已提交 6fcdf3c + 8c3edf9 |
| P1-06 异步结构化日志 | VERIFIED | `ctest -R LoggerTest` 4/4；Debug 73/73；ASan 73/73；TSan LoggerTest 干净（在案 #4 关闭）；已提交 3c25557 |
| P1-06A EventLoop/TimerQueue fd 生命周期竞态修复 | VERIFIED | **TSan 全量 73/73 ×2 轮全绿**（全部在案竞态关闭）；Debug/ASan 73/73；已提交 6f29a1c |
| P1-07 对抗审查修复批次 | VERIFIED | Debug/ASan 78/78；TSan 78/78 ×2 无 WARNING；**五轮双轴对抗审查闭环通过**（TimerQueue 竞争、Logger 级别语义、FATAL 不可丢、hard 关闭路径、Closed 断言、acceptErrorCount、幂等退出）；已提交 00db852 |

> **P1 整体状态：`REMEDIATION`**（2026-08-10 完成性审查判不通过，见
> [P1_COMPLETION_REVIEW_00db852.md](P1_COMPLETION_REVIEW_00db852.md)；
> 整改计划 [POST_P1_IMPLEMENTATION_PLAN.md](POST_P1_IMPLEMENTATION_PLAN.md)，
> 执行 P1R-00→P1R-09 后重新验收。上表各 P1 任务的历史验证记录保持有效，
> 不代表 P1 整体完成性验收通过。）

| P1R-00 恢复事实、任务状态与审计边界 | VERIFIED | `git diff --check` exit 0；状态唯一（P1R-00 唯一 IN_PROGRESS→VERIFIED）；SOP 构建路径/TSan setarch/CLAUDE.md 同步；已提交 0156395 |
| P1R-01 测试链接真实生产目标 | VERIFIED | 生产 `.cc` 由 mymuduo target 编译一次（compile_commands 断言 21 源单条目，RED 捕获 Buffer.cc×8）；Debug/ASan/TSan 79/79 ×TSan 两轮无 WARNING；Release OFF 构建成功；已提交 8e2673f |
| P1R-02 Codec/编码结果契约 | VERIFIED | EncodeResult + OutputCodec（encodedSize/encode）；encode 全字段自校验（bodyLength==body.size、16MiB 上限、失败不改 output）；decode 拒非 JSON contentType；协议错误 force-close；Debug/ASan/TSan 86/86 ×TSan 两轮无 WARNING；进程 smoke ALL_PASS（坏帧 EOF + v1/v2 注册登录等价）；已提交 f76b750 |
| P1R-03 有界发送准入 | VERIFIED | SendOutcome{disposition,pressure} 无歧义语义（AcceptedPaused/WouldBlock 区分）；连接级原子预算 CAS 准入（任意线程）；低水位恢复回调一次；空消息 1B 帧开销不绕过；stall timer weak_ptr 断环（LSan）；Debug/ASan/TSan 89/89 ×TSan 两轮无 WARNING；进程 smoke ALL_PASS；已提交 b325472 |
| P1R-04 loop-affine 关闭 | VERIFIED | forceClose() 任意线程安全（queueInLoop 调度到所属 loop + shared_from_this）；forceCloseInLoop 私有；跨线程重复 forceClose 幂等、回调一次；threadNum=2 + 3 连接 forceCloseAll 无 TSan 报告；Debug/ASan/TSan 91/91；SIGTERM 挂连接退出 smoke PASS；已提交 1e8100a |
| P1R-05 信号与退出 | VERIFIED | self-pipe 两端 O_NONBLOCK|FD_CLOEXEC（S-01 关闭）；handler async-signal-safe 无阻塞路径；进程 CTest（信号风暴 30× → 退出码 0 + DRAINED 恰一次幂等；TSan setarch + libcrypto suppression）；Debug/ASan/TSan 92/92；已提交 c9d889b |
| P1R-06 过载与 EMFILE | VERIFIED | accept token bucket（rate/burst 独立计数 rateRejected）+ maxConnections 独立计数；EMFILE idle-fd 恢复（/dev/null 借还 + 退避）；进程 CTest fd-exhaustion（CONNECTED=123 真耗尽 → RECOVERED=5）；reconnect storm 30× 稳定连接登录 PASS；Debug/ASan/TSan 96/96；已提交 5b3e27f |
| P1R-07 Timer 契约 | VERIFIED | cancel(TimerId{})/已取消/已触发均幂等 no-op（A-01）；重复 timer planned phase 保持 + 跳周期不漂移（F-05，120ms 回调 × 50ms interval 600ms 内 ≤5）；Debug/ASan/TSan 100/100 ×TSan 两轮；TimerQueue 3×5 轮稳定；已提交 62f36be |
| P1R-08 结构化日志 | VERIFIED | LogEvent 值对象（timestamp/level/threadId/component/eventName/message 入队前固定）；批量取队列（≤64）+ '\n' 移除逐条 flush（F-06）；test recorder sink 断言字段/顺序/线程不串扰；logger-bench + 报告 97c0311（async p50≈250ns、dropped 可查、无未解释回退）；Debug/ASan/TSan 103/103；已提交 97c0311 |
