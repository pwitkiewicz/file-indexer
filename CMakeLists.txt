cmake_minimum_required(VERSION 3.16)
project(SOP_P4 C)

set(CMAKE_C_STANDARD 99)

add_executable(file-indexer main.c)

target_link_libraries(file-indexer pthread)