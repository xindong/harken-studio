# Build the independent Xiph reference only for Engine tests.
include(FetchContent)
FetchContent_Declare(hkoref
    URL https://downloads.xiph.org/releases/ogg/libogg-1.3.6.tar.xz
    URL_HASH SHA256=5c8253428e181840cd20d41f3ca16557a9cc04bad4a3d04cce84808677fa1061
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR harken-reference-sources-only)
FetchContent_Declare(hkvref
    URL https://downloads.xiph.org/releases/vorbis/libvorbis-1.3.7.tar.xz
    URL_HASH SHA256=b33cc4934322bcbf6efcbacf49e3ca01aadbea4114ec9589d1b1e9d20f72954b
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR harken-reference-sources-only)
FetchContent_MakeAvailable(hkoref hkvref)

set(ref_include ${CMAKE_CURRENT_BINARY_DIR}/include)
set(INCLUDE_STDINT_H 1)
set(SIZE16 int16_t)
set(USIZE16 uint16_t)
set(SIZE32 int32_t)
set(USIZE32 uint32_t)
set(SIZE64 int64_t)
set(USIZE64 uint64_t)
configure_file(${hkoref_SOURCE_DIR}/include/ogg/config_types.h.in
               ${ref_include}/ogg/config_types.h @ONLY)

# Qualify against the full upstream decoder and its private floor metadata.
# Isolated header names keep fixture callers bound to this exact release.
configure_file(${hkvref_SOURCE_DIR}/include/vorbis/vorbisfile.h
               ${ref_include}/harken_reference/vorbisfile.h COPYONLY)
configure_file(${hkvref_SOURCE_DIR}/include/vorbis/codec.h
               ${ref_include}/harken_reference/codec.h COPYONLY)
configure_file(${hkvref_SOURCE_DIR}/lib/codec_internal.h
               ${ref_include}/harken_reference/codec_internal.h COPYONLY)
set(vorbis_sources
    analysis.c bitrate.c block.c codebook.c envelope.c floor0.c floor1.c
    info.c lookup.c lpc.c lsp.c mapping0.c mdct.c psy.c registry.c res0.c
    sharedbook.c smallft.c synthesis.c vorbisfile.c window.c)
list(TRANSFORM vorbis_sources PREPEND "${hkvref_SOURCE_DIR}/lib/")
add_library(harken_test_vorbis_reference STATIC ${vorbis_sources}
    ${hkoref_SOURCE_DIR}/src/bitwise.c
    ${hkoref_SOURCE_DIR}/src/framing.c
    ../Common/VorbisFixtureSupport.c)
target_include_directories(harken_test_vorbis_reference
    PUBLIC ${ref_include} ${hkvref_SOURCE_DIR}/include
           ${hkoref_SOURCE_DIR}/include
    PRIVATE ${hkvref_SOURCE_DIR}/lib)
if(MSVC)
    target_compile_options(harken_test_vorbis_reference PRIVATE /W0 /utf-8)
    target_compile_definitions(harken_test_vorbis_reference PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(harken_test_vorbis_reference PRIVATE -w)
    target_link_libraries(harken_test_vorbis_reference PUBLIC m)
endif()
