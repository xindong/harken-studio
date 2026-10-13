cmake_minimum_required(VERSION 3.21)

set(VORBIS_SOURCES
    lib/analysis.c
    lib/bitrate.c
    lib/block.c
    lib/codebook.c
    lib/envelope.c
    lib/floor0.c
    lib/floor1.c
    lib/info.c
    lib/lookup.c
    lib/lpc.c
    lib/lsp.c
    lib/mapping0.c
    lib/mdct.c
    lib/psy.c
    lib/registry.c
    lib/res0.c
    lib/sharedbook.c
    lib/smallft.c
    lib/synthesis.c
    lib/window.c
)

set(VORBISENC_SOURCES
    lib/vorbisenc.c
)

add_library(vorbis STATIC ${VORBIS_SOURCES})
add_library(vorbisenc STATIC ${VORBISENC_SOURCES})
add_library(vorbisfile STATIC lib/vorbisfile.c)

target_include_directories(vorbis PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_include_directories(vorbis PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/lib
)

target_include_directories(vorbisenc PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_include_directories(vorbisenc PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/lib
)

target_include_directories(vorbisfile PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_include_directories(vorbisfile PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/lib
)

target_link_libraries(vorbis PUBLIC ogg)
target_link_libraries(vorbisenc PUBLIC vorbis)
target_link_libraries(vorbisfile PUBLIC vorbis)

if(MSVC)
    target_compile_options(vorbis PRIVATE /W0 /utf-8)
    target_compile_options(vorbisenc PRIVATE /W0 /utf-8)
    target_compile_options(vorbisfile PRIVATE /W0 /utf-8)
    target_compile_definitions(vorbis PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_definitions(vorbisenc PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_definitions(vorbisfile PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(vorbis PRIVATE -w)
    target_compile_options(vorbisenc PRIVATE -w)
    target_compile_options(vorbisfile PRIVATE -w)
endif()
