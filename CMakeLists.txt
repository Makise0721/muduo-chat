cmake_minimum_required(VERSION 3.10)
project(ChatServer)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -std=c++11")

set(EXECUTABLE_OUTPUT_PATH ${PROJECT_SOURCE_DIR}/bin)

include_directories(${PROJECT_SOURCE_DIR}/thirdparty)
include_directories(${PROJECT_SOURCE_DIR}/mymuduo)
include_directories(${PROJECT_SOURCE_DIR}/chatserver/include)

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(MYSQL REQUIRED mysqlclient)
pkg_check_modules(HIREDIS REQUIRED hiredis)

add_subdirectory(mymuduo)

aux_source_directory(chatserver/src CHATSERVER_SRC)

add_executable(ChatServer chatserver/main.cpp ${CHATSERVER_SRC})
add_executable(ChatClient chatserver/client.cpp)

target_link_libraries(ChatServer mymuduo ${MYSQL_LIBRARIES} ${HIREDIS_LIBRARIES} pthread)
target_link_libraries(ChatClient pthread)
