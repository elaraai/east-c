cmake_minimum_required(VERSION 3.16)

# Read version from VERSION file
file(READ "${CMAKE_SOURCE_DIR}/VERSION" EAST_VERSION)
string(STRIP "${EAST_VERSION}" EAST_VERSION)

# Strip prerelease suffix (e.g. "-beta.0") for CMake project() which only accepts numeric versions
string(REGEX REPLACE "-.*" "" EAST_CMAKE_VERSION "${EAST_VERSION}")

project(east-c VERSION ${EAST_CMAKE_VERSION} LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Compiler warnings
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)
endif()

# Fetch PCRE2 for JavaScript-compatible regex
include(FetchContent)
FetchContent_Declare(
    pcre2
    URL https://github.com/PCRE2Project/pcre2/releases/download/pcre2-10.44/pcre2-10.44.tar.gz
    URL_HASH SHA256=86b9cb0aa3bcb7994faa88018292bc704cdbb708e785f7c74352ff6ea7d3175b
)
set(PCRE2_BUILD_PCRE2_8 ON CACHE BOOL "" FORCE)
set(PCRE2_BUILD_PCRE2_16 OFF CACHE BOOL "" FORCE)
set(PCRE2_BUILD_PCRE2_32 OFF CACHE BOOL "" FORCE)
set(PCRE2_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(PCRE2_BUILD_PCRE2GREP OFF CACHE BOOL "" FORCE)
set(PCRE2_SUPPORT_UNICODE ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(pcre2)

enable_testing()

add_subdirectory(packages/east-c)
add_subdirectory(packages/east-c-std)
add_subdirectory(packages/east-c-cli)
