find_package(GTest CONFIG REQUIRED)

add_executable(BufferTest unit/BufferTest.cpp)
target_include_directories(BufferTest PRIVATE ${PROJECT_SOURCE_DIR}/mymuduo)
target_link_libraries(BufferTest GTest::gtest_main GTest::gtest mymuduo Threads::Threads)
set_target_properties(BufferTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

include(GoogleTest)

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

find_program(PYTHON3_EXECUTABLE python3 REQUIRED)
add_test(NAME single-compile-check
    COMMAND ${PYTHON3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_single_compile.py
        ${CMAKE_BINARY_DIR}/compile_commands.json)

find_program(BASH_EXECUTABLE bash REQUIRED)
add_test(NAME process-signal-shutdown
    COMMAND ${BASH_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/signal_shutdown_test.sh
        $<TARGET_FILE:ChatServer>
        20)
set_tests_properties(process-signal-shutdown PROPERTIES TIMEOUT 60)

add_test(NAME process-fd-exhaustion
    COMMAND ${BASH_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/fd_exhaustion_test.sh
        $<TARGET_FILE:ChatServer>)
set_tests_properties(process-fd-exhaustion PROPERTIES TIMEOUT 90)

add_test(NAME DomainCharacterization
    COMMAND ${BASH_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/domain_characterization_test.sh
        $<TARGET_FILE:ChatServer>)
set_tests_properties(DomainCharacterization PROPERTIES TIMEOUT 180)

set(GTEST_EMULATOR "setarch;x86_64;-R")
foreach(TEST_TARGET IN ITEMS BufferTest EventLoopTest EventLoopThreadPoolTest TcpConnectionTest BenchStatsTest StreamCodecTest LegacyJsonLineCodecTest BinaryFrameCodecTest TimerQueueTest BackpressureTest TcpServerTest LoggerTest DualProtocolCharacterizationTest ChatApplicationRegistrationTest UserRepositoryContractTest MySQLUserRepositoryTest BlockingExecutorTest EventLoopResponsivenessTest)
    set_target_properties(${TEST_TARGET} PROPERTIES CROSSCOMPILING_EMULATOR "${GTEST_EMULATOR}")
endforeach()
gtest_discover_tests(BufferTest)
gtest_discover_tests(EventLoopTest)
gtest_discover_tests(EventLoopThreadPoolTest)
gtest_discover_tests(TcpConnectionTest)
gtest_discover_tests(BenchStatsTest)
gtest_discover_tests(StreamCodecTest)
gtest_discover_tests(LegacyJsonLineCodecTest)
gtest_discover_tests(BinaryFrameCodecTest)
gtest_discover_tests(TimerQueueTest)
gtest_discover_tests(BackpressureTest)
gtest_discover_tests(TcpServerTest)
gtest_discover_tests(LoggerTest)
gtest_discover_tests(DualProtocolCharacterizationTest)
gtest_discover_tests(ChatApplicationRegistrationTest)
gtest_discover_tests(UserRepositoryContractTest)
gtest_discover_tests(MySQLUserRepositoryTest)
gtest_discover_tests(BlockingExecutorTest)
gtest_discover_tests(EventLoopResponsivenessTest)
