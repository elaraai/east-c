add_library(east-c
    src/arena.c
    src/gc.c
    src/hashmap.c
    src/types.c
    src/values.c
    src/ir.c
    src/env.c
    src/compiler.c
    src/platform.c
    src/builtins/registry.c
    src/builtins/integer.c
    src/builtins/float_ops.c
    src/builtins/boolean.c
    src/builtins/string.c
    src/builtins/comparison.c
    src/builtins/datetime_ops.c
    src/builtins/blob.c
    src/builtins/array.c
    src/builtins/set_ops.c
    src/builtins/dict_ops.c
    src/builtins/ref_ops.c
    src/builtins/vector.c
    src/builtins/matrix.c
    src/builtins/patch.c
    src/serialization/json.c
    src/serialization/beast.c
    src/serialization/beast2.c
    src/serialization/csv.c
    src/serialization/east_parser.c
    src/serialization/east_printer.c
    src/serialization/east_tokenizer.c
    src/serialization/binary_utils.c
    src/type_of_type.c
)

target_include_directories(east-c PUBLIC include)
target_link_libraries(east-c PRIVATE m pcre2-8)
target_compile_definitions(east-c PRIVATE PCRE2_CODE_UNIT_WIDTH=8)

# Tests
add_executable(test_types tests/test_types.c)
target_link_libraries(test_types east-c)
add_test(NAME test_types COMMAND test_types)

add_executable(test_values tests/test_values.c)
target_link_libraries(test_values east-c)
add_test(NAME test_values COMMAND test_values)

add_executable(test_compiler tests/test_compiler.c)
target_link_libraries(test_compiler east-c)
add_test(NAME test_compiler COMMAND test_compiler)

add_executable(test_builtins tests/test_builtins.c)
target_link_libraries(test_builtins east-c)
add_test(NAME test_builtins COMMAND test_builtins)

add_executable(test_serialization tests/test_serialization.c)
target_link_libraries(test_serialization east-c)
add_test(NAME test_serialization COMMAND test_serialization)

# Compliance test (run manually with a JSON IR file)
add_executable(test_compliance tests/test_compliance.c)
target_link_libraries(test_compliance east-c)
