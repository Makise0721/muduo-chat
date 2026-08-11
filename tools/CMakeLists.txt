add_executable(chat-bench chat-bench/main.cpp)
target_include_directories(chat-bench PRIVATE ${PROJECT_SOURCE_DIR}/thirdparty)
target_compile_definitions(chat-bench PRIVATE
    GIT_COMMIT="${GIT_COMMIT}"
    CXX_FLAGS_STRING="${CMAKE_CXX_FLAGS}"
    BUILD_TYPE="${CMAKE_BUILD_TYPE}")
target_link_libraries(chat-bench pthread)

add_executable(dbmigrate dbmigrate/main.cpp)
target_link_libraries(dbmigrate chatserver_core ${MYSQL_LIBRARIES} pthread)

add_executable(logger-bench logger-bench/main.cpp)
target_include_directories(logger-bench PRIVATE ${PROJECT_SOURCE_DIR}/mymuduo)
target_link_libraries(logger-bench mymuduo Threads::Threads)
