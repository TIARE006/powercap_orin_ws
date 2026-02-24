cmake_minimum_required(VERSION 3.10)
project(powercap_orin_ws LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(dvfs_tool src/dvfs_tool.cpp)
