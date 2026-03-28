cmake_minimum_required(VERSION 3.16)
project(dvfs_tool LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(include)

add_library(jetson_platform
    src/common/sysfs_io.cpp
    src/common/time_utils.cpp
    src/jetson/cpu_freq_device.cpp
    src/jetson/gpu_freq_device.cpp
    src/jetson/thermal_sensor.cpp
    src/jetson/fan_device.cpp
    src/jetson/ina260_reader.cpp
    src/jetson/tegrastats_monitor.cpp
    src/jetson/jetson_platform.cpp
    src/jetson/jtop_reader.cpp
    src/app/csv_logger.cpp
    src/app/watch_renderer.cpp
)

target_link_libraries(jetson_platform pthread)

add_executable(dvfs_tool src/main.cpp)
target_link_libraries(dvfs_tool jetson_platform pthread)