add_library(harken_editor_meter STATIC
    MeterPanel.cpp
    MeterPanel.h
    MeterBallistics.h
    MeterSource.cpp
    MeterSource.h
    FrequencyScale.h
    SmoothingUtils.h
    MeterColorMap.h
    MeterRenderCommon.h
    MeterScale.cpp
    MeterScale.h
    MeterInteractionRenderer.cpp
    MeterInteractionRenderer.h
    PeakMeterRenderer.cpp
    PeakMeterRenderer.h
    SpectrumMeterRenderer.cpp
    SpectrumMeterRenderer.h
    SpectrogramMeterRenderer.cpp
    SpectrogramMeterRenderer.h
    SpectrogramHistory.cpp
    SpectrogramHistory.h
)

set_target_properties(harken_editor_meter PROPERTIES
    AUTOMOC ON
    AUTOGEN_BUILD_DIR "${CMAKE_BINARY_DIR}/_autogen/harken_editor_meter"
)

target_include_directories(harken_editor_meter PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(harken_editor_meter PUBLIC
    harken_editor_foundation
    harken_editor_common
    harken_editor_selector
    harken_editor_monitor_data
)

if(MSVC)
    target_compile_options(harken_editor_meter PRIVATE /W4 /wd4100 /wd4189 /utf-8)
    target_compile_definitions(harken_editor_meter PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
