# harken_editor_selector -- STATIC library for the shared object selector popup
# Depends on harken_editor_foundation + harken_editor_shared (TreeRowStyle keeps the
# tree look identical to Hierarchy/ProjectExplorer). Deliberately NOT registered
# with WorkspaceWindowManager, so it never appears in the Views menu.

add_library(harken_editor_selector STATIC
    SelectorWindow.cpp
    SelectorWindow.h
    RefDomainAdapter.cpp
    RefDomainAdapter.h
    # Output routing D3: [icon][name][...] reference field control; the "..."
    # button opens SelectorWindow. Consumed by the Inspector ObjectRef drawer.
    ObjectRefWidget.cpp
    ObjectRefWidget.h
    ObjectRefNavigation.cpp
    ObjectRefNavigation.h
)

set_target_properties(harken_editor_selector PROPERTIES
    AUTOMOC ON
    AUTOGEN_BUILD_DIR "${CMAKE_BINARY_DIR}/_autogen/harken_editor_selector"
)

# PUBLIC: consumers include "Selector/SelectorWindow.h" via this directory's parent (Source/Widgets/)
target_include_directories(harken_editor_selector PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(harken_editor_selector PUBLIC
    harken_editor_foundation
    harken_editor_shared
    harken_editor_common
)

if(MSVC)
    target_compile_options(harken_editor_selector PRIVATE /W4 /wd4100 /wd4189 /utf-8)
    target_compile_definitions(harken_editor_selector PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
