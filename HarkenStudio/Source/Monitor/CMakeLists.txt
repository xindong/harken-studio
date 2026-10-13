# harken_editor_monitor_data -- Monitor adapter / host context.
# Zero Qt Widgets. Public surface is open/close/poll/status plus
# P5-6 Adapter write methods (Validate/Apply/Stop/Acquire) posted to
# the logic thread. P3-7 Host remains open + getCaptureControlStatus.
# Links Qt6::Core + harken_runtime_bridge (Foundation is UI-coupled; this
# data layer talks to the already-submitted Vn_Monitor* C API).

add_library(harken_editor_monitor_data STATIC
    MonitorHostContext.h
    MonitorAdapter.h
    MonitorAdapter.cpp
    MonitorCaptureControl.h
    MonitorCaptureControl.cpp
    MonitorRemoteStatus.h
    MonitorRemoteStatus.cpp
    MonitorRemoteAllowlist.h
    MonitorRemoteAllowlist.cpp
    MonitorRecordingBrowser.h
    MonitorRecordingBrowser.cpp
    MonitorSessionCompareUi.h
    MonitorSessionCompareUi.cpp
    MonitorCaptureControlHost.h
    MonitorCaptureControlHost.cpp
    MonitorPerformanceLiveHost.h
    MonitorPerformanceLiveHost.cpp
    CapturePlanIntent.h
    CapturePlanIntent.cpp
    MonitorDecoder.h
    MonitorDecoder.cpp
    EventCaptureController.h
    EventCaptureController.cpp
    MonitorStore.h
    MonitorStore.cpp
    MonitorProjectionTypes.h
    MonitorStateCut.h
    MonitorStateCut.cpp
    MonitorGeneration.h
    MonitorGeneration.cpp
    MonitorDrop.h
    MonitorDrop.cpp

    # P2-5 availability
    MonitorAvailability.h
    MonitorAvailability.cpp
    # P2-6 session-file
    MonitorSessionFile.h
    MonitorSessionFile.cpp
    PlayableLifecycleProjection.h
    PlayableLifecycleProjection.cpp
    EffectMonitorProjection.h
    EffectMonitorProjection.cpp
    MonitorReplayIntegrity.h
    MonitorReplayIntegrity.cpp
    BusTopologyProjection.h
    BusTopologyProjection.cpp
    DspTimelineProjection.h
    DspTimelineProjection.cpp
    LineageProjection.h
    LineageProjection.cpp
    MonitorTapBinding.h
    MonitorTapBinding.cpp
    WaveformBucketProjection.h
    WaveformBucketProjection.cpp
    ScopeTriggerProjection.h
    ScopeTriggerProjection.cpp
    SpectrumDisplayProjection.h
    SpectrumDisplayProjection.cpp
    MonitorReplayIntegrity.h
    MonitorReplayIntegrity.cpp
)

set_target_properties(harken_editor_monitor_data PROPERTIES AUTOMOC ON)

target_include_directories(harken_editor_monitor_data PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(harken_editor_monitor_data PUBLIC
    Qt6::Core
    harken_runtime_bridge
)

if(MSVC)
    target_compile_options(harken_editor_monitor_data PRIVATE /W4 /wd4100 /wd4189 /utf-8)
    target_compile_definitions(harken_editor_monitor_data PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
