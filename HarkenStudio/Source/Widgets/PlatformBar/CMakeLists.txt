# harken_editor_platformbar -- STATIC library for the PlatformBar widget.
# Pattern: leaf (foundation + common).
# Consumers: harken_editor_lib (MainWindow.cpp).
# Depends on: harken_editor_foundation (PlatformContextService, ProjectManager)
#             harken_editor_common (SegmentedControl).

add_library(harken_editor_platformbar STATIC
    PlatformBar.cpp
    PlatformBar.h
    PlaybackControls.cpp
    PlaybackControls.h
)

set_target_properties(harken_editor_platformbar PROPERTIES
    AUTOMOC ON
    AUTOGEN_BUILD_DIR "${CMAKE_BINARY_DIR}/_autogen/harken_editor_platformbar"
)

target_include_directories(harken_editor_platformbar PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_link_libraries(harken_editor_platformbar PUBLIC
    harken_editor_foundation
    harken_editor_common
)

if(MSVC)
    target_compile_options(harken_editor_platformbar PRIVATE /W4 /wd4100 /wd4189 /utf-8)
    target_compile_definitions(harken_editor_platformbar PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
