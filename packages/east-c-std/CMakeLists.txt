add_library(east-c-std
    src/console.c
    src/fs.c
    src/path.c
    src/crypto.c
    src/time_ops.c
    src/random.c
    src/fetch.c
    src/parallel.c
    src/test.c
    src/register_all.c
)

target_include_directories(east-c-std PUBLIC include)
find_package(Threads REQUIRED)
target_link_libraries(east-c-std PUBLIC east-c PRIVATE m Threads::Threads)

# Optional libcurl for fetch
find_package(CURL QUIET)
if(CURL_FOUND)
    target_link_libraries(east-c-std PRIVATE CURL::libcurl)
    target_compile_definitions(east-c-std PRIVATE EAST_HAS_CURL=1)
endif()

# Tests
add_executable(test_console tests/test_console.c)
target_link_libraries(test_console east-c-std)
add_test(NAME test_console COMMAND test_console)

add_executable(test_fs tests/test_fs.c)
target_link_libraries(test_fs east-c-std)
add_test(NAME test_fs COMMAND test_fs)

add_executable(test_platform tests/test_platform.c)
target_link_libraries(test_platform east-c-std)
add_test(NAME test_platform COMMAND test_platform)

# Compliance test runner for std platform functions
add_executable(test_std_compliance tests/test_compliance.c)
target_link_libraries(test_std_compliance east-c-std)
