set(compiler_identity "${CMAKE_CURRENT_BINARY_DIR}/generated/$<CONFIG>/ResourceCompilerIdentity.cpp")
add_custom_command(
    OUTPUT "${compiler_identity}"
    COMMAND ${CMAKE_COMMAND}
        "-DCOMPILER_ARCHIVES=$<TARGET_FILE:harkenizer_resource_compiler>;$<TARGET_FILE:harken_studio_pack_build>;$<TARGET_FILE:harken_studio_media_decode>"
        "-DOUTPUT=${compiler_identity}"
        -P "${CMAKE_CURRENT_LIST_DIR}/CompilerIdentity.cmake"
    DEPENDS harkenizer_resource_compiler harken_studio_pack_build harken_studio_media_decode
        "${CMAKE_CURRENT_LIST_DIR}/CompilerIdentity.cmake"
    VERBATIM
)

add_library(harkenizer_compiler_client STATIC
    "${CMAKE_CURRENT_LIST_DIR}/ResourceCompilerClient.cpp"
    "${compiler_identity}"
)
target_include_directories(harkenizer_compiler_client PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../..")
target_link_libraries(harkenizer_compiler_client PUBLIC Qt6::Core)
