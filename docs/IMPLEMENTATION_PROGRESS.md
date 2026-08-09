# IMPLEMENTATION_PROGRESS

状态索引：只有经过验证的完成项才能标记完成，且必须附命令与证据。

| 任务 | 状态 | 证据（命令/结果） |
|---|---|---|
| P0-00 构建产物隔离 | VERIFIED | `cmake --build /tmp/muduo-chat-build/p0-00` exit 0；产物在 binary tree（bin/ChatServer、bin/ChatClient、lib/libmymuduo.so）；增量 build exit 0；`git diff --check` exit 0；已提交 63a76ed |
| P0-01 CMake/CTest 测试骨架 | VERIFIED | `ctest --test-dir /tmp/muduo-chat-build/p0-01` 6/6 通过；`ctest -N` = 6 tests；`-DENABLE_TESTS=OFF` build exit 0；无需 MySQL；已提交 3274bca |
| P0-02A Buffer contract | VERIFIED | `ctest -R BufferTest` 11/11 通过；全量 11/11；生产源码零改动；已提交 18a3b20 |
| P0-02B EventLoop contract | VERIFIED | `ctest -R EventLoopTest` 4/4 通过；全量 15/15；生产源码零改动；已提交 88a74c6 |
| P0-02C EventLoopThreadPool contract | VERIFIED | `ctest -R EventLoopThreadPoolTest` 3/3 通过；全量 18/18；生产源码零改动；已提交 a9d8f76 |
| P0-02D TcpConnection characterization | VERIFIED | `ctest -R TcpConnectionTest` 3/3 通过；全量 21/21；生产源码零改动；未提交（等待授权） |
