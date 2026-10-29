# harken_editor_dialogs -- STATIC library for editor dialog widgets.
# Consumers: harken_editor_lib and the HarkenStudio executable startup wrapper.
# Depends on: harken_editor_foundation (BaseDialog, AppSettings, ThemeManager,
#             DialogManager, ProjectManager, ProjectTypes, ProjectPackExporter,
#             ActionManager, UndoCommands, ShortcutManager,
#             AssetPipeline/PathUtils).

add_library(harken_editor_dialogs STATIC
    AboutDialog.cpp
    AboutDialog.h
    ColorEditorDialog.cpp
    ColorEditorDialog.h
    RemoteConnectPanel.cpp
    RemoteConnectPanel.h
    NewProjectDialog.cpp
    NewProjectDialog.h
    TemplateChooserWidget.cpp
    TemplateChooserWidget.h
    Settings/SettingsPage.cpp
    Settings/SettingsPage.h
    Settings/SettingsPanel.cpp
    Settings/SettingsPanel.h
    Settings/PreferencesPanel.cpp
    Settings/PreferencesPanel.h
    Settings/ProjectSettingsPanel.cpp
    Settings/ProjectSettingsPanel.h
    Settings/Pages/AppearancePage.cpp
    Settings/Pages/AppearancePage.h
    Settings/Pages/ExtensionPage.cpp
    Settings/Pages/ExtensionPage.h
    Settings/Pages/ShortcutsPage.cpp
    Settings/Pages/ShortcutsPage.h
    Settings/Pages/LayoutPage.cpp
    Settings/Pages/LayoutPage.h
    Settings/Pages/AIAgentPage.cpp
    Settings/Pages/AIAgentPage.h
    Settings/Pages/ProjectConfigPage.cpp
    Settings/Pages/ProjectConfigPage.h
    Settings/Pages/ProjectPublishPage.cpp
    Settings/Pages/ProjectPublishPage.h
    Settings/ProjectPlatformDialog.cpp
    Settings/ProjectPlatformDialog.h
    Settings/ProjectPlatformEditor.cpp
    Settings/ProjectPlatformEditor.h
    StartupLoadingWindow.cpp
    StartupLoadingWindow.h
    StartupRenderer.cpp
    StartupRenderer.h
)

set_target_properties(harken_editor_dialogs PROPERTIES
    AUTOMOC ON
    AUTOGEN_BUILD_DIR "${CMAKE_BINARY_DIR}/_autogen/harken_editor_dialogs"
)

# PUBLIC: consumers can include "Dialogs/Settings/PreferencesPanel.h" via this directory's parent (src/)
target_include_directories(harken_editor_dialogs PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_link_libraries(harken_editor_dialogs
    PUBLIC harken_editor_foundation harken_editor_common
    PRIVATE Qt6::Network Qt6::OpenGLWidgets
)

if(MSVC)
    target_compile_options(harken_editor_dialogs PRIVATE /W4 /wd4100 /wd4189 /utf-8)
    target_compile_definitions(harken_editor_dialogs PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
