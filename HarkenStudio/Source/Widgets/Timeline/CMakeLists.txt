add_library(harken_editor_timeline STATIC
    TimelineCanvas.cpp
    TimelineCanvas.h
    TimelineClipCanvas.cpp
    TimelineClipCanvas.h
    TimelineEditorPanel.cpp
    TimelineEditorPanel.h
    TimelineHeaderColumn.cpp
    TimelineHeaderColumn.h
    TimelineRenderer.cpp
    TimelineRenderer.h
    TimelineRuler.cpp
    TimelineRuler.h
    TimelineTicks.cpp
    TimelineTicks.h
)

set_target_properties(harken_editor_timeline PROPERTIES
    AUTOMOC ON
    AUTOGEN_BUILD_DIR "${CMAKE_BINARY_DIR}/_autogen/harken_editor_timeline"
)

target_include_directories(harken_editor_timeline PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(harken_editor_timeline PUBLIC
    harken_editor_foundation
    harken_editor_common
    harken_editor_shared
)

if(MSVC)
    target_compile_options(harken_editor_timeline PRIVATE /W4 /wd4100 /wd4189 /utf-8)
    target_compile_definitions(harken_editor_timeline PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
