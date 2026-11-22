add_library(harken_editor_file_viewer STATIC
    FileViewerPanel.cpp
    FileViewerPanel.h
    FileViewerWidget.cpp
    FileViewerWidget.h
)

set_target_properties(harken_editor_file_viewer PROPERTIES
    AUTOMOC ON
    AUTOGEN_BUILD_DIR "${CMAKE_BINARY_DIR}/_autogen/harken_editor_file_viewer"
)

target_include_directories(harken_editor_file_viewer PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(harken_editor_file_viewer PUBLIC
    harken_editor_source_editor
    harken_editor_shared
)

if(MSVC)
    target_compile_options(harken_editor_file_viewer PRIVATE /W4 /wd4100 /wd4189 /utf-8)
    target_compile_definitions(harken_editor_file_viewer PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
