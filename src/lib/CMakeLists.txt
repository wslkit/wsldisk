# Configure-time git stamp. Re-running CMake refreshes it; a stale value in an
# incremental build is harmless because the version banner is informational.
find_package(Git QUIET)
set(WSLDISK_GIT_REVISION "unknown")
if(Git_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE git_revision
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE git_result)
    if(git_result EQUAL 0 AND NOT git_revision STREQUAL "")
        set(WSLDISK_GIT_REVISION "${git_revision}")
    endif()
endif()

configure_file(version.cpp.in "${CMAKE_CURRENT_BINARY_DIR}/version.cpp" @ONLY)

# The cache catalogue is data the tool carries rather than a file beside it, so
# `usage` and `clean` cannot get out of step with the binary and there is nothing
# extra to install. Editing the TOML re-runs CMake, because it is listed as a
# configure dependency.
set(caches_toml "${CMAKE_SOURCE_DIR}/data/caches.toml")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${caches_toml}")
file(READ "${caches_toml}" WSLDISK_CACHES_TOML)
configure_file(model/catalogue_data.h.in
               "${CMAKE_CURRENT_BINARY_DIR}/generated/model/catalogue_data.h" @ONLY)

add_library(wsldisk_lib STATIC
    errors.cpp
    errors.h
    interfaces.h
    version.h
    "${CMAKE_CURRENT_BINARY_DIR}/version.cpp"

    model/catalogue.cpp
    model/catalogue.h
    model/config.cpp
    model/config.h
    model/disk_info.cpp
    model/disk_info.h
    model/distro.cpp
    model/distro.h
    model/orphans.cpp
    model/orphans.h
    model/size.cpp
    model/size.h
    model/text.cpp
    model/text.h
    model/wsl_output.cpp
    model/wsl_output.h

    ops/compact.cpp
    ops/compact.h
    ops/operation.cpp
    ops/operation.h
    ops/move.cpp
    ops/move.h
    ops/relink.cpp
    ops/relink.h
    ops/runner.cpp
    ops/runner.h
    ops/trim.cpp
    ops/trim.h
    ops/usage.cpp
    ops/usage.h

    platform/clock.cpp
    platform/clock.h
    platform/editor.cpp
    platform/editor.h
    platform/elevation.cpp
    platform/elevation.h
    platform/filesystem.cpp
    platform/filesystem.h
    platform/registry.cpp
    platform/registry.h
    platform/scoped_handle.h
    platform/virtual_disk.cpp
    platform/virtual_disk.h
    platform/win32_api.cpp
    platform/win32_api.h
    platform/win32_error.cpp
    platform/win32_error.h
    platform/wsl_host.cpp
    platform/wsl_host.h)

add_library(wsldisk::lib ALIAS wsldisk_lib)

target_include_directories(wsldisk_lib
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
    PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/model")

# VirtDisk.lib is the Virtual Disk Service; it is not in the default link set.
# toml++ is PRIVATE: config.cpp and catalogue.cpp are the only translation units
# that parse TOML, and what they hand back is plain C++.
find_package(tomlplusplus CONFIG REQUIRED)
target_link_libraries(wsldisk_lib
    PUBLIC wsldisk::flags
    # advapi32: token and SID checks behind --elevate. shell32: ShellExecuteEx
    # with "runas", the only way to ask for an administrator token. bcrypt: the
    # 128 random bits in the result pipe's name.
    PRIVATE virtdisk uuid advapi32 shell32 bcrypt tomlplusplus::tomlplusplus)

set_target_properties(wsldisk_lib PROPERTIES FOLDER "wsldisk")
