# harken_editor_pack -- STATIC library for the PackManagerPanel widget.
# Pattern: leaf (single PUBLIC link to foundation).
# Consumers: harken_editor_lib (MainWindow.cpp:59).
# Depends on: harken_editor_foundation (ProjectPackExporter.h, FontManager.h).

add_library(harken_editor_pack STATIC
    PackManagerPanel.cpp
    PackManagerPanel.h
    PackStructureDialog.cpp
    PackStructureDialog.h
)

set_target_properties(harken_editor_pack PROPERTIES
    AUTOMOC ON
    AUTOGEN_BUILD_DIR "${CMAKE_BINARY_DIR}/_autogen/harken_editor_pack"
)

target_include_directories(harken_editor_pack PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_link_libraries(harken_editor_pack PUBLIC harken_editor_foundation harken_editor_common)

if(MSVC)
    target_compile_options(harken_editor_pack PRIVATE /W4 /wd4100 /wd4189 /utf-8)
    target_compile_definitions(harken_editor_pack PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
