cmake_minimum_required(VERSION 3.16)

# ── Library ───────────────────────────────────────────────────────────────────

add_library(records
    src/codec.cpp
    src/draft.cpp
)

target_include_directories(records PUBLIC include)

target_compile_options(records PRIVATE -Wall -Wextra -Wpedantic)

# ── Tests ─────────────────────────────────────────────────────────────────────

enable_testing()
find_package(GTest REQUIRED)

add_executable(records_tests
    tests/test_codec.cpp
    tests/test_draft.cpp
)

target_link_libraries(records_tests PRIVATE
    records
    GTest::gtest_main
)

target_compile_options(records_tests PRIVATE -Wall -Wextra)

add_test(NAME records_tests COMMAND records_tests)
