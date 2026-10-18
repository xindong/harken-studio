find_package(Qt6 REQUIRED COMPONENTS Widgets Sql Network Svg LinguistTools)
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(HARKEN_HUB_LEGAL_DIR "${CMAKE_CURRENT_BINARY_DIR}/legal")
set(HARKEN_HUB_LEGAL_FILES
    "${HARKEN_HUB_LEGAL_DIR}/USER_AGREEMENT.txt"
    "${HARKEN_HUB_LEGAL_DIR}/USER_AGREEMENT.zh-CN.txt"
    "${HARKEN_HUB_LEGAL_DIR}/PRIVACY_POLICY.txt"
    "${HARKEN_HUB_LEGAL_DIR}/PRIVACY_POLICY.zh-CN.txt")
add_custom_command(
    OUTPUT ${HARKEN_HUB_LEGAL_FILES}
    COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/.scripts/release/product_legal.py"
        --source-root "${CMAKE_SOURCE_DIR}"
        --hub-resources "${HARKEN_HUB_LEGAL_DIR}"
    DEPENDS
        "${CMAKE_SOURCE_DIR}/.scripts/release/product_legal.py"
        "${CMAKE_SOURCE_DIR}/USER_TERMS.md"
        "${CMAKE_SOURCE_DIR}/USER_TERMS.zh-CN.md"
    VERBATIM)
add_custom_target(harken_hub_legal_documents DEPENDS ${HARKEN_HUB_LEGAL_FILES})

function(harken_add_hub_legal_resources target)
    set(legal_dir "${CMAKE_BINARY_DIR}/HarkenHub/legal")
    set(legal_files)
    foreach(name USER_AGREEMENT.txt USER_AGREEMENT.zh-CN.txt
                 PRIVACY_POLICY.txt PRIVACY_POLICY.zh-CN.txt)
        list(APPEND legal_files "${legal_dir}/${name}")
        set_source_files_properties("${legal_dir}/${name}" PROPERTIES
            GENERATED TRUE QT_RESOURCE_ALIAS "${name}")
    endforeach()
    qt_add_resources(${target} "harken_hub_legal"
        PREFIX "/Resources/legal" FILES ${legal_files})
    add_dependencies(${target} harken_hub_legal_documents)
endfunction()

include(FetchContent)
set(MZ_COMPAT OFF CACHE BOOL "" FORCE)
set(MZ_ZLIB ON CACHE BOOL "" FORCE)
set(MZ_BZIP2 OFF CACHE BOOL "" FORCE)
set(MZ_LZMA OFF CACHE BOOL "" FORCE)
set(MZ_PPMD OFF CACHE BOOL "" FORCE)
set(MZ_ZSTD OFF CACHE BOOL "" FORCE)
set(MZ_PKCRYPT OFF CACHE BOOL "" FORCE)
set(MZ_WZAES OFF CACHE BOOL "" FORCE)
set(MZ_OPENSSL OFF CACHE BOOL "" FORCE)
set(MZ_ICONV OFF CACHE BOOL "" FORCE)
set(MZ_FETCH_LIBS ON CACHE BOOL "" FORCE)
set(MZ_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MZ_BUILD_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(ZLIB_TAG 12731092979c6d07f42da27da673a9f6c7b13586)
FetchContent_Declare(
    minizip-ng
    GIT_REPOSITORY https://github.com/zlib-ng/minizip-ng.git
    GIT_TAG 7b2387161c542fa9f427352dcdef76097d0d692b
)
FetchContent_MakeAvailable(minizip-ng)

FetchContent_Declare(
    ed25519
    GIT_REPOSITORY https://github.com/orlp/ed25519.git
    GIT_TAG b1f19fab4aebe607805620d25a5e42566ce46a0e
)
FetchContent_MakeAvailable(ed25519)
add_library(harken_ed25519 STATIC
    ${ed25519_SOURCE_DIR}/src/fe.c
    ${ed25519_SOURCE_DIR}/src/ge.c
    ${ed25519_SOURCE_DIR}/src/keypair.c
    ${ed25519_SOURCE_DIR}/src/sc.c
    ${ed25519_SOURCE_DIR}/src/sha512.c
    ${ed25519_SOURCE_DIR}/src/sign.c
    ${ed25519_SOURCE_DIR}/src/verify.c
)
target_include_directories(harken_ed25519 PUBLIC ${ed25519_SOURCE_DIR}/src)
target_compile_definitions(harken_ed25519 PUBLIC ED25519_NO_SEED)

set(HARKEN_HUB_SOURCES
    Source/App/FileLog.cpp
    Source/App/SingleInstanceGuard.cpp
    Source/Download/BackoffPolicy.cpp
    Source/Download/Downloader.cpp
    Source/Packaging/PackageReader.cpp
    Source/Packaging/PackageVerifier.cpp
    Source/Security/BinaryTrustPolicy.cpp
    Source/Security/ManifestVerifier.cpp
    Source/Security/TrustAnchors.cpp
    Source/Localization/HubLocalization.cpp
    Source/Localization/ProjectOpenTranslations.cpp
    Source/ReleaseNotes/ReleaseNotesCatalog.cpp
    Source/ReleaseNotes/ReleaseNotesStrings.cpp
    Source/Settings/HubSettings.cpp
    Source/Templates/ProjectTemplateCatalog.cpp
    Source/UI/ButtonPulseAnimator.cpp
    Source/UI/NoHoverItemDelegate.cpp
    Source/Transactions/InstallExecutor.cpp
    Source/Transactions/InstallIndex.cpp
    Source/Transactions/InstallPackageCache.cpp
    Source/Transactions/InstallRequest.cpp
    Source/Transactions/RepairExecutor.cpp
    Source/Transactions/TransactionDiagnostics.cpp
    Source/Transactions/TransactionJournal.cpp
    Source/Transactions/TransactionReclaimer.cpp
    Source/Transactions/UninstallExecutor.cpp
    Source/Update/ServerEndpoints.cpp
    Source/Update/ServerVersionCatalog.cpp
    Source/Update/UpdateChecker.cpp
    Source/Update/UpdateStateStore.cpp
    Source/Update/UpdateCoordinator.cpp
    Source/Update/UpdateNotifier.cpp
    Source/Update/UpdateQueue.cpp
    Source/Update/BridgeDispatcher.cpp
    Source/Update/UpdateBridgeConsumer.cpp
    Source/Update/HubSelfUpdate.cpp
    Source/Update/SelfUpdateHealthCheck.cpp
    Source/Integrations/IntegrationStore.cpp
    Source/Integrations/IntegrationFileSystem.cpp
    Source/Integrations/EngineTargetStore.cpp
    Source/Integrations/EngineTargetProbe.cpp
    Source/Integrations/ProjectIntegrationExecutor.cpp
    Source/Integrations/HubUnityIntegrationService.cpp
    Source/Integrations/Unity/UnityInstallation.cpp
    Source/Integrations/Unity/UnityInstallation.h
    Source/Integrations/Unity/UnityIntegrationService.cpp
    Source/Integrations/Unity/UnityIntegrationService.h
    Source/Projects/ProjectStore.cpp
    Source/Projects/ProjectProbe.cpp
    Source/Projects/ProjectVersionSelection.cpp
    Source/Projects/ProjectIntakeConsumer.cpp
    Source/Versions/StudioCapabilities.cpp
    Source/Versions/VersionScanner.cpp
    Source/Versions/LocalBuildRegistry.cpp
    Source/Versions/LocalBuildRefreshCoordinator.cpp
    Source/Versions/StudioLaunchTarget.cpp
    Source/Versions/IntegrityChecker.cpp
    Source/Versions/LaunchDiagnostics.cpp
    Source/Versions/SelfTestRunner.cpp
    Source/Launch/LaunchPreflight.cpp
    Source/Launch/ProjectOpenAnalyzer.cpp
    Source/Launch/ProjectCreateFlow.cpp
    Source/Launch/ProjectOpenFlow.cpp
    Source/Launch/StudioLaunchMonitor.cpp
    Source/Launch/StudioLauncher.cpp
    Source/Legal/ConsentStore.cpp
    Source/Maintenance/BuildPinRegistry.cpp
    Source/Maintenance/CacheManager.cpp
    Source/Maintenance/LogExporter.cpp
    Source/Maintenance/LogTailer.cpp
    Source/Platform/AtomicFile.cpp
    Source/Platform/ElevationCommands.cpp
    Source/Transactions/HandoffArea.cpp
    Source/UI/BoundedHeaderView.cpp
    Source/UI/ConsentDialog.cpp
    Source/UI/EngineIntegrationsPage.cpp
    Source/UI/HubIcons.cpp
    Source/UI/ServerVersionsDialog.cpp
    Source/UI/HubWindow.cpp
    Source/UI/LogConsoleDialog.cpp
    Source/UI/ReleaseNotesDialog.cpp
    Source/UI/ProjectsPage.cpp
    Source/UI/TitleBar.cpp
    Source/UI/VersionsPage.cpp

)

if(WIN32)
    list(APPEND HARKEN_HUB_SOURCES
        Source/Platform/Windows/PlatformPaths.cpp
        Source/Platform/Windows/Elevation.cpp
        Source/Platform/Windows/ProcessScan.cpp
        Source/Platform/Windows/LoginLaunch.cpp
        Source/Platform/Windows/AclHardening.cpp
        Source/Platform/Windows/UpdateBridgeDeployment.cpp
        Source/Platform/Windows/UpdateBridgeTask.cpp
        Source/Platform/Windows/UserSession.cpp
        Source/Platform/Windows/UserSessionReady.cpp)
elseif(APPLE)
    list(APPEND HARKEN_HUB_SOURCES
        Source/Platform/macOS/PlatformPaths.cpp
        Source/Platform/macOS/Elevation.cpp
        Source/Platform/macOS/ProcessScan.cpp
        Source/Platform/macOS/LoginLaunch.cpp
        Source/Platform/macOS/AclHardening.cpp
        Source/Platform/macOS/UpdateBridgeTask.mm
        Source/Platform/macOS/UserSession.cpp)
else()
    message(FATAL_ERROR
        "HarkenHub supports Windows and macOS only; configure with HARKEN_BUILD_HUB=OFF on this platform")
endif()

function(harken_configure_hub_library target)
    target_include_directories(${target} PUBLIC Source Source/Integrations/Unity)
    target_link_libraries(${target} PUBLIC
        Qt6::Widgets
        Qt6::Sql
        Qt6::Network
        Qt6::Svg
        harken_buildinfo
    )
    target_link_libraries(${target} PRIVATE MINIZIP::minizip harken_ed25519)
    if(WIN32)
        target_link_libraries(${target} PRIVATE
            advapi32
            wbemuuid shell32
            crypt32
            ole32
            oleaut32
            taskschd
            userenv
            wintrust
            wtsapi32
            comsuppw)
    elseif(APPLE)
        target_link_libraries(${target} PRIVATE
            "-framework CoreFoundation"
            "-framework Foundation"
            "-framework ServiceManagement")
    endif()
    if(TARGET QWindowKit::Widgets)
        target_link_libraries(${target} PUBLIC QWindowKit::Widgets)
        target_compile_definitions(${target} PUBLIC HARKEN_HAS_QWINDOWKIT)
    endif()
endfunction()

add_library(harken_hub_lib STATIC ${HARKEN_HUB_SOURCES})
harken_configure_hub_library(harken_hub_lib)
if(HARKEN_BUILD_HUB_UNIT_TESTS)
    add_library(harken_hub_test_lib STATIC ${HARKEN_HUB_SOURCES})
    harken_configure_hub_library(harken_hub_test_lib)
    target_compile_definitions(harken_hub_test_lib PRIVATE
        HARKEN_HUB_TEST_OVERRIDES)
endif()

if(WIN32)
    add_executable(harken_hub WIN32 Source/App/Main.cpp "resources.qrc"
        "HarkenHub.rc")
else()
    set_source_files_properties("Resources/macOS/HarkenHub.icns"
        PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
    add_executable(harken_hub Source/App/Main.cpp "resources.qrc"
        "Resources/macOS/HarkenHub.icns")
endif()

target_link_libraries(harken_hub PRIVATE harken_hub_lib)
harken_add_hub_legal_resources(harken_hub)
if(WIN32)
    target_link_libraries(harken_hub PRIVATE user32)
endif()

qt_add_translations(harken_hub
    SOURCE_TARGETS harken_hub_lib harken_hub
    TS_FILES Translations/harken_hub_zh_CN.ts
    RESOURCE_PREFIX "/i18n"
    LUPDATE_TARGET harken_hub_lupdate
    LRELEASE_TARGET harken_hub_lrelease
    NO_GENERATE_PLURALS_TS_FILE)

get_filename_component(QT_TRANSLATIONS_DIR
    "${Qt6_DIR}/../../../translations" ABSOLUTE)
set(QTBASE_ZH_CN_QM "${QT_TRANSLATIONS_DIR}/qtbase_zh_CN.qm")
if(EXISTS "${QTBASE_ZH_CN_QM}")
    set_source_files_properties("${QTBASE_ZH_CN_QM}"
        PROPERTIES QT_RESOURCE_ALIAS "qtbase_zh_CN.qm")
    qt_add_resources(harken_hub "harken_hub_qt_translations"
        PREFIX "/i18n"
        FILES "${QTBASE_ZH_CN_QM}")
else()
    message(STATUS
        "Qt Simplified Chinese translation is unavailable; Qt-provided UI will remain in English")
endif()

set_target_properties(harken_hub PROPERTIES OUTPUT_NAME "HarkenHub")

harken_attach_version_resource(
    harken_hub
    "Harken Hub"
    "HarkenHub.exe"
    hub)

if(APPLE)
    set_target_properties(harken_hub PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "HarkenHub"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.harkensound.harkenhub"
        MACOSX_BUNDLE_BUNDLE_VERSION "${HARKEN_BI_HUB_VERSION_QUAD}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING
            "${HARKEN_BI_HUB_VERSION_MAJOR}.${HARKEN_BI_HUB_VERSION_MINOR}.${HARKEN_BI_HUB_VERSION_PATCH}"
        MACOSX_BUNDLE_COPYRIGHT "Copyright (C) ${HARKEN_BI_YEAR} XD."
        MACOSX_BUNDLE_INFO_PLIST
            "${CMAKE_CURRENT_SOURCE_DIR}/Resources/macOS/Info.plist.in"
    )

    add_custom_command(TARGET harken_hub POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_BUNDLE_DIR:harken_hub>/Contents/Library/LaunchDaemons"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/Resources/macOS/com.harkensound.harkenhub.updatebridge.plist"
            "$<TARGET_BUNDLE_DIR:harken_hub>/Contents/Library/LaunchDaemons/com.harkensound.harkenhub.updatebridge.plist"
        COMMENT "Embedding the update bridge daemon definition"
    )

    find_program(MACDEPLOYQT_EXE macdeployqt
        HINTS ${Qt6_DIR}/../../../bin
        REQUIRED)

    add_custom_command(TARGET harken_hub POST_BUILD
        COMMAND ${MACDEPLOYQT_EXE}
            $<TARGET_BUNDLE_DIR:harken_hub>
            -always-overwrite
        COMMENT "Deploying Qt runtime for Harken Hub"
    )
endif()

if(WIN32)
    add_executable(harken_hub_setup WIN32
        Source/Setup/SetupStubMain.cpp
        "HarkenHubSetup.rc")
    set_target_properties(harken_hub_setup PROPERTIES
        OUTPUT_NAME "HarkenHubSetup"
        AUTOMOC OFF
        AUTORCC OFF
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/setup")
    target_link_options(harken_hub_setup PRIVATE
        "/MANIFESTUAC:level='requireAdministrator'")
    harken_attach_version_resource(
        harken_hub_setup
        "Harken Hub Setup"
        "HarkenHubSetup.exe"
        hub)

    if(NOT TARGET harken_editor)
        file(GENERATE
            OUTPUT "$<TARGET_FILE_DIR:harken_hub>/qt.conf"
            CONTENT "[Paths]\nPlugins = .\n"
        )
    endif()

    find_program(WINDEPLOYQT_EXE windeployqt
        HINTS ${Qt6_DIR}/../../../bin
        REQUIRED)

    add_custom_command(TARGET harken_hub POST_BUILD
        COMMAND ${WINDEPLOYQT_EXE}
            $<$<CONFIG:Debug>:--debug>
            $<$<NOT:$<CONFIG:Debug>>:--release>
            --no-translations
            --no-compiler-runtime
            --no-system-d3d-compiler
            --no-system-dxc-compiler
            --no-opengl-sw
            --no-quick-import
            $<TARGET_FILE:harken_hub>
        COMMENT "Deploying Qt runtime for Harken Hub"
    )

    add_executable(harken_hub_update_bridge WIN32
        Source/App/UpdateBridgeMain.cpp)
    set_target_properties(harken_hub_update_bridge PROPERTIES
        OUTPUT_NAME "HarkenHubUpdateBridge"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/Updater")
    target_link_libraries(harken_hub_update_bridge PRIVATE harken_hub_lib)
    harken_attach_version_resource(
        harken_hub_update_bridge
        "Harken Hub Update Bridge"
        "HarkenHubUpdateBridge.exe"
        hub)

    add_custom_command(TARGET harken_hub_update_bridge POST_BUILD
        COMMAND ${WINDEPLOYQT_EXE}
            $<$<CONFIG:Debug>:--debug>
            $<$<NOT:$<CONFIG:Debug>>:--release>
            --no-translations
            --no-compiler-runtime
            --no-system-d3d-compiler
            --no-system-dxc-compiler
            --no-opengl-sw
            --no-quick-import
            $<TARGET_FILE:harken_hub_update_bridge>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:zlib-ng>"
            "$<TARGET_FILE_DIR:harken_hub_update_bridge>"
        COMMENT "Deploying the runtime for the update bridge"
    )
endif()
