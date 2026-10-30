add_library(harken_studio_media_decode STATIC MediaDecode.cpp)
target_include_directories(harken_studio_media_decode PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../ThirdParty/miniaudio)

add_library(harkenizer_resource_compiler STATIC ResourceCompiler.cpp)
target_link_libraries(harkenizer_resource_compiler
    PUBLIC harken_studio_types
    PRIVATE harken_studio_pack_build harken_studio_media_decode)
target_include_directories(harkenizer_resource_compiler
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../../ThirdParty/miniaudio
        ${CMAKE_CURRENT_SOURCE_DIR}/../../Common)
