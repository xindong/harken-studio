add_library(harken_editor_grid_editor STATIC
    MixingEditorWidget.cpp
    MixingEditorWidget.h
    MixingRowCanvas.cpp
    MixingRowCanvas.h
    DiscreteGridWidget.cpp
    DiscreteGridWidget.h
    GridEditorPanel.cpp
    GridEditorPanel.h
    GridEditorWidget.cpp
    GridEditorWidget.h
    SelectionListWidget.cpp
    SelectionListWidget.h
    GridHeaderRef.h
    GridValueFormatting.h
)

set_target_properties(harken_editor_grid_editor PROPERTIES
    AUTOMOC ON
    AUTOGEN_BUILD_DIR "${CMAKE_BINARY_DIR}/_autogen/harken_editor_grid_editor"
)

target_include_directories(harken_editor_grid_editor PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(harken_editor_grid_editor PUBLIC
    harken_editor_foundation
    harken_editor_curve_editor
    harken_editor_selector
)

if(MSVC)
    target_compile_options(harken_editor_grid_editor PRIVATE /W4 /wd4100 /wd4189 /utf-8)
    target_compile_definitions(harken_editor_grid_editor PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
