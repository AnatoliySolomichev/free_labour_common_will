cmake_minimum_required(VERSION 3.16)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBSODIUM REQUIRED libsodium)
pkg_check_modules(LMDB      REQUIRED lmdb)

add_executable(bc src/main.cpp)

target_link_libraries(bc PRIVATE
    blockchain
    records
    ${LIBSODIUM_LIBRARIES}
    ${LMDB_LIBRARIES}
)

target_include_directories(bc PRIVATE
    ${LIBSODIUM_INCLUDE_DIRS}
    ${LMDB_INCLUDE_DIRS}
)

target_compile_options(bc PRIVATE -Wall -Wextra)
