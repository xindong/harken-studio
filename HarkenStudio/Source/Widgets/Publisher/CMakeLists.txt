# harken_editor_publisher -- STATIC library for the PublisherPanel widget.
# Pattern: leaf (single PUBLIC link to foundation).
# Consumers: harken_editor_lib (MainWindow.cpp:59).
# Depends on: harken_editor_foundation (ProjectPackExporter.h, FontManager.h).

add_library(harken_editor_publisher STATIC
    PublisherPanel.cpp
    PublisherPanel.h
    PackStructureDialog.cpp
    PackStructureDialog.h
)

set_target_properties(harken_editor_publisher PROPERTIES
    AUTOMOC ON
    AUTOGEN_BUILD_DIR "${CMAKE_BINARY_DIR}/_autogen/harken_editor_publisher"
)

target_include_directories(harken_editor_publisher PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_link_libraries(harken_editor_publisher PUBLIC harken_editor_foundation harken_editor_common harken_editor_shared)

if(MSVC)
    target_compile_options(harken_editor_publisher PRIVATE /W4 /wd4100 /wd4189 /utf-8)
    target_compile_definitions(harken_editor_publisher PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
