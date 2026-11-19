add_library(harken_studio_build_pipeline STATIC
    CurrentWorldCompile.h
    CurrentWorldCompile.cpp
    PackagingBridge.h
    PackagingBridge.cpp
)

target_include_directories(harken_studio_build_pipeline PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
)

target_include_directories(harken_studio_build_pipeline PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../ThirdParty
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../ThirdParty/miniaudio
)

target_link_libraries(harken_studio_build_pipeline PUBLIC
    Qt6::Core
    harken_runtime_bridge
    harken_studio_pack_build
    harken_current_world_compiler
    harken_bus_compiler
)

target_link_libraries(harken_studio_build_pipeline PRIVATE
    harken_studio_media_decode
    harkenizer_resource_compiler
    harkenizer_compiler_client
    vorbisfile
)

# Reference-slot resolution is shared with the program compiler and the
# offline explain runner, so the editor consumes the same rule rather than
# keeping its own copy. Guarded because the library belongs to the desktop
# tooling layer, which a runtime-only configuration does not build.
if(TARGET harken_reference_slots)
    target_link_libraries(harken_studio_build_pipeline PRIVATE harken_reference_slots)
endif()

if(MSVC)
    target_compile_options(harken_studio_build_pipeline PRIVATE /W4 /wd4100 /wd4189 /utf-8)
    target_compile_definitions(harken_studio_build_pipeline PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
