cmake_minimum_required(VERSION 3.21)

include(CheckIncludeFiles)
include(CheckTypeSize)

check_include_files(inttypes.h  INCLUDE_INTTYPES_H)
check_include_files(stdint.h    INCLUDE_STDINT_H)
check_include_files(sys/types.h INCLUDE_SYS_TYPES_H)

set(SIZE16  int16_t)
set(USIZE16 uint16_t)
set(SIZE32  int32_t)
set(USIZE32 uint32_t)
set(SIZE64  int64_t)
set(USIZE64 uint64_t)

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/include/ogg/config_types.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/include/ogg/config_types.h
    @ONLY
)

add_library(ogg STATIC
    src/bitwise.c
    src/framing.c
)

target_include_directories(ogg PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}/include
)

if(MSVC)
    target_compile_options(ogg PRIVATE /W0 /utf-8)
else()
    target_compile_options(ogg PRIVATE -w)
endif()
