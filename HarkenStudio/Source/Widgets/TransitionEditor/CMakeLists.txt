add_library(harken_editor_transition_editor STATIC
    TransitionEditorPanel.cpp
    TransitionEditorPanel.h
    TransitionEditorWidget.cpp
    TransitionEditorWidget.h
    TransitionFadeButton.cpp
    TransitionFadeButton.h
)

set_target_properties(harken_editor_transition_editor PROPERTIES
    AUTOMOC ON
    AUTOGEN_BUILD_DIR "${CMAKE_BINARY_DIR}/_autogen/harken_editor_transition_editor"
)

target_include_directories(harken_editor_transition_editor PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(harken_editor_transition_editor PUBLIC
    harken_editor_foundation
    harken_editor_shared
    harken_editor_common
    # TransitionEditorWidget builds Selector/ObjectRefWidget for rule endpoints
    # and the bridge picker (opens the shared SelectorWindow popup).
    harken_editor_selector
    # Transition fade popup embeds CurveEditor/CurveCanvas.
    harken_editor_curve_editor
)

if(MSVC)
    target_compile_options(harken_editor_transition_editor PRIVATE /W4 /wd4100 /wd4189 /utf-8)
    target_compile_definitions(harken_editor_transition_editor PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
