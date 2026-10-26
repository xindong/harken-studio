cmake_minimum_required(VERSION 3.24)

file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/.cmake/api/v1/query")
if(COMMAND cmake_file_api)
    cmake_file_api(QUERY API_VERSION 1 CODEMODEL 2 CACHE 2 TOOLCHAINS 1)
else()
    foreach(_release_query codemodel-v2 cache-v2 toolchains-v1)
        file(WRITE "${CMAKE_BINARY_DIR}/.cmake/api/v1/query/${_release_query}" "")
    endforeach()
endif()

project(HarkenCore LANGUAGES C CXX)

if(APPLE)
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Both toolchains use the MSVC frontend; /utf-8 keeps source and execution
# charsets deterministic (repository files are UTF-8 without BOM).
if(MSVC)
    add_compile_options(/utf-8)
endif()

# One predictable exit per build tree: executables and shared libraries
# (Windows DLLs) land in bin/, static and import libraries in lib/.
# Multi-config generators append the configuration name (bin/Debug).
# Collocation is load-bearing in two places: the editor exe resolves the
# engine DLL, the deployed Qt runtime, and the project templates next to
# itself, and harken_host resolves packc next to itself.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Sanitizer support (optional)
# Flags are applied globally via add_compile_options/add_link_options so that
# all targets declared AFTER this point inherit them (Runtime, Editor, Tests).
option(HARKEN_ENABLE_ASAN  "Enable AddressSanitizer"             OFF)
option(HARKEN_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer"   OFF)
option(HARKEN_ENABLE_TSAN  "Enable ThreadSanitizer"              OFF)

# TSan is incompatible with ASan/UBSan runtimes
if(HARKEN_ENABLE_TSAN AND (HARKEN_ENABLE_ASAN OR HARKEN_ENABLE_UBSAN))
    message(FATAL_ERROR "HARKEN_ENABLE_TSAN is mutually exclusive with HARKEN_ENABLE_ASAN / HARKEN_ENABLE_UBSAN")
endif()

if(HARKEN_ENABLE_ASAN)
    if(MSVC)
        # cl.exe and clang-cl both accept /fsanitize=address; runtime auto-linked by driver.
        add_compile_options(/fsanitize=address)
        # /RTC1 (default in Debug) is incompatible with ASan — strip it
        string(REPLACE "/RTC1" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
        string(REPLACE "/RTC1" "" CMAKE_C_FLAGS_DEBUG   "${CMAKE_C_FLAGS_DEBUG}")
    else()
        add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address)
    endif()
endif()

if(HARKEN_ENABLE_UBSAN)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        # Exclude vptr (RTTI dependency) and function (Windows clang-cl quirks).
        # -fno-sanitize-recover makes the process abort on UB so CI actually fails.
        #
        # Frame-pointer flag is compiler-driver-dependent:
        #   - GCC/Clang driver: -fno-omit-frame-pointer
        #   - clang-cl (MSVC-compat mode): /Oy- (clang-cl emits "unknown argument"
        #     warning for the GCC-style flag, which becomes fatal under googletest's /WX)
        set(_harken_ubsan_flags
            -fsanitize=undefined
            -fno-sanitize=vptr,function
            -fno-sanitize-recover=undefined)
        add_compile_options(${_harken_ubsan_flags})
        if(MSVC)
            add_compile_options(/Oy-)
        else()
            add_compile_options(-fno-omit-frame-pointer)
        endif()
        # Linux/macOS: link goes through the clang/gcc driver, which needs these
        # flags to inject the UBSan runtime. On Windows (MSVC/clang-cl), CMake
        # calls lld-link directly — it would warn "ignoring unknown argument".
        # Windows runtime is wired explicitly via /defaultlib below.
        if(NOT MSVC)
            add_link_options(${_harken_ubsan_flags} -fno-omit-frame-pointer)
        endif()
        unset(_harken_ubsan_flags)
    else()
        message(WARNING "HARKEN_ENABLE_UBSAN requires Clang or GCC; ${CMAKE_CXX_COMPILER_ID} ignored")
    endif()
endif()

if(HARKEN_ENABLE_TSAN)
    if(WIN32)
        message(FATAL_ERROR "ThreadSanitizer is not supported on Windows (compiler-rt does not ship a Windows TSan runtime)")
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
        add_link_options(-fsanitize=thread)
    else()
        message(FATAL_ERROR "HARKEN_ENABLE_TSAN requires Clang or GCC")
    endif()
endif()

# Windows clang-cl: link goes through CMake's vs_link_exe → lld-link directly,
# bypassing the clang-cl driver. The driver normally injects sanitizer runtime
# libs and resource-dir LIBPATH; without it, we must do both manually.
# - ASan: .obj files contain "#pragma comment(lib, clang_rt.asan_dynamic-x86_64)",
#         but lld-link can't find them without /LIBPATH to the LLVM resource dir.
# - UBSan: -fsanitize=undefined is Clang-style and does NOT emit defaultlib pragma,
#         so we must explicitly link clang_rt.ubsan_standalone*.lib.
if((HARKEN_ENABLE_ASAN OR HARKEN_ENABLE_UBSAN)
   AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang"
   AND MSVC)
    execute_process(
        COMMAND "${CMAKE_CXX_COMPILER}" /clang:-print-resource-dir
        OUTPUT_VARIABLE _harken_clang_resource_dir
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _harken_clang_rd_rc
    )
    if(NOT _harken_clang_rd_rc EQUAL 0 OR _harken_clang_resource_dir STREQUAL "")
        message(FATAL_ERROR "Failed to detect clang-cl resource directory for sanitizer LIBPATH (rc=${_harken_clang_rd_rc})")
    endif()
    file(TO_CMAKE_PATH "${_harken_clang_resource_dir}" _harken_clang_resource_dir)
    set(_harken_sanitizer_lib_dir "${_harken_clang_resource_dir}/lib/windows")
    if(NOT EXISTS "${_harken_sanitizer_lib_dir}")
        message(FATAL_ERROR "clang-cl sanitizer lib dir not found: ${_harken_sanitizer_lib_dir}")
    endif()
    add_link_options("/LIBPATH:${_harken_sanitizer_lib_dir}")
    message(STATUS "[Harken] Sanitizer LIBPATH: ${_harken_sanitizer_lib_dir}")

    if(HARKEN_ENABLE_ASAN)
        # MS-recommended ASan link recipe for /MD dynamic CRT
        # (https://learn.microsoft.com/cpp/sanitizers/asan-runtime):
        #  - clang_rt.asan_dynamic-x86_64.lib: ASan dynamic runtime (interceptors, __asan_init,
        #    __asan_report_*, __asan_memset/memcpy, etc.). Normally pulled via clang-cl pragma,
        #    but pulling it explicitly avoids surprises when pragma-driven /defaultlib doesn't
        #    fire (e.g. when /wholearchive on a sibling lib confuses dependency walking).
        #  - clang_rt.asan_dynamic_runtime_thunk-x86_64.lib: stack/shadow ops
        #    (__asan_stack_malloc_N, __asan_set_shadow_*) emitted by compiler-generated stack
        #    instrumentation. Must be /wholearchive — otherwise the linker only pulls referenced
        #    symbols and misses thunks called from code not yet linked.
        add_link_options("/defaultlib:clang_rt.asan_dynamic-x86_64.lib")
        add_link_options("/wholearchive:clang_rt.asan_dynamic_runtime_thunk-x86_64.lib")
    endif()
    if(HARKEN_ENABLE_UBSAN AND NOT HARKEN_ENABLE_ASAN)
        # Explicit UBSan runtime — Clang-style -fsanitize=undefined doesn't emit defaultlib pragma.
        # Only link clang_rt.ubsan_standalone-x86_64.lib (C runtime). The _cxx variant covers
        # vptr/function checks (which we disabled) AND is shipped only as /MT (static CRT),
        # which conflicts with the project's /MD (dynamic CRT) — linking it triggers
        # "/failifmismatch: RuntimeLibrary MD_DynamicRelease vs MT_StaticRelease".
        #
        # When ASan is also enabled, do NOT link ubsan_standalone — clang_rt.asan_dynamic
        # already provides the UBSan handlers AND declares
        # /alternatename:__ubsan_default_options=__ubsan_default_options__def itself;
        # adding ubsan_standalone redeclares the same alternatename → lld-link conflict.
        add_link_options("/defaultlib:clang_rt.ubsan_standalone-x86_64.lib")
    endif()

    unset(_harken_clang_resource_dir)
    unset(_harken_sanitizer_lib_dir)
    unset(_harken_clang_rd_rc)
endif()

option(HARKEN_BUILD_CORE "Build the Harken Core runtime" OFF)
option(HARKEN_BUILD_SHARED "Build the HarkenCore shared library (C# binding native lib)" OFF)
option(HARKEN_BUILD_EDITOR "Build the Harken Studio editor" OFF)
option(HARKEN_BUILD_HUB "Build the Harken Hub application" OFF)
option(HARKEN_BUILD_TOOLS "Build the host, compiler, and preview tooling" OFF)
option(HARKEN_BUILD_RUNTIME_UNIT_TESTS "Build the runtime correctness tests" OFF)
option(HARKEN_BUILD_EDITOR_UNIT_TESTS "Build Editor/Qt unit tests" OFF)
option(HARKEN_BUILD_HUB_UNIT_TESTS "Build Hub unit tests" OFF)
option(HARKEN_BUILD_RUNTIME_BENCHMARKS "Build the runtime performance benchmarks" OFF)
option(HARKEN_BUILD_RT_QUALIFICATION
       "Build the private real-time qualification Runtime and runner" OFF)

if(HARKEN_RUNTIME_ONLY)
    message(FATAL_ERROR
        "HARKEN_RUNTIME_ONLY has been removed. Select HARKEN_BUILD_CORE, "
        "HARKEN_BUILD_EDITOR, HARKEN_BUILD_HUB, HARKEN_BUILD_TOOLS, and the "
        "HARKEN_BUILD_*_UNIT_TESTS options explicitly.")
endif()

foreach(_harken_core_consumer
        HARKEN_BUILD_EDITOR
        HARKEN_BUILD_RUNTIME_UNIT_TESTS
        HARKEN_BUILD_RUNTIME_BENCHMARKS
        HARKEN_BUILD_RT_QUALIFICATION
        HARKEN_BUILD_TOOLS
        HARKEN_BUILD_SHARED)
    if(${_harken_core_consumer} AND NOT HARKEN_BUILD_CORE)
        message(FATAL_ERROR "${_harken_core_consumer} requires HARKEN_BUILD_CORE=ON")
    endif()
endforeach()

foreach(_harken_tools_consumer HARKEN_BUILD_EDITOR HARKEN_BUILD_RUNTIME_UNIT_TESTS)
    if(${_harken_tools_consumer} AND NOT HARKEN_BUILD_TOOLS)
        message(FATAL_ERROR "${_harken_tools_consumer} requires HARKEN_BUILD_TOOLS=ON")
    endif()
endforeach()

if(HARKEN_BUILD_EDITOR_UNIT_TESTS AND NOT HARKEN_BUILD_EDITOR)
    message(FATAL_ERROR "HARKEN_BUILD_EDITOR_UNIT_TESTS requires HARKEN_BUILD_EDITOR=ON")
endif()
if(HARKEN_BUILD_HUB_UNIT_TESTS AND NOT HARKEN_BUILD_HUB)
    message(FATAL_ERROR "HARKEN_BUILD_HUB_UNIT_TESTS requires HARKEN_BUILD_HUB=ON")
endif()

if(ANDROID OR CMAKE_SYSTEM_NAME STREQUAL "iOS")
    foreach(_harken_desktop_consumer
            HARKEN_BUILD_EDITOR
            HARKEN_BUILD_HUB
            HARKEN_BUILD_TOOLS
            HARKEN_BUILD_RT_QUALIFICATION)
        if(${_harken_desktop_consumer})
            message(FATAL_ERROR "${_harken_desktop_consumer} requires a desktop target platform")
        endif()
    endforeach()
endif()

option(HARKEN_RT_PROBE_DEVICE_CALLBACK
       "Instrument Runtime-owned work inside the production device callback" OFF)
option(HARKEN_RT_PROBE_MIX_PUBLICATION
       "Instrument the end-of-block Runtime publication window" OFF)
option(HARKEN_RT_PROBE_WORKER_BARRIER
       "Instrument the existing render-worker completion barrier" OFF)

set(HARKEN_RT_PROBE_DEFINITIONS "")
foreach(_harken_rt_probe_control
        HARKEN_RT_PROBE_DEVICE_CALLBACK
        HARKEN_RT_PROBE_MIX_PUBLICATION
        HARKEN_RT_PROBE_WORKER_BARRIER)
    if(${_harken_rt_probe_control})
        list(APPEND HARKEN_RT_PROBE_DEFINITIONS
             ${_harken_rt_probe_control})
    endif()
endforeach()
if(HARKEN_RT_PROBE_DEFINITIONS AND NOT HARKEN_BUILD_RT_QUALIFICATION)
    message(FATAL_ERROR
        "HARKEN_RT_PROBE_* controls require "
        "HARKEN_BUILD_RT_QUALIFICATION=ON; shipping Runtime targets never "
        "accept probe definitions")
endif()

# Engine modules live under HarkenEngine/Source/Runtime (project convention).
set(HARKEN_RUNTIME_DIR ${CMAKE_CURRENT_SOURCE_DIR}/HarkenEngine/Source/Runtime)
set(HARKEN_THIRD_PARTY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/HarkenEngine/ThirdParty)

if(HARKEN_BUILD_CORE)
    set(HARKEN_SCHEMA_SOURCE_DIR "${CMAKE_SOURCE_DIR}/HarkenSchema"
        CACHE PATH "Harken Schema source tree")
endif()

# Configure-time build identity (product version / git commit / build label),
# exposed through the generated harken_buildinfo.h and embedded into
# Windows binary file properties by harken_attach_version_resource().
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/BuildInfo.cmake)

# Python 3 is discovered at the root so the engine's regeneration targets and
# the product test generated-contract checks share one interpreter discovery.
if(HARKEN_BUILD_TOOLS)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
endif()

if(HARKEN_BUILD_EDITOR OR HARKEN_BUILD_TOOLS)
    set(HARKEN_SCHEMA_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated/schema")
    set(HARKEN_SCHEMA_GENERATED_CORE_DIR "${HARKEN_SCHEMA_GENERATED_DIR}/Core")
    set(HARKEN_SCHEMA_GENERATED_PROJECT_HEADER
        "${HARKEN_SCHEMA_GENERATED_CORE_DIR}/ProjectSchemaGenerated.h")
    set(HARKEN_SCHEMA_GENERATED_BUILD_HEADER
        "${HARKEN_SCHEMA_GENERATED_CORE_DIR}/SchemaBuildGenerated.h")
    set(HARKEN_SCHEMA_GENERATED_VALIDATOR_HEADER
        "${HARKEN_SCHEMA_GENERATED_CORE_DIR}/ProjectSchemaValidator.generated.h")
    set(HARKEN_SCHEMA_GENERATED_VALIDATOR_SOURCE
        "${HARKEN_SCHEMA_GENERATED_CORE_DIR}/ProjectSchemaValidator.generated.cpp")
    set(HARKEN_SCHEMA_GENERATED_FILES
        ${HARKEN_SCHEMA_GENERATED_PROJECT_HEADER}
        ${HARKEN_SCHEMA_GENERATED_BUILD_HEADER}
        ${HARKEN_SCHEMA_GENERATED_VALIDATOR_HEADER}
        ${HARKEN_SCHEMA_GENERATED_VALIDATOR_SOURCE})
    file(GLOB_RECURSE HARKEN_SCHEMA_INPUTS CONFIGURE_DEPENDS
        "${HARKEN_SCHEMA_SOURCE_DIR}/*.json")
    add_custom_command(
        OUTPUT ${HARKEN_SCHEMA_GENERATED_FILES}
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_SOURCE_DIR}/.scripts/schema_archive.py"
            --schema-root "${HARKEN_SCHEMA_SOURCE_DIR}"
            --generate-code "${HARKEN_SCHEMA_GENERATED_CORE_DIR}"
        DEPENDS
            "${CMAKE_SOURCE_DIR}/.scripts/schema_archive.py"
            ${HARKEN_SCHEMA_INPUTS}
        VERBATIM)
    add_custom_target(harken_schema_codegen
        DEPENDS ${HARKEN_SCHEMA_GENERATED_FILES})
endif()

if(HARKEN_BUILD_CORE)
    add_subdirectory(HarkenEngine)
endif()

if(HARKEN_BUILD_TOOLS)
    add_subdirectory(HarkenStudio/Source/Tooling)
endif()

if(HARKEN_BUILD_RUNTIME_UNIT_TESTS OR HARKEN_BUILD_EDITOR_UNIT_TESTS OR
   HARKEN_BUILD_HUB_UNIT_TESTS OR TARGET harken_rt_probe_test OR
   TARGET harken_host_selection_copy_test)
    enable_testing()
endif()
if(TARGET harken_rt_probe_test)
    add_test(NAME harken_rt_probe_test COMMAND harken_rt_probe_test)
endif()

if(HARKEN_BUILD_RUNTIME_UNIT_TESTS OR HARKEN_BUILD_EDITOR_UNIT_TESTS OR
   HARKEN_BUILD_HUB_UNIT_TESTS)
    include(GoogleTest)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.zip
        URL_HASH SHA256=40d4ec942217dcc84a9ebe2a68584ada7d4a33a8ee958755763278ea1c5e18ff
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    # Match the engine's CRT choice (dynamic runtime) instead of gtest's default.
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
    # Clang 21+ added -Wcharacter-conversion, which fires inside gtest's own
    # headers (v1.17.0) and becomes fatal under googletest's /WX. Suppress it
    # on the third-party targets only; -Wno-unknown-warning-option keeps older
    # clang (which lacks the diagnostic) from tripping /WX on the flag itself.
    # Drop this once a googletest release ships the upstream fix (issue #4762).
    if(MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        foreach(_harken_gtest_target gtest gtest_main gmock gmock_main)
            if(TARGET ${_harken_gtest_target})
                target_compile_options(${_harken_gtest_target} PRIVATE
                    -Wno-unknown-warning-option -Wno-character-conversion)
            endif()
        endforeach()
    endif()
endif()

# The Qt front end (editor). A self-contained subtree with its own
# third-party closure and the legacy-engine compat stub; consumes only
# harken_buildinfo from the runtime side. Switchable so runtime-focused CI
# can configure without Qt (the subtree hard-requires Qt6 at find_package).
if(HARKEN_BUILD_EDITOR OR HARKEN_BUILD_HUB)
    add_subdirectory(HarkenIntegration/Tooling)
endif()

if(HARKEN_BUILD_EDITOR)
    add_subdirectory(HarkenStudio)
endif()

if(HARKEN_BUILD_HUB)
    add_subdirectory(HarkenHub)
endif()

function(harken_add_ci_test_target product)
    get_property(_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
    set(_executables)
    foreach(_target IN LISTS _targets)
        get_target_property(_type ${_target} TYPE)
        if(_type STREQUAL "EXECUTABLE")
            list(APPEND _executables ${_target})
        endif()
    endforeach()
    add_custom_target(harken_${product}_ci_tests DEPENDS ${_executables})
endfunction()

if(HARKEN_BUILD_RUNTIME_UNIT_TESTS OR HARKEN_BUILD_RUNTIME_BENCHMARKS)
    add_subdirectory(HarkenEngine/Test)
endif()

if(HARKEN_BUILD_EDITOR_UNIT_TESTS)
    add_subdirectory(HarkenStudio/Test)
endif()

if(HARKEN_BUILD_HUB_UNIT_TESTS)
    add_subdirectory(HarkenHub/Test)
endif()
if(TARGET harken_runtime_ci_tests AND TARGET harken_host_selection_copy_test)
    add_dependencies(harken_runtime_ci_tests harken_host_selection_copy_test)
endif()

get_property(_harken_versioned_binary_targets GLOBAL PROPERTY HARKEN_VERSIONED_BINARY_TARGETS)
configure_file(cmake/ReleaseIdentity.json.in generated/release-identity.json @ONLY)
if(_harken_versioned_binary_targets)
    message(STATUS "[BuildInfo] versioned binary targets=${_harken_versioned_binary_targets}")
endif()

# Debugging happens in Visual Studio; F5 should launch a test or benchmark
# executable, not ALL_BUILD, when one exists in this configuration.
if(TARGET harken_runtime_tests)
    set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                 PROPERTY VS_STARTUP_PROJECT harken_runtime_tests)
elseif(TARGET harken_editor_tests)
    set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                 PROPERTY VS_STARTUP_PROJECT harken_editor_tests)
elseif(TARGET harken_benchmarks)
    set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                 PROPERTY VS_STARTUP_PROJECT harken_benchmarks)
endif()
