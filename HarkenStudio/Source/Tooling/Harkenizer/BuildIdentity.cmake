set(build_identity "${CMAKE_CURRENT_BINARY_DIR}/generated/$<CONFIG>/ProjectBuildIdentity.cpp")
set(build_identity_targets
    harkenizer_core
    harkenizer_resource_compiler
    harkenizer_compiler_client
    harken_runtime_bridge
    harken_current_world_compiler
    harken_project_schema
    harken_bus_compiler
    harken_reference_slots
    harken_studio_pack_build
    harken_studio_media_decode
    harken_studio_runtime
    harken_integration_tooling
    vorbis
    vorbisenc
    vorbisfile
    ogg
)
set(build_identity_files)
foreach(build_target IN LISTS build_identity_targets)
    list(APPEND build_identity_files "$<TARGET_FILE:${build_target}>")
endforeach()
add_custom_command(
    OUTPUT "${build_identity}"
    COMMAND ${CMAKE_COMMAND}
        "-DCOMPILER_ARCHIVES=${build_identity_files}"
        "-DOUTPUT=${build_identity}"
        "-DIDENTITY_FUNCTION=projectBuildIdentity"
        -P "${CMAKE_CURRENT_LIST_DIR}/CompilerIdentity.cmake"
    DEPENDS ${build_identity_targets}
        "${CMAKE_CURRENT_LIST_DIR}/CompilerIdentity.cmake"
    VERBATIM
)
add_library(harkenizer_build_identity STATIC "${build_identity}")
add_library(harkenizer_build INTERFACE)
target_link_libraries(harkenizer_build INTERFACE harkenizer_core harkenizer_build_identity)
