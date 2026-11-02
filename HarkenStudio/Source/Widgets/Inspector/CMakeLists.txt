# harken_editor_inspector -- STATIC library for the Inspector module.
# Pattern: multi-PUBLIC-link non-leaf (links foundation + common + event).
# Largest single module in the Editor, pimpl-isolated since S2.
#
# Consumers: harken_editor_lib (MainWindow.cpp + Core/Actions/SystemActions.cpp + Core/IPCHandlers_System.cpp)
# Depends on: harken_editor_foundation (Core/AppSettings + Qt6::Widgets via transitive)
#             harken_editor_common      (InspectorPanel_p.h pimpl exposes ParameterSlider/CollapsibleSection/ToggleSwitch)
#
# Note: requires add_subdirectory order Common -> Inspector
#       in the parent CMakeLists.txt, so the sibling targets exist at link time.

add_library(harken_editor_inspector STATIC
    InspectorPanel.cpp
    InspectorPanel.h
    InspectorPanel_p.h
    # Tabbed Inspector page container (generic tabs vocabulary host).
    InspectorTabContainer.cpp
    InspectorTabContainer.h
    # Advanced launch-entry row (status count + Open
    # Editor detach button; replaces inline complex sections in the Advanced tab).
    InspectorLaunchEntry.cpp
    InspectorLaunchEntry.h
    InspectorPanel_Bus.cpp
    InspectorPanel_Container.cpp
    InspectorPanel_Layer.cpp
    InspectorPanel_Misc.cpp
    InspectorPanel_Segment.cpp
    InspectorPanel_Sound.cpp
    InspectorPanel_Track.cpp
    InspectorPanel_Variable.cpp
    # Inspector unification S3: descriptor-driven Layer 2 renderer (consumes a
    # C++ PropertySchema; sole Inspector renderer since the declarative path was
    # retired in S6).
    PropertyEditorEngine.cpp
    PropertyEditorEngine.h
    PropertyEditorEngine_FieldGroups.cpp
    FieldGroupInspectorView.cpp
    FieldGroupInspectorView.h
    # Inspector unification S6: descriptor -> JSON schema dumper. Surviving
    # producer for the get_inspector_schema IPC (legacy schemaToJson() shape).
    PropertySchemaJson.cpp
    PropertySchemaJson.h
    # Inspector unification S5: VAudio::schema() filtered to the Sound-page
    # scalar allowlist (engine-rendered scalar sections for the Sound page).
    SoundInspectorSchema.cpp
    SoundInspectorSchema.h
    ContentObjectSchemaRows.h
    # Action inspector S2: empty scalar schema (Action page = single
    # "steps" section provider on the Basic tab, no scalar fields).
    ActionInspectorSchema.cpp
    ActionInspectorSchema.h
    # Attenuation inspector S1: empty scalar schema (Attenuation page = single
    # "dimensions" section provider on the Basic tab, no scalar fields).
    AttenuationInspectorSchema.cpp
    AttenuationInspectorSchema.h
    # Inspector redesign S2: VBus::schema() filtered to the Bus-page
    # scalar allowlist (descriptor-driven 5-tab inline).
    BusInspectorSchema.cpp
    BusInspectorSchema.h
    # Layer scalar schema (VMixNode chassis allowlist for the Layer page).
    LayerInspectorSchema.cpp
    LayerInspectorSchema.h
    ContainerInspectorSchema.cpp
    ContainerInspectorSchema.h
    TrackInspectorSchema.cpp
    TrackInspectorSchema.h
    SegmentInspectorSchema.cpp
    SegmentInspectorSchema.h
    VariableInspectorSchema.cpp
    VariableInspectorSchema.h
    ProjectItemInspectorRows.h
    InspectorLayout.cpp
    InspectorLayout.h
    # Advanced launch-slot -> editor panel-type map (the
    # Inspector half of the launch-router; QtCore-only lookup).
    EditorLaunchMap.cpp
    EditorLaunchMap.h
    InspectorCustomization.h
    InspectorCustomizationRegistry.cpp
    InspectorCustomizationRegistry.h
    InspectorSlotKind.h
    InspectorSlotButton.cpp
    InspectorSlotButton.h
    InspectorSlotStrip.cpp
    InspectorSlotStrip.h
    # Inspector unification S5: startup registration of the built-in
    # customizations (called from MainWindow after initializeEditorSingletons).
    InspectorCustomizationBootstrap.cpp
    InspectorCustomizationBootstrap.h
    # S4 concrete customizations (reuse existing editors; engine wires them in).
    Drawers/ObjectRefFieldDrawer.cpp
    Drawers/ObjectRefFieldDrawer.h
    Drawers/ColorFieldDrawer.cpp
    Drawers/ColorFieldDrawer.h
    # Output routing D1: generic PropType::List renderer (rows per elementSchema,
    # add/remove; read-only derived lists reuse it with hidden mutations).
    Drawers/ListFieldDrawer.cpp
    Drawers/ListFieldDrawer.h
    # Variable declaration: paired low/high editor for a PropType::Range row.
    Drawers/RangeFieldDrawer.cpp
    Drawers/RangeFieldDrawer.h
    BindingFieldDecorator.cpp
    BindingFieldDecorator.h
    AttenuationFieldDecorator.cpp
    AttenuationFieldDecorator.h
    BindingFieldEditor.cpp
    BindingFieldEditor.h
    BindingAddMenu.cpp
    BindingAddMenu.h
    Providers/ClipsSectionProvider.cpp
    Providers/ClipsSectionProvider.h
    # Selection slot: Variable Control page launches Grid Editor; the matrix
    # lives there. Other modes keep their behavior / queue controls.
    Providers/SelectionSectionProvider.cpp
    Providers/SelectionSectionProvider.h
    Providers/SourcesSectionProvider.cpp
    Providers/SourcesSectionProvider.h
    Providers/StepsSectionProvider.cpp
    Providers/StepsSectionProvider.h
    # Attenuation inspector S1: Dimensions list (one row per dimension type with
    # curve thumbnail; commits via add/remove_attenuation_dimension).
    Providers/DimensionsSectionProvider.cpp
    Providers/DimensionsSectionProvider.h
    WidgetFactory.cpp
    WidgetFactory.h
    ObjectReferencePicker.cpp
    ObjectReferencePicker.h
)

set_target_properties(harken_editor_inspector PROPERTIES
    AUTOMOC ON
    AUTOGEN_BUILD_DIR "${CMAKE_BINARY_DIR}/_autogen/harken_editor_inspector"
)

# PUBLIC: consumers can include "Inspector/InspectorPanel.h" via this directory's parent (src/Widgets/)
target_include_directories(harken_editor_inspector PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

# Foundation brings Qt6::Widgets + Core headers (AppSettings).
# harken_editor_common brings ParameterSlider/CollapsibleSection/ToggleSwitch (exposed via InspectorPanel_p.h pimpl).
target_link_libraries(harken_editor_inspector PUBLIC
    harken_editor_foundation
    harken_editor_common
    harken_editor_shared
    harken_editor_blueprint
    # Output routing D3: ObjectRefFieldDrawer builds Selector/ObjectRefWidget
    # (opens the shared SelectorWindow popup).
    harken_editor_selector
)

# U4 S5: Sources range popup reuses SourceEditorWaveformView.
target_link_libraries(harken_editor_inspector PRIVATE
    harken_editor_source_editor
    harken_editor_curve_editor
)

if(MSVC)
    target_compile_options(harken_editor_inspector PRIVATE /W4 /wd4100 /wd4189 /utf-8)
    target_compile_definitions(harken_editor_inspector PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
