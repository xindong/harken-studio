add_library(harken_editor_time_series STATIC
    TimeSeriesChartModel.cpp
    TimeSeriesChartModel.h
    TimeSeriesChart.cpp
    TimeSeriesChart.h
    MonitorCursorModel.cpp
    MonitorCursorModel.h
    MonitorCursorMark.cpp
    MonitorCursorMark.h
)

set_target_properties(harken_editor_time_series PROPERTIES
    AUTOMOC ON
    AUTOGEN_BUILD_DIR "${CMAKE_BINARY_DIR}/_autogen/harken_editor_time_series"
)

target_include_directories(harken_editor_time_series PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
)

target_link_libraries(harken_editor_time_series PUBLIC
    harken_editor_foundation
)

if(MSVC)
    target_compile_options(harken_editor_time_series PRIVATE /W4 /wd4100 /wd4189 /utf-8)
    target_compile_definitions(harken_editor_time_series PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
