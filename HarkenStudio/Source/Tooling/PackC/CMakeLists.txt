add_library(harken_studio_media_decode STATIC MediaDecode.cpp)
target_include_directories(harken_studio_media_decode PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../ThirdParty/miniaudio)

add_executable(harken_packc PackcMain.cpp)
set_target_properties(harken_packc PROPERTIES OUTPUT_NAME packc)
harken_attach_version_resource(harken_packc "Harken Pack Compiler" "packc.exe")
target_link_libraries(harken_packc PRIVATE
    harken_studio_pack_build harken_studio_media_decode)
target_include_directories(harken_packc PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../ThirdParty/miniaudio
    ${CMAKE_CURRENT_SOURCE_DIR}/../Common)

add_library(harken_editor_metadata STATIC EditorWaveformPublication.cpp)
target_link_libraries(harken_editor_metadata PUBLIC harken_studio_pack_build)
target_include_directories(harken_editor_metadata PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../Common)

add_executable(harken_editor_metadata_cli EditorMetadataMain.cpp)
set_target_properties(harken_editor_metadata_cli PROPERTIES OUTPUT_NAME harken-editor-metadata)
target_link_libraries(harken_editor_metadata_cli PRIVATE harken_editor_metadata)
target_include_directories(harken_editor_metadata_cli PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../Common)
