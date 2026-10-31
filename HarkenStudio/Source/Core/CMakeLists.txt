# harken_editor_foundation -- STATIC library containing all Core and Query subsystems.
# Singletons: AppSettings / AssetRegistry / AudioEngine / ProjectManager / ActionManager /
#             ThemeManager / FontManager.
# Consumed by: harken_editor_lib (via target_link_libraries PUBLIC).

add_library(harken_editor_foundation STATIC
    # --- Core root ---
    ActionClosureEditorAdapter.cpp
    CurrentWorldEditorAdapter.cpp
    ProjectInstanceEditorAdapter.cpp
    ReferenceSlotEditorAdapter.cpp
    RuntimeCapabilityEditorAdapter.cpp
    VariableConsumerEditing.cpp
    AppSettings.cpp
    AppSettings.h
    AuthoringPolicy.cpp
    AuthoringPolicy.h
    AttenuationStore.cpp
    AttenuationStore.h
    AudioEngine.cpp
    AudioEngine.h
    AudioEngine_Bus.cpp
    AudioEngine_Debug.cpp
    AudioEngine_LiveProperties.cpp
    AudioEngine_Preview.cpp
    AudioEngine_Spatial.cpp
    AuditionOverlay.cpp
    AuditionOverlay.h
    VariableRehearsal.cpp
    VariableRehearsal.h
    BusPlacementRules.cpp
    BusPlacementRules.h
    Component.h
    ContextInfo.cpp
    ContextInfo.h
    ContextInfoService.cpp
    ContextInfoService.h
    HelpCatalog.cpp
    HelpCatalog.h
    ReentrancyGuard.h
    NumericFormat.h
    FileSystemWatcher.cpp
    FileSystemWatcher.h
    FileWatcher.cpp
    FileWatcher.h
    IPCBinaryProtocol.h
    ProjectManager_RuntimeSync.cpp
    ProjectObjectIndex.cpp
    ProjectObjectIndex.h
    RuntimeSyncCoordinator.cpp
    RuntimeSyncCoordinator.h
    RuntimeCapabilityTypes.h
    RuntimeContainerLowering.cpp
    RuntimeContainerLowering.h
    ISettingsStorage.h
    LanguageManager.cpp
    LanguageManager.h
    PhysicalFolderOperations.cpp
    PhysicalFolderOperations.h
    ProjectSchemaRules.h
    SchemaCompatibilityProbe.cpp
    SchemaCompatibilityProbe.h
    NamedLayoutStore.cpp
    NamedLayoutStore.h
    BuiltinLayouts.cpp
    BuiltinLayouts.h
    # Eliminate-Pack S4 — .cache/project_index/ dual index (design §2.4).
    # Eliminate-Pack S4.3 — integrity scan (design §2.2 Pass 3 / §2.10).
    RuntimeContentBundleBuilder.cpp
    RuntimeContentBundleBuilder.h
    PlatformContext.h
    PlatformContextService.cpp
    PlatformContextService.h
    LanguageContextService.cpp
    LanguageContextService.h
    AuthoringPlatform.h
    ProjectManager.cpp
    ProjectManager.h
    PhysicalFolderReferenceEdits.cpp
    PhysicalFolderReferenceEdits.h
    PropertyEditSession.cpp
    PropertyEditSession.h
    ProjectTemplateLoader.cpp
    ProjectTemplateLoader.h
    ProjectItemSaveState.cpp
    ProjectItemSaveState.h
    ProjectSavePlan.h
    ProjectPlatformCommandSchema.h
    PanelInstanceRegistry.cpp
    PanelInstanceRegistry.h
    ComponentRecipeRegistry.cpp
    ComponentRecipeRegistry.h
    ParameterPresenterRegistry.cpp
    ParameterPresenterRegistry.h
    WorkspaceWindowTypes.cpp
    WorkspaceWindowTypes.h
    JsonSettingsStorage.cpp
    JsonSettingsStorage.h
    MemorySettingsStorage.cpp
    MemorySettingsStorage.h
    QSettingsStorage.cpp
    QSettingsStorage.h
    ShortcutManager.cpp
    ShortcutManager.h
    SceneObject.h
    ThemeManager.cpp
    ThemeManager.h
    QtControlSkin.cpp
    QtControlSkin.h
    HarkenStyle.cpp
    HarkenStyle.h
    WidgetAppearance.cpp
    WidgetAppearance.h
    ThemeSnapshot.h
    ColorPalette.cpp
    ColorPalette.h
    ThemeColorCatalog.cpp
    ThemeColorCatalog.h
    ThemeColorResolver.cpp
    ThemeColorResolver.h
    ThemePackageDto.h
    ThemePackageParser.cpp
    ThemePackageParser.h
    ThemePackageLoader.cpp
    ThemePackageLoader.h
    ThemeAssetResolver.cpp
    ThemeAssetResolver.h
    ThemeAssetCatalog.cpp
    ThemeAssetCatalog.h
    BuiltinTheme.cpp
    BuiltinTheme.h
    ItemPropertySchema.h
    VariableImpactSemantics.cpp
    VariableImpactSemantics.h
    BindingEditing.cpp
    BindingEditing.h
    TransitionFieldGating.cpp
    TransitionFieldGating.h
    TransitionRehearsal.cpp
    TransitionRehearsal.h
    Timeline/TimelineEditing.cpp
    Timeline/TimelineEditing.h
    ItemTypeIcons.cpp
    ItemTypeIcons.h
    FontManager.cpp
    FontManager.h
    BaseDialog.cpp
    BaseDialog.h
    FramelessChrome.cpp
    FramelessChrome.h
    AuditionManager.cpp
    AuditionManager.h
    UIScaleManager.cpp
    UIScaleManager.h
    UndoCommands.cpp
    UndoCommands.h
    HarkenMath.h
    ActionManager.h
    ActionManager.cpp
    DialogManager.cpp
    DialogManager.h
    EditorShellRegistry.cpp
    EditorShellRegistry.h
    EditorBootstrap.cpp
    EditorBootstrap.h
    StartupProgress.cpp
    StartupProgress.h
    # --- Core/Actions: all Action files moved to harken_editor_lib (depend on IPCServer/UI layer) ---
    # --- Core/AssetPipeline ---
    AssetPipeline/AssetGarbageCollector.cpp
    AssetPipeline/AssetGarbageCollector.h
    AssetPipeline/AssetImport.cpp
    AssetPipeline/SourceFileSystem.cpp
    AssetPipeline/SourceFileSystem.h
    AssetPipeline/AssetTypes.h
    AssetPipeline/AudioImportGate.cpp
    AssetPipeline/AudioImportGate.h
    AssetPipeline/MediaPool.cpp
    AssetPipeline/MediaPool.h
    AssetPipeline/WaveformAnalysis.h
    # --- Core/Components (header-only) ---
    Components/VAudio.h
    Components/VSpatial.h
    Components/VVariable.h
    Components/VAction.h
    Components/VTrack.h
    Components/VSegment.h
    # --- Core/ObjectLists ---
    ObjectLists/IObjectList.h
    ObjectLists/IObjectList.cpp
    # --- Utils (relative path from src/Core/) ---
    ../Utils/SvgIconEngine.cpp
    ../Utils/SvgIconEngine.h
    # --- Query (relative path from src/Core/) ---
    ../Query/IVQLDataSource.h
    ../Query/LMDBBenchmark.cpp
    ../Query/LMDBBenchmark.h
    ../Query/LMDBIndexer.cpp
    ../Query/LMDBIndexer.h
    ../Query/LMDBQueryPipeline.cpp
    ../Query/LMDBQueryPipeline.h
    ../Query/LMDBQueryTask.cpp
    ../Query/LMDBQueryTask.h
    ../Query/LMDBService.cpp
    ../Query/LMDBService.h
    ../Query/MemoryDataSource.cpp
    ../Query/MemoryDataSource.h
    ../Query/VQLEngine.cpp
    ../Query/VQLEngine.h
    ../Query/VQLEvaluator.cpp
    ../Query/VQLEvaluator.h
    ../Query/VQLFieldCatalog.cpp
    ../Query/VQLFieldCatalog.h
    ../Query/VQLLexer.cpp
    ../Query/VQLLexer.h
    ../Query/VQLObserver.cpp
    ../Query/VQLObserver.h
    ../Query/VQLParser.cpp
    ../Query/VQLParser.h
    ../Query/VQLTypes.h
)

add_dependencies(harken_editor_foundation harken_schema_codegen)

set_target_properties(harken_editor_foundation PROPERTIES
    AUTOMOC ON
    AUTOGEN_BUILD_DIR "${CMAKE_BINARY_DIR}/_autogen/harken_editor_foundation"
    AUTOUIC ON
)

# Keep this list aligned with harken_editor_lib's PCH (Editor/CMakeLists.txt).
# Foundation is the Editor-Core layer; widgets-heavy headers (QPainter etc.)
# rarely appear in pure Core .cpp's, but their cost when unused is the PCH
# warm-up only (no per-TU re-parse), so keeping both PCH lists symmetric
# avoids drift and simplifies mental model.
target_precompile_headers(harken_editor_foundation PRIVATE
    <QWidget>
    <QString>
    <QJsonObject>
    <QJsonArray>
    <QUndoCommand>
    <QTimer>
    <QDebug>
    <QList>
    <QMap>
    <QVector>
    <QVariant>
    <QAction>
    <QMenu>
    <QToolBar>
    <QHBoxLayout>
    <QVBoxLayout>
    <QPushButton>
    <QLabel>
    <QPainter>
    <memory>
    <string>
    <vector>
    <unordered_map>
    <functional>
)

# Public include dirs -- consumers (harken_editor_lib) can include Core files as "Core/Foo.h"
# and Core files can include each other as "Foo.h" (no prefix needed).
target_include_directories(harken_editor_foundation PUBLIC
    ${HARKEN_SCHEMA_GENERATED_DIR}
    ${HARKEN_SCHEMA_GENERATED_CORE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../Query
    ${CMAKE_CURRENT_SOURCE_DIR}/../Utils
    # Editor-vendored single-header libraries (dr_wav).
    ${CMAKE_CURRENT_SOURCE_DIR}/../../ThirdParty
)

target_include_directories(harken_editor_foundation PRIVATE
    ${CMAKE_SOURCE_DIR}/HarkenStudio/Source/Tooling/CurrentWorldCompiler/include
    ${CMAKE_SOURCE_DIR}/HarkenStudio/Source/Tooling/BusCompiler/include
    ${CMAKE_SOURCE_DIR}/HarkenStudio/Source/Tooling/ProjectSchema/include
    ${CMAKE_SOURCE_DIR}/HarkenStudio/Source/Tooling/Common
)

target_link_libraries(harken_editor_foundation PUBLIC
    harkenizer_build
    harken_integration_tooling
    Qt6::Widgets
    Qt6::Concurrent
    Qt6::Svg
    # The runtime bridge is the only sanctioned path to the engine: it links
    # the engine library PRIVATE, which still propagates the library itself
    # to the final link while keeping its headers invisible here.
    harken_runtime_bridge
    harken_bus_compiler
    harken_buildinfo
    harken_playable_properties
    vorbis
    vorbisenc
    ogg
    r8brain
)


if(APPLE)
    find_library(IOKIT_FRAMEWORK IOKit REQUIRED)
    find_library(COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
    target_link_libraries(harken_editor_foundation PUBLIC
        ${IOKIT_FRAMEWORK}
        ${COREFOUNDATION_FRAMEWORK}
    )
endif()

if(NOT HARKEN_DISABLE_LMDB)
    target_link_libraries(harken_editor_foundation PUBLIC lmdb)
    target_compile_definitions(harken_editor_foundation PUBLIC HARKEN_HAS_LMDB)
endif()

if(MSVC)
    target_compile_options(harken_editor_foundation PRIVATE /W4 /wd4100 /wd4189 /utf-8)
    target_compile_definitions(harken_editor_foundation PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
