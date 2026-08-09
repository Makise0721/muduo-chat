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

| P1-01 StreamCodec 与 v2 framing | VERIFIED | `ctest -R StreamCodecTest` 15/15；Debug 全量 43/43；ASan 43/43；golden bytes 固化；10000 轮随机风暴无报告；未提交（等待授权） |
