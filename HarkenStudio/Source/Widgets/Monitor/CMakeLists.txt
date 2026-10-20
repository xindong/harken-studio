add_library(harken_editor_monitor STATIC
    MonitorCatalogModel.cpp
    MonitorCatalogModel.h
    MonitorPanel.cpp
    MonitorPanel.h
    MonitorPlayablesPanel.cpp
    MonitorPlayablesPanel.h
    MonitorPlayablesSource.cpp
    MonitorPlayablesSource.h
    MonitorEffectsModel.cpp
    MonitorEffectsModel.h
    MonitorEffectsPanel.cpp
    MonitorEffectsPanel.h
    MonitorEffectsSource.cpp
    MonitorEffectsSource.h
    MonitorBusesModel.cpp
    MonitorBusesModel.h
    MonitorBusesPanel.cpp
    MonitorBusesPanel.h
    MonitorBusesSource.cpp
    MonitorBusesSource.h
    MonitorDspTimelinePanel.cpp
    MonitorDspTimelinePanel.h
    MonitorDspTimelineSource.cpp
    MonitorDspTimelineSource.h
    MonitorLineagePanel.cpp
    MonitorLineagePanel.h
    MonitorLineageSource.cpp
    MonitorLineageSource.h
    MonitorTapSelectPanel.cpp
    MonitorTapSelectPanel.h
    MonitorTapSelectSource.cpp
    MonitorTapSelectSource.h
    MonitorWaveformPanel.cpp
    MonitorWaveformPanel.h
    MonitorWaveformSource.cpp
    MonitorWaveformSource.h
    MonitorScopePanel.cpp
    MonitorScopePanel.h
    MonitorScopeSource.cpp
    MonitorScopeSource.h
    MonitorSpectrumPanel.cpp
    MonitorSpectrumPanel.h
    MonitorSpectrumSource.cpp
    MonitorSpectrumSource.h
    MonitorVoicesModel.cpp
    MonitorVoicesModel.h
    MonitorVoicesPanel.cpp
    MonitorVoicesPanel.h
    MonitorVoicesSource.cpp
    MonitorVoicesSource.h
    MonitorPanelKeys.cpp
    MonitorPanelKeys.h
    MonitorPerformanceChartSource.cpp
    MonitorPerformanceChartSource.h
    MonitorPlaceholderPage.cpp
    MonitorPlaceholderPage.h
    MonitorCaptureControlStrip.cpp
    MonitorCaptureControlStrip.h
    MonitorRemoteStatusStrip.cpp
    MonitorRemoteStatusStrip.h
    MonitorRecordingPanel.cpp
    MonitorRecordingPanel.h
    MonitorSessionComparePanel.cpp
    MonitorSessionComparePanel.h
    MonitorPerformancePanel.cpp
    MonitorPerformancePanel.h
)

set_target_properties(harken_editor_monitor PROPERTIES
    AUTOMOC ON
    AUTOGEN_BUILD_DIR "${CMAKE_BINARY_DIR}/_autogen/harken_editor_monitor"
)

target_include_directories(harken_editor_monitor PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
)

target_link_libraries(harken_editor_monitor PUBLIC
    harken_editor_foundation
    harken_editor_time_series
    harken_editor_monitor_data
)

if(MSVC)
    target_compile_options(harken_editor_monitor PRIVATE /W4 /wd4100 /wd4189 /utf-8)
    target_compile_definitions(harken_editor_monitor PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
