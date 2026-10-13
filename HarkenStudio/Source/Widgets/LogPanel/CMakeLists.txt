# harken_editor_logpanel -- STATIC library for the LogPanel widget.
# Pattern: leaf (single PUBLIC link to foundation).
# Consumers: harken_editor_lib (MainWindow.cpp:40, Core/AssetPipeline/ImportAssetsCommand.cpp:17).
# Depends on: harken_editor_foundation (AppSettings, DomainColors, FontManager, ThemeManager,
# SvgIconEngine).

add_library(harken_editor_logpanel STATIC
    LogPanel.cpp
    LogPanel.h
)

set_target_properties(harken_editor_logpanel PROPERTIES
    AUTOMOC ON
    AUTOGEN_BUILD_DIR "${CMAKE_BINARY_DIR}/_autogen/harken_editor_logpanel"
)

target_include_directories(harken_editor_logpanel PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_link_libraries(harken_editor_logpanel PUBLIC harken_editor_foundation harken_editor_common)

if(MSVC)
    target_compile_options(harken_editor_logpanel PRIVATE /W4 /wd4100 /wd4189 /utf-8)
    target_compile_definitions(harken_editor_logpanel PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
