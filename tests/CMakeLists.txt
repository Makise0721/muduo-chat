find_package(GTest CONFIG REQUIRED)

add_executable(BufferTest unit/BufferTest.cpp ${PROJECT_SOURCE_DIR}/mymuduo/Buffer.cc)
target_include_directories(BufferTest PRIVATE ${PROJECT_SOURCE_DIR}/mymuduo)
target_link_libraries(BufferTest GTest::gtest_main GTest::gtest)
set_target_properties(BufferTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

include(GoogleTest)
gtest_discover_tests(BufferTest)

set(MYMUDUO_DIR ${PROJECT_SOURCE_DIR}/mymuduo)

add_executable(EventLoopTest
    unit/EventLoopTest.cpp
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
gtest_discover_tests(EventLoopTest)

add_executable(EventLoopThreadPoolTest
    unit/EventLoopThreadPoolTest.cpp
    ${MYMUDUO_DIR}/EventLoopThreadPool.cc
    ${MYMUDUO_DIR}/EventLoopThread.cc
    ${MYMUDUO_DIR}/Thread.cc
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
gtest_discover_tests(EventLoopThreadPoolTest)

add_executable(TcpConnectionTest
    unit/TcpConnectionTest.cpp
    ${MYMUDUO_DIR}/TcpConnection.cc
    ${MYMUDUO_DIR}/Socket.cc
    ${MYMUDUO_DIR}/InetAddress.cc
    ${MYMUDUO_DIR}/Buffer.cc
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
gtest_discover_tests(TcpConnectionTest)
