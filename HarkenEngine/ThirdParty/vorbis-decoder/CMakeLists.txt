add_library(harken_vorbis_decoder STATIC
    decoder.c bitwise.c block.c synthesis.c info.c codebook.c sharedbook.c
    floor1.c res0.c mapping0.c mdct.c window.c registry.c)
target_include_directories(harken_vorbis_decoder PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(harken_vorbis_decoder PROPERTIES
    POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
if(MSVC)
    target_compile_definitions(harken_vorbis_decoder PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
