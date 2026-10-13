# Toolchain for the windows-ninja-clang-* presets: clang-cl targeting the MSVC
# ABI, self-contained so that configure and build succeed from any plain shell.
#
# vcvars used to provide three things; each is replaced here:
#
#   1. Tool locations. clang-cl, lld-link, llvm-mt, ninja, and rc.exe are
#      resolved to absolute paths below, so nothing depends on the caller's
#      PATH. Visual Studio is still the source of the binaries -- it is just
#      no longer an environment the developer has to enter.
#
#   2. MSVC / Windows SDK headers and libraries at build time. When INCLUDE
#      and LIB are absent from the environment, clang-cl and lld-link locate
#      the newest installed MSVC toolset and Windows SDK by themselves, so
#      ninja can be invoked from a shell with no injected variables at all.
#
#   3. Environment for nested configure-time tool builds. QWindowKit's
#      qmsetup helper is configured and built by a child cmake process during
#      our configure; that child inherits only this process's environment. A
#      vcvars-equivalent environment (PATH / INCLUDE / LIB) is therefore
#      exported to this cmake process -- it exists only for the lifetime of
#      the configure run and never leaks into the developer's shell. CC/CXX
#      steer the child to MSVC cl.exe: qmsetup embeds a duplicate manifest
#      that lld-link rejects and link.exe tolerates, and our own targets are
#      unaffected because their compilers are pinned right here.
#
# Override knob: set HARKEN_VS_DIR (cache or environment) to a Visual Studio
# installation root to bypass vswhere auto-detection, e.g. when several VS
# versions coexist and the newest is not the wanted one.

if(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
    message(FATAL_ERROR "The windows-clang-cl toolchain requires a Windows host.")
endif()

# --- Visual Studio root ------------------------------------------------------

set(HARKEN_VS_DIR "" CACHE PATH
    "Visual Studio installation root; empty selects the newest via vswhere")
set(_harken_vs_dir "${HARKEN_VS_DIR}")
if(NOT _harken_vs_dir AND DEFINED ENV{HARKEN_VS_DIR})
    set(_harken_vs_dir "$ENV{HARKEN_VS_DIR}")
endif()

if(NOT _harken_vs_dir)
    # vswhere's location is fixed by the VS installer and present with every
    # modern VS / Build Tools installation.
    set(_harken_vswhere "$ENV{ProgramFiles\(x86\)}/Microsoft Visual Studio/Installer/vswhere.exe")
    if(NOT EXISTS "${_harken_vswhere}")
        set(_harken_vswhere "C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe")
    endif()
    if(NOT EXISTS "${_harken_vswhere}")
        message(FATAL_ERROR
            "vswhere.exe not found; is Visual Studio (or Build Tools) with the "
            "C++ workload installed? Set HARKEN_VS_DIR to override.")
    endif()
    execute_process(
        COMMAND "${_harken_vswhere}" -latest -products * -nologo
                -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64
                -property installationPath
        OUTPUT_VARIABLE _harken_vs_dir
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _harken_vswhere_rc)
    if(NOT _harken_vswhere_rc EQUAL 0 OR NOT _harken_vs_dir)
        message(FATAL_ERROR
            "vswhere found no VS installation with the C++ x64 toolset. "
            "Set HARKEN_VS_DIR to override.")
    endif()
endif()
file(TO_CMAKE_PATH "${_harken_vs_dir}" _harken_vs_dir)

# --- Newest MSVC toolset and Windows SDK under that installation -------------

file(GLOB _harken_msvc_candidates "${_harken_vs_dir}/VC/Tools/MSVC/*")
list(SORT _harken_msvc_candidates COMPARE NATURAL ORDER DESCENDING)
if(NOT _harken_msvc_candidates)
    message(FATAL_ERROR "No MSVC toolset under ${_harken_vs_dir}/VC/Tools/MSVC")
endif()
list(GET _harken_msvc_candidates 0 _harken_msvc_dir)

cmake_host_system_information(RESULT _harken_kits_root
    QUERY WINDOWS_REGISTRY "HKLM/SOFTWARE/Microsoft/Windows Kits/Installed Roots"
    VALUE "KitsRoot10")
set(_harken_kits_fallback "C:/Program Files (x86)/Windows Kits/10")
if(NOT _harken_kits_root OR NOT EXISTS "${_harken_kits_root}/Include")
    set(_harken_kits_root "${_harken_kits_fallback}")
endif()
file(TO_CMAKE_PATH "${_harken_kits_root}" _harken_kits_root)
string(REGEX REPLACE "/$" "" _harken_kits_root "${_harken_kits_root}")

file(GLOB _harken_sdk_candidates RELATIVE "${_harken_kits_root}/Include"
    "${_harken_kits_root}/Include/10.*")
list(SORT _harken_sdk_candidates COMPARE NATURAL ORDER DESCENDING)
if(NOT _harken_sdk_candidates)
    message(FATAL_ERROR "No Windows 10/11 SDK found under ${_harken_kits_root}")
endif()
list(GET _harken_sdk_candidates 0 _harken_sdk_ver)

set(_harken_llvm_bin "${_harken_vs_dir}/VC/Tools/Llvm/x64/bin")
set(_harken_msvc_bin "${_harken_msvc_dir}/bin/Hostx64/x64")
set(_harken_sdk_bin  "${_harken_kits_root}/bin/${_harken_sdk_ver}/x64")
set(_harken_ninja_dir "${_harken_vs_dir}/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja")

foreach(_harken_tool
        "${_harken_llvm_bin}/clang-cl.exe"
        "${_harken_llvm_bin}/lld-link.exe"
        "${_harken_llvm_bin}/llvm-mt.exe"
        "${_harken_sdk_bin}/rc.exe")
    if(NOT EXISTS "${_harken_tool}")
        message(FATAL_ERROR
            "${_harken_tool} not found. The VS installation needs the "
            "'C++ Clang tools for Windows' component and a Windows SDK.")
    endif()
endforeach()

# --- Pin the tools by absolute path -------------------------------------------

set(CMAKE_C_COMPILER   "${_harken_llvm_bin}/clang-cl.exe")
set(CMAKE_CXX_COMPILER "${_harken_llvm_bin}/clang-cl.exe")
set(CMAKE_LINKER       "${_harken_llvm_bin}/lld-link.exe" CACHE FILEPATH "")
set(CMAKE_MT           "${_harken_llvm_bin}/llvm-mt.exe"  CACHE FILEPATH "")
set(CMAKE_RC_COMPILER  "${_harken_sdk_bin}/rc.exe"        CACHE FILEPATH "")

# Prefer the VS-bundled ninja so no separate install is required; a ninja the
# developer put on PATH still wins because the cache entry would already be set.
if(NOT CMAKE_MAKE_PROGRAM AND EXISTS "${_harken_ninja_dir}/ninja.exe")
    set(CMAKE_MAKE_PROGRAM "${_harken_ninja_dir}/ninja.exe" CACHE FILEPATH "")
endif()

# rc.exe has no INCLUDE environment at build time (ninja runs in the caller's
# plain shell), and third-party .rc files include <windows.h>; hand it the
# same directories vcvars would have put into INCLUDE. The joined uppercase
# /I form is the one spelling accepted by both rc.exe and clang-cl -- the
# latter matters because cmcldeps preprocesses .rc files through the C
# compiler with these same flags for dependency scanning.
file(TO_NATIVE_PATH "${_harken_msvc_dir}/include" _harken_rc_msvc_inc)
file(TO_NATIVE_PATH "${_harken_kits_root}/Include/${_harken_sdk_ver}" _harken_rc_sdk_inc)
set(CMAKE_RC_FLAGS_INIT "/nologo \"/I${_harken_rc_msvc_inc}\" \"/I${_harken_rc_sdk_inc}\\ucrt\" \"/I${_harken_rc_sdk_inc}\\um\" \"/I${_harken_rc_sdk_inc}\\shared\" \"/I${_harken_rc_sdk_inc}\\winrt\"")

# --- vcvars-equivalent environment for this configure process only -----------
# Guarded so that child cmake processes (try_compile, nested tool builds) that
# re-read this file do not prepend PATH a second time.

if(NOT DEFINED ENV{HARKEN_MSVC_ENV_READY})
    set(ENV{HARKEN_MSVC_ENV_READY} "1")

    file(TO_NATIVE_PATH "${_harken_msvc_dir}" _harken_n_msvc)
    file(TO_NATIVE_PATH "${_harken_kits_root}/Include/${_harken_sdk_ver}" _harken_n_sdk_inc)
    file(TO_NATIVE_PATH "${_harken_kits_root}/Lib/${_harken_sdk_ver}" _harken_n_sdk_lib)
    file(TO_NATIVE_PATH "${_harken_msvc_bin}" _harken_n_msvc_bin)
    file(TO_NATIVE_PATH "${_harken_sdk_bin}" _harken_n_sdk_bin)
    file(TO_NATIVE_PATH "${_harken_llvm_bin}" _harken_n_llvm_bin)
    file(TO_NATIVE_PATH "${_harken_ninja_dir}" _harken_n_ninja_dir)

    set(ENV{INCLUDE} "${_harken_n_msvc}\\include;${_harken_n_sdk_inc}\\ucrt;${_harken_n_sdk_inc}\\um;${_harken_n_sdk_inc}\\shared;${_harken_n_sdk_inc}\\winrt;${_harken_n_sdk_inc}\\cppwinrt")
    set(ENV{LIB} "${_harken_n_msvc}\\lib\\x64;${_harken_n_sdk_lib}\\ucrt\\x64;${_harken_n_sdk_lib}\\um\\x64")
    set(ENV{PATH} "${_harken_n_msvc_bin};${_harken_n_sdk_bin};${_harken_n_llvm_bin};${_harken_n_ninja_dir};$ENV{PATH}")

    # Nested configure-time tool builds (qmsetup) must compile with MSVC cl,
    # not clang-cl: see the header comment. Our own targets ignore these.
    set(ENV{CC} "cl")
    set(ENV{CXX} "cl")
endif()
