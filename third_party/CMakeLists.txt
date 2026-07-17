# ── cpp-httplib (header-only) ─────────────────────────────────────────────────
# Compiled with TLS support (`https://` in --via URLs and peer sync).
# CPPHTTPLIB_OPENSSL_SUPPORT must be identical in every translation unit that
# includes httplib.h — hence one INTERFACE target instead of per-module flags.

find_package(OpenSSL REQUIRED)

add_library(httplib INTERFACE)
target_include_directories(httplib INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/httplib)
target_compile_definitions(httplib INTERFACE CPPHTTPLIB_OPENSSL_SUPPORT)
target_link_libraries(httplib INTERFACE OpenSSL::SSL OpenSSL::Crypto)
