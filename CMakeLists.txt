cmake_minimum_required(VERSION 3.16)
project(free_labour_common_will LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()
add_subdirectory(modules/blockchain)
add_subdirectory(modules/records)
add_subdirectory(modules/sync)
add_subdirectory(modules/cli)
add_subdirectory(modules/aggregator)
