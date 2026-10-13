# harken_editor_shared -- STATIC library for cross-widget Editor helpers.
# Pattern: same shape as harken_editor_common (foundation-only leaf lib).
# Consumers: harken_editor_explorer (S2), harken_editor_hierarchy (S3).
#
# Contents:
#   - ProjectItemMenuController: data-driven "New Child -> <Type>" creation
#     subsystem extracted from ProjectExplorer in S2 of factory-entry-unification.
#   - TreeRowStyle: shared project-tree look (base stylesheet, bucket row
#     construction and painting, branch guides/chevrons) used by Hierarchy,
#     ProjectExplorer and SelectorWindow.
#   - ProjectItemMime: the application/x-harken-project-item(s) drag payload codec
#     shared by the project trees (drag sources) and ObjectRefWidget (drop target).

add_library(harken_editor_shared STATIC
    InlineRenameEdit.cpp
    InlineRenameEdit.h
    RenameClickHandler.cpp
    RenameClickHandler.h
    ProjectItemMenuController.cpp
    ProjectItemMenuController.h
    TreeExpansionWidget.cpp
    TreeExpansionWidget.h
    TreeRowStyle.cpp
    TreeRowStyle.h
    TypeFilter.h
    SchemaFieldPick.h
    TreeRowPing.cpp
    TreeRowPing.h
    LocationPingAnimation.h
    ProjectItemMime.cpp
    ProjectItemMime.h
    RefDomain.h
    RefIdMime.cpp
    RefIdMime.h
    AudioAssetLabels.cpp
    AudioAssetLabels.h
    ProjectObjectLabels.h
    InspectorFormRow.cpp
    InspectorFormRow.h
    PlatformFieldMerge.cpp
    PlatformFieldMerge.h
)

set_target_properties(harken_editor_shared PROPERTIES
    AUTOMOC ON
    AUTOGEN_BUILD_DIR "${CMAKE_BINARY_DIR}/_autogen/harken_editor_shared"
)

# PUBLIC: consumers can include "Shared/ProjectItemMenuController.h"
# via this directory's parent (src/Widgets/).
target_include_directories(harken_editor_shared PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

# Foundation brings Qt6::Widgets + Core (ProjectTypes / ProjectManager) headers.
target_link_libraries(harken_editor_shared PUBLIC
    harken_editor_foundation
)

if(MSVC)
    target_compile_options(harken_editor_shared PRIVATE /W4 /wd4100 /wd4189 /utf-8)
    target_compile_definitions(harken_editor_shared PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
