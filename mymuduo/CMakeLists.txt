cmake_minimum_required(VERSION 3.10)
project(mymuduo)
#设置动态库的位置
set(LIBRARY_OUTPUT_PATH ${PROJECT_SOURCE_DIR}/lib)
#设置调试信息
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -std=c++11")
#定义参与编译的源代码文件
aux_source_directory(. SRC_LIST)
#编译动态库
add_library(mymuduo SHARED ${SRC_LIST})