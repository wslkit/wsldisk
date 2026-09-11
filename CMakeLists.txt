cmake_minimum_required(VERSION 3.28)

# The vcpkg toolchain must be picked up before project(); CMakePresets.json sets
# CMAKE_TOOLCHAIN_FILE, this is the fallback for plain `cmake -S . -B build` users.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{VCPKG_ROOT})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE STRING "vcpkg toolchain file")
endif()

project(wsldisk
    VERSION 0.1.0
    DESCRIPTION "Compact, shrink, move, inspect and snapshot WSL2 virtual disks"
    HOMEPAGE_URL "https://github.com/wslkit/wsldisk"
    LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "wsldisk targets Windows only (it drives the Virtual Disk Service API).")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# No C++20 modules here. Turning the scan off drops a build step per source file
# and keeps compile_commands.json free of the `@...modmap` arguments that
# clang-tidy cannot consume.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
option(WSLDISK_BUILD_TESTS "Build the test suites" ON)
option(WSLDISK_ENABLE_COVERAGE "Instrument for clang-cl source-based coverage" OFF)
option(WSLDISK_ENABLE_ASAN "Build with AddressSanitizer" OFF)
option(WSLDISK_BUILD_FUZZERS "Build the libFuzzer targets (clang-cl only)" OFF)
option(WSLDISK_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(WSLDISK_ENABLE_ASAN)
    # /RTC1 is incompatible with the sanitizer.
    string(REPLACE "/RTC1" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
endif()

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

include(cmake/CompilerFlags.cmake)
include(cmake/Coverage.cmake)

add_subdirectory(src/lib)
add_subdirectory(src/cli)

if(WSLDISK_BUILD_TESTS)
    enable_testing()
    include(CTest)
    add_subdirectory(tests)
endif()
