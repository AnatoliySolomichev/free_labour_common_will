cmake_minimum_required(VERSION 3.16)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LMDB REQUIRED lmdb)

# ── Library ───────────────────────────────────────────────────────────────────

add_library(sync
    src/participant_cache.cpp
    src/merge_dialogue.cpp
)

target_include_directories(sync
    PUBLIC  include
    PRIVATE ${LMDB_INCLUDE_DIRS}
)

target_link_libraries(sync
    PUBLIC  blockchain
    PRIVATE ${LMDB_LIBRARIES}
)

target_compile_options(sync PRIVATE -Wall -Wextra -Wpedantic)

# ── Tests ─────────────────────────────────────────────────────────────────────

enable_testing()
find_package(GTest REQUIRED)

add_executable(sync_tests
    tests/test_participant_cache.cpp
    tests/test_merge_dialogue.cpp
)

target_link_libraries(sync_tests PRIVATE
    sync
    GTest::gtest_main
)

target_compile_options(sync_tests PRIVATE -Wall -Wextra)

add_test(NAME sync_tests COMMAND sync_tests)
