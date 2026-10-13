add_library(harken_editor_external_editor STATIC
    ExternalEditorPanel.cpp
    ExternalEditorPanel.h
)

set_target_properties(harken_editor_external_editor PROPERTIES
    AUTOMOC ON
    AUTOGEN_BUILD_DIR "${CMAKE_BINARY_DIR}/_autogen/harken_editor_external_editor"
)

target_include_directories(harken_editor_external_editor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(harken_editor_external_editor PUBLIC
    harken_editor_curve_editor
    harken_editor_inspector
    harken_editor_source_editor
)

if(MSVC)
    target_compile_options(harken_editor_external_editor PRIVATE /W4 /wd4100 /wd4189 /utf-8)
    target_compile_definitions(harken_editor_external_editor PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
