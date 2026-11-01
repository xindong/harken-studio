# harken_editor_common -- STATIC library for common reusable Widget components.
# Pattern: same as harken_editor_foundation. S3 is the template for all subsequent module splits.
# Consumers: harken_editor_lib, harken_editor_blueprint (NodeItem uses WaveformRenderer)

add_library(harken_editor_common STATIC
    AmplitudeAxis.cpp
    AmplitudeAxis.h
    AmplitudeCurvePopup.cpp
    AmplitudeCurvePopup.h
    AxisCrosshair.cpp
    AxisCrosshair.h
    TimeAxis.cpp
    TimeAxis.h
    CollapsibleSection.cpp
    CollapsibleSection.h
    DialogFormLayout.cpp
    DialogFormLayout.h
    DropZoneWidget.cpp
    DropZoneWidget.h
    FocusFilter.h
    ComponentRecipeHost.cpp
    ComponentRecipeHost.h
    DirectJumpSlider.cpp
    DirectJumpSlider.h
    ParameterBinding.cpp
    ParameterBinding.h
    ParameterValueEditor.cpp
    ParameterValueEditor.h
    ParameterPresenter.h
    PanelScrollArea.cpp
    PanelScrollArea.h
    ParameterControlHost.cpp
    ParameterControlHost.h
    LinearSliderPresenter.cpp
    LinearSliderPresenter.h
    ListFooter.cpp
    ListFooter.h
    ElidedLabel.h
    TableColumnLayout.cpp
    TableColumnLayout.h
    TableGroupHeader.cpp
    TableGroupHeader.h
    TableItemDelegate.cpp
    TableItemDelegate.h
    TableWidget.cpp
    TableWidget.h
    ResizableTableView.cpp
    ResizableTableView.h
    MatrixTableWidget.cpp
    MatrixTableWidget.h
    MenuComboBox.cpp
    MenuComboBox.h
    MultiSelectComboBox.cpp
    MultiSelectComboBox.h
    VerticalFaderPresenter.cpp
    VerticalFaderPresenter.h
    ParameterSlider.cpp
    ParameterSlider.h
    DecimalSpinBox.h
    NumericToolTip.h
    RehearsalSlider.cpp
    RehearsalSlider.h
    AxisRehearsalRow.cpp
    AxisRehearsalRow.h
    RangeField.cpp
    RangeField.h
    ReferenceImpactDialog.cpp
    ReferenceImpactDialog.h
    ReorderHeaderView.cpp
    ReorderHeaderView.h
    ProportionalTreeWidget.cpp
    ProportionalTreeWidget.h
    SearchField.cpp
    SearchField.h
    SegmentedControl.cpp
    SegmentedControl.h
    SplitButton.cpp
    SplitButton.h
    StatusTip.h
    SwitchIndicator.h
    SourceRangeThumb.cpp
    SourceRangeThumb.h
    ToggleSwitch.cpp
    ToggleSwitch.h
    ToastPresenter.cpp
    ToastPresenter.h
    ValueSegmentedControl.cpp
    ValueSegmentedControl.h
    WaveformRenderer.cpp
    WaveformRenderer.h
    WheelGuard.cpp
    WheelGuard.h
)

set_target_properties(harken_editor_common PROPERTIES
    AUTOMOC ON
    AUTOGEN_BUILD_DIR "${CMAKE_BINARY_DIR}/_autogen/harken_editor_common"
)

# PUBLIC: consumers can include "Common/WaveformRenderer.h" via this directory's parent (src/Widgets/)
target_include_directories(harken_editor_common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

# Foundation brings Qt6::Widgets + all Core headers transitively
target_link_libraries(harken_editor_common PUBLIC
    harken_editor_foundation
)

if(MSVC)
    target_compile_options(harken_editor_common PRIVATE /W4 /wd4100 /wd4189 /utf-8)
    target_compile_definitions(harken_editor_common PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
