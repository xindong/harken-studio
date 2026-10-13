# harken_editor_titlebar -- STATIC library for the TitleBarWidget.
# Pattern: leaf (foundation + QWindowKit).
# Consumers: harken_editor_lib (MainWindow.h:35 -- header-level public dependency).
# Depends on: harken_editor_foundation (Qt6::Widgets via transitive: QWidget, QLabel,
# QPushButton, QMenuBar, QHBoxLayout).

add_library(harken_editor_titlebar STATIC
    TitleBarButton.cpp
    TitleBarButton.h
    TitleBarWidget.cpp
    TitleBarWidget.h
    FloatingTitleBar.cpp
    FloatingTitleBar.h
    DialogTitleBar.cpp
    DialogTitleBar.h
    FramelessChromeInstall.cpp
    FramelessChromeInstall.h
)

set_target_properties(harken_editor_titlebar PROPERTIES
    AUTOMOC ON
    AUTOGEN_BUILD_DIR "${CMAKE_BINARY_DIR}/_autogen/harken_editor_titlebar"
)

target_include_directories(harken_editor_titlebar PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_link_libraries(harken_editor_titlebar PUBLIC harken_editor_foundation)

# FramelessChromeInstall wires DialogTitleBar into Core's frameless-chrome hook via QWindowKit.
target_link_libraries(harken_editor_titlebar PRIVATE QWindowKit::Widgets)
target_compile_definitions(harken_editor_titlebar PRIVATE HARKEN_HAS_QWINDOWKIT)

if(MSVC)
    target_compile_options(harken_editor_titlebar PRIVATE /W4 /wd4100 /wd4189 /utf-8)
    target_compile_definitions(harken_editor_titlebar PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
