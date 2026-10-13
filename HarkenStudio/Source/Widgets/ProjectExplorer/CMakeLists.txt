# harken_editor_explorer -- STATIC library for the ProjectExplorer module (project tree + drag-drop import).
# Pattern: non-leaf widget library, links foundation + sibling libs.
# Consumers: harken_editor_lib (MainWindow.cpp, Core/Actions/ProjectActions.cpp,
#                              Core/IPC/IPCServerInternal.h, Core/ProjectSerializer.cpp,
#                              Core/SelectionRouter.cpp, Core/BusController.cpp).
# Depends on: harken_editor_foundation
#             harken_editor_common   (ProjectExplorer.cpp includes Common/SearchField.h
#                                   for the Sources tab filter)
#             harken_editor_shared   (ProjectItemMenuController via ProjectTreePanel)
#             harken_editor_projecttree (ProjectExplorer subclasses ProjectTreePanel)
# Sources tab lists AssetRegistry and emits sourceEditorRequested(sourceId);
# it does not link harken_editor_source_editor. MainWindow opens the standalone panel.
# Note: requires add_subdirectory(Source/Widgets/Common), Shared, and ProjectTree
#       BEFORE add_subdirectory(Source/Widgets/ProjectExplorer) in the parent
#       CMakeLists.txt. SourceEditor may follow or precede this directory.

add_library(harken_editor_explorer STATIC
    ExplorerSection.cpp
    ExplorerSection.h
    ProjectExplorer.cpp
    ProjectExplorer.h
    SourcesTreeWidget.cpp
    SourcesTreeWidget.h
)

set_target_properties(harken_editor_explorer PROPERTIES
    AUTOMOC ON
    AUTOGEN_BUILD_DIR "${CMAKE_BINARY_DIR}/_autogen/harken_editor_explorer"
)

# PUBLIC: consumers can include "ProjectExplorer/ProjectExplorer.h" via this directory's parent (src/Widgets/)
target_include_directories(harken_editor_explorer PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

# Foundation brings Qt6::Widgets + Core headers; harken_editor_common brings SearchField for the Sources tab filter.
# harken_editor_shared brings ProjectItemMenuController (S2 of factory-entry-unification).
# harken_editor_projecttree brings the shared ProjectTreePanel/ProjectTreeWidget skeleton (Phase0 WP3 batch C).
target_link_libraries(harken_editor_explorer PUBLIC
    harken_editor_foundation
    harken_editor_common
    harken_editor_shared
    harken_editor_projecttree
)

if(MSVC)
    target_compile_options(harken_editor_explorer PRIVATE /W4 /wd4100 /wd4189 /utf-8)
    target_compile_definitions(harken_editor_explorer PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
