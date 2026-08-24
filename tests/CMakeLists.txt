find_package(GTest CONFIG REQUIRED)

add_executable(BufferTest unit/BufferTest.cpp)
target_include_directories(BufferTest PRIVATE ${PROJECT_SOURCE_DIR}/mymuduo)
target_link_libraries(BufferTest GTest::gtest_main GTest::gtest mymuduo Threads::Threads)
set_target_properties(BufferTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

include(GoogleTest)

# gtest 测试统一注入 TSAN_OPTIONS（libmysqlclient 经 libcrypto 维护全局分配器
# 记账，TSan 会误报为单线程 race，与 signal/fd/domain 脚本同一 suppression）。
# 注：gtest_discover_tests 的 PROPERTIES 参数在 CMake 3.28 模块中未写入生成文件，
# 故经 CROSSCOMPILING_EMULATOR 用 env 注入；非 TSan 树无害。
set(GTEST_EMULATOR
    "/usr/bin/env;TSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/scripts/tsan.supp;setarch;x86_64;-R")

function(discover_gtest TARGET)
    gtest_discover_tests(${TARGET})
endfunction()

add_executable(EventLoopTest unit/EventLoopTest.cpp)
target_include_directories(EventLoopTest PRIVATE ${PROJECT_SOURCE_DIR}/mymuduo)
target_link_libraries(EventLoopTest GTest::gtest_main GTest::gtest mymuduo Threads::Threads)
set_target_properties(EventLoopTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

add_executable(EventLoopThreadPoolTest unit/EventLoopThreadPoolTest.cpp)
target_include_directories(EventLoopThreadPoolTest PRIVATE ${PROJECT_SOURCE_DIR}/mymuduo)
target_link_libraries(EventLoopThreadPoolTest GTest::gtest_main GTest::gtest mymuduo Threads::Threads)
set_target_properties(EventLoopThreadPoolTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

add_executable(TcpConnectionTest unit/TcpConnectionTest.cpp)
target_include_directories(TcpConnectionTest PRIVATE ${PROJECT_SOURCE_DIR}/mymuduo)
target_link_libraries(TcpConnectionTest GTest::gtest_main GTest::gtest mymuduo Threads::Threads)
set_target_properties(TcpConnectionTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

add_executable(BenchStatsTest unit/BenchStatsTest.cpp)
target_include_directories(BenchStatsTest PRIVATE
    ${PROJECT_SOURCE_DIR}/thirdparty
    ${PROJECT_SOURCE_DIR}/tools/chat-bench)
target_link_libraries(BenchStatsTest GTest::gtest_main GTest::gtest)
set_target_properties(BenchStatsTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

add_executable(StreamCodecTest unit/StreamCodecTest.cpp)
target_include_directories(StreamCodecTest PRIVATE ${PROJECT_SOURCE_DIR}/mymuduo)
target_link_libraries(StreamCodecTest GTest::gtest_main GTest::gtest mymuduo Threads::Threads)
set_target_properties(StreamCodecTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

add_executable(LegacyJsonLineCodecTest unit/LegacyJsonLineCodecTest.cpp)
target_include_directories(LegacyJsonLineCodecTest PRIVATE ${PROJECT_SOURCE_DIR}/mymuduo)
target_link_libraries(LegacyJsonLineCodecTest GTest::gtest_main GTest::gtest mymuduo Threads::Threads)
set_target_properties(LegacyJsonLineCodecTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

add_executable(BinaryFrameCodecTest unit/BinaryFrameCodecTest.cpp)
target_include_directories(BinaryFrameCodecTest PRIVATE ${PROJECT_SOURCE_DIR}/mymuduo)
target_link_libraries(BinaryFrameCodecTest GTest::gtest_main GTest::gtest mymuduo Threads::Threads)
set_target_properties(BinaryFrameCodecTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

add_executable(TimerQueueTest unit/TimerQueueTest.cpp)
target_include_directories(TimerQueueTest PRIVATE ${PROJECT_SOURCE_DIR}/mymuduo)
target_link_libraries(TimerQueueTest GTest::gtest_main GTest::gtest mymuduo Threads::Threads)
set_target_properties(TimerQueueTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

add_executable(BackpressureTest unit/BackpressureTest.cpp)
target_include_directories(BackpressureTest PRIVATE ${PROJECT_SOURCE_DIR}/mymuduo)
target_link_libraries(BackpressureTest GTest::gtest_main GTest::gtest mymuduo Threads::Threads)
set_target_properties(BackpressureTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

add_executable(TcpServerTest unit/TcpServerTest.cpp)
target_include_directories(TcpServerTest PRIVATE ${PROJECT_SOURCE_DIR}/mymuduo)
target_link_libraries(TcpServerTest GTest::gtest_main GTest::gtest mymuduo Threads::Threads)
set_target_properties(TcpServerTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

add_executable(LoggerTest unit/LoggerTest.cpp)
target_include_directories(LoggerTest PRIVATE ${PROJECT_SOURCE_DIR}/mymuduo)
target_link_libraries(LoggerTest GTest::gtest_main GTest::gtest mymuduo Threads::Threads)
set_target_properties(LoggerTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

add_executable(DualProtocolCharacterizationTest unit/DualProtocolCharacterizationTest.cpp)
target_include_directories(DualProtocolCharacterizationTest PRIVATE ${PROJECT_SOURCE_DIR}/mymuduo)
target_link_libraries(DualProtocolCharacterizationTest GTest::gtest_main GTest::gtest mymuduo Threads::Threads)
set_target_properties(DualProtocolCharacterizationTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

add_executable(ChatApplicationRegistrationTest unit/ChatApplicationRegistrationTest.cpp)
target_link_libraries(ChatApplicationRegistrationTest GTest::gtest_main GTest::gtest chatserver_core Threads::Threads)
set_target_properties(ChatApplicationRegistrationTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

add_executable(UserRepositoryContractTest unit/UserRepositoryContractTest.cpp)
target_link_libraries(UserRepositoryContractTest GTest::gtest_main GTest::gtest chatserver_core Threads::Threads)
set_target_properties(UserRepositoryContractTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

add_executable(MySQLUserRepositoryTest unit/MySQLUserRepositoryTest.cpp)
target_link_libraries(MySQLUserRepositoryTest GTest::gtest_main GTest::gtest chatserver_core Threads::Threads)
set_target_properties(MySQLUserRepositoryTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

add_executable(BlockingExecutorTest unit/BlockingExecutorTest.cpp)
target_include_directories(BlockingExecutorTest PRIVATE ${PROJECT_SOURCE_DIR}/mymuduo)
target_link_libraries(BlockingExecutorTest GTest::gtest_main GTest::gtest chatserver_core mymuduo Threads::Threads)
set_target_properties(BlockingExecutorTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

# P3-12 RED：可靠消息指标 recorder 单元测试（引用尚不存在的
# app/ReliableMessageMetrics.hpp → 当前 RED 编译失败即合法 RED）。
add_executable(ReliableMessageMetricsTest unit/ReliableMessageMetricsTest.cpp)
target_include_directories(ReliableMessageMetricsTest PRIVATE ${PROJECT_SOURCE_DIR}/mymuduo)
target_link_libraries(ReliableMessageMetricsTest GTest::gtest_main GTest::gtest chatserver_core mymuduo Threads::Threads)
set_target_properties(ReliableMessageMetricsTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

add_executable(SessionSerialExecutorTest unit/SessionSerialExecutorTest.cpp)
target_include_directories(SessionSerialExecutorTest PRIVATE ${PROJECT_SOURCE_DIR}/mymuduo)
target_link_libraries(SessionSerialExecutorTest GTest::gtest_main GTest::gtest chatserver_core mymuduo Threads::Threads)
set_target_properties(SessionSerialExecutorTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

add_executable(EventLoopResponsivenessTest unit/EventLoopResponsivenessTest.cpp)
target_include_directories(EventLoopResponsivenessTest PRIVATE ${PROJECT_SOURCE_DIR}/mymuduo)
target_link_libraries(EventLoopResponsivenessTest GTest::gtest_main GTest::gtest chatserver_core mymuduo Threads::Threads)
set_target_properties(EventLoopResponsivenessTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

add_executable(ConnectionPoolTest unit/ConnectionPoolTest.cpp)
target_link_libraries(ConnectionPoolTest GTest::gtest_main GTest::gtest chatserver_core Threads::Threads)
set_target_properties(ConnectionPoolTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

add_executable(MySQLPoolIntegrationTest unit/MySQLPoolIntegrationTest.cpp)
target_link_libraries(MySQLPoolIntegrationTest GTest::gtest_main GTest::gtest chatserver_core Threads::Threads)
set_target_properties(MySQLPoolIntegrationTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

add_executable(SessionApplicationTest unit/SessionApplicationTest.cpp)
target_include_directories(SessionApplicationTest PRIVATE ${PROJECT_SOURCE_DIR}/mymuduo)
target_link_libraries(SessionApplicationTest GTest::gtest_main GTest::gtest chatserver_core mymuduo Threads::Threads)
set_target_properties(SessionApplicationTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

add_executable(SessionRegistryTest unit/SessionRegistryTest.cpp)
target_include_directories(SessionRegistryTest PRIVATE ${PROJECT_SOURCE_DIR}/mymuduo)
target_link_libraries(SessionRegistryTest GTest::gtest_main GTest::gtest chatserver_core mymuduo Threads::Threads)
set_target_properties(SessionRegistryTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

add_executable(FriendRepositoryContractTest unit/FriendRepositoryContractTest.cpp)
target_link_libraries(FriendRepositoryContractTest GTest::gtest_main GTest::gtest chatserver_core Threads::Threads)
set_target_properties(FriendRepositoryContractTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

add_executable(GroupRepositoryContractTest unit/GroupRepositoryContractTest.cpp)
target_link_libraries(GroupRepositoryContractTest GTest::gtest_main GTest::gtest chatserver_core Threads::Threads)
set_target_properties(GroupRepositoryContractTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

add_executable(FriendGroupApplicationTest unit/FriendGroupApplicationTest.cpp)
target_link_libraries(FriendGroupApplicationTest GTest::gtest_main GTest::gtest chatserver_core Threads::Threads)
set_target_properties(FriendGroupApplicationTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

add_executable(OfflineMessageTest unit/OfflineMessageTest.cpp)
target_link_libraries(OfflineMessageTest GTest::gtest_main GTest::gtest chatserver_core Threads::Threads)
set_target_properties(OfflineMessageTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

add_executable(MessageApplicationTest unit/MessageApplicationTest.cpp)
target_link_libraries(MessageApplicationTest GTest::gtest_main GTest::gtest chatserver_core Threads::Threads)
set_target_properties(MessageApplicationTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

add_executable(MessageAcceptanceConcurrencyTest unit/MessageAcceptanceConcurrencyTest.cpp)
target_link_libraries(MessageAcceptanceConcurrencyTest GTest::gtest_main GTest::gtest chatserver_core Threads::Threads)
set_target_properties(MessageAcceptanceConcurrencyTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

add_executable(MultiReactorTest unit/MultiReactorTest.cpp)
target_include_directories(MultiReactorTest PRIVATE ${PROJECT_SOURCE_DIR}/mymuduo)
target_link_libraries(MultiReactorTest GTest::gtest_main GTest::gtest mymuduo Threads::Threads)
set_target_properties(MultiReactorTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

add_executable(ConfigTest unit/ConfigTest.cpp)
target_link_libraries(ConfigTest GTest::gtest_main GTest::gtest chatserver_core Threads::Threads)
set_target_properties(ConfigTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

add_executable(SchemaMigrationTest unit/SchemaMigrationTest.cpp)
target_link_libraries(SchemaMigrationTest GTest::gtest_main GTest::gtest chatserver_core Threads::Threads)
set_target_properties(SchemaMigrationTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

add_executable(ReliableMessagingContractTest unit/ReliableMessagingContractTest.cpp)
target_link_libraries(ReliableMessagingContractTest GTest::gtest_main GTest::gtest chatserver_core Threads::Threads)
set_target_properties(ReliableMessagingContractTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

add_executable(ReliableMessageSchemaTest unit/ReliableMessageSchemaTest.cpp)
target_link_libraries(ReliableMessageSchemaTest GTest::gtest_main GTest::gtest chatserver_core Threads::Threads)
set_target_properties(ReliableMessageSchemaTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

add_executable(MySQLReliableMessagingTest unit/MySQLReliableMessagingTest.cpp unit/ReliableMessagingContractTest.cpp)
target_link_libraries(MySQLReliableMessagingTest GTest::gtest_main GTest::gtest chatserver_core Threads::Threads)
set_target_properties(MySQLReliableMessagingTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

add_executable(MessageAcceptanceApplicationTest unit/MessageAcceptanceApplicationTest.cpp)
target_link_libraries(MessageAcceptanceApplicationTest GTest::gtest_main GTest::gtest chatserver_core Threads::Threads)
set_target_properties(MessageAcceptanceApplicationTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

add_executable(ReliableProtocolGoldenTest unit/ReliableProtocolGoldenTest.cpp)
target_include_directories(ReliableProtocolGoldenTest PRIVATE ${PROJECT_SOURCE_DIR}/mymuduo)
target_link_libraries(ReliableProtocolGoldenTest GTest::gtest_main GTest::gtest chatserver_core Threads::Threads)
set_target_properties(ReliableProtocolGoldenTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

add_executable(DeliveryCoordinatorTest unit/DeliveryCoordinatorTest.cpp)
target_link_libraries(DeliveryCoordinatorTest GTest::gtest_main GTest::gtest chatserver_core Threads::Threads)
set_target_properties(DeliveryCoordinatorTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

add_executable(DeliveryRetryTest unit/DeliveryRetryTest.cpp)
target_link_libraries(DeliveryRetryTest GTest::gtest_main GTest::gtest chatserver_core Threads::Threads)
set_target_properties(DeliveryRetryTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

add_executable(OutboxRelayTest unit/OutboxRelayTest.cpp)
target_link_libraries(OutboxRelayTest GTest::gtest_main GTest::gtest chatserver_core Threads::Threads)
set_target_properties(OutboxRelayTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

add_executable(OutboxCrashRecoveryTest unit/OutboxCrashRecoveryTest.cpp)
target_link_libraries(OutboxCrashRecoveryTest GTest::gtest_main GTest::gtest chatserver_core Threads::Threads)
set_target_properties(OutboxCrashRecoveryTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

# P4-03 RED：OutboxPublisher port 与 Recording/Kafka 双 adapter 契约（引用尚不存在的
# app/OutboxPublisher.hpp、app/RecordingPublisher.hpp、app/KafkaPublisher.hpp →
# 编译失败即合法 RED）。真实 Kafka 集成，不 skip。
add_executable(OutboxPublisherContractTest unit/OutboxPublisherContractTest.cpp)
target_link_libraries(OutboxPublisherContractTest GTest::gtest_main GTest::gtest chatserver_core Threads::Threads)
set_target_properties(OutboxPublisherContractTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

add_executable(MessageRetentionTest unit/MessageRetentionTest.cpp)
target_link_libraries(MessageRetentionTest GTest::gtest_main GTest::gtest chatserver_core Threads::Threads)
set_target_properties(MessageRetentionTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

add_executable(MySqlLeaseRoundTripTest unit/MySqlLeaseRoundTripTest.cpp)
target_link_libraries(MySqlLeaseRoundTripTest GTest::gtest_main GTest::gtest chatserver_core Threads::Threads)
set_target_properties(MySqlLeaseRoundTripTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

add_executable(LegacyOfflineMigrationTest unit/LegacyOfflineMigrationTest.cpp)
target_link_libraries(LegacyOfflineMigrationTest GTest::gtest_main GTest::gtest chatserver_core Threads::Threads)
set_target_properties(LegacyOfflineMigrationTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

add_executable(BackfillReentrancyTest unit/BackfillReentrancyTest.cpp)
target_link_libraries(BackfillReentrancyTest GTest::gtest_main GTest::gtest chatserver_core Threads::Threads)
set_target_properties(BackfillReentrancyTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

add_executable(LegacyCutoverTest unit/LegacyCutoverTest.cpp)
target_link_libraries(LegacyCutoverTest GTest::gtest_main GTest::gtest chatserver_core Threads::Threads)
set_target_properties(LegacyCutoverTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

add_executable(SessionDeliverySinkTest
    unit/SessionDeliverySinkTest.cpp
    ${PROJECT_SOURCE_DIR}/chatserver/src/SessionDeliverySink.cpp)
target_include_directories(SessionDeliverySinkTest PRIVATE ${PROJECT_SOURCE_DIR}/mymuduo)
target_link_libraries(SessionDeliverySinkTest GTest::gtest_main GTest::gtest chatserver_core mymuduo Threads::Threads)
set_target_properties(SessionDeliverySinkTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

# P4-01 RED：PresenceDirectory 领域 interface in-memory contract（引用尚不存在的
# app/PresenceDirectory.hpp → 编译失败即合法 RED）。
add_executable(PresenceContractTest unit/PresenceContractTest.cpp)
target_link_libraries(PresenceContractTest GTest::gtest_main GTest::gtest chatserver_core Threads::Threads)
set_target_properties(PresenceContractTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

# P4-02 RED：RedisPresenceDirectory 真实 Redis contract + 故障测试（引用尚不存在的
# app/RedisPresenceDirectory.hpp 与 app/RedisConn.hpp → 编译失败即合法 RED）。
add_executable(RedisPresenceContractTest unit/RedisPresenceContractTest.cpp)
target_link_libraries(RedisPresenceContractTest GTest::gtest_main GTest::gtest chatserver_core Threads::Threads)
set_target_properties(RedisPresenceContractTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

# P4-05 RED：Gateway 定向投递与 epoch 校验契约（引用尚不存在的
# app/GatewayTransport.hpp、app/InProcessGatewayTransport.hpp、
# app/RecordingGatewayTransport.hpp 与 app/GatewayAwareDeliverySink.hpp →
# 编译失败即合法 RED）。
add_executable(GatewayDeliveryContractTest unit/GatewayDeliveryContractTest.cpp)
target_link_libraries(GatewayDeliveryContractTest GTest::gtest_main GTest::gtest chatserver_core Threads::Threads)
set_target_properties(GatewayDeliveryContractTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

# P4-05 RED：Presence 生产接线契约（引用尚不存在的 app/GatewayTransport.hpp 与
# app/GatewayAwareDeliverySink.hpp → 编译失败即合法 RED）。真实 Redis 集成，不 skip。
add_executable(GatewayPresenceWiringContractTest unit/GatewayPresenceWiringContractTest.cpp)
target_link_libraries(GatewayPresenceWiringContractTest GTest::gtest_main GTest::gtest chatserver_core Threads::Threads)
set_target_properties(GatewayPresenceWiringContractTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

# P5-00 阶段 A RED：统一 Telemetry interface 与低基数预算契约（引用尚不存在的
# app/Telemetry.hpp 与 app/RecordingTelemetry.hpp → 编译失败即合法 RED）。
add_executable(TelemetryContractTest unit/TelemetryContractTest.cpp)
target_link_libraries(TelemetryContractTest GTest::gtest_main GTest::gtest chatserver_core Threads::Threads)
set_target_properties(TelemetryContractTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

# P5-00 阶段 B RED：统一快照同源生成（引用尚不存在的 app/MetricsSnapshot.hpp →
# 编译失败即合法 RED，沿阶段 A 先例）。
add_executable(MetricsSnapshotTest unit/MetricsSnapshotTest.cpp)
target_link_libraries(MetricsSnapshotTest GTest::gtest_main GTest::gtest chatserver_core mymuduo Threads::Threads)
set_target_properties(MetricsSnapshotTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

# P5-00 阶段 B RED：Prometheus 导出面与 /metrics 端点（引用尚不存在的
# app/PrometheusTelemetrySink.hpp 与 app/MetricsEndpoint.hpp → 编译失败即合法 RED；
# 端点 smoke 经 mymuduo EventLoop/TcpServer 随机端口）。
add_executable(PrometheusEndpointTest unit/PrometheusEndpointTest.cpp)
target_include_directories(PrometheusEndpointTest PRIVATE ${PROJECT_SOURCE_DIR}/mymuduo)
target_link_libraries(PrometheusEndpointTest GTest::gtest_main GTest::gtest chatserver_core mymuduo Threads::Threads)
set_target_properties(PrometheusEndpointTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

# P5-00 阶段 B RED：缺口接线 getter 存在性 + 零起始（引用尚不存在的
# EventLoop::loopLagProbeMs / TcpConnection::outstandingBytes /
# TcpServer::acceptReasonCounts / RedisPresenceDirectory::fencingConflicts /
# KafkaEventConsumer::consumerLag/rebalanceCount → member not declared 编译失败
# 即合法 RED）。
add_executable(GapWiringContractTest unit/GapWiringContractTest.cpp)
target_include_directories(GapWiringContractTest PRIVATE ${PROJECT_SOURCE_DIR}/mymuduo)
target_link_libraries(GapWiringContractTest GTest::gtest_main GTest::gtest chatserver_core mymuduo Threads::Threads)
set_target_properties(GapWiringContractTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

find_program(PYTHON3_EXECUTABLE python3 REQUIRED)
add_test(NAME single-compile-check
    COMMAND ${PYTHON3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_single_compile.py
        ${CMAKE_BINARY_DIR}/compile_commands.json)

find_program(BASH_EXECUTABLE bash REQUIRED)
# 进程测试共享端口（v1 各异但 v2 恒为 7000）：RESOURCE_LOCK chat_server_ports
# 防止 ctest -j 并行互杀。
add_test(NAME process-signal-shutdown
    COMMAND ${BASH_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/signal_shutdown_test.sh
        $<TARGET_FILE:ChatServer>
        20)
set_tests_properties(process-signal-shutdown PROPERTIES TIMEOUT 60 RESOURCE_LOCK chat_server_ports)

add_test(NAME process-fd-exhaustion
    COMMAND ${BASH_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/fd_exhaustion_test.sh
        $<TARGET_FILE:ChatServer>)
set_tests_properties(process-fd-exhaustion PROPERTIES TIMEOUT 90 RESOURCE_LOCK chat_server_ports)

add_test(NAME DomainCharacterization
    COMMAND ${BASH_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/domain_characterization_test.sh
        $<TARGET_FILE:ChatServer>)
set_tests_properties(DomainCharacterization PROPERTIES TIMEOUT 180 RESOURCE_LOCK chat_server_ports)

add_test(NAME MultiReactorProcess
    COMMAND ${BASH_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/multireactor_test.sh
        $<TARGET_FILE:ChatServer>)
set_tests_properties(MultiReactorProcess PROPERTIES TIMEOUT 360 RESOURCE_LOCK chat_server_ports)

add_test(NAME DeliveryAckProcess
    COMMAND ${BASH_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/delivery_ack_test.sh
        $<TARGET_FILE:ChatServer>)
set_tests_properties(DeliveryAckProcess PROPERTIES TIMEOUT 360 RESOURCE_LOCK chat_server_ports)

add_test(NAME BackpressureDeliveryProcess
    COMMAND ${BASH_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/backpressure_delivery_test.sh
        $<TARGET_FILE:ChatServer>)
set_tests_properties(BackpressureDeliveryProcess PROPERTIES TIMEOUT 600 RESOURCE_LOCK chat_server_ports)

add_test(NAME shutdown-order
    COMMAND ${BASH_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/shutdown_order_test.sh
        $<TARGET_FILE:ChatServer>)
set_tests_properties(shutdown-order PROPERTIES TIMEOUT 90 RESOURCE_LOCK chat_server_ports)

add_test(NAME config-fail-fast
    COMMAND ${BASH_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/config_fail_test.sh
        $<TARGET_FILE:ChatServer>)
set_tests_properties(config-fail-fast PROPERTIES TIMEOUT 90 RESOURCE_LOCK chat_server_ports)

add_test(NAME server-metrics
    COMMAND ${BASH_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/metrics_test.sh
        $<TARGET_FILE:ChatServer>)
set_tests_properties(server-metrics PROPERTIES TIMEOUT 60 RESOURCE_LOCK chat_server_ports)

add_test(NAME ReconnectReplayProcess
    COMMAND ${BASH_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/reconnect_replay_test.sh
        $<TARGET_FILE:ChatServer>)
set_tests_properties(ReconnectReplayProcess PROPERTIES TIMEOUT 180 RESOURCE_LOCK chat_server_ports)

add_test(NAME OutboxCrashRecoveryProcess
    COMMAND ${BASH_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/outbox_crash_recovery_test.sh
        $<TARGET_FILE:ChatServer>)
set_tests_properties(OutboxCrashRecoveryProcess PROPERTIES TIMEOUT 240 RESOURCE_LOCK chat_server_ports)

add_test(NAME LegacyCutoverRehearsalProcess
    COMMAND ${BASH_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/legacy_cutover_rehearsal_test.sh
        $<TARGET_FILE:ChatServer>
        $<TARGET_FILE:backfill>)
set_tests_properties(LegacyCutoverRehearsalProcess PROPERTIES TIMEOUT 240 RESOURCE_LOCK chat_server_ports)

# P3-12 RED：10 kill 点 × 三层断言（客户端可见结果 + DB 状态 + reliable_* 指标
# 可定位）。reliable_* 尚不存在 → RED 运行失败即合法 RED；TIMEOUT 900 覆盖
# 10 个场景（每场景真实 spawn/kill/重启 + MySQL 断言）。
add_test(NAME ReliableMessageFaultProcess
    COMMAND ${BASH_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/reliable_message_fault_test.sh
        $<TARGET_FILE:ChatServer>)
set_tests_properties(ReliableMessageFaultProcess PROPERTIES TIMEOUT 900 RESOURCE_LOCK chat_server_ports)

# P4-06 RED：三节点 chaos + 容量/风暴保护（真实 Redis/Kafka/MySQL，不 skip）。
# l5_lastseen_unbounded 与 db0_presence_accumulation 当前实现必须失败 → 合法 RED；
# 其余场景照常执行。TIMEOUT 1200 覆盖多场景真实 spawn/kill/重启 + Redis/Kafka/MySQL
# 故障注入与收敛断言。
add_test(NAME ClusterChaosProcess
    COMMAND ${BASH_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/cluster_chaos_test.sh
        $<TARGET_FILE:ChatServer>)
set_tests_properties(ClusterChaosProcess PROPERTIES TIMEOUT 1200 RESOURCE_LOCK chat_server_ports)

foreach(TEST_TARGET IN ITEMS BufferTest EventLoopTest EventLoopThreadPoolTest TcpConnectionTest BenchStatsTest StreamCodecTest LegacyJsonLineCodecTest BinaryFrameCodecTest TimerQueueTest BackpressureTest TcpServerTest LoggerTest DualProtocolCharacterizationTest ChatApplicationRegistrationTest UserRepositoryContractTest MySQLUserRepositoryTest BlockingExecutorTest ReliableMessageMetricsTest SessionSerialExecutorTest EventLoopResponsivenessTest ConnectionPoolTest MySQLPoolIntegrationTest SessionApplicationTest SessionRegistryTest FriendRepositoryContractTest GroupRepositoryContractTest FriendGroupApplicationTest OfflineMessageTest MessageApplicationTest MessageAcceptanceConcurrencyTest     MultiReactorTest ConfigTest SchemaMigrationTest ReliableMessagingContractTest ReliableMessageSchemaTest MySQLReliableMessagingTest MessageAcceptanceApplicationTest ReliableProtocolGoldenTest DeliveryCoordinatorTest DeliveryRetryTest OutboxRelayTest OutboxCrashRecoveryTest OutboxPublisherContractTest MessageRetentionTest MySqlLeaseRoundTripTest LegacyOfflineMigrationTest BackfillReentrancyTest LegacyCutoverTest SessionDeliverySinkTest PresenceContractTest RedisPresenceContractTest GatewayDeliveryContractTest GatewayPresenceWiringContractTest TelemetryContractTest MetricsSnapshotTest PrometheusEndpointTest GapWiringContractTest)
    set_target_properties(${TEST_TARGET} PROPERTIES CROSSCOMPILING_EMULATOR "${GTEST_EMULATOR}")
endforeach()
discover_gtest(ReliableMessageSchemaTest)
discover_gtest(SchemaMigrationTest)
discover_gtest(ReliableMessagingContractTest)
discover_gtest(BufferTest)
discover_gtest(EventLoopTest)
discover_gtest(EventLoopThreadPoolTest)
discover_gtest(TcpConnectionTest)
discover_gtest(BenchStatsTest)
discover_gtest(StreamCodecTest)
discover_gtest(LegacyJsonLineCodecTest)
discover_gtest(BinaryFrameCodecTest)
discover_gtest(TimerQueueTest)
discover_gtest(BackpressureTest)
discover_gtest(TcpServerTest)
discover_gtest(LoggerTest)
discover_gtest(DualProtocolCharacterizationTest)
discover_gtest(ChatApplicationRegistrationTest)
discover_gtest(UserRepositoryContractTest)
discover_gtest(MySQLUserRepositoryTest)
discover_gtest(BlockingExecutorTest)
discover_gtest(ReliableMessageMetricsTest)
discover_gtest(SessionSerialExecutorTest)
discover_gtest(EventLoopResponsivenessTest)
discover_gtest(ConnectionPoolTest)
discover_gtest(MySQLPoolIntegrationTest)
discover_gtest(SessionApplicationTest)
discover_gtest(SessionRegistryTest)
discover_gtest(FriendRepositoryContractTest)
discover_gtest(GroupRepositoryContractTest)
discover_gtest(FriendGroupApplicationTest)
discover_gtest(OfflineMessageTest)
discover_gtest(MessageApplicationTest)
discover_gtest(MessageAcceptanceConcurrencyTest)
discover_gtest(MultiReactorTest)
discover_gtest(ConfigTest)
discover_gtest(MySQLReliableMessagingTest)
discover_gtest(MessageAcceptanceApplicationTest)
discover_gtest(ReliableProtocolGoldenTest)
discover_gtest(DeliveryCoordinatorTest)
discover_gtest(DeliveryRetryTest)
discover_gtest(OutboxRelayTest)
discover_gtest(OutboxCrashRecoveryTest)
discover_gtest(OutboxPublisherContractTest)
discover_gtest(MessageRetentionTest)
discover_gtest(MySqlLeaseRoundTripTest)
discover_gtest(LegacyOfflineMigrationTest)
discover_gtest(BackfillReentrancyTest)
discover_gtest(LegacyCutoverTest)
discover_gtest(SessionDeliverySinkTest)
discover_gtest(PresenceContractTest)
discover_gtest(RedisPresenceContractTest)
discover_gtest(GatewayDeliveryContractTest)
discover_gtest(GatewayPresenceWiringContractTest)
discover_gtest(TelemetryContractTest)
discover_gtest(MetricsSnapshotTest)
discover_gtest(PrometheusEndpointTest)
discover_gtest(GapWiringContractTest)

# P4-04 RED：幂等 Delivery consumer 契约（引用尚不存在的 app/OutboxEventConsumer.hpp、
# app/KafkaEventConsumer.hpp、app/WakeupProgressHandler.hpp 与 MessageStore 的
# dead-letter 扩展 → 编译失败即合法 RED，沿 P4-02/P4-03 先例）。真实 Kafka 消费
# 用例（127.0.0.1:9092，env KAFKA_TEST_HOST/PORT）+ MySQL dead-letter 双跑，不 skip。
add_executable(OutboxConsumerContractTest unit/OutboxConsumerContractTest.cpp)
target_link_libraries(OutboxConsumerContractTest GTest::gtest_main GTest::gtest chatserver_core Threads::Threads)
set_target_properties(OutboxConsumerContractTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)
set_target_properties(OutboxConsumerContractTest PROPERTIES CROSSCOMPILING_EMULATOR "${GTEST_EMULATOR}")
discover_gtest(OutboxConsumerContractTest)
