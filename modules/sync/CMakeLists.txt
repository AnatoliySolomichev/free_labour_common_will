cmake_minimum_required(VERSION 3.16)

# ── Library ───────────────────────────────────────────────────────────────────

add_library(sync
    src/participant_cache.cpp
)

target_include_directories(sync PUBLIC include)

target_link_libraries(sync PUBLIC blockchain)

target_compile_options(sync PRIVATE -Wall -Wextra -Wpedantic)

# ── Tests ─────────────────────────────────────────────────────────────────────

enable_testing()
find_package(GTest REQUIRED)

add_executable(sync_tests
    tests/test_participant_cache.cpp
)

target_link_libraries(sync_tests PRIVATE
    sync
    GTest::gtest_main
)

target_compile_options(sync_tests PRIVATE -Wall -Wextra)

add_test(NAME sync_tests COMMAND sync_tests)
