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

add_executable(MessageRepositoryContractTest unit/MessageRepositoryContractTest.cpp)
target_link_libraries(MessageRepositoryContractTest GTest::gtest_main GTest::gtest chatserver_core Threads::Threads)
set_target_properties(MessageRepositoryContractTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

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

add_executable(SessionDeliverySinkTest
    unit/SessionDeliverySinkTest.cpp
    ${PROJECT_SOURCE_DIR}/chatserver/src/SessionDeliverySink.cpp)
target_include_directories(SessionDeliverySinkTest PRIVATE ${PROJECT_SOURCE_DIR}/mymuduo)
target_link_libraries(SessionDeliverySinkTest GTest::gtest_main GTest::gtest chatserver_core mymuduo Threads::Threads)
set_target_properties(SessionDeliverySinkTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

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

foreach(TEST_TARGET IN ITEMS BufferTest EventLoopTest EventLoopThreadPoolTest TcpConnectionTest BenchStatsTest StreamCodecTest LegacyJsonLineCodecTest BinaryFrameCodecTest TimerQueueTest BackpressureTest TcpServerTest LoggerTest DualProtocolCharacterizationTest ChatApplicationRegistrationTest UserRepositoryContractTest MySQLUserRepositoryTest BlockingExecutorTest EventLoopResponsivenessTest ConnectionPoolTest MySQLPoolIntegrationTest SessionApplicationTest SessionRegistryTest FriendRepositoryContractTest GroupRepositoryContractTest FriendGroupApplicationTest MessageRepositoryContractTest OfflineMessageTest MessageApplicationTest MessageAcceptanceConcurrencyTest MultiReactorTest ConfigTest SchemaMigrationTest ReliableMessagingContractTest ReliableMessageSchemaTest MySQLReliableMessagingTest MessageAcceptanceApplicationTest ReliableProtocolGoldenTest DeliveryCoordinatorTest SessionDeliverySinkTest)
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
discover_gtest(EventLoopResponsivenessTest)
discover_gtest(ConnectionPoolTest)
discover_gtest(MySQLPoolIntegrationTest)
discover_gtest(SessionApplicationTest)
discover_gtest(SessionRegistryTest)
discover_gtest(FriendRepositoryContractTest)
discover_gtest(GroupRepositoryContractTest)
discover_gtest(FriendGroupApplicationTest)
discover_gtest(MessageRepositoryContractTest)
discover_gtest(OfflineMessageTest)
discover_gtest(MessageApplicationTest)
discover_gtest(MessageAcceptanceConcurrencyTest)
discover_gtest(MultiReactorTest)
discover_gtest(ConfigTest)
discover_gtest(MySQLReliableMessagingTest)
discover_gtest(MessageAcceptanceApplicationTest)
discover_gtest(ReliableProtocolGoldenTest)
discover_gtest(DeliveryCoordinatorTest)
discover_gtest(SessionDeliverySinkTest)
