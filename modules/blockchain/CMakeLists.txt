cmake_minimum_required(VERSION 3.16)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBSODIUM REQUIRED libsodium)
pkg_check_modules(LMDB      REQUIRED lmdb)
# CBOR is implemented manually (see src/serializer.cpp) — no external CBOR library needed.

# ── Library ──────────────────────────────────────────────────────────────────

add_library(blockchain
    src/crypto.cpp
    src/errors.cpp
    src/serializer.cpp
    src/storage.cpp
    src/validator.cpp
    src/seal_manager.cpp
    src/merge_session.cpp
    src/blockchain.cpp
)

target_include_directories(blockchain
    PUBLIC  include
    PRIVATE ${LIBSODIUM_INCLUDE_DIRS}
            ${LMDB_INCLUDE_DIRS}
)

target_link_libraries(blockchain PRIVATE
    ${LIBSODIUM_LIBRARIES}
    ${LMDB_LIBRARIES}
)

target_compile_options(blockchain PRIVATE -Wall -Wextra -Wpedantic)

# ── Demo executable ───────────────────────────────────────────────────────────

add_executable(blockchain_demo src/main.cpp)
target_link_libraries(blockchain_demo PRIVATE blockchain)

# ── Tests ─────────────────────────────────────────────────────────────────────

enable_testing()
find_package(GTest REQUIRED)

add_executable(blockchain_tests
    tests/test_types.cpp
    tests/test_crypto.cpp
    tests/test_serializer.cpp
    tests/test_storage.cpp
    tests/test_validator.cpp
    tests/test_blockchain.cpp
    tests/test_seal_manager.cpp
    tests/test_merge_session.cpp
)

target_link_libraries(blockchain_tests PRIVATE
    blockchain
    GTest::gtest_main
    GTest::gmock
)

target_compile_options(blockchain_tests PRIVATE -Wall -Wextra)

add_test(NAME blockchain_tests COMMAND blockchain_tests)
