cmake_minimum_required(VERSION 3.28)
project(LSM_tree)

set(CMAKE_CXX_STANDARD 17)

add_executable(LSM src/main.cpp)
target_include_directories(LSM PRIVATE inc)
