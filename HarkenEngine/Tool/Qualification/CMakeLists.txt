find_package(Git QUIET)
if(GIT_FOUND)
    set(_harken_rtq_git_executable "${GIT_EXECUTABLE}")
else()
    set(_harken_rtq_git_executable "")
endif()

function(harken_rtq_attach_source_identity target_name)
    set(_generated_dir
        "${CMAKE_CURRENT_BINARY_DIR}/generated/${target_name}")
    set(_identity_header
        "${_generated_dir}/harken_rtq_source_identity.h")
    set(_identity_refresh
        "${_generated_dir}/harken_rtq_source_identity.refresh")
    set(_identity_command
        "${CMAKE_COMMAND}"
        "-DHARKEN_RTQ_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
        "-DHARKEN_RTQ_GIT_EXECUTABLE=${_harken_rtq_git_executable}"
        "-DHARKEN_RTQ_OUTPUT=${_identity_header}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/GenerateSourceIdentity.cmake")

    add_custom_command(
        OUTPUT "${_identity_header}" "${_identity_refresh}"
        COMMAND ${_identity_command}
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/GenerateSourceIdentity.cmake"
        COMMENT "Refreshing Runtime qualification source identity"
        VERBATIM
    )
    set_source_files_properties(
        "${_identity_header}" "${_identity_refresh}"
        PROPERTIES GENERATED TRUE)
    set_source_files_properties(
        "${_identity_refresh}" PROPERTIES SYMBOLIC TRUE)
    target_sources(${target_name} PRIVATE
        "${_identity_header}"
        "${_identity_refresh}")
    target_compile_definitions(${target_name} PRIVATE
        HARKEN_RTQ_SOURCE_IDENTITY_HEADER="${_identity_header}")
endfunction()

add_executable(harken_rt_qualification
    RuntimeQualification.cpp
)
harken_rtq_attach_source_identity(harken_rt_qualification)

target_link_libraries(harken_rt_qualification PRIVATE
    harken_core_rt_qualification
    harken_buildinfo
)

target_compile_definitions(harken_rt_qualification PRIVATE
    ${HARKEN_RT_PROBE_DEFINITIONS}
    HARKEN_RTQ_CMAKE_GENERATOR="${CMAKE_GENERATOR}"
    HARKEN_RTQ_COMPILER_ID="${CMAKE_CXX_COMPILER_ID}"
    HARKEN_RTQ_COMPILER_VERSION="${CMAKE_CXX_COMPILER_VERSION}"
    HARKEN_RTQ_COMPILER_PATH="${CMAKE_CXX_COMPILER}"
    HARKEN_RTQ_GENERATOR_TOOLSET="${CMAKE_GENERATOR_TOOLSET}"
    HARKEN_RTQ_VS_TOOLSET_VERSION="${CMAKE_VS_PLATFORM_TOOLSET_VERSION}"
    HARKEN_RTQ_WINDOWS_SDK="${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}"
    HARKEN_RTQ_BUILD_CONFIGURATION="$<CONFIG>"
)

target_include_directories(harken_rt_qualification PRIVATE
    ${HARKEN_RUNTIME_DIR}/api/src
    ${HARKEN_RUNTIME_DIR}/core/include
    ${HARKEN_RUNTIME_DIR}/core/contract/include
    ${HARKEN_RUNTIME_DIR}/dsp/include
    ${HARKEN_RUNTIME_DIR}/media/include
    ${HARKEN_RUNTIME_DIR}/param/include
    ${HARKEN_RUNTIME_DIR}/render/include
)

if(HARKEN_RT_PROBE_DEFINITIONS)
    add_executable(harken_rt_probe_test
        ${CMAKE_SOURCE_DIR}/HarkenEngine/Test/UnitTest/RtProbeTest.cpp
    )
    target_link_libraries(harken_rt_probe_test PRIVATE
        harken_core_rt_qualification
    )
    target_compile_definitions(harken_rt_probe_test PRIVATE
        ${HARKEN_RT_PROBE_DEFINITIONS}
    )
    target_include_directories(harken_rt_probe_test PRIVATE
        ${HARKEN_RUNTIME_DIR}/core/include
        ${HARKEN_RUNTIME_DIR}/core/contract/include
        ${HARKEN_RUNTIME_DIR}/render/include
    )
    # This target's ctest registration lives in the top-level CMakeLists, not
    # here: this directory is processed from HarkenEngine before enable_testing()
    # runs, so an add_test() at this point would not be emitted.
endif()

if(HARKEN_RT_PROBE_DEVICE_CALLBACK)
    add_executable(harken_rt_observer_cost
        ObserverCost.cpp
    )
    harken_rtq_attach_source_identity(harken_rt_observer_cost)
    target_link_libraries(harken_rt_observer_cost PRIVATE
        harken_core_rt_qualification
        harken_buildinfo
    )
    target_compile_definitions(harken_rt_observer_cost PRIVATE
        HARKEN_RT_PROBE_DEVICE_CALLBACK
        HARKEN_RTQ_CMAKE_GENERATOR="${CMAKE_GENERATOR}"
        HARKEN_RTQ_COMPILER_ID="${CMAKE_CXX_COMPILER_ID}"
        HARKEN_RTQ_COMPILER_VERSION="${CMAKE_CXX_COMPILER_VERSION}"
        HARKEN_RTQ_COMPILER_PATH="${CMAKE_CXX_COMPILER}"
        HARKEN_RTQ_GENERATOR_TOOLSET="${CMAKE_GENERATOR_TOOLSET}"
        HARKEN_RTQ_VS_TOOLSET_VERSION="${CMAKE_VS_PLATFORM_TOOLSET_VERSION}"
        HARKEN_RTQ_WINDOWS_SDK="${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}"
    )
    target_include_directories(harken_rt_observer_cost PRIVATE
        ${HARKEN_RUNTIME_DIR}/core/include
        ${HARKEN_RUNTIME_DIR}/core/contract/include
    )
endif()
