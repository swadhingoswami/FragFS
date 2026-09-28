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
# Core library
# ---------------------------------------------------------------------------
# fragfs_core contains everything except the CLI and (later) the filesystem
# adapters. It must never depend on FUSE or on any CLI concerns.
add_library(fragfs_core STATIC
    src/core/fragment.cpp
    src/core/version.cpp)

target_include_directories(fragfs_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

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
