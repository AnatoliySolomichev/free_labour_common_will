cmake_minimum_required(VERSION 3.16)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LMDB REQUIRED lmdb)

# ── Library ───────────────────────────────────────────────────────────────────

add_library(aggregator
    src/aggregator.cpp
    src/server.cpp
)

target_include_directories(aggregator
    PUBLIC  include
    PRIVATE ${LMDB_INCLUDE_DIRS}
            ${CMAKE_SOURCE_DIR}/third_party/httplib
)

target_link_libraries(aggregator
    PUBLIC  blockchain
    PRIVATE ${LMDB_LIBRARIES}
)

target_compile_options(aggregator PRIVATE -Wall -Wextra)
# No CPPHTTPLIB_OPENSSL_SUPPORT defined → httplib compiles in plain-HTTP-only mode.

# ── Server executable ─────────────────────────────────────────────────────────

add_executable(aggregator_server src/main.cpp)
target_link_libraries(aggregator_server PRIVATE aggregator)

# ── Tests ─────────────────────────────────────────────────────────────────────

enable_testing()
find_package(GTest REQUIRED)

add_executable(aggregator_tests
    tests/test_aggregator.cpp
)

target_include_directories(aggregator_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/httplib
)

target_link_libraries(aggregator_tests PRIVATE
    aggregator
    GTest::gtest_main
)

target_compile_options(aggregator_tests PRIVATE -Wall -Wextra)

add_test(NAME aggregator_tests COMMAND aggregator_tests)
