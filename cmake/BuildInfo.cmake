# BuildInfo - configure-time build identity and Windows version resources.
#
# This file intentionally avoids personal and host identity. The generated
# values describe only source identity, build provenance, target platform, and
# whether the checkout had uncommitted changes at configure time.
#
# C++ consumers may link `harken_buildinfo` and include
# `harken_buildinfo.h`. Windows binaries should call
# `harken_attach_version_resource(<target> <description> <original-filename> [product])`
# after target creation so the same identity is visible in file properties.

set(_bi_version_file "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
if(NOT EXISTS "${_bi_version_file}")
    message(FATAL_ERROR "[BuildInfo] VERSION file missing at ${_bi_version_file}")
endif()
file(READ "${_bi_version_file}" HARKEN_VERSION)
string(STRIP "${HARKEN_VERSION}" HARKEN_VERSION)
if(NOT HARKEN_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "[BuildInfo] VERSION must contain MAJOR.MINOR.PATCH without a channel suffix")
endif()

set(_bi_hub_version_file "${CMAKE_CURRENT_SOURCE_DIR}/HarkenHub/VERSION")
if(NOT EXISTS "${_bi_hub_version_file}")
    message(FATAL_ERROR "[BuildInfo] VERSION file missing at ${_bi_hub_version_file}")
endif()
file(READ "${_bi_hub_version_file}" HARKEN_HUB_VERSION)
string(STRIP "${HARKEN_HUB_VERSION}" HARKEN_HUB_VERSION)
if(NOT HARKEN_HUB_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "[BuildInfo] HarkenHub/VERSION must contain MAJOR.MINOR.PATCH without a channel suffix")
endif()

# --- git source identity ---
set(HARKEN_BI_GIT_COMMIT "unknown")
set(HARKEN_BI_GIT_COMMIT_SHORT "unknown")
set(HARKEN_BI_GIT_DIRTY "false")
set(_bi_git_branch "")

find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE _bi_git_commit
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)

    if(_bi_git_commit)
        set(HARKEN_BI_GIT_COMMIT "${_bi_git_commit}")
        string(SUBSTRING "${HARKEN_BI_GIT_COMMIT}" 0 8 HARKEN_BI_GIT_COMMIT_SHORT)
    endif()

    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --abbrev-ref HEAD
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE _bi_git_branch
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)

    execute_process(
        COMMAND "${GIT_EXECUTABLE}" status --porcelain
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE _bi_status_out
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(_bi_status_out)
        set(HARKEN_BI_GIT_DIRTY "true")
    endif()

    # Reconfigure when HEAD moves or the current branch ref advances.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/.git/HEAD")
    if(_bi_git_branch)
        set(_bi_ref "${CMAKE_SOURCE_DIR}/.git/refs/heads/${_bi_git_branch}")
        if(EXISTS "${_bi_ref}")
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_bi_ref}")
        endif()
    endif()
endif()

# --- timestamp ---
string(TIMESTAMP HARKEN_BI_TIMESTAMP         "%Y-%m-%dT%H:%M:%SZ" UTC)
string(TIMESTAMP HARKEN_BI_TIMESTAMP_COMPACT "%Y%m%dT%H%M%SZ"    UTC)
string(TIMESTAMP HARKEN_BI_YEAR              "%Y"                 UTC)

# --- target platform (windows-x64 / macos-arm64 / linux-x64 / ...) ---
string(TOLOWER "${CMAKE_SYSTEM_NAME}" _bi_sys_lower)
if(_bi_sys_lower STREQUAL "darwin")
    set(_bi_os_short "macos")
elseif(_bi_sys_lower STREQUAL "windows")
    set(_bi_os_short "windows")
else()
    set(_bi_os_short "${_bi_sys_lower}")
endif()

set(_bi_target_processor "${CMAKE_SYSTEM_PROCESSOR}")
if(_bi_sys_lower STREQUAL "darwin" AND CMAKE_OSX_ARCHITECTURES)
    set(_bi_osx_architectures ${CMAKE_OSX_ARCHITECTURES})
    list(REMOVE_DUPLICATES _bi_osx_architectures)
    list(LENGTH _bi_osx_architectures _bi_osx_arch_count)
    if(_bi_osx_arch_count EQUAL 1)
        list(GET _bi_osx_architectures 0 _bi_target_processor)
    elseif(_bi_osx_arch_count GREATER 1)
        set(_bi_target_processor "universal")
    endif()
endif()

string(TOLOWER "${_bi_target_processor}" _bi_proc_lower)
if(_bi_proc_lower MATCHES "^(amd64|x86_64|x64)$")
    set(_bi_arch "x64")
elseif(_bi_proc_lower MATCHES "^(arm64|aarch64)$")
    set(_bi_arch "arm64")
elseif(_bi_proc_lower MATCHES "^(x86|i386|i686)$")
    set(_bi_arch "x86")
else()
    set(_bi_arch "${_bi_proc_lower}")
endif()
set(HARKEN_BI_TARGET_PLATFORM "${_bi_os_short}-${_bi_arch}")

# --- build type, build channel & CI metadata ---
set(HARKEN_BI_BUILD_TYPE_VALUES local ci)
string(JOIN ", " HARKEN_BI_BUILD_TYPE_VALUES_TEXT
    ${HARKEN_BI_BUILD_TYPE_VALUES})

if("$ENV{GITHUB_ACTIONS}" STREQUAL "true")
    set(HARKEN_BI_BUILD_TYPE "ci")
else()
    set(HARKEN_BI_BUILD_TYPE "local")
endif()

set(HARKEN_BI_BUILD_CHANNEL_VALUES stable beta dev nightly)
string(JOIN ", " HARKEN_BI_BUILD_CHANNEL_VALUES_TEXT
    ${HARKEN_BI_BUILD_CHANNEL_VALUES})

# Precedence: -DHARKEN_BUILD_CHANNEL=... > $ENV{HARKEN_BUILD_CHANNEL} > dev.
if(NOT DEFINED HARKEN_BUILD_CHANNEL)
    if(DEFINED ENV{HARKEN_BUILD_CHANNEL} AND NOT "$ENV{HARKEN_BUILD_CHANNEL}" STREQUAL "")
        set(HARKEN_BUILD_CHANNEL "$ENV{HARKEN_BUILD_CHANNEL}")
    else()
        set(HARKEN_BUILD_CHANNEL "dev")
    endif()
endif()
if(HARKEN_BUILD_CHANNEL STREQUAL "")
    set(HARKEN_BUILD_CHANNEL "dev")
endif()
list(FIND HARKEN_BI_BUILD_CHANNEL_VALUES "${HARKEN_BUILD_CHANNEL}"
    _bi_build_channel_index)
if(_bi_build_channel_index EQUAL -1)
    message(FATAL_ERROR
        "[BuildInfo] HARKEN_BUILD_CHANNEL must be one of: "
        "${HARKEN_BI_BUILD_CHANNEL_VALUES_TEXT}; got '${HARKEN_BUILD_CHANNEL}'")
endif()
set(HARKEN_BI_BUILD_CHANNEL "${HARKEN_BUILD_CHANNEL}")

set(HARKEN_BI_CI_RUN_NUMBER "$ENV{HARKEN_CI_RUN_NUMBER}")
if(HARKEN_BI_CI_RUN_NUMBER STREQUAL "" AND NOT "$ENV{GITHUB_RUN_NUMBER}" STREQUAL "")
    set(HARKEN_BI_CI_RUN_NUMBER "$ENV{GITHUB_RUN_NUMBER}")
endif()
set(HARKEN_BI_CI_RUN_ATTEMPT "$ENV{HARKEN_CI_RUN_ATTEMPT}")
if(HARKEN_BI_CI_RUN_ATTEMPT STREQUAL "" AND NOT "$ENV{GITHUB_RUN_ATTEMPT}" STREQUAL "")
    set(HARKEN_BI_CI_RUN_ATTEMPT "$ENV{GITHUB_RUN_ATTEMPT}")
endif()
if(HARKEN_BI_CI_RUN_ATTEMPT STREQUAL "")
    set(HARKEN_BI_CI_RUN_ATTEMPT "1")
endif()

# --- stable readable build id ---
if(HARKEN_BI_BUILD_TYPE STREQUAL "ci")
    if(HARKEN_BI_CI_RUN_NUMBER STREQUAL "")
        set(_bi_run_component "run0")
    else()
        set(_bi_run_component "run${HARKEN_BI_CI_RUN_NUMBER}")
    endif()
    if(NOT HARKEN_BI_CI_RUN_ATTEMPT STREQUAL "1")
        set(_bi_run_component "${_bi_run_component}.${HARKEN_BI_CI_RUN_ATTEMPT}")
    endif()
    set(HARKEN_BI_BUILD_ID "${HARKEN_BI_BUILD_CHANNEL}-${HARKEN_BI_TARGET_PLATFORM}-${_bi_run_component}-${HARKEN_BI_GIT_COMMIT_SHORT}-${HARKEN_BI_TIMESTAMP_COMPACT}")
else()
    set(HARKEN_BI_BUILD_ID "local-${HARKEN_BI_TARGET_PLATFORM}-${HARKEN_BI_GIT_COMMIT_SHORT}-${HARKEN_BI_TIMESTAMP_COMPACT}")
endif()
if(HARKEN_BI_GIT_DIRTY STREQUAL "true")
    set(HARKEN_BI_BUILD_ID "${HARKEN_BI_BUILD_ID}.dirty")
endif()
set(HARKEN_BI_LABEL "${HARKEN_BI_BUILD_ID}")

# --- 4-part semver for PE VERSIONINFO (major.minor.patch.build) ---
string(REGEX MATCH "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$" _match "${HARKEN_VERSION}")
set(HARKEN_BI_VERSION_MAJOR "${CMAKE_MATCH_1}")
set(HARKEN_BI_VERSION_MINOR "${CMAKE_MATCH_2}")
set(HARKEN_BI_VERSION_PATCH "${CMAKE_MATCH_3}")
if(HARKEN_BI_CI_RUN_NUMBER MATCHES "^[0-9]+$")
    math(EXPR HARKEN_BI_VERSION_BUILD "${HARKEN_BI_CI_RUN_NUMBER} % 65536")
else()
    set(HARKEN_BI_VERSION_BUILD "0")
endif()
set(HARKEN_BI_VERSION_QUAD "${HARKEN_BI_VERSION_MAJOR}.${HARKEN_BI_VERSION_MINOR}.${HARKEN_BI_VERSION_PATCH}.${HARKEN_BI_VERSION_BUILD}")

string(REGEX MATCH "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$" _match "${HARKEN_HUB_VERSION}")
set(HARKEN_BI_HUB_VERSION_MAJOR "${CMAKE_MATCH_1}")
set(HARKEN_BI_HUB_VERSION_MINOR "${CMAKE_MATCH_2}")
set(HARKEN_BI_HUB_VERSION_PATCH "${CMAKE_MATCH_3}")
set(HARKEN_BI_HUB_VERSION_QUAD "${HARKEN_BI_HUB_VERSION_MAJOR}.${HARKEN_BI_HUB_VERSION_MINOR}.${HARKEN_BI_HUB_VERSION_PATCH}.${HARKEN_BI_VERSION_BUILD}")

set(_bi_fileflags 0)
if(HARKEN_BI_BUILD_TYPE STREQUAL "local" OR HARKEN_BI_GIT_DIRTY STREQUAL "true")
    math(EXPR _bi_fileflags "${_bi_fileflags} | 0x8") # VS_FF_PRIVATEBUILD
endif()
if(NOT HARKEN_BI_BUILD_CHANNEL STREQUAL "stable")
    math(EXPR _bi_fileflags "${_bi_fileflags} | 0x2") # VS_FF_PRERELEASE
endif()
set(HARKEN_BI_FILEFLAGS "${_bi_fileflags}L")

set(_bi_hub_fileflags 0)
if(HARKEN_BI_BUILD_TYPE STREQUAL "local" OR HARKEN_BI_GIT_DIRTY STREQUAL "true")
    math(EXPR _bi_hub_fileflags "${_bi_hub_fileflags} | 0x8") # VS_FF_PRIVATEBUILD
endif()
if(NOT HARKEN_BI_BUILD_CHANNEL STREQUAL "stable")
    math(EXPR _bi_hub_fileflags "${_bi_hub_fileflags} | 0x2") # VS_FF_PRERELEASE
endif()
set(HARKEN_BI_HUB_FILEFLAGS "${_bi_hub_fileflags}L")

# --- generate BuildInfo.h ---
set(HARKEN_BI_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated/harken_buildinfo")
file(MAKE_DIRECTORY "${HARKEN_BI_GENERATED_DIR}")
configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/BuildInfo.h.in"
    "${HARKEN_BI_GENERATED_DIR}/harken_buildinfo.h"
    @ONLY)

add_library(harken_buildinfo INTERFACE)
target_include_directories(harken_buildinfo INTERFACE "${HARKEN_BI_GENERATED_DIR}")

function(_harken_rc_escape OUT_VAR VALUE)
    string(REPLACE "\\" "\\\\" _escaped "${VALUE}")
    string(REPLACE "\"" "\\\"" _escaped "${_escaped}")
    set(${OUT_VAR} "${_escaped}" PARENT_SCOPE)
endfunction()

_harken_rc_escape(HARKEN_RC_VERSION "${HARKEN_VERSION}")
_harken_rc_escape(HARKEN_RC_VERSION_QUAD "${HARKEN_BI_VERSION_QUAD}")
_harken_rc_escape(HARKEN_RC_HUB_VERSION "${HARKEN_HUB_VERSION}")
_harken_rc_escape(HARKEN_RC_HUB_VERSION_QUAD "${HARKEN_BI_HUB_VERSION_QUAD}")
_harken_rc_escape(HARKEN_RC_BUILD_ID "${HARKEN_BI_BUILD_ID}")
_harken_rc_escape(HARKEN_RC_BUILD_TYPE "${HARKEN_BI_BUILD_TYPE}")
_harken_rc_escape(HARKEN_RC_BUILD_CHANNEL "${HARKEN_BI_BUILD_CHANNEL}")
_harken_rc_escape(HARKEN_RC_GIT_COMMIT "${HARKEN_BI_GIT_COMMIT}")
_harken_rc_escape(HARKEN_RC_GIT_COMMIT_SHORT "${HARKEN_BI_GIT_COMMIT_SHORT}")
_harken_rc_escape(HARKEN_RC_GIT_DIRTY "${HARKEN_BI_GIT_DIRTY}")
_harken_rc_escape(HARKEN_RC_TARGET_PLATFORM "${HARKEN_BI_TARGET_PLATFORM}")
_harken_rc_escape(HARKEN_RC_TIMESTAMP "${HARKEN_BI_TIMESTAMP}")
_harken_rc_escape(HARKEN_RC_CI_RUN_NUMBER "${HARKEN_BI_CI_RUN_NUMBER}")
_harken_rc_escape(HARKEN_RC_CI_RUN_ATTEMPT "${HARKEN_BI_CI_RUN_ATTEMPT}")

set(HARKEN_VERSIONED_BINARY_TARGETS "")

function(harken_attach_version_resource TARGET_NAME FILE_DESCRIPTION ORIGINAL_FILENAME)
    if(NOT TARGET "${TARGET_NAME}")
        message(FATAL_ERROR "[BuildInfo] target '${TARGET_NAME}' does not exist")
    endif()

    get_target_property(_harken_target_type "${TARGET_NAME}" TYPE)
    if(NOT _harken_target_type MATCHES "^(EXECUTABLE|SHARED_LIBRARY|MODULE_LIBRARY)$")
        message(FATAL_ERROR
            "[BuildInfo] target '${TARGET_NAME}' is ${_harken_target_type}; "
            "version resources are only valid for executable or DLL-like targets")
    endif()

    set_property(GLOBAL APPEND PROPERTY HARKEN_VERSIONED_BINARY_TARGETS "${TARGET_NAME}")
    set(_harken_versioned_targets ${HARKEN_VERSIONED_BINARY_TARGETS})
    list(APPEND _harken_versioned_targets "${TARGET_NAME}")
    set(HARKEN_VERSIONED_BINARY_TARGETS "${_harken_versioned_targets}" PARENT_SCOPE)

    if(NOT WIN32)
        return()
    endif()

    if(_harken_target_type STREQUAL "EXECUTABLE")
        set(HARKEN_RC_FILETYPE "0x1L") # VFT_APP
    else()
        set(HARKEN_RC_FILETYPE "0x2L") # VFT_DLL
    endif()
    if(ARGC GREATER 3 AND "${ARGV3}" STREQUAL "hub")
        set(HARKEN_BI_VERSION_MAJOR "${HARKEN_BI_HUB_VERSION_MAJOR}")
        set(HARKEN_BI_VERSION_MINOR "${HARKEN_BI_HUB_VERSION_MINOR}")
        set(HARKEN_BI_VERSION_PATCH "${HARKEN_BI_HUB_VERSION_PATCH}")
        set(HARKEN_BI_FILEFLAGS "${HARKEN_BI_HUB_FILEFLAGS}")
        set(HARKEN_RC_VERSION "${HARKEN_RC_HUB_VERSION}")
        set(HARKEN_RC_VERSION_QUAD "${HARKEN_RC_HUB_VERSION_QUAD}")
    endif()
    _harken_rc_escape(HARKEN_RC_FILE_DESCRIPTION "${FILE_DESCRIPTION}")
    _harken_rc_escape(HARKEN_RC_INTERNAL_NAME "${TARGET_NAME}")
    _harken_rc_escape(HARKEN_RC_ORIGINAL_FILENAME "${ORIGINAL_FILENAME}")

    set(_harken_rc_out "${HARKEN_BI_GENERATED_DIR}/${TARGET_NAME}_version.rc")
    configure_file(
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/VersionInfo.rc.in"
        "${_harken_rc_out}"
        @ONLY)
    target_sources("${TARGET_NAME}" PRIVATE "${_harken_rc_out}")
    source_group("Generated" FILES "${_harken_rc_out}")
endfunction()

message(STATUS "[BuildInfo] version=${HARKEN_VERSION} hub=${HARKEN_HUB_VERSION} build_type=${HARKEN_BI_BUILD_TYPE} build_channel=${HARKEN_BI_BUILD_CHANNEL} platform=${HARKEN_BI_TARGET_PLATFORM}")
message(STATUS "[BuildInfo] build_id=${HARKEN_BI_BUILD_ID}")

unset(_bi_status_out)
unset(_bi_git_commit)
unset(_bi_git_branch)
unset(_bi_ref)
unset(_bi_sys_lower)
unset(_bi_target_processor)
unset(_bi_osx_architectures)
unset(_bi_osx_arch_count)
unset(_bi_proc_lower)
unset(_bi_arch)
unset(_bi_os_short)
unset(_bi_run_component)
unset(_bi_build_channel_index)
unset(_bi_fileflags)
unset(_bi_hub_fileflags)
unset(_bi_version_file)
unset(_bi_hub_version_file)
