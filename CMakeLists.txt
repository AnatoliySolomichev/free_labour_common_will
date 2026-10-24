cmake_minimum_required(VERSION 3.16)
project(free_labour_common_will LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()
add_subdirectory(third_party)
add_subdirectory(modules/blockchain)
add_subdirectory(modules/records)
add_subdirectory(modules/sync)
add_subdirectory(modules/cli)
add_subdirectory(modules/aggregator)
add_subdirectory(modules/extension)

# End-to-end smoke test: the live "bread with oven and flour" demo, run against
# a real aggregator over HTTP. Doubles as the human-readable demo (docs/pilot).
# Guarded by TEST_E2E because it needs curl and a free TCP port; unit tests do
# not. Enable with: cmake -DTEST_E2E=ON
option(TEST_E2E "Run the end-to-end demo as a ctest" OFF)
if(TEST_E2E)
    add_test(
        NAME e2e_demo
        COMMAND ${CMAKE_COMMAND} -E env BUILD=${CMAKE_BINARY_DIR}
                bash ${CMAKE_SOURCE_DIR}/docs/pilot/demo.sh
    )
    set_tests_properties(e2e_demo PROPERTIES
        DEPENDS "bc;aggregator_server" TIMEOUT 120)

    # Big, diverse walkthrough: a whole townlet (~14 chains) exercising every
    # command — matching, deals, amortization, paper circulation, merges (DAG),
    # seals, pledges, footprint, revocation, fraud tooling. Own port to run
    # alongside e2e_demo; longer timeout because of the pairwise merges.
    add_test(
        NAME e2e_grand_demo
        COMMAND ${CMAKE_COMMAND} -E env BUILD=${CMAKE_BINARY_DIR} PORT=19191
                bash ${CMAKE_SOURCE_DIR}/docs/pilot/grand-demo.sh
    )
    set_tests_properties(e2e_grand_demo PROPERTIES
        DEPENDS "bc;aggregator_server" TIMEOUT 360)

    # The social layer (ИР-026, social.md): reactions, comments, the circle and
    # a bot farm seen only as a number. Own port, like the other walkthroughs.
    add_test(
        NAME e2e_social_demo
        COMMAND ${CMAKE_COMMAND} -E env BUILD=${CMAKE_BINARY_DIR} PORT=19342
                bash ${CMAKE_SOURCE_DIR}/docs/pilot/social-demo.sh
    )
    set_tests_properties(e2e_social_demo PROPERTIES
        DEPENDS "bc;aggregator_server" TIMEOUT 120)

    # Contests (ИР-026, contests.md): rules identified by their content hash, a
    # round with a beacon, attempts, verdicts, the table through one's circle.
    add_test(
        NAME e2e_contest_demo
        COMMAND ${CMAKE_COMMAND} -E env BUILD=${CMAKE_BINARY_DIR} PORT=19352
                bash ${CMAKE_SOURCE_DIR}/docs/pilot/contest-demo.sh
    )
    set_tests_properties(e2e_contest_demo PROPERTIES
        DEPENDS "bc;aggregator_server" TIMEOUT 120)

    # Product reviews (ИР-026, reviews.md): product keys, "the same product"
    # claims through one's circle, verified purchases of goods made in-system.
    add_test(
        NAME e2e_review_demo
        COMMAND ${CMAKE_COMMAND} -E env BUILD=${CMAKE_BINARY_DIR} PORT=19362
                bash ${CMAKE_SOURCE_DIR}/docs/pilot/review-demo.sh
    )
    set_tests_properties(e2e_review_demo PROPERTIES
        DEPENDS "bc;aggregator_server" TIMEOUT 120)

    # Browser extension (docs/extension.md): the native messaging host protocol,
    # and — when Node.js and Playwright are installed — the real Chromium; with
    # Firefox and geckodriver, the real Firefox with bc.xpi installed unsigned.
    add_test(
        NAME e2e_extension
        COMMAND ${CMAKE_COMMAND} -E env BUILD=${CMAKE_BINARY_DIR} PORT=19372
                bash ${CMAKE_SOURCE_DIR}/modules/extension/tests/host-e2e.sh
    )
    set_tests_properties(e2e_extension PROPERTIES
        DEPENDS "bc;aggregator_server" TIMEOUT 180)
endif()
