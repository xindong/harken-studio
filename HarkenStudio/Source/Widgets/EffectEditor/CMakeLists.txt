add_library(harken_editor_effect_editor STATIC
    EffectEditorPanel.cpp
    EffectEditorPanel.h
)

set_target_properties(harken_editor_effect_editor PROPERTIES
    AUTOMOC ON
    AUTOGEN_BUILD_DIR "${CMAKE_BINARY_DIR}/_autogen/harken_editor_effect_editor"
)

target_include_directories(harken_editor_effect_editor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(harken_editor_effect_editor PUBLIC harken_editor_foundation)

if(MSVC)
    target_compile_options(harken_editor_effect_editor PRIVATE /W4 /wd4100 /wd4189 /utf-8)
    target_compile_definitions(harken_editor_effect_editor PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
