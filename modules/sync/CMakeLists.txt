cmake_minimum_required(VERSION 3.16)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LMDB REQUIRED lmdb)

# ── Library ───────────────────────────────────────────────────────────────────

add_library(sync
    src/participant_cache.cpp
    src/merge_dialogue.cpp
    src/dialogue_channel.cpp
    src/snapshot_exchange.cpp
)

target_include_directories(sync
    PUBLIC  include
    PRIVATE ${LMDB_INCLUDE_DIRS}
)

target_link_libraries(sync
    PUBLIC  blockchain
    PRIVATE httplib
            ${LMDB_LIBRARIES}
)

target_compile_options(sync PRIVATE -Wall -Wextra -Wpedantic)

# ── Tests ─────────────────────────────────────────────────────────────────────

enable_testing()
find_package(GTest REQUIRED)

add_executable(sync_tests
    tests/test_participant_cache.cpp
    tests/test_merge_dialogue.cpp
    tests/test_dialogue_channel.cpp
    tests/test_snapshot_exchange.cpp
)

# aggregator + httplib: only for the live-relay end-to-end test.
target_link_libraries(sync_tests PRIVATE
    sync
    aggregator
    httplib
    GTest::gtest_main
)

target_compile_options(sync_tests PRIVATE -Wall -Wextra)

add_test(NAME sync_tests COMMAND sync_tests)
