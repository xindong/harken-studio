add_library(harken_editor_control STATIC
    ControlPanel.cpp
    ControlPanel.h
    RehearsalTable.cpp
    RehearsalTable.h
    VariableRehearsalRow.cpp
    VariableRehearsalRow.h
    VariableBindingTable.cpp
    VariableBindingTable.h
)

set_target_properties(harken_editor_control PROPERTIES
    AUTOMOC ON
    AUTOGEN_BUILD_DIR "${CMAKE_BINARY_DIR}/_autogen/harken_editor_control"
)

target_include_directories(harken_editor_control PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_link_libraries(harken_editor_control PUBLIC
    harken_editor_foundation
    harken_editor_common
    harken_editor_shared
    harken_editor_selector
)

if(MSVC)
    target_compile_options(harken_editor_control PRIVATE /W4 /wd4100 /wd4189 /utf-8)
    target_compile_definitions(harken_editor_control PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
