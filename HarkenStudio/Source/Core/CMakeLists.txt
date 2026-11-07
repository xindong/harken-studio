add_library(harken_studio_core STATIC
    ../Widgets/Inspector/PropertySchemaJson.cpp
    ../Widgets/Inspector/FieldGroupInspectorView.cpp
    ../Widgets/Inspector/SoundInspectorSchema.cpp
    ../Widgets/Inspector/ContainerInspectorSchema.cpp
    ../Widgets/Inspector/TrackInspectorSchema.cpp
    ../Widgets/Inspector/SegmentInspectorSchema.cpp
    ../Widgets/Inspector/LayerInspectorSchema.cpp
    ../Widgets/Inspector/BusInspectorSchema.cpp
    ../Widgets/Inspector/ActionInspectorSchema.cpp
    ../Widgets/Inspector/VariableInspectorSchema.cpp
    ../Widgets/Inspector/AttenuationInspectorSchema.cpp
    ../Widgets/Inspector/EffectInspectorSchema.cpp
    ActionClosureEditorAdapter.cpp
    CurrentWorldEditorAdapter.cpp
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
    PhysicalFolderOperations.cpp
    PhysicalFolderOperations.h
    ProjectSchemaRules.h
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
    JsonSettingsStorage.cpp
    JsonSettingsStorage.h
    MemorySettingsStorage.cpp
    MemorySettingsStorage.h
    QSettingsStorage.cpp
    QSettingsStorage.h
    SceneObject.h
    ItemPropertySchema.h
    VariableImpactSemantics.cpp
    VariableImpactSemantics.h
    VariableRangeEditing.cpp
    VariableRangeEditing.h
    BindingEditing.cpp
    BindingEditing.h
    TransitionFieldGating.cpp
    TransitionFieldGating.h
    TransitionRehearsal.cpp
    TransitionRehearsal.h
    Timeline/TimelineEditing.cpp
    Timeline/TimelineEditing.h
    AuditionManager.cpp
    AuditionManager.h
    UndoCommands.cpp
    UndoCommands.h
    HarkenMath.h
    ActionManager.h
    ActionManager.cpp
    StartupProgress.cpp
    StartupProgress.h
    AssetPipeline/AssetGarbageCollector.cpp
    AssetPipeline/AssetGarbageCollector.h
    AssetPipeline/AssetImport.cpp
    AssetPipeline/SourceFileSystem.cpp
    AssetPipeline/SourceFileSystem.h
    AssetPipeline/SourceWaveformView.cpp
    AssetPipeline/SourceWaveformView.h
    AssetPipeline/AssetTypes.h
    AssetPipeline/AudioImportGate.cpp
    AssetPipeline/AudioImportGate.h
    AssetPipeline/MediaPool.cpp
    AssetPipeline/MediaPool.h
    AssetPipeline/WaveformAnalysis.h
    Components/VAudio.h
    Components/VSpatial.h
    Components/VVariable.h
    Components/VAction.h
    Components/VTrack.h
    Components/VSegment.h
    ObjectLists/IObjectList.h
    ObjectLists/IObjectList.cpp
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
    LogService.cpp
    LogService.h
    UndoHistory.cpp
    UndoHistory.h
    ${HARKEN_STUDIO_COMMAND_SOURCES}
)

add_dependencies(harken_studio_core harken_schema_codegen)
set_target_properties(harken_studio_core PROPERTIES AUTOMOC ON)
target_precompile_headers(harken_studio_core PRIVATE
    <QString> <QJsonObject> <QJsonArray> <QTimer> <QDebug>
    <memory> <string> <vector> <unordered_map> <functional>)
target_include_directories(harken_studio_core PUBLIC
    ${HARKEN_SCHEMA_GENERATED_DIR}
    ${HARKEN_SCHEMA_GENERATED_CORE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../Query
    ${CMAKE_CURRENT_SOURCE_DIR}/../Utils
    ${CMAKE_CURRENT_SOURCE_DIR}/../../ThirdParty)
target_include_directories(harken_studio_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../Widgets
    ${CMAKE_SOURCE_DIR}/HarkenStudio/Source/Tooling/CurrentWorldCompiler/include
    ${CMAKE_SOURCE_DIR}/HarkenStudio/Source/Tooling/BusCompiler/include
    ${CMAKE_SOURCE_DIR}/HarkenStudio/Source/Tooling/ProjectSchema/include
    ${CMAKE_SOURCE_DIR}/HarkenStudio/Source/Tooling/Common)
target_link_libraries(harken_studio_core PUBLIC
    harkenizer_build Qt6::Core Qt6::Network Qt6::Concurrent
    harken_runtime_bridge harken_editor_monitor_data harken_bus_compiler
    harken_buildinfo harken_playable_properties vorbis vorbisenc ogg r8brain)
if(APPLE)
    find_library(IOKIT_FRAMEWORK IOKit REQUIRED)
    find_library(COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
    target_link_libraries(harken_studio_core PUBLIC ${IOKIT_FRAMEWORK} ${COREFOUNDATION_FRAMEWORK})
endif()
if(NOT HARKEN_DISABLE_LMDB)
    target_link_libraries(harken_studio_core PUBLIC lmdb)
    target_compile_definitions(harken_studio_core PUBLIC HARKEN_HAS_LMDB)
endif()
if(MSVC)
    target_compile_options(harken_studio_core PRIVATE /W4 /wd4100 /wd4189 /utf-8)
    target_compile_definitions(harken_studio_core PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

if(NOT HARKEN_BUILD_EDITOR)
    return()
endif()

add_library(harken_editor_foundation STATIC
    ProjectInstanceEditorAdapter.cpp
    ContextInfo.cpp
    ContextInfo.h
    ContextInfoService.cpp
    ContextInfoService.h
    HelpCatalog.cpp
    HelpCatalog.h
    LanguageManager.cpp
    LanguageManager.h
    SchemaCompatibilityProbe.cpp
    SchemaCompatibilityProbe.h
    NamedLayoutStore.cpp
    NamedLayoutStore.h
    BuiltinLayouts.cpp
    BuiltinLayouts.h
    PanelInstanceRegistry.cpp
    PanelInstanceRegistry.h
    ComponentRecipeRegistry.cpp
    ComponentRecipeRegistry.h
    ParameterPresenterRegistry.cpp
    ParameterPresenterRegistry.h
    WorkspaceWindowTypes.cpp
    WorkspaceWindowTypes.h
    ShortcutManager.cpp
    ShortcutManager.h
    ThemeManager.cpp
    ThemeManager.h
    QtControlSkin.cpp
    QtControlSkin.h
    ToolTip.cpp
    ToolTip.h
    HarkenStyle.cpp
    HarkenStyle.h
    WidgetAppearance.cpp
    WidgetAppearance.h
    ThemeSnapshot.h
    ThemeScale.cpp
    ThemeScale.h
    ThemeLayout.h
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
    ItemTypeIcons.cpp
    ItemTypeIcons.h
    FontManager.cpp
    FontManager.h
    BaseDialog.cpp
    BaseDialog.h
    FramelessChrome.cpp
    FramelessChrome.h
    DialogManager.cpp
    DialogManager.h
    EditorShellRegistry.cpp
    EditorShellRegistry.h
    EditorBootstrap.cpp
    EditorBootstrap.h
    ../Utils/SvgIconEngine.cpp
    ../Utils/SvgIconEngine.h
    UndoActions.cpp
    UndoActions.h
)
set_target_properties(harken_editor_foundation PROPERTIES AUTOMOC ON AUTOUIC ON)
target_link_libraries(harken_editor_foundation PUBLIC harken_studio_core Qt6::Widgets Qt6::Svg)
if(MSVC)
    target_compile_options(harken_editor_foundation PRIVATE /W4 /wd4100 /wd4189 /utf-8)
    target_compile_definitions(harken_editor_foundation PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
