# harken_editor_assistant -- STATIC library for the AIAssistantPanel widget.
# Pattern: leaf (foundation link only). Note: AIAssistantPanel.cpp uses
# AIAssistantService from harken_editor_lib. Its symbols are resolved at the
# final executable link when the harken_editor_lib OBJECT files and STATIC
# libraries are combined.
# Consumers: harken_editor_lib (MainWindow.cpp:44).
# Depends on: harken_editor_foundation (AppSettings, ThemeManager, FontManager,
# DomainColors) + harken_editor_lib OBJECT
# (AIAssistantService at executable link).

add_library(harken_editor_assistant STATIC
    AIAssistantPanel.cpp
    AIAssistantPanel.h
)

set_target_properties(harken_editor_assistant PROPERTIES
    AUTOMOC ON
    AUTOGEN_BUILD_DIR "${CMAKE_BINARY_DIR}/_autogen/harken_editor_assistant"
)

target_include_directories(harken_editor_assistant PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_link_libraries(harken_editor_assistant PUBLIC harken_editor_foundation)

if(MSVC)
    target_compile_options(harken_editor_assistant PRIVATE /W4 /wd4100 /wd4189 /utf-8)
    target_compile_definitions(harken_editor_assistant PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
