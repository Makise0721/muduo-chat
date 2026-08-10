cmake_minimum_required(VERSION 3.10)
project(ChatServer)

option(ENABLE_TESTS "Build unit tests" ON)
include(CTest)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -std=c++11")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

include_directories(${PROJECT_SOURCE_DIR}/thirdparty)
include_directories(${PROJECT_SOURCE_DIR}/mymuduo)
include_directories(${PROJECT_SOURCE_DIR}/chatserver/include)

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(MYSQL REQUIRED mysqlclient)

add_subdirectory(mymuduo)

add_library(chatserver_core STATIC
    chatserver/src/app/ChatApplication.cpp
    chatserver/src/app/InMemoryUserRepository.cpp)
target_include_directories(chatserver_core PUBLIC
    ${PROJECT_SOURCE_DIR}/chatserver/include
    ${PROJECT_SOURCE_DIR}/mymuduo)
target_compile_features(chatserver_core PUBLIC cxx_std_11)

aux_source_directory(chatserver/src CHATSERVER_SRC)

add_executable(ChatServer chatserver/main.cpp ${CHATSERVER_SRC} chatserver/src/LegacyUserRepository.cpp)
add_executable(ChatClient chatserver/client.cpp)

target_link_libraries(ChatServer mymuduo chatserver_core ${MYSQL_LIBRARIES} pthread)
target_link_libraries(ChatClient pthread)

execute_process(COMMAND git rev-parse HEAD
    OUTPUT_VARIABLE GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT GIT_COMMIT)
    set(GIT_COMMIT "unknown")
endif()

add_subdirectory(tools)

if(ENABLE_TESTS)
    add_subdirectory(tests)
endif()
