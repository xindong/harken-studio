add_library(harken_editor_information STATIC
    InformationPanel.cpp
    InformationPanel.h
)

set_target_properties(harken_editor_information PROPERTIES
    AUTOMOC ON
    AUTOGEN_BUILD_DIR "${CMAKE_BINARY_DIR}/_autogen/harken_editor_information"
)

target_include_directories(harken_editor_information PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_link_libraries(harken_editor_information PUBLIC harken_editor_foundation)

if(MSVC)
    target_compile_options(harken_editor_information PRIVATE /W4 /wd4100 /wd4189 /utf-8)
    target_compile_definitions(harken_editor_information PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
