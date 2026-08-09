add_executable(chat-bench chat-bench/main.cpp)
target_include_directories(chat-bench PRIVATE ${PROJECT_SOURCE_DIR}/thirdparty)
target_compile_definitions(chat-bench PRIVATE
    GIT_COMMIT="${GIT_COMMIT}"
    CXX_FLAGS_STRING="${CMAKE_CXX_FLAGS}"
    BUILD_TYPE="${CMAKE_BUILD_TYPE}")
target_link_libraries(chat-bench pthread)
