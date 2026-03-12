# east-c-wasm: WebAssembly build of east-c core
#
# Built with Emscripten. Produces east-c.wasm + east-c.js glue.
#
# Usage:
#   make setup-wasm   # install emsdk (one-time)
#   make wasm          # build WASM output
#
# Or manually:
#   emcmake cmake -S . -B build-wasm -DEAST_BUILD_WASM=ON
#   cmake --build build-wasm

add_executable(east-c-wasm
    src/wasm_api.c
)

target_link_libraries(east-c-wasm PRIVATE east-c)

# Emscripten link flags
set(EAST_WASM_EXPORTS
    "_east_wasm_init"
    "_east_wasm_register_platform"
    "_east_wasm_compile"
    "_east_wasm_call"
    "_east_wasm_call_with_args"
    "_east_wasm_free"
    "_east_wasm_gc"
    "_east_wasm_malloc"
    "_east_wasm_free_buf"
    "_east_wasm_set_platform_context"
    "_east_wasm_last_error"
    "_malloc"
    "_free"
)

# Join exports into a JSON array string
list(JOIN EAST_WASM_EXPORTS "," EAST_WASM_EXPORTS_STR)
set(EAST_WASM_EXPORTS_JSON "[${EAST_WASM_EXPORTS_STR}]")

set_target_properties(east-c-wasm PROPERTIES
    OUTPUT_NAME "east-c"
    SUFFIX ".js"
)

target_link_options(east-c-wasm PRIVATE
    "SHELL:-s WASM=1"
    "SHELL:-s EXPORTED_FUNCTIONS='${EAST_WASM_EXPORTS_JSON}'"
    "SHELL:-s EXPORTED_RUNTIME_METHODS='[\"ccall\",\"cwrap\",\"HEAPU8\",\"HEAP32\",\"HEAPU32\",\"UTF8ToString\",\"stringToUTF8\",\"lengthBytesUTF8\"]'"
    "SHELL:-s ALLOW_MEMORY_GROWTH=1"
    "SHELL:-s MODULARIZE=1"
    "SHELL:-s EXPORT_ES6=1"
    "SHELL:-s EXPORT_NAME='createEastWasmModule'"
    "SHELL:-s ENVIRONMENT='web,worker,node'"
    "SHELL:-s NO_EXIT_RUNTIME=1"
    "SHELL:-s FILESYSTEM=0"
    "SHELL:-s TOTAL_STACK=1048576"
    "SHELL:-s INITIAL_MEMORY=16777216"
    # Async support — allows C to call async JS platform functions
    "SHELL:-s ASYNCIFY"
    "SHELL:-s ASYNCIFY_IMPORTS=['js_platform_call']"
    "SHELL:-s ASYNCIFY_STACK_SIZE=4096"
    # Optimizations
    "SHELL:-O3"
    "SHELL:--closure 0"
)

# Install the WASM artifacts to dist/
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/east-c.js
    ${CMAKE_CURRENT_BINARY_DIR}/east-c.wasm
    DESTINATION ${CMAKE_CURRENT_SOURCE_DIR}/dist
)
