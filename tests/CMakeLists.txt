find_package(GTest CONFIG REQUIRED)

add_executable(BufferTest unit/BufferTest.cpp)
target_include_directories(BufferTest PRIVATE ${PROJECT_SOURCE_DIR}/mymuduo)
target_link_libraries(BufferTest GTest::gtest_main GTest::gtest)
set_target_properties(BufferTest PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED ON)

include(GoogleTest)
gtest_discover_tests(BufferTest)
