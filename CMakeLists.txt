cmake_minimum_required(VERSION 3.16)

project(FragFS
    VERSION 0.1.0
    DESCRIPTION "Zero-copy logical file aggregation layer"
    LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Global language settings
# ---------------------------------------------------------------------------
# We target C++17 on Linux and macOS. Extensions are disabled so that the code
# stays within the standard and behaves identically on both platforms.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default to RelWithDebInfo: we want optimised reads but usable stack traces
# while the project is still being built out.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        Debug Release RelWithDebInfo MinSizeRel)
endif()

# ---------------------------------------------------------------------------
# Warnings
# ---------------------------------------------------------------------------
# An INTERFACE target lets us opt targets in individually, so third-party
# dependencies (added later) are never forced to satisfy our warning policy.
add_library(fragfs_warnings INTERFACE)
target_compile_options(fragfs_warnings INTERFACE
    $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wconversion
    >)

# ---------------------------------------------------------------------------
# Sanitizers
# ---------------------------------------------------------------------------
# Optional AddressSanitizer + UndefinedBehaviorSanitizer build. Used by CI and
# available locally to catch out-of-bounds accesses and integer UB while the
# mapping and parsing code is still small.
option(FRAGFS_ENABLE_SANITIZERS
    "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
if(FRAGFS_ENABLE_SANITIZERS)
    # Note: the sanitizer flags are listed one per generator expression. A
    # comma inside a generator-expression value (e.g. `address,undefined`)
    # would be parsed as an argument separator, so it must be split.
    add_compile_options(
        $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-fsanitize=address>
        $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-fsanitize=undefined>
        $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-fno-omit-frame-pointer>
    )
    add_link_options(
        $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-fsanitize=address>
        $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-fsanitize=undefined>
    )
endif()

# ---------------------------------------------------------------------------
# Core library
# ---------------------------------------------------------------------------
# fragfs_core contains everything except the CLI and (later) the filesystem
# adapters. It must never depend on FUSE or on any CLI concerns.
add_library(fragfs_core STATIC
    src/core/error.cpp
    src/core/fragment.cpp
    src/core/mapper.cpp
    src/core/metadata.cpp
    src/core/version.cpp
    src/platform/posix_file.cpp)

target_include_directories(fragfs_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

# std::filesystem requires an explicit library on GCC before 9.
target_link_libraries(fragfs_core PUBLIC
    $<$<AND:$<CXX_COMPILER_ID:GNU>,$<VERSION_LESS:$<CXX_COMPILER_VERSION>,9>>:stdc++fs>)

target_link_libraries(fragfs_core PRIVATE fragfs_warnings)

# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
add_executable(fragfs src/cli/main.cpp)
target_link_libraries(fragfs PRIVATE fragfs_core fragfs_warnings)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
enable_testing()
add_subdirectory(tests)

# ---------------------------------------------------------------------------
# Benchmarks
# ---------------------------------------------------------------------------
option(FRAGFS_BUILD_BENCHMARKS "Build the FragFS benchmark suite" ON)
if(FRAGFS_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()
