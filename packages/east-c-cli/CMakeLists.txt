add_executable(east-c-cli src/main.c)
set_target_properties(east-c-cli PROPERTIES OUTPUT_NAME east-c)
target_link_libraries(east-c-cli east-c-std)
target_compile_definitions(east-c-cli PRIVATE
    EAST_CLI_VERSION="${EAST_VERSION}"
    EAST_RUNTIME_VERSION="${EAST_VERSION}"
)
