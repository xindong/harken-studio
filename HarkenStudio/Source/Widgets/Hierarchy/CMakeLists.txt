# harken_editor_hierarchy -- STATIC library for the Hierarchy module (independent clone of ProjectExplorer).
# Pattern: same shape as harken_editor_explorer (non-leaf, links foundation + one sibling lib).
# Depends on: harken_editor_foundation
#             harken_editor_common   (Hierarchy.h includes Common/DropZoneWidget.h
#                                   for the AudioDropTarget mixin)

add_library(harken_editor_hierarchy STATIC
    Hierarchy.cpp
    Hierarchy.h
)

set_target_properties(harken_editor_hierarchy PROPERTIES
    AUTOMOC ON
    AUTOGEN_BUILD_DIR "${CMAKE_BINARY_DIR}/_autogen/harken_editor_hierarchy"
)

# PUBLIC: consumers can include "Hierarchy/Hierarchy.h" via this directory's parent (src/Widgets/)
target_include_directories(harken_editor_hierarchy PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

# Foundation brings Qt6::Widgets + Core headers; harken_editor_common brings DropZoneWidget for AudioDropTarget mixin.
# harken_editor_shared brings ProjectItemMenuController (S3 of factory-entry-unification).
# harken_editor_projecttree brings the shared ProjectTreePanel/ProjectTreeWidget skeleton (Phase0 WP3 batch C).
target_link_libraries(harken_editor_hierarchy PUBLIC
    harken_editor_foundation
    harken_editor_common
    harken_editor_shared
    harken_editor_projecttree
)

if(MSVC)
    target_compile_options(harken_editor_hierarchy PRIVATE /W4 /wd4100 /wd4189 /utf-8)
    target_compile_definitions(harken_editor_hierarchy PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
