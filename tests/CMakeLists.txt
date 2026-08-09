find_package(GTest CONFIG REQUIRED)

add_executable(BufferTest unit/BufferTest.cpp ${PROJECT_SOURCE_DIR}/mymuduo/Buffer.cc)
target_include_directories(BufferTest PRIVATE ${PROJECT_SOURCE_DIR}/mymuduo)
target_link_libraries(BufferTest GTest::gtest_main GTest::gtest)
set_target_properties(BufferTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

include(GoogleTest)

set(MYMUDUO_DIR ${PROJECT_SOURCE_DIR}/mymuduo)

add_executable(EventLoopTest
    unit/EventLoopTest.cpp
    ${MYMUDUO_DIR}/TimerQueue.cc
    ${MYMUDUO_DIR}/EventLoop.cc
    ${MYMUDUO_DIR}/Channel.cc
    ${MYMUDUO_DIR}/Poller.cc
    ${MYMUDUO_DIR}/DefaultPoller.cc
    ${MYMUDUO_DIR}/EPollPoller.cc
    ${MYMUDUO_DIR}/CurrentThread.cc
    ${MYMUDUO_DIR}/Timestamp.cc
    ${MYMUDUO_DIR}/Logger.cc
)
target_include_directories(EventLoopTest PRIVATE ${PROJECT_SOURCE_DIR}/mymuduo)
target_link_libraries(EventLoopTest GTest::gtest_main GTest::gtest)
set_target_properties(EventLoopTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

add_executable(EventLoopThreadPoolTest
    unit/EventLoopThreadPoolTest.cpp
    ${MYMUDUO_DIR}/EventLoopThreadPool.cc
    ${MYMUDUO_DIR}/EventLoopThread.cc
    ${MYMUDUO_DIR}/Thread.cc
    ${MYMUDUO_DIR}/TimerQueue.cc
    ${MYMUDUO_DIR}/EventLoop.cc
    ${MYMUDUO_DIR}/Channel.cc
    ${MYMUDUO_DIR}/Poller.cc
    ${MYMUDUO_DIR}/DefaultPoller.cc
    ${MYMUDUO_DIR}/EPollPoller.cc
    ${MYMUDUO_DIR}/CurrentThread.cc
    ${MYMUDUO_DIR}/Timestamp.cc
    ${MYMUDUO_DIR}/Logger.cc
)
target_include_directories(EventLoopThreadPoolTest PRIVATE ${PROJECT_SOURCE_DIR}/mymuduo)
target_link_libraries(EventLoopThreadPoolTest GTest::gtest_main GTest::gtest)
set_target_properties(EventLoopThreadPoolTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

add_executable(TcpConnectionTest
    unit/TcpConnectionTest.cpp
    ${MYMUDUO_DIR}/TcpConnection.cc
    ${MYMUDUO_DIR}/Socket.cc
    ${MYMUDUO_DIR}/InetAddress.cc
    ${MYMUDUO_DIR}/Buffer.cc
    ${MYMUDUO_DIR}/TimerQueue.cc
    ${MYMUDUO_DIR}/EventLoop.cc
    ${MYMUDUO_DIR}/Channel.cc
    ${MYMUDUO_DIR}/Poller.cc
    ${MYMUDUO_DIR}/DefaultPoller.cc
    ${MYMUDUO_DIR}/EPollPoller.cc
    ${MYMUDUO_DIR}/CurrentThread.cc
    ${MYMUDUO_DIR}/Timestamp.cc
    ${MYMUDUO_DIR}/Logger.cc
)
target_include_directories(TcpConnectionTest PRIVATE ${PROJECT_SOURCE_DIR}/mymuduo)
target_link_libraries(TcpConnectionTest GTest::gtest_main GTest::gtest)
set_target_properties(TcpConnectionTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

add_executable(BenchStatsTest unit/BenchStatsTest.cpp)
target_include_directories(BenchStatsTest PRIVATE
    ${PROJECT_SOURCE_DIR}/thirdparty
    ${PROJECT_SOURCE_DIR}/tools/chat-bench)
target_link_libraries(BenchStatsTest GTest::gtest_main GTest::gtest)
set_target_properties(BenchStatsTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

add_executable(StreamCodecTest
    unit/StreamCodecTest.cpp
    ${MYMUDUO_DIR}/StreamCodec.cc
    ${MYMUDUO_DIR}/Buffer.cc
)
target_include_directories(StreamCodecTest PRIVATE ${PROJECT_SOURCE_DIR}/mymuduo)
target_link_libraries(StreamCodecTest GTest::gtest_main GTest::gtest)
set_target_properties(StreamCodecTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

add_executable(LegacyJsonLineCodecTest
    unit/LegacyJsonLineCodecTest.cpp
    ${MYMUDUO_DIR}/LegacyJsonLineCodec.cc
    ${MYMUDUO_DIR}/Buffer.cc
)
target_include_directories(LegacyJsonLineCodecTest PRIVATE ${PROJECT_SOURCE_DIR}/mymuduo)
target_link_libraries(LegacyJsonLineCodecTest GTest::gtest_main GTest::gtest)
set_target_properties(LegacyJsonLineCodecTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

add_executable(BinaryFrameCodecTest
    unit/BinaryFrameCodecTest.cpp
    ${MYMUDUO_DIR}/BinaryFrameCodec.cc
    ${MYMUDUO_DIR}/StreamCodec.cc
    ${MYMUDUO_DIR}/Buffer.cc
)
target_include_directories(BinaryFrameCodecTest PRIVATE ${PROJECT_SOURCE_DIR}/mymuduo)
target_link_libraries(BinaryFrameCodecTest GTest::gtest_main GTest::gtest)
set_target_properties(BinaryFrameCodecTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

add_executable(TimerQueueTest
    unit/TimerQueueTest.cpp
    ${MYMUDUO_DIR}/TimerQueue.cc
    ${MYMUDUO_DIR}/EventLoop.cc
    ${MYMUDUO_DIR}/Channel.cc
    ${MYMUDUO_DIR}/Poller.cc
    ${MYMUDUO_DIR}/DefaultPoller.cc
    ${MYMUDUO_DIR}/EPollPoller.cc
    ${MYMUDUO_DIR}/CurrentThread.cc
    ${MYMUDUO_DIR}/Timestamp.cc
    ${MYMUDUO_DIR}/Logger.cc
)
target_include_directories(TimerQueueTest PRIVATE ${PROJECT_SOURCE_DIR}/mymuduo)
target_link_libraries(TimerQueueTest GTest::gtest_main GTest::gtest)
set_target_properties(TimerQueueTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

add_executable(BackpressureTest
    unit/BackpressureTest.cpp
    ${MYMUDUO_DIR}/TcpConnection.cc
    ${MYMUDUO_DIR}/Socket.cc
    ${MYMUDUO_DIR}/InetAddress.cc
    ${MYMUDUO_DIR}/Buffer.cc
    ${MYMUDUO_DIR}/TimerQueue.cc
    ${MYMUDUO_DIR}/EventLoop.cc
    ${MYMUDUO_DIR}/Channel.cc
    ${MYMUDUO_DIR}/Poller.cc
    ${MYMUDUO_DIR}/DefaultPoller.cc
    ${MYMUDUO_DIR}/EPollPoller.cc
    ${MYMUDUO_DIR}/CurrentThread.cc
    ${MYMUDUO_DIR}/Timestamp.cc
    ${MYMUDUO_DIR}/Logger.cc
)
target_include_directories(BackpressureTest PRIVATE ${PROJECT_SOURCE_DIR}/mymuduo)
target_link_libraries(BackpressureTest GTest::gtest_main GTest::gtest)
set_target_properties(BackpressureTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

add_executable(TcpServerTest
    unit/TcpServerTest.cpp
    ${MYMUDUO_DIR}/TcpServer.cc
    ${MYMUDUO_DIR}/Acceptor.cc
    ${MYMUDUO_DIR}/Socket.cc
    ${MYMUDUO_DIR}/InetAddress.cc
    ${MYMUDUO_DIR}/TcpConnection.cc
    ${MYMUDUO_DIR}/Buffer.cc
    ${MYMUDUO_DIR}/TimerQueue.cc
    ${MYMUDUO_DIR}/EventLoop.cc
    ${MYMUDUO_DIR}/Channel.cc
    ${MYMUDUO_DIR}/Poller.cc
    ${MYMUDUO_DIR}/DefaultPoller.cc
    ${MYMUDUO_DIR}/EPollPoller.cc
    ${MYMUDUO_DIR}/EventLoopThreadPool.cc
    ${MYMUDUO_DIR}/EventLoopThread.cc
    ${MYMUDUO_DIR}/Thread.cc
    ${MYMUDUO_DIR}/CurrentThread.cc
    ${MYMUDUO_DIR}/Timestamp.cc
    ${MYMUDUO_DIR}/Logger.cc
)
target_include_directories(TcpServerTest PRIVATE ${PROJECT_SOURCE_DIR}/mymuduo)
target_link_libraries(TcpServerTest GTest::gtest_main GTest::gtest)
set_target_properties(TcpServerTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

set(GTEST_EMULATOR "setarch;x86_64;-R")
foreach(TEST_TARGET IN ITEMS BufferTest EventLoopTest EventLoopThreadPoolTest TcpConnectionTest BenchStatsTest StreamCodecTest LegacyJsonLineCodecTest BinaryFrameCodecTest TimerQueueTest BackpressureTest TcpServerTest)
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
