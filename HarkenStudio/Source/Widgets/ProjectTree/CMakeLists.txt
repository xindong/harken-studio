# harken_editor_projecttree -- STATIC library for the shared project-tree skeleton
# (ProjectTreePanel base panel + ProjectTreeWidget tree control), Phase0 WP3 batch C.
# Pattern: non-leaf widget library, links foundation + sibling libs.
# Consumers: harken_editor_explorer, harken_editor_hierarchy (both panels are
#            difference-layer subclasses of ProjectTreePanel).
# Depends on: harken_editor_foundation
#             harken_editor_common   (ProjectTreeWidget.h includes Common/DropZoneWidget.h
#                                   for the AudioDropTarget mixin)
#             harken_editor_shared   (ProjectTreePanel.h includes ProjectItemMenuController.h)
# Note: requires add_subdirectory(Source/Widgets/Common) and
#       add_subdirectory(Source/Widgets/Shared) BEFORE this directory
#       in the parent CMakeLists.txt.

add_library(harken_editor_projecttree STATIC
    ProjectTreePanel.cpp
    ProjectTreePanel.h
    ProjectTreeWidget.cpp
    ProjectTreeWidget.h
)

set_target_properties(harken_editor_projecttree PROPERTIES
    AUTOMOC ON
    AUTOGEN_BUILD_DIR "${CMAKE_BINARY_DIR}/_autogen/harken_editor_projecttree"
)

# PUBLIC: consumers can include "ProjectTree/ProjectTreePanel.h" via this directory's parent (src/Widgets/)
target_include_directories(harken_editor_projecttree PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(harken_editor_projecttree PUBLIC
    harken_editor_foundation
    harken_editor_common
    harken_editor_shared
)

if(MSVC)
    target_compile_options(harken_editor_projecttree PRIVATE /W4 /wd4100 /wd4189 /utf-8)
    target_compile_definitions(harken_editor_projecttree PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
