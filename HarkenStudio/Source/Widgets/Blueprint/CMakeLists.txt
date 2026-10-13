# harken_editor_blueprint -- STATIC library for the Blueprint Canvas (S1a derivation only).
# Pattern: same as harken_editor_meter / foundation / common. S1a follows the leaf template.
# Consumers: harken_editor_lib (transitively from MainWindow once S1b lands), harken_editor_tests.
# Depends on: harken_editor_foundation (Core/ProjectTypes.h via Foundation's PUBLIC include dir).
#
add_library(harken_editor_blueprint STATIC
    BlueprintEdgeKind.h
    BlueprintEdgeKind.cpp
    BlueprintCanvasView.h
    BlueprintCanvasView.cpp
    NodeItem.h
    NodeItem.cpp
    ConnectionPath.h
    ConnectionPath.cpp
    NodeContentEditor.h
    NodeContentEditor.cpp
    NodeContentInfo.h
    NodeGraphLayer.h
    BlueprintCanvasWidget.h
    BlueprintCanvasWidget.cpp
    BlueprintCanvasWidget_NodeEditing.cpp
    BlueprintCanvasWidget_NodePresentation.cpp
    BlueprintPanel.h
    BlueprintPanel.cpp
    BlueprintVisibilityMenu.h
    BlueprintVisibilityMenu.cpp
    BlueprintConnectCommand.h
    BlueprintConnectCommand.cpp
    # S2 — drag-drop audio import: commonPrefix / snapToGrid free utilities
    # consumed by BlueprintCanvasWidget drop handling and by gtest.
    BlueprintImportUtils.h
    BlueprintImportUtils.cpp
    # Blueprint merge S2: BlueprintInspectorPanel.{h,cpp} removed — its
    # topology + Override functionality was absorbed by the unified
    # InspectorPanel (Container/Bus pages).
    # IBlueprintSource interface + the signal-chain source (per-object signal
    # chain). Pure descriptor extraction; the canvas materialises
    # NodeItem/ConnectionPath.
    IBlueprintSource.h
    SignalChainSource.h
    SignalChainSource.cpp
    SignalPathGraph.h
    SignalPathGraph.cpp
    SignalPathFocus.h
    SignalPathFocus.cpp
    SignalPathEditing.h
    SignalPathEditing.cpp
    SignalPathGainEditing.h
    SignalPathGainEditing.cpp
    SignalPathScope.h
    SignalPathScope.cpp
    SignalPathSource.h
    SignalPathSource.cpp
    SignalPathPlayback.h
    SignalPathPlayback.cpp
    # Shared edit-result type referenced by BlueprintCanvasWidget::ConnectResult.
    BlueprintEditTypes.h
)

set_target_properties(harken_editor_blueprint PROPERTIES
    AUTOMOC ON
    AUTOGEN_BUILD_DIR "${CMAKE_BINARY_DIR}/_autogen/harken_editor_blueprint"
)

# PUBLIC: consumers can include "Blueprint/BlueprintEdgeKind.h" via this directory's parent (src/Widgets/)
target_include_directories(harken_editor_blueprint PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

# Foundation brings Qt6::Widgets + ProjectTypes.h (QPointF, ProjectItem, Send) transitively.
# Common brings WaveformRenderer, included by NodeItem.h and IBlueprintSource.h.
target_link_libraries(harken_editor_blueprint PUBLIC
    harken_editor_foundation
    harken_editor_common
    harken_editor_shared
    PRIVATE harken_editor_selector
)

if(MSVC)
    target_compile_options(harken_editor_blueprint PRIVATE /W4 /wd4100 /wd4189 /utf-8)
    target_compile_definitions(harken_editor_blueprint PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
