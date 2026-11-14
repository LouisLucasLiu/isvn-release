cmake_minimum_required(VERSION 3.16)

project(isvn VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(SQLite3 REQUIRED)

option(ISVN_BUILD_GUI "Build the Qt Quick GUI shell" OFF)

add_library(blake3_portable STATIC
    third_party/blake3/blake3.c
    third_party/blake3/blake3_dispatch.c
    third_party/blake3/blake3_portable.c
)

target_include_directories(blake3_portable PUBLIC third_party/blake3)
target_compile_definitions(blake3_portable PRIVATE
    BLAKE3_NO_SSE2
    BLAKE3_NO_SSE41
    BLAKE3_NO_AVX2
    BLAKE3_NO_AVX512
    BLAKE3_USE_NEON=0
)

add_library(isvn_common
    src/common/fs.cpp
    src/common/hash.cpp
    src/common/system.cpp
)
target_include_directories(isvn_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(isvn_common PUBLIC blake3_portable)
target_compile_options(isvn_common PRIVATE -Wall -Wextra -Wpedantic)

add_library(isvn_lineage
    src/version/lineage.cpp
)

target_include_directories(isvn_lineage
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(isvn_lineage
    PUBLIC
        SQLite::SQLite3
        isvn_common
)

target_compile_options(isvn_lineage
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
)

add_library(isvn_object_store
    src/storage/object_store.cpp
    src/storage/gc.cpp
)
target_include_directories(isvn_object_store PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(isvn_object_store PUBLIC SQLite::SQLite3 isvn_common)
target_compile_options(isvn_object_store PRIVATE -Wall -Wextra -Wpedantic)

add_library(isvn_svn_ops
    src/version/svn_ops.cpp
)
target_include_directories(isvn_svn_ops PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(isvn_svn_ops PRIVATE -Wall -Wextra -Wpedantic)

# ── Build / deployment identity ──
# Injected at configure time; missing git metadata falls back to "unknown" and never
# fails the build. Re-run cmake before a release build to refresh the commit/time.
#
# A source tree exported without git metadata (a release source tree) passes its
# commit explicitly with -DISVN_SOURCE_COMMIT=<40-hex sha>.  It takes precedence
# over git, and git is only consulted when this source directory is the top of
# its own working tree, so a tree unpacked inside another repository never
# reports that repository's commit.
set(ISVN_SOURCE_COMMIT "" CACHE STRING
    "Full source commit SHA-1 for builds from a tree without git metadata")
find_package(Git QUIET)
set(ISVN_GIT_COMMIT "unknown")
if(ISVN_SOURCE_COMMIT)
    if(NOT ISVN_SOURCE_COMMIT MATCHES "^[0-9a-f]+$")
        message(FATAL_ERROR "ISVN_SOURCE_COMMIT must be a full lowercase SHA-1")
    endif()
    string(LENGTH "${ISVN_SOURCE_COMMIT}" _isvn_commit_length)
    if(NOT _isvn_commit_length EQUAL 40)
        message(FATAL_ERROR "ISVN_SOURCE_COMMIT must be a full lowercase SHA-1")
    endif()
    string(SUBSTRING "${ISVN_SOURCE_COMMIT}" 0 7 ISVN_GIT_COMMIT)
elseif(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --show-toplevel
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE _isvn_toplevel OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET RESULT_VARIABLE _isvn_toplevel_rc)
    set(_isvn_git_rc 1)
    if(_isvn_toplevel_rc EQUAL 0)
        get_filename_component(_isvn_toplevel "${_isvn_toplevel}" REALPATH)
        get_filename_component(_isvn_source_dir "${CMAKE_CURRENT_SOURCE_DIR}" REALPATH)
        if(_isvn_toplevel STREQUAL _isvn_source_dir)
            execute_process(
                COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
                WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                OUTPUT_VARIABLE _isvn_commit OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET RESULT_VARIABLE _isvn_git_rc)
        endif()
    endif()
    if(_isvn_git_rc EQUAL 0 AND NOT _isvn_commit STREQUAL "")
        set(ISVN_GIT_COMMIT "${_isvn_commit}")
        # Flag a dirty working tree (tracked changes only) so a binary built from
        # uncommitted edits is not mistaken for a clean commit.
        execute_process(
            COMMAND ${GIT_EXECUTABLE} status --porcelain --untracked-files=no
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            OUTPUT_VARIABLE _isvn_dirty OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(NOT _isvn_dirty STREQUAL "")
            set(ISVN_GIT_COMMIT "${ISVN_GIT_COMMIT}-dirty")
        endif()
    endif()
endif()
string(TIMESTAMP ISVN_BUILD_TIME "%Y-%m-%dT%H:%M:%SZ" UTC)
if(CMAKE_BUILD_TYPE)
    set(ISVN_BUILD_TYPE_STR "${CMAKE_BUILD_TYPE}")
else()
    set(ISVN_BUILD_TYPE_STR "unknown")
endif()
# Keep the normal developer identity stable while permitting a separately
# configured, traceable release-candidate build.  Production release builds
# continue to choose their identity explicitly at configure time.
set(ISVN_APP_VERSION "0.1.0-dev" CACHE STRING
    "isvn application version embedded in CLI and GUI build identity")

add_library(isvn_version STATIC src/version.cpp)
target_include_directories(isvn_version PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(isvn_version PRIVATE
    ISVN_VERSION="${ISVN_APP_VERSION}"
    ISVN_GIT_COMMIT="${ISVN_GIT_COMMIT}"
    ISVN_BUILD_TIME="${ISVN_BUILD_TIME}"
    ISVN_BUILD_TYPE="${ISVN_BUILD_TYPE_STR}")
target_compile_options(isvn_version PRIVATE -Wall -Wextra -Wpedantic)

add_library(isvn_commands
    src/cli/commands.cpp
)
target_include_directories(isvn_commands PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(isvn_commands PUBLIC isvn_lineage isvn_object_store isvn_svn_ops isvn_version)
target_compile_options(isvn_commands PRIVATE -Wall -Wextra -Wpedantic)

add_executable(isvn
    src/cli/isvn_main.cpp
)
target_link_libraries(isvn PRIVATE isvn_commands)
target_compile_options(isvn PRIVATE -Wall -Wextra -Wpedantic)
install(TARGETS isvn RUNTIME DESTINATION .)

include(CTest)

if(BUILD_TESTING)
    add_executable(test_lineage tests/test_lineage.cpp)
    target_link_libraries(test_lineage PRIVATE isvn_lineage SQLite::SQLite3)
    target_compile_options(test_lineage PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME lineage COMMAND test_lineage)

    add_executable(test_object_store tests/test_object_store.cpp)
    target_link_libraries(test_object_store PRIVATE isvn_object_store SQLite::SQLite3)
    target_compile_options(test_object_store PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME object_store COMMAND test_object_store)

    add_executable(test_gc tests/test_gc.cpp)
    target_link_libraries(test_gc PRIVATE isvn_object_store SQLite::SQLite3)
    target_compile_definitions(test_gc PRIVATE ISVN_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_options(test_gc PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME gc COMMAND test_gc)

    add_executable(test_svn_ops tests/test_svn_ops.cpp)
    target_link_libraries(test_svn_ops PRIVATE isvn_svn_ops isvn_common)
    target_compile_definitions(test_svn_ops PRIVATE ISVN_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_options(test_svn_ops PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME svn_ops COMMAND test_svn_ops)

    add_executable(test_commands tests/test_commands.cpp)
    target_link_libraries(test_commands PRIVATE isvn_commands)
    target_compile_definitions(test_commands PRIVATE ISVN_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_options(test_commands PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME commands COMMAND test_commands)
endif()

if(ISVN_BUILD_GUI)
    add_subdirectory(gui)
endif()
