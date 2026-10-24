# Browser extension (docs/extension.md). Chromium loads this directory as is
# ("unpacked"). Firefox keeps an unpacked extension only until restart, so for
# it we pack bc.xpi — a zip with manifest.json at the root — which Firefox ESR
# installs for good, unsigned (extension.md §5).
set(EXTENSION_FILES manifest.json popup.html popup.css popup.js)
set(EXTENSION_XPI ${CMAKE_CURRENT_BINARY_DIR}/bc.xpi)

add_custom_command(
    OUTPUT ${EXTENSION_XPI}
    COMMAND ${CMAKE_COMMAND} -E tar cf ${EXTENSION_XPI} --format=zip ${EXTENSION_FILES}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    DEPENDS ${EXTENSION_FILES}
    COMMENT "Packing the browser extension for Firefox: bc.xpi"
    VERBATIM
)
add_custom_target(extension_xpi ALL DEPENDS ${EXTENSION_XPI})
