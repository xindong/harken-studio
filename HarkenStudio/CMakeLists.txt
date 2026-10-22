# Harken Studio — the Qt front end. Everything under this directory is
# editor-side: it is never linked into harken_core and carries its own
# third-party closure. Engine contact is isolated in Source/RuntimeBridge and
# uses the harken_core in-process C API.

# Editor tree opts into C++ exceptions. Runtime stays exception-disabled
# (Audio Thread no-throw discipline); Editor is Qt6 GUI and uses try/catch
# for LMDB recovery + QtConcurrent async safety. clang-cl defaults EH off,
# so  this must be explicit; MSVC accepts /EHsc as a harmless duplicate.
# /permissive- and _CRT_SECURE_NO_WARNINGS mirror the reference project's
# global settings, scoped here so the runtime tree keeps its own flags.
if(MSVC)
    add_compile_options(/EHsc /permissive- /FS)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

# Binary output directories are set globally at the top level (bin/ for
# runtime,  lib/ for archives); the editor exe, the engine DLL, the deployed
# Qt runtime, and the project templates all land side by side in bin/.

# --- Front-end third-party libraries (vendored) ---
add_subdirectory(ThirdParty/libogg)
add_subdirectory(ThirdParty/libvorbis)
add_subdirectory(ThirdParty/r8brain-free-src)

# LMDB support (optional — persistent search index backend)
option(HARKEN_DISABLE_LMDB "Disable LMDB search backend (fallback to memory)" OFF)
if(NOT HARKEN_DISABLE_LMDB)
    add_subdirectory(ThirdParty/lmdb)
endif()

# Qt Advanced Docking System (QtADS v4.3.1)
set(ADS_VERSION "4.3.1" CACHE STRING "QtADS version (vendored, no git describe)")
set(BUILD_STATIC ON CACHE BOOL "Build QtADS as static library")
set(BUILD_EXAMPLES OFF CACHE BOOL "Skip QtADS examples")
add_subdirectory(ThirdParty/QtADS)

# QWindowKit — custom frameless window chrome. Pinned to the exact commit the
# reference project's builds used (its upstream declared a moving branch,
# which is not reproducible).
include(FetchContent)
FetchContent_Declare(
    QWindowKit
    GIT_REPOSITORY https://github.com/stdware/qwindowkit.git
    GIT_TAG        c783b89d119a672b0aa4262b520c0783d0612689
)
set(QWINDOWKIT_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(QWINDOWKIT_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(QWindowKit)

# Find Qt6 (located through CMAKE_PREFIX_PATH = $env{VENUS_QT_DIR}, preset-injected)
find_package(Qt6 REQUIRED COMPONENTS Widgets OpenGLWidgets Network Concurrent Svg)

function(harken_remove_missing_agl_framework target_name)
    if(NOT TARGET ${target_name})
        return()
    endif()

    foreach(property_name
            LINK_LIBRARIES
            LINK_OPTIONS
            INTERFACE_LINK_LIBRARIES
            INTERFACE_LINK_OPTIONS
            IMPORTED_LINK_DEPENDENT_LIBRARIES
            IMPORTED_LINK_DEPENDENT_LIBRARIES_DEBUG
            IMPORTED_LINK_DEPENDENT_LIBRARIES_RELEASE
            IMPORTED_LINK_DEPENDENT_LIBRARIES_RELWITHDEBINFO
            IMPORTED_LINK_DEPENDENT_LIBRARIES_MINSIZEREL
            IMPORTED_LINK_INTERFACE_LIBRARIES
            IMPORTED_LINK_INTERFACE_LIBRARIES_DEBUG
            IMPORTED_LINK_INTERFACE_LIBRARIES_RELEASE
            IMPORTED_LINK_INTERFACE_LIBRARIES_RELWITHDEBINFO
            IMPORTED_LINK_INTERFACE_LIBRARIES_MINSIZEREL)
        get_target_property(_harken_original_links ${target_name} ${property_name})
        if(NOT _harken_original_links)
            continue()
        endif()

        set(_harken_sanitized_links "")
        set(_harken_pending_framework_flag FALSE)
        foreach(_harken_link IN LISTS _harken_original_links)
            if(_harken_pending_framework_flag)
                if(NOT _harken_link STREQUAL "AGL")
                    list(APPEND _harken_sanitized_links "-framework" "${_harken_link}")
                endif()
                set(_harken_pending_framework_flag FALSE)
                continue()
            endif()

            if(_harken_link STREQUAL "-framework AGL"
                    OR _harken_link MATCHES "(^|/)AGL\\.framework(/|$)")
                continue()
            elseif(_harken_link STREQUAL "-framework")
                set(_harken_pending_framework_flag TRUE)
            else()
                list(APPEND _harken_sanitized_links "${_harken_link}")
            endif()
        endforeach()
        if(_harken_pending_framework_flag)
            list(APPEND _harken_sanitized_links "-framework")
        endif()

        set_target_properties(${target_name} PROPERTIES
            ${property_name} "${_harken_sanitized_links}")
    endforeach()
endfunction()

if(APPLE)
    set(_harken_macos_sdk_root "${CMAKE_OSX_SYSROOT}")
    if(NOT _harken_macos_sdk_root)
        execute_process(
            COMMAND xcrun --sdk macosx --show-sdk-path
            OUTPUT_VARIABLE _harken_macos_sdk_root
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
    endif()

    if(_harken_macos_sdk_root
            AND NOT EXISTS "${_harken_macos_sdk_root}/System/Library/Frameworks/AGL.framework")
        set(_harken_macos_agl_framework_missing TRUE)
        harken_remove_missing_agl_framework(QWKCore)
        harken_remove_missing_agl_framework(QWKWidgets)
        harken_remove_missing_agl_framework(WrapOpenGL::WrapOpenGL)
        harken_remove_missing_agl_framework(Qt6::Gui)
        harken_remove_missing_agl_framework(Qt6::GuiPrivate)
        harken_remove_missing_agl_framework(Qt6::Widgets)
        harken_remove_missing_agl_framework(Qt6::WidgetsPrivate)
        harken_remove_missing_agl_framework(Qt6::OpenGL)
        harken_remove_missing_agl_framework(Qt6::OpenGLWidgets)
    endif()
endif()

# ==============================================================================
# harken_editor_lib — OBJECT library containing all Editor code except Main.cpp
# Reusable by harken_editor (production) and harken_editor_tests (testing).
# ==============================================================================
# Source Files — explicit list (no GLOB_RECURSE)
# When adding new files, add them here and re-run cmake configure.
set(HARKEN_EDITOR_LIB_SOURCES
    # --- Source ---
    Source/MainWindow.cpp
    Source/MainWindow.h
    # --- Source/Core (UI-coupled layer — depends on Widget headers, cannot go in Foundation) ---
    Source/Core/LayoutController.cpp
    Source/Core/LayoutController.h
    Source/Core/QtAdsWindowAdapter.cpp
    Source/Core/QtAdsWindowAdapter.h
    Source/Core/QtAdsThemeAdapter.cpp
    Source/Core/QtAdsThemeAdapter.h
    Source/Core/SelectionRouter.cpp
    Source/Core/SelectionRouter.h
    Source/Core/StandaloneEditorDispatch.cpp
    Source/Core/StandaloneEditorDispatch.h
    Source/Core/CurveSelection.h
    Source/Core/BusController.cpp
    Source/Core/BusController.h
    Source/Core/WorkspaceWindowManager.cpp
    Source/Core/WorkspaceWindowManager.h
    Source/Core/WorkspaceWindowRegistry.cpp
    Source/Core/WorkspaceWindowRegistry.h
    Source/Core/WorkspaceDockFactory.cpp
    Source/Core/WorkspaceDockFactory.h
    Source/Core/HubClient/HubClient.cpp
    Source/Core/HubClient/HubClient.h
    Source/Core/HubClient/HubIntake.cpp
    Source/Core/HubClient/HubIntake.h
    Source/Core/HubLauncher.cpp
    Source/Core/HubLauncher.h
    Source/Core/StartupCoordinator.cpp
    Source/Core/StartupCoordinator.h
    Source/Core/ProjectInstance.cpp
    Source/Core/ProjectInstance.h
    Source/Core/ProjectFormatVersion.h
    Source/Core/ProjectPreflight/ProjectPreflight.cpp
    Source/Core/ProjectPreflight/ProjectPreflight.h
    Source/Core/ProjectOpenProtocol.cpp
    Source/Core/ProjectOpenProtocol.h
    Source/Core/ProjectOpenApproval.cpp
    Source/Core/ProjectOpenApproval.h
    Source/Core/IPC/IPCServer.h
    Source/Core/IPC/IPCServer.cpp
    Source/Core/AI/AICommandCatalog.h
    Source/Core/AI/AICommandCatalog.cpp
    Source/Core/AI/AIPermissionModel.h
    Source/Core/AI/AIPermissionModel.cpp
    Source/Core/AI/AICommandValidator.h
    Source/Core/AI/AICommandValidator.cpp
    Source/Core/AI/AIDispatchPolicy.h
    Source/Core/AI/AIDispatchPolicy.cpp
    Source/Core/AI/AIResultNormalizer.h
    Source/Core/AI/AIResultNormalizer.cpp
    Source/Core/AI/AIEndpointValidation.h
    Source/Core/AI/AIAgentConfig.h
    Source/Core/AI/AIAgentConfig.cpp
    Source/Core/AI/AIDurableMemory.h
    Source/Core/AI/AIDurableMemoryStore.h
    Source/Core/AI/AIDurableMemoryStore.cpp
    Source/Core/AI/AIDurableMemoryService.h
    Source/Core/AI/AIDurableMemoryService.cpp
    Source/Core/AI/AIDurableMemoryRetrieval.h
    Source/Core/AI/AIDurableMemoryRetrieval.cpp
    Source/Core/AI/AIConversationStore.h
    Source/Core/AI/AIConversationStore.cpp
    Source/Core/AI/AIConversationSessionStore.h
    Source/Core/AI/AIConversationSessionStore.cpp
    Source/Core/AI/ILLMProvider.h
    Source/Core/AI/OpenAICompatibleLLMProvider.h
    Source/Core/AI/OpenAICompatibleLLMProvider.cpp
    Source/Core/AI/AIAssistantService.h
    Source/Core/AI/AIAssistantService.cpp
    Source/Core/AI/AIMarkdownRenderer.h
    Source/Core/AI/AIMarkdownRenderer.cpp
    Source/Core/AI/AIStepEvent.h
    Source/Core/IPC/IPCServerInternal.h
    Source/Core/IPC/IPCServer_RollbackUndo.cpp
    Source/Core/IPC/IPCHandlers_Advanced.cpp
    Source/Core/IPC/IPCHandlers_Asset.cpp
    Source/Core/IPC/IPCHandlers_Container.cpp
    Source/Core/IPC/IPCHandlers_Editor.cpp
    Source/Core/IPC/IPCHandlers_Mixer.cpp
    Source/Core/IPC/IPCHandlers_Project.cpp
    Source/Core/IPC/IPCHandlers_Spatial.cpp
    Source/Core/IPC/IPCHandlers_System.cpp
    Source/Core/ProjectSerializer.cpp
    Source/Core/AssetPipeline/ImportAssetsCommand.cpp
    Source/Core/AssetPipeline/ImportSoundSourcesCommand.cpp
    Source/Core/TemplateApplyService.cpp
    Source/Core/TemplateApplyService.h
    Source/Core/Actions/MixerActions.cpp
    Source/Core/Actions/ProjectActions.cpp
    Source/Core/Actions/ProjectPlatformActions.cpp
    Source/Core/Actions/FieldGroupActions.cpp
    Source/Core/Actions/BindingActions.cpp
    Source/Core/Actions/EditorActions.cpp
    Source/Core/Actions/AuditionActions.cpp
    Source/Core/Actions/SystemActions.cpp
    Source/Core/Actions/ContainerActions.cpp
    Source/Core/Actions/SpatialActions.cpp
    Source/Core/Actions/AdvancedActions.cpp
    Source/Core/Actions/AdvancedActionResult.h
    Source/Core/Actions/AssetActions.cpp
    Source/Core/Actions/TemplateActions.cpp
    # Plan 16 S5a IS-17/18 -- unified path-or-name selector (Rule G2 multi-selector)
    # shared helper for 14 SystemActions + 28 AdvancedActions handlers. Header
    # ActionSelectors.h is included by both *.cpp via "ActionSelectors.h"
    # (sibling within Source/Core/Actions/). Implementation forwards path-mode
    # input to VNX::findItemByPathInProject (defined in IPCHandlers_Project.cpp,
    # same target -- harken_editor_lib).
    Source/Core/Actions/ActionSelectors.cpp
    Source/Core/Actions/ActionSelectors.h
    Source/Core/Actions/ReadonlyActions.cpp
    # --- Source/Dialogs --- (split to harken_editor_dialogs in S10a)
    # --- Source/Widgets/ControlPanel --- (split to harken_editor_control in S10a)
    # --- Source/Widgets/Assistant --- (split to harken_editor_assistant in S10a)
    # --- Source/Widgets/Publisher --- (split to harken_editor_publisher in S10a)
    # --- Source/Platform ---
    Source/Platform/NativeDropTarget_Win.h
    Source/Platform/ExternalDropDispatcher.h
    Source/Platform/ExternalDropDispatcher.cpp
    Source/Platform/SecretStore.h
    # --- Source/Widgets (top-level) ---
    Source/Widgets/DragDropOverlay.h
    # --- Source/Widgets/Inspector --- (split to harken_editor_inspector in S9)
    # --- Source/Widgets/LogPanel --- (split to harken_editor_logpanel in S10a)
    # --- Source/Widgets/ProjectExplorer --- (split to harken_editor_explorer in S8)
    # --- Source/Widgets/TitleBar --- (split to harken_editor_titlebar in S10a)
)

# Platform-specific credential vault backend (SEC-002 compliance).
# Non-Win/Mac ports must not persist credentials — SecretStore.h #errors there.
# The macOS backend uses only CoreFoundation/Security C APIs, so it stays .cpp
# (not .mm) and remains compatible with harken_editor_lib's C++ PCH.
if(WIN32)
    list(APPEND HARKEN_EDITOR_LIB_SOURCES Source/Platform/SecretStore_Win.cpp)
elseif(APPLE)
    list(APPEND HARKEN_EDITOR_LIB_SOURCES Source/Platform/SecretStore_Mac.cpp)
endif()

# Runtime bridge -- the only sanctioned editor target that may talk to the
# engine. Must precede Source/Core: harken_editor_foundation links it; all
# editor runtime access goes through it.
add_subdirectory(Source/RuntimeBridge)
add_subdirectory(Source/Monitor)
add_subdirectory(Source/Core)
# S1a Blueprint — leaf lib (depends on Foundation only).
add_subdirectory(Source/Widgets/Blueprint)
add_subdirectory(Source/Widgets/Common)
# Output inspector Phase 2 — shared object selector popup (leaf lib, Foundation only).
add_subdirectory(Source/Widgets/Selector)
# factory-entry-unification S2 — shared "New Child" creation subsystem
# (ProjectItemMenuController). Foundation-only leaf lib; must be added
# BEFORE ProjectExplorer / Hierarchy since both link it.
add_subdirectory(Source/Widgets/Shared)
add_subdirectory(Source/Widgets/Meter)
add_subdirectory(Source/Widgets/TimeSeries)
add_subdirectory(Source/Widgets/Monitor)
add_subdirectory(Source/Widgets/SourceEditor)
add_subdirectory(Source/Widgets/CurveEditor)
add_subdirectory(Source/Widgets/ExternalEditor)
# Phase2 WP3: Grid Editor panel (leaf lib, Foundation only).
add_subdirectory(Source/Widgets/GridEditor)
add_subdirectory(Source/Widgets/Timeline)
add_subdirectory(Source/Widgets/TransitionEditor)
# Phase0 WP3 batch C: shared project-tree skeleton (ProjectTreePanel +
# ProjectTreeWidget); must come AFTER Common + Shared, BEFORE ProjectExplorer /
# Hierarchy since both link it.
add_subdirectory(Source/Widgets/ProjectTree)
# Sources tab lists AssetRegistry and emits sourceEditorRequested(sourceId);
# it does not link SourceEditor. MainWindow opens the standalone panel.
add_subdirectory(Source/Widgets/ProjectExplorer)
# Hierarchy: independent clone of ProjectExplorer (initial mirror, may diverge).
add_subdirectory(Source/Widgets/Hierarchy)
# S9 split: Inspector must come AFTER Common
# (InspectorPanel_p.h includes Common widgets).
add_subdirectory(Source/Widgets/Inspector)
# S10a splits: 7 leaf libs (Wizards merged into Dialogs)
add_subdirectory(Source/Dialogs)
add_subdirectory(Source/Widgets/Publisher)
add_subdirectory(Source/Widgets/LogPanel)
add_subdirectory(Source/Widgets/Information)
add_subdirectory(Source/Widgets/Assistant)
add_subdirectory(Source/Widgets/TitleBar)
add_subdirectory(Source/Widgets/ControlPanel)
add_subdirectory(Source/Widgets/PlatformBar)

add_library(harken_editor_lib OBJECT ${HARKEN_EDITOR_LIB_SOURCES})

target_link_libraries(harken_editor_lib PUBLIC harken_buildinfo)
target_link_libraries(harken_editor_lib PUBLIC harken_editor_foundation)
target_link_libraries(harken_editor_lib PUBLIC harken_editor_monitor_data)
target_link_libraries(harken_editor_lib PUBLIC harken_editor_common)
target_link_libraries(harken_editor_lib PUBLIC harken_editor_shared)
target_link_libraries(harken_editor_lib PUBLIC harken_editor_selector)
target_link_libraries(harken_editor_lib PUBLIC harken_editor_blueprint)
target_link_libraries(harken_editor_lib PUBLIC harken_editor_meter)
target_link_libraries(harken_editor_lib PUBLIC harken_editor_time_series)
target_link_libraries(harken_editor_lib PUBLIC harken_editor_monitor)
target_link_libraries(harken_editor_lib PUBLIC harken_editor_source_editor)
target_link_libraries(harken_editor_lib PUBLIC harken_editor_curve_editor)
target_link_libraries(harken_editor_lib PUBLIC harken_editor_external_editor)
target_link_libraries(harken_editor_lib PUBLIC harken_editor_grid_editor)
target_link_libraries(harken_editor_lib PUBLIC harken_editor_timeline)
target_link_libraries(harken_editor_lib PUBLIC harken_editor_transition_editor)
target_link_libraries(harken_editor_lib PUBLIC harken_editor_explorer)
target_link_libraries(harken_editor_lib PUBLIC harken_editor_hierarchy)
target_link_libraries(harken_editor_lib PUBLIC harken_editor_inspector)
# S10a splits
target_link_libraries(harken_editor_lib PUBLIC harken_editor_dialogs)
target_link_libraries(harken_editor_lib PUBLIC harken_editor_publisher)
target_link_libraries(harken_editor_lib PUBLIC harken_editor_logpanel)
target_link_libraries(harken_editor_lib PUBLIC harken_editor_information)
target_link_libraries(harken_editor_lib PUBLIC harken_editor_assistant)
target_link_libraries(harken_editor_lib PUBLIC harken_editor_titlebar)
target_link_libraries(harken_editor_lib PUBLIC harken_editor_control)
target_link_libraries(harken_editor_lib PUBLIC harken_editor_platformbar)

set_target_properties(harken_editor_lib PROPERTIES
    AUTOMOC ON
    AUTOGEN_BUILD_DIR "${CMAKE_BINARY_DIR}/_autogen/harken_editor_lib"
    AUTOUIC ON
)

# Precompiled Headers
target_precompile_headers(harken_editor_lib PRIVATE
    <QWidget>
    <QString>
    <QJsonObject>
    <QJsonArray>
    <QUndoCommand>
    <QTimer>
    <QDebug>
    <memory>
    <string>
    <vector>
    <unordered_map>
    <functional>
)

target_include_directories(harken_editor_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/Source
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Core
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Widgets
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Dialogs
)

target_link_libraries(harken_editor_lib PUBLIC
    Qt6::Widgets
    Qt6::OpenGLWidgets
    Qt6::Svg
    Qt6::Network
    Qt6::Concurrent
    vorbis
    vorbisenc
    ogg
    r8brain
    qt6advanceddocking
    QWindowKit::Widgets
)

target_compile_definitions(harken_editor_lib PUBLIC HARKEN_HAS_QWINDOWKIT)

if(NOT HARKEN_DISABLE_LMDB)
    target_link_libraries(harken_editor_lib PUBLIC lmdb)
    target_compile_definitions(harken_editor_lib PUBLIC HARKEN_HAS_LMDB)
endif()

# SecretStore backend on macOS uses Keychain Services + CoreFoundation.
# Windows side links Advapi32 via #pragma comment inside SecretStore_Win.cpp.
if(APPLE)
    target_link_libraries(harken_editor_lib PUBLIC
        "-framework Security"
        "-framework CoreFoundation"
    )
endif()

if(MSVC)
    target_compile_options(harken_editor_lib PRIVATE /W4 /wd4100 /wd4189 /utf-8)
    target_compile_definitions(harken_editor_lib PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# ==============================================================================
# harken_editor — Thin executable wrapper (binary name kept as HarkenStudio.exe via OUTPUT_NAME)
# ==============================================================================
if(WIN32)
    # Windows PE VERSIONINFO — filled from cmake/BuildInfo.cmake so the identity
    # embedded in the exe matches what the About dialog and startup logs report.
    # Right Click → Properties → Details reads FileVersion / ProductVersion /
    # Comments (which carries the full BuildInfo Label + commit + timestamp).
    add_executable(harken_editor WIN32 Source/Main.cpp "resources.qrc" "HarkenStudio.rc")
else()
    set_source_files_properties("Resources/icons/HarkenStudio.icns"
        PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
    add_executable(harken_editor Source/Main.cpp "resources.qrc"
        "Resources/icons/HarkenStudio.icns")
endif()

harken_attach_version_resource(
    harken_editor
    "Harken Studio"
    "HarkenStudio.exe")

set_target_properties(harken_editor PROPERTIES
    OUTPUT_NAME "HarkenStudio"
    AUTORCC ON
)

if(APPLE)
    set_target_properties(harken_editor PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "HarkenStudio"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.harkensound.harkenstudio"
        MACOSX_BUNDLE_BUNDLE_VERSION "${HARKEN_BI_VERSION_QUAD}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING
            "${HARKEN_BI_VERSION_MAJOR}.${HARKEN_BI_VERSION_MINOR}.${HARKEN_BI_VERSION_PATCH}"
        MACOSX_BUNDLE_ICON_FILE "HarkenStudio.icns"
    )
endif()

if(WIN32)
    set_target_properties(harken_editor PROPERTIES
        VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:harken_editor>"
        VS_DEBUGGER_ENVIRONMENT "QT_QPA_PLATFORM=windows"
    )

    file(GENERATE
        OUTPUT "$<TARGET_FILE_DIR:harken_editor>/qt.conf"
        CONTENT "[Paths]\nPlugins = .\n"
    )
endif()

target_link_libraries(harken_editor PRIVATE harken_editor_lib)

if(NOT Python3_Interpreter_FOUND)
    message(FATAL_ERROR "Python 3 is required to publish the Studio Schema snapshot")
endif()

add_custom_command(TARGET harken_editor POST_BUILD
    COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/.scripts/schema_archive.py"
        --schema-root "${HARKEN_SCHEMA_SOURCE_DIR}"
        --check-generated "${HARKEN_SCHEMA_GENERATED_CORE_DIR}"
        --publish "$<TARGET_FILE_DIR:harken_editor>/schema"
    COMMENT "Publishing the Studio Schema build snapshot"
)

if(_harken_macos_agl_framework_missing)
    harken_remove_missing_agl_framework(harken_editor)
endif()

if(WIN32)
    target_link_libraries(harken_editor_lib PUBLIC user32 advapi32)
    target_link_libraries(harken_editor PRIVATE opengl32 ole32 shell32)
endif()

# Qt runtime deployment via windeployqt (Windows)
if(WIN32)
    find_program(WINDEPLOYQT_EXE windeployqt
        HINTS ${Qt6_DIR}/../../../bin
        REQUIRED)

    add_custom_command(TARGET harken_editor POST_BUILD
        COMMAND ${WINDEPLOYQT_EXE}
            $<$<CONFIG:Debug>:--debug>
            $<$<NOT:$<CONFIG:Debug>>:--release>
            --no-translations
            --no-compiler-runtime
            --no-system-d3d-compiler
            --no-system-dxc-compiler
            --no-opengl-sw
            --no-quick-import
            $<TARGET_FILE:harken_editor>
        # windeployqt only ships the "windows" platform plugin; deploy the
        # offscreen plugin too so headless runs (tests under CI/agents) can
        # request QT_QPA_PLATFORM=offscreen instead of dying on a missing
        # platform.
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:Qt6::QOffscreenIntegrationPlugin>"
            "$<TARGET_FILE_DIR:harken_editor>/platforms/"
        COMMENT "Deploying Qt runtime via windeployqt"
    )

    # Built-in project templates — drop *.json next to the executable.
    # On first launch ProjectTemplateLoader::ensureUserTemplatesSeeded() copies
    # them into %APPDATA%/HarkenSound/HarkenStudio/Templates/, which is the
    # runtime source of truth.
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Templates")
        file(GLOB HARKEN_BUILTIN_TEMPLATES "${CMAKE_CURRENT_SOURCE_DIR}/Templates/*.json")
        foreach(_tpl IN LISTS HARKEN_BUILTIN_TEMPLATES)
            add_custom_command(TARGET harken_editor POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_tpl}"
                    $<TARGET_FILE_DIR:harken_editor>
                COMMENT "Copying built-in project template ${_tpl}"
            )
        endforeach()
    endif()
endif()

if(APPLE)
    if(NOT TARGET Qt6::QOffscreenIntegrationPlugin)
        message(FATAL_ERROR "Qt offscreen platform plugin is required for Studio Schema verification")
    endif()
    find_program(MACDEPLOYQT_EXE macdeployqt
        HINTS ${Qt6_DIR}/../../../bin
        REQUIRED)

    if(TARGET harken_packc)
        add_dependencies(harken_editor harken_packc)
        add_custom_command(TARGET harken_editor POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:harken_packc>"
                "$<TARGET_FILE_DIR:harken_editor>/$<TARGET_FILE_NAME:harken_packc>"
            COMMENT "Bundling harken_packc beside Harken Studio"
        )
    endif()

    add_custom_command(TARGET harken_editor POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_BUNDLE_DIR:harken_editor>/Contents/PlugIns/platforms"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:Qt6::QOffscreenIntegrationPlugin>"
            "$<TARGET_BUNDLE_DIR:harken_editor>/Contents/PlugIns/platforms/"
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_SOURCE_DIR}/.scripts/release/deploy_macos_bundle.py"
            --build-dir "${CMAKE_BINARY_DIR}"
            --bundle "$<TARGET_BUNDLE_DIR:harken_editor>"
            --tool "${MACDEPLOYQT_EXE}"
            --configuration "$<CONFIG>"
        COMMENT "Deploying Qt runtime via macdeployqt"
    )

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Templates")
        file(GLOB HARKEN_BUILTIN_TEMPLATES "${CMAKE_CURRENT_SOURCE_DIR}/Templates/*.json")
        foreach(_tpl IN LISTS HARKEN_BUILTIN_TEMPLATES)
            add_custom_command(TARGET harken_editor POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_tpl}"
                    $<TARGET_FILE_DIR:harken_editor>
                COMMENT "Copying built-in project template ${_tpl}"
            )
        endforeach()
    endif()
endif()
