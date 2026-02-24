cmake_minimum_required(VERSION 3.16)
project(powercap_orin CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(dvfs_probe src/dvfs_probe.cpp)
add_executable(dvfs_set src/dvfs_set.cpp)
add_executable(logger src/logger.cpp)