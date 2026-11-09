add_library(harken_editor_transition_fade STATIC
    TransitionFadeEditor.cpp
    TransitionFadeEditor.h
    TransitionFadePreviewField.cpp
    TransitionFadePreviewField.h
    TransitionFadeTimeline.cpp
    TransitionFadeTimeline.h
    TransitionRuleEditing.cpp
    TransitionRuleEditing.h
)

set_target_properties(harken_editor_transition_fade PROPERTIES
    AUTOMOC ON
    AUTOGEN_BUILD_DIR "${CMAKE_BINARY_DIR}/_autogen/harken_editor_transition_fade"
)

target_include_directories(harken_editor_transition_fade PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(harken_editor_transition_fade PUBLIC
    harken_editor_foundation
    harken_editor_common
    harken_editor_shared
    harken_editor_curve_editor
    harken_editor_timeline
)

if(MSVC)
    target_compile_options(harken_editor_transition_fade PRIVATE /W4 /wd4100 /wd4189 /utf-8)
    target_compile_definitions(harken_editor_transition_fade PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
