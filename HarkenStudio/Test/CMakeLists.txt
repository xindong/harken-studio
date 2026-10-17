add_executable(harken_editor_tests
    ${CMAKE_SOURCE_DIR}/HarkenStudio/resources.qrc
    UnitTest/TestMain.cpp
    UnitTest/ThemePackageParserTest.cpp
    UnitTest/ThemePackageLoaderTest.cpp
    UnitTest/ThemeManagerTest.cpp
    UnitTest/ThemeIpcTest.cpp
    UnitTest/ThemeAssetResolverTest.cpp
    UnitTest/ThemeEndToEndTest.cpp
    UnitTest/QtControlSkinTest.cpp
    UnitTest/ProportionalTreeWidgetTest.cpp
    UnitTest/TableWidgetTest.cpp
    UnitTest/SegmentedControlTest.cpp
    UnitTest/ThemeWidgetMigrationTest.cpp
    UnitTest/ThemeMetricsMigrationTest.cpp
    UnitTest/ShortcutManagerTest.cpp
    UnitTest/StartupLoadingWindowTest.cpp
    UnitTest/TitleBarWidgetTest.cpp
    UnitTest/ToastManagerTest.cpp
    UnitTest/QtAdsThemeAdapterTest.cpp
    UnitTest/QtAdsTabLifecycleTest.cpp
    UnitTest/WorkspaceDockPinTest.cpp
    UnitTest/LayoutZoneToggleTest.cpp
    UnitTest/BuiltinLayoutsTest.cpp
    UnitTest/WaveformRendererTest.cpp
    UnitTest/TimeSeriesChartModelTest.cpp
    UnitTest/TimeSeriesChartTest.cpp
    UnitTest/MonitorCursorModelTest.cpp
    UnitTest/MonitorCursorMarkTest.cpp
    UnitTest/MonitorAdapterTest.cpp
    UnitTest/MonitorCaptureControlTest.cpp
    UnitTest/MonitorRemoteStatusTest.cpp
    UnitTest/MonitorRemoteReconnectTest.cpp
    UnitTest/MonitorRecordingTest.cpp
    UnitTest/MonitorSessionCompareUiTest.cpp
    UnitTest/CapturePlanIntentTest.cpp
    UnitTest/MonitorCapturePlanTest.cpp
    UnitTest/MonitorDecoderTest.cpp
    UnitTest/MonitorStoreTest.cpp
    UnitTest/MonitorStateCutTest.cpp
    UnitTest/MonitorGenerationTest.cpp
    UnitTest/MonitorDropTest.cpp

    # P2-5 availability
    UnitTest/MonitorAvailabilityTest.cpp
    # P2-6 session-file
    UnitTest/MonitorSessionFileTest.cpp
    UnitTest/Monitor/support/MonitorFixtureLoader.cpp
    UnitTest/CurveRendererTest.cpp
    UnitTest/NumericFormatTest.cpp
    UnitTest/TimelineRendererTest.cpp
    UnitTest/TimelineTicksTest.cpp
    UnitTest/CurveEditorWidgetTest.cpp
    UnitTest/PeakMeterRendererTest.cpp
    UnitTest/VUMeterRendererTest.cpp
    UnitTest/SpectrumMeterRendererTest.cpp
    UnitTest/SpectrogramMeterRendererTest.cpp
    UnitTest/PollReentryGuardTest.cpp
    UnitTest/StableIdTest.cpp
    UnitTest/PackGroupSerializationTest.cpp
    UnitTest/ProjectTemplateLifecycleTest.cpp
    UnitTest/AttenuationDefSerializationTest.cpp
    UnitTest/AttenuationCurveEvalTest.cpp
    UnitTest/AttenuationCurveHandleGeometryTest.cpp
    UnitTest/AttenuationCurvePasteLimitTest.cpp
    UnitTest/AttenuationCurvePointCoordinateTest.cpp
    UnitTest/AttenuationCurveUndoTest.cpp
    UnitTest/CurveCanvasDomainTest.cpp
    UnitTest/AttenuationBindingUndoTest.cpp
    UnitTest/AttenuationRuntimeSyncTest.cpp
    UnitTest/AudioEnginePreviewTest.cpp
    UnitTest/AdvancedActionResultTest.cpp
    UnitTest/PackGroupCommandTest.cpp
    UnitTest/PreparedProjectAssetReconcileTest.cpp
    UnitTest/ProjectSerializationTest.cpp
    UnitTest/PlayablePropertyAuthoringTest.cpp
    UnitTest/FieldGroupProjectAdapterTest.cpp
    UnitTest/FieldGroupResolutionTest.cpp
    UnitTest/FieldGroupJsonTest.cpp
    UnitTest/FieldGroupEditingTest.cpp
    UnitTest/FieldGroupLifecycleTest.cpp
    UnitTest/FieldGroupStorageTest.cpp
    UnitTest/FieldGroupLegacyEntryTest.cpp
    UnitTest/FieldGroupActionsTest.cpp
    UnitTest/FieldGroupInspectorViewTest.cpp
    UnitTest/FieldGroupInspectorTest.cpp
    UnitTest/PlatformOverrideAuthoringTest.cpp
    UnitTest/PlatformOverrideActionTest.cpp
    UnitTest/PlatformOverrideCompileTest.cpp
    UnitTest/PlatformOverrideSerializationTest.cpp
    UnitTest/PlatformContextServiceTest.cpp
    UnitTest/ProjectPlatformModelTest.cpp
    UnitTest/ProjectPlatformCommandTest.cpp
    UnitTest/ProjectPlatformAuthoringTest.cpp
    UnitTest/ProjectPlatformBuildTest.cpp
    UnitTest/ProjectPlatformUiIpcTest.cpp
    UnitTest/CurrentWorldPrimaryScopeTest.cpp
    UnitTest/PlatformBarTest.cpp
    UnitTest/LanguageContextServiceTest.cpp
    UnitTest/PlatformFieldMergeTest.cpp
    UnitTest/PlatformOverrideInspectorTest.cpp
    UnitTest/PlayablePropertyOccurrenceLifecycleTest.cpp
    UnitTest/TargetPlatformsCommandTest.cpp
    UnitTest/ProjectLanguageSemanticsTest.cpp
    UnitTest/ProjectLanguagesCommandTest.cpp
    UnitTest/HubClientArgumentsTest.cpp
    UnitTest/HubLauncherTest.cpp
    UnitTest/ProjectInstanceTest.cpp
    UnitTest/MigrationVersionGateTest.cpp
    UnitTest/ProjectUpgradeManagerTest.cpp
    UnitTest/ProjectOpenProtocolTest.cpp
    UnitTest/ProjectOpenApprovalTest.cpp
    UnitTest/ProjectUpgradeMessagesTest.cpp
    UnitTest/ProjectSafetyFoundationTest.cpp
    UnitTest/SchemaArchiveTest.cpp
    UnitTest/SchemaCompatibilityProbeTest.cpp
    UnitTest/ProjectDocumentRoundTripTest.cpp
    UnitTest/ProjectMigrationTest.cpp
    UnitTest/ProjectPreflightTest.cpp
    UnitTest/PackBuildSessionTest.cpp
    UnitTest/PackBuildSessionLogicTest.cpp
    UnitTest/EffectChainSemanticsTest.cpp
    UnitTest/RuntimeContainerLoweringTest.cpp
    UnitTest/AssetConverterCacheTest.cpp
    UnitTest/AudioFormatDecodeTest.cpp
    UnitTest/MediaPoolRefreshTest.cpp
    UnitTest/PlatformProfileResolutionTest.cpp
    UnitTest/ProjectPackExporterCacheTest.cpp
    UnitTest/ProjectPackExporterInitPackTest.cpp
    UnitTest/ProjectPackExporterSharedPackTest.cpp
    UnitTest/ProjectPackExporterManifestTest.cpp
    UnitTest/ProjectPackExporterLayoutTest.cpp
    UnitTest/PackExportOrchestrationTest.cpp
    UnitTest/DualPlatformExportDigestTest.cpp
    UnitTest/ExportedPackLifecycleTest.cpp
    UnitTest/CanonicalBusSendSnapshotParserTest.cpp
    UnitTest/CapabilityMapTest.cpp
    UnitTest/RemoteConnectionTest.cpp
    UnitTest/DebugHonestyMapperTest.cpp
    UnitTest/MixerLevelsHonestyTest.cpp
    UnitTest/EngineNotificationHonestyTest.cpp
    UnitTest/LogCategoryHonestyTest.cpp
    UnitTest/WorkspaceWindowContractTest.cpp
    UnitTest/MonitorPanelModuleTest.cpp
    UnitTest/MonitorPerformancePanelTest.cpp
    UnitTest/MonitorPerformanceLiveTest.cpp
    UnitTest/MonitorCatalogTest.cpp
    UnitTest/MonitorVoicesTest.cpp
    UnitTest/MonitorPlayablesTest.cpp
    UnitTest/MonitorEffectsTest.cpp
    UnitTest/MonitorBusesTest.cpp
    UnitTest/MonitorDspTimelineTest.cpp
    UnitTest/MonitorLineageTest.cpp
    UnitTest/MonitorTapSelectTest.cpp
    UnitTest/MonitorWaveformTest.cpp
    UnitTest/MonitorScopeTest.cpp
    UnitTest/MonitorSpectrumTest.cpp
    UnitTest/MeterPanelTest.cpp
    UnitTest/MonitorSharedCursorTest.cpp
    UnitTest/PanelRegistryTeardownTest.cpp
    UnitTest/AppSettingsPanelFontTest.cpp
    UnitTest/AppSettingsLanguageTest.cpp
    UnitTest/PropertyEditSessionTest.cpp
    UnitTest/ParameterBindingTest.cpp
    UnitTest/ParameterControlHostTest.cpp
    UnitTest/ComponentRecipeHostTest.cpp
    UnitTest/ParameterSliderTest.cpp
    UnitTest/StepsParameterHostTest.cpp
    UnitTest/PropertyEditorEngineTest.cpp
    UnitTest/InspectorPageDispatchTest.cpp
    UnitTest/InspectorFocusTest.cpp
    UnitTest/InspectorRowSetGoldTest.cpp
    UnitTest/InspectorSectionWidgetTest.cpp
    UnitTest/ActionPageSectionTest.cpp
    UnitTest/NotesColorCommandTest.cpp
    UnitTest/VariableModeCommandTest.cpp
    UnitTest/VariableInspectorFormTest.cpp
    UnitTest/ResolveComponentTypedTest.cpp
    UnitTest/ListPropertyTest.cpp
    UnitTest/ObjectRefDrawerTest.cpp
    UnitTest/ObjectRefDropTest.cpp
    UnitTest/RefIdMimeTest.cpp
    UnitTest/RefDomainAdapterTest.cpp
    UnitTest/AudioAssetRefTest.cpp
    UnitTest/ObjectReferencePickerTest.cpp
    UnitTest/ProjectTreeConsistencyTest.cpp
    UnitTest/ProjectExplorerSourcesTabTest.cpp
    UnitTest/NativeDropTargetTest.cpp
    UnitTest/ProjectTreeDragDropTest.cpp
    UnitTest/FolderAddressingTest.cpp
    UnitTest/BucketFolderStoreTest.cpp
    UnitTest/ProjectItemMenuControllerAnchorTest.cpp
    UnitTest/ProjectTreePanelAnchorTest.cpp
    UnitTest/MixNodeSendsTest.cpp
    UnitTest/BusTopologySyncTest.cpp
    UnitTest/BusOutputVolumePreviewTest.cpp
    UnitTest/CanonicalRuntimeProjectAdapterTest.cpp
    UnitTest/RuntimeSyncPresenceTest.cpp
    UnitTest/RuntimeSyncCoordinatorTest.cpp
    UnitTest/CurrentWorldSyncBridgeTest.cpp
    UnitTest/CurrentWorldProjectAdapterTest.cpp
    UnitTest/BlueprintOutGraphTest.cpp
    UnitTest/OutputSendExclusionTest.cpp
    UnitTest/SignalChainNodeDedupTest.cpp
    UnitTest/SignalPathGraphTest.cpp
    UnitTest/SignalPathLayoutTest.cpp
    UnitTest/SignalPathRenderingTest.cpp
    UnitTest/SignalPathFocusTest.cpp
    UnitTest/BlueprintCanvasViewTest.cpp
    UnitTest/BlueprintNodeGraphVisualTest.cpp
    UnitTest/BlueprintNodeStatusTest.cpp
    UnitTest/BlueprintDropInteractionTest.cpp
    UnitTest/BlueprintAudioImportTest.cpp
    UnitTest/SignalPathObservationTest.cpp
    UnitTest/SignalPathDeletionTest.cpp
    UnitTest/SignalPathConnectionTest.cpp
    UnitTest/SignalPathGainEditingTest.cpp
    UnitTest/VolumeAggregationTest.cpp
    UnitTest/SpatialRuntimeValueMappingTest.cpp
    UnitTest/ActionReferenceLifecycleTest.cpp
    UnitTest/ActionIdentityTest.cpp
    UnitTest/ActionCatalogExportTest.cpp
    UnitTest/ActionStepEditingTest.cpp
    UnitTest/ActionStepPropertyRouteTest.cpp
    UnitTest/ActionStepTargetResolveTest.cpp
    UnitTest/ActionClosureSemanticsTest.cpp
    UnitTest/PackBuildPlanTest.cpp
    UnitTest/PackManagerLanguageTest.cpp
    UnitTest/PackGenerationRecordTest.cpp
    UnitTest/RoutingInspectorSchemaTest.cpp
    UnitTest/DispatcherReentryTest.cpp
    UnitTest/IpcContainerCommandFamilyTest.cpp
    UnitTest/IpcMixerCommandFamilyTest.cpp
    UnitTest/IpcSchemaCoverageTest.cpp
    UnitTest/ImportAssetsCommandTest.cpp
    UnitTest/ImportSoundSourcesCommandTest.cpp
    UnitTest/ObjectIdContractTest.cpp
    UnitTest/ObjectIdRepairTest.cpp
    UnitTest/AuditionOverlayTest.cpp
    UnitTest/ComponentCatalogTest.cpp
    UnitTest/ComponentAxisTest.cpp
    UnitTest/ContextTopicResolutionTest.cpp
    UnitTest/HelpCatalogKeyTest.cpp
    UnitTest/InformationPanelWiringTest.cpp
    UnitTest/TimelineCoordinatesTest.cpp
    UnitTest/TimelineLedgerTest.cpp
    UnitTest/TimelineSemanticsTest.cpp
    UnitTest/TimelineSerializationTest.cpp
    UnitTest/TimelineEditingTest.cpp
    UnitTest/TimelinePropertyRouteTest.cpp
    UnitTest/AbsoluteAuthoringAcceptanceTest.cpp
    UnitTest/TimelineStructuralIntegrityTest.cpp
    UnitTest/VariableSemanticsTest.cpp
    UnitTest/VariableRehearsalInspectorTest.cpp
    UnitTest/GridSemanticsTest.cpp
    UnitTest/TransitionSemanticsTest.cpp
    UnitTest/TransitionFieldGatingTest.cpp
    UnitTest/TransitionPropertyRouteTest.cpp
    UnitTest/TransitionRehearsalTest.cpp
    UnitTest/ContainerBehaviorSemanticsTest.cpp
    UnitTest/LoopCountSemanticsTest.cpp
    UnitTest/LoopSectionProviderTest.cpp
    UnitTest/SourcesSectionProviderTest.cpp
    UnitTest/SoundSourceSemanticsTest.cpp
    UnitTest/SoundDistinguishLanguageTest.cpp
    UnitTest/SoundSourceEditingTest.cpp
    UnitTest/SoundSourceDerivativeTest.cpp
    UnitTest/BindingSemanticsTest.cpp
    UnitTest/BindingContractTest.cpp
    UnitTest/BindingEditingTest.cpp
    UnitTest/BindingInspectorTest.cpp
    UnitTest/BindingFieldEditorTest.cpp
    UnitTest/BindingCurveEditorHostTest.cpp
    UnitTest/BindingRuntimeLoweringTest.cpp
    UnitTest/VariableConsumerSemanticsTest.cpp
    UnitTest/VariableImpactSemanticsTest.cpp
    UnitTest/PreviewPanelTest.cpp
    UnitTest/InspectorSlotStripTest.cpp
    UnitTest/GridPropertyRouteTest.cpp
    UnitTest/ContainerGridCopyTest.cpp
    UnitTest/ContainerSelectionZeroOccurrenceTest.cpp
    UnitTest/DirectChildReferenceCleanupTest.cpp
    UnitTest/PlaybackReferenceValidationTest.cpp
    UnitTest/GridEditorPanelTest.cpp
    UnitTest/SourceEditorPanelTest.cpp
    UnitTest/SourceEditorPcmSliceTest.cpp
    UnitTest/SourceEditorWorkspaceTest.cpp
    UnitTest/StandaloneEditorDispatchTest.cpp
    UnitTest/TimelineEditorPanelTest.cpp
    UnitTest/TransitionEditorPanelTest.cpp
    UnitTest/TransitionEditorGatingTest.cpp
    UnitTest/MixingEditorWidgetTest.cpp
    UnitTest/GridAndBindingAuthoringScenarioTest.cpp
    UnitTest/TimelinePlacementAuthoringScenarioTest.cpp
    UnitTest/TrackPieceIpcScenarioTest.cpp
    UnitTest/ActionTransitionAuthoringScenarioTest.cpp
    UnitTest/ActionTransitionIpcScenarioTest.cpp
    UnitTest/TimelineBehaviorAuthoringScenarioTest.cpp
    UnitTest/PlayablePropertyIpcScenarioTest.cpp
    UnitTest/PackBuildScenarioTest.cpp
    UnitTest/RuntimeCapabilityScenarioTest.cpp
    UnitTest/PackBuildIpcScenarioTest.cpp
    UnitTest/EditorWorkflowScenarioTest.cpp
    UnitTest/ItemTypeCatalogTest.cpp
    UnitTest/VQLContractTest.cpp
    UnitTest/LMDBBenchmarkTest.cpp
    UnitTest/ItemTypeIconAssetTest.cpp
    UnitTest/CreationMenuCoverageTest.cpp
    UnitTest/NewObjectNamingTest.cpp
    UnitTest/ReferenceSlotResolutionTest.cpp
    UnitTest/PlacementRulesTest.cpp
    UnitTest/AICommandCatalogTest.cpp
    UnitTest/AIPermissionModelTest.cpp
    UnitTest/AICommandValidatorTest.cpp
    UnitTest/AIDispatchPolicyTest.cpp
    UnitTest/AIResultNormalizerTest.cpp
    UnitTest/AIAgentConfigTest.cpp
    UnitTest/AIDurableMemoryStoreTest.cpp
    UnitTest/AIDurableMemoryServiceTest.cpp
    UnitTest/AIDurableMemoryRetrievalTest.cpp
    UnitTest/AIConversationStoreTest.cpp
    UnitTest/AIConversationSessionStoreTest.cpp
    UnitTest/AIMarkdownRendererTest.cpp
    UnitTest/AISettingsTest.cpp
    UnitTest/ThemeSettingsDialogTest.cpp
    UnitTest/ProjectSettingsDialogTest.cpp
    UnitTest/SettingsApplyContractTest.cpp
    UnitTest/MainWindowSettingsMenuTest.cpp
    UnitTest/Support/QtFatalToStderr.cpp
    UnitTest/Support/MockLLMProvider.h
    UnitTest/Support/MockLLMProvider.cpp
    UnitTest/Support/SyntheticHttpServer.h
    UnitTest/Support/SyntheticHttpServer.cpp
    UnitTest/SyntheticHttpServerTest.cpp
    UnitTest/MockLLMProviderTest.cpp
    UnitTest/OpenAICompatibleLLMProviderTest.cpp
    UnitTest/AIAssistantServiceTest.cpp
    UnitTest/AIConversationPersistenceTest.cpp
    UnitTest/AIAssistantPanelWiringTest.cpp
    UnitTest/AIUnrestrictedModeTest.cpp
)
set_target_properties(harken_editor_tests PROPERTIES
    AUTOMOC ON
    AUTORCC ON
)

if(MSVC)
    target_compile_options(harken_editor_tests PRIVATE /FS)
endif()

if(WIN32)
    set_target_properties(harken_editor_tests PROPERTIES
        VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:harken_editor_tests>"
        VS_DEBUGGER_ENVIRONMENT "QT_QPA_PLATFORM=windows"
    )
endif()
get_target_property(_studio_test_runtime_sources harken_core SOURCES)
set_target_properties(harken_editor_tests PROPERTIES HARKEN_INTERNAL_RUNTIME_TEST ON)
get_target_property(_studio_test_runtime_includes harken_core INCLUDE_DIRECTORIES)
get_target_property(_studio_test_runtime_links harken_core LINK_LIBRARIES)
get_target_property(_studio_preview_source_dir harken_studio_runtime SOURCE_DIR)
add_library(harken_studio_runtime_test SHARED ${_studio_test_runtime_sources}
    ${_studio_preview_source_dir}/src/PreviewAbi.cpp
    ${_studio_preview_source_dir}/src/PreviewAbiLayout.cpp
    ${_studio_preview_source_dir}/src/PreviewProject.cpp)
target_include_directories(harken_studio_runtime_test PRIVATE
    ${_studio_test_runtime_includes} ${_studio_preview_source_dir}/include)
target_link_libraries(harken_studio_runtime_test PRIVATE ${_studio_test_runtime_links})
target_compile_definitions(harken_studio_runtime_test PRIVATE
    HARKEN_DEVELOPMENT_PREVIEW=$<BOOL:${HARKEN_DEVELOPMENT_PREVIEW}>)
if(HARKEN_ENABLE_STAGE_TAPS)
    target_compile_definitions(harken_studio_runtime_test PRIVATE HARKEN_ENABLE_STAGE_TAPS)
endif()
set_target_properties(harken_studio_runtime_test PROPERTIES
    WINDOWS_EXPORT_ALL_SYMBOLS ON OUTPUT_NAME HarkenStudioRuntimeTest)

target_link_libraries(harken_editor_tests PRIVATE
    harken_editor_lib
    harken_current_world_compiler
    harken_voice_slot_layout_contract
    harken_resource_facts_contract
    GTest::gtest
)
target_include_directories(harken_editor_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/HarkenStudio/Source
    ${CMAKE_SOURCE_DIR}/HarkenStudio/Source/Core
    ${CMAKE_SOURCE_DIR}/HarkenStudio/Source/Tooling/ProjectSchema/include
    ${HARKEN_RUNTIME_DIR}/api/src
    ${HARKEN_RUNTIME_DIR}/api/include
    ${HARKEN_RUNTIME_DIR}/media/include
    ${HARKEN_RUNTIME_DIR}/core/include
    ${HARKEN_RUNTIME_DIR}/dsp/include
    ${HARKEN_RUNTIME_DIR}/param/include
    ${HARKEN_RUNTIME_DIR}/render/include
    ${HARKEN_RUNTIME_DIR}/remote/include
)
target_compile_definitions(harken_editor_tests PRIVATE
    HARKEN_TEMP_DIR="${PROJECT_SOURCE_DIR}/.temp"
    HARKEN_PACKC_PATH="$<TARGET_FILE:harken_packc>"
    HARKEN_HOST_PATH="$<TARGET_FILE:harken_host>"
    HARKEN_STUDIO_SOURCE_DIR="${CMAKE_SOURCE_DIR}/HarkenStudio"
    HARKEN_MONITOR_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/HarkenEngine/Test/UnitTest/monitor"
    HARKEN_MONITOR_EDITOR_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/UnitTest/Monitor/fixtures")
add_dependencies(harken_editor_tests harken_packc harken_host)
find_package(Qt6 REQUIRED COMPONENTS Widgets Network)
add_executable(harken_project_instance_process
    UnitTest/ProjectInstanceProcess.cpp
    ../Source/Core/ProjectInstance.cpp
    ../Source/Core/ProjectInstance.h)
set_target_properties(harken_project_instance_process PROPERTIES AUTOMOC ON)
target_include_directories(harken_project_instance_process PRIVATE
    ${CMAKE_SOURCE_DIR}/HarkenStudio/Source)
target_link_libraries(harken_project_instance_process PRIVATE Qt6::Widgets Qt6::Network)
if(WIN32)
    target_compile_definitions(harken_project_instance_process PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(harken_project_instance_process PRIVATE user32 advapi32)
elseif(APPLE)
    target_link_libraries(harken_project_instance_process PRIVATE "-framework CoreFoundation")
endif()
target_compile_definitions(harken_editor_tests PRIVATE
    HARKEN_PROJECT_INSTANCE_PROCESS_PATH="$<TARGET_FILE:harken_project_instance_process>")
add_dependencies(harken_editor_tests harken_project_instance_process)
if(TARGET harken_core_shared)
    find_package(Qt6 REQUIRED COMPONENTS Core Network)
    add_executable(harken_remote_test_game UnitTest/RemoteGame.cpp)
    target_link_libraries(harken_remote_test_game PRIVATE harken_core_shared Qt6::Core)
    target_sources(harken_editor_tests PRIVATE UnitTest/RemoteConnectionProcessTest.cpp)
    target_compile_definitions(harken_editor_tests PRIVATE
        HARKEN_REMOTE_GAME_PATH="$<TARGET_FILE:harken_remote_test_game>")
    add_dependencies(harken_editor_tests harken_remote_test_game)
    if(WIN32)
        add_custom_command(TARGET harken_remote_test_game POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:harken_core_shared>" "$<TARGET_FILE_DIR:harken_remote_test_game>"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:Qt6::Core>" "$<TARGET_FILE_DIR:harken_remote_test_game>")
    endif()
endif()
add_custom_command(TARGET harken_editor_tests POST_BUILD
    COMMAND ${Python3_EXECUTABLE}
        "${CMAKE_SOURCE_DIR}/.scripts/schema_archive.py"
        --schema-root "${HARKEN_SCHEMA_SOURCE_DIR}"
        --check-generated "${HARKEN_SCHEMA_GENERATED_CORE_DIR}"
        --publish "$<TARGET_FILE_DIR:harken_editor_tests>/schema"
    COMMENT "Publishing the Studio Schema build snapshot for editor tests"
)
if(TARGET harken_exported_pack_qualification)
    target_compile_definitions(harken_editor_tests PRIVATE
        HARKEN_EXPORTED_PACK_QUAL_PATH="$<TARGET_FILE:harken_exported_pack_qualification>")
    add_dependencies(harken_editor_tests harken_exported_pack_qualification)
endif()

add_executable(harken_pack_studio_qualification
    Benchmark/PackFoundationStudioQualification.cpp
)
target_link_libraries(harken_pack_studio_qualification PRIVATE harken_editor_lib)
target_compile_definitions(harken_pack_studio_qualification PRIVATE
    HARKEN_QUALIFICATION_CONFIGURATION="$<CONFIG>")
target_include_directories(harken_pack_studio_qualification PRIVATE
    ${CMAKE_SOURCE_DIR}/HarkenStudio/Source
)

if(WIN32)
    find_program(WINDEPLOYQT_EXE windeployqt
        HINTS ${Qt6_DIR}/../../../bin
        REQUIRED)

    add_custom_command(TARGET harken_editor_tests POST_BUILD
        COMMAND ${WINDEPLOYQT_EXE}
            $<$<CONFIG:Debug>:--debug>
            $<$<NOT:$<CONFIG:Debug>>:--release>
            --no-translations
            --no-compiler-runtime
            --no-system-d3d-compiler
            --no-system-dxc-compiler
            --no-opengl-sw
            --no-quick-import
            $<TARGET_FILE:harken_editor_tests>
        COMMENT "Deploying Qt runtime for editor tests"
    )

    add_custom_command(TARGET harken_pack_studio_qualification POST_BUILD
        COMMAND ${WINDEPLOYQT_EXE}
            $<$<CONFIG:Debug>:--debug>
            $<$<NOT:$<CONFIG:Debug>>:--release>
            --no-translations
            --no-compiler-runtime
            --no-system-d3d-compiler
            --no-system-dxc-compiler
            --no-opengl-sw
            --no-quick-import
            $<TARGET_FILE:harken_pack_studio_qualification>
        COMMENT "Deploying Qt runtime for Studio Pack qualification"
    )

    if(TARGET Qt6::QOffscreenIntegrationPlugin)
        add_custom_command(TARGET harken_editor_tests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:Qt6::QOffscreenIntegrationPlugin>"
                "$<TARGET_FILE_DIR:harken_editor_tests>/platforms/"
        )
    endif()
endif()

# Editor tests load the Qt runtime, which the build graph does not order
# ahead of this executable's link step; running the freshly linked binary
# during the build is therefore not reliable. Defer test enumeration to
# ctest time, when the full runtime environment is guaranteed present.
#
# Every case becomes its own ctest entry, so "ctest -j" runs many editor
# processes at once. On a real windowing platform those processes compete
# for one machine-wide keyboard focus, and any case asserting hasFocus()
# fails whenever another process wins it. Pin the offscreen platform, where
# activation is per-process state; this also matches how CI runs the same
# binary. Running the executable directly still honours the environment.
set(_harken_editor_daily_suites
    AuthoredPackLifecycle.*
    ExportedPackLifecycle.*
    ExportedPackReadOnlyLifecycle.*
    ProjectPlatformBuild.*
    EditorProjectPackExporterManifest.*
    EditorProjectPackExporterSharedPack.*)
list(JOIN _harken_editor_daily_suites ":" _harken_editor_daily_filter)
set(_harken_editor_qualification_tests
    ExportedPackLifecycle.WasapiImmediateSoundHarness
    ExportedPackLifecycle.RelWithDebInfoQualificationReport)
list(JOIN _harken_editor_qualification_tests ":" _harken_editor_qualification_filter)

gtest_discover_tests(harken_editor_tests
    DISCOVERY_MODE PRE_TEST
    TEST_FILTER "-${_harken_editor_daily_filter}:${_harken_editor_qualification_filter}"
    TEST_LIST harken_editor_pr_tests
    PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
gtest_discover_tests(harken_editor_tests
    DISCOVERY_MODE PRE_TEST
    TEST_FILTER "${_harken_editor_daily_filter}-${_harken_editor_qualification_filter}"
    TEST_LIST harken_editor_daily_tests
    PROPERTIES LABELS harken-daily ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
gtest_discover_tests(harken_editor_tests
    DISCOVERY_MODE PRE_TEST
    TEST_FILTER "${_harken_editor_qualification_filter}"
    TEST_LIST harken_editor_qualification_tests
    PROPERTIES LABELS harken-qualification ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(harken_editor_metadata_tests UnitTest/EditorWaveformPublicationTest.cpp)
target_link_libraries(harken_editor_metadata_tests PRIVATE harken_editor_metadata harken_core GTest::gtest_main)
target_include_directories(harken_editor_metadata_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    ${HARKEN_SCHEMA_GENERATED_CORE_DIR}
    ${HARKEN_RUNTIME_DIR}/core/include ${HARKEN_RUNTIME_DIR}/core/contract/include
    ${HARKEN_RUNTIME_DIR}/media/include ${HARKEN_RUNTIME_DIR}/param/include
    ${HARKEN_RUNTIME_DIR}/dsp/include
    ${HARKEN_RUNTIME_DIR}/render/include
    ${CMAKE_SOURCE_DIR}/HarkenEngine/Source/Tooling/Common)
target_compile_definitions(harken_editor_metadata_tests PRIVATE HARKEN_TEMP_DIR="${CMAKE_SOURCE_DIR}/.temp")
target_compile_definitions(harken_editor_metadata_tests PRIVATE
    HARKEN_EDITOR_METADATA_GOLDEN_DIR="${CMAKE_SOURCE_DIR}/HarkenSDK/Test/Fixtures/EditorMetadata/2")
gtest_discover_tests(harken_editor_metadata_tests TEST_PREFIX "EditorMetadata.")

add_executable(harken_project_schema_tests
    UnitTest/ProjectSchemaFrontEndTest.cpp
    UnitTest/ProjectSchemaFixtureTest.cpp
    UnitTest/ProjectSchemaChassisTest.cpp
    UnitTest/ProjectSchemaPlayablePropertyRegistryTest.cpp
    UnitTest/ProjectSchemaSignalGraphTest.cpp
    UnitTest/ProjectSchemaReferenceResolutionTest.cpp
    UnitTest/ProjectSchemaPropertyFoldTest.cpp
    UnitTest/ProjectSchemaSendResolutionTest.cpp
    UnitTest/ProjectSchemaEffectChainTest.cpp
    UnitTest/ProjectSchemaNormalizationTest.cpp
    UnitTest/ProjectSchemaCoverageTest.cpp
    UnitTest/ProjectSchemaIrTest.cpp
    UnitTest/ProjectSchemaHardeningTest.cpp
    UnitTest/ProjectSchemaEstimationTest.cpp
    UnitTest/ProjectSchemaProgramTest.cpp
    UnitTest/VariableContractArtifactTest.cpp
    UnitTest/ReferenceSlotResolverTest.cpp
)
target_link_libraries(harken_project_schema_tests PRIVATE
    harken_project_schema harken_voice_slot_layout_contract GTest::gtest_main)
target_include_directories(harken_project_schema_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${HARKEN_RUNTIME_DIR}/core/include ${HARKEN_RUNTIME_DIR}/api/include)
gtest_discover_tests(harken_project_schema_tests
    TEST_PREFIX "ProjectSchema."
    PROPERTIES TIMEOUT 120)

add_executable(harken_bus_compiler_tests UnitTest/BusCompilerTest.cpp)
target_link_libraries(harken_bus_compiler_tests PRIVATE harken_bus_compiler GTest::gtest_main)
target_include_directories(harken_bus_compiler_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${HARKEN_RUNTIME_DIR}/api/include)
gtest_discover_tests(harken_bus_compiler_tests TEST_PREFIX "BusCompiler." PROPERTIES TIMEOUT 120)

add_executable(harken_studio_runtime_api_tests UnitTest/StudioRuntimeApiTest.cpp)
target_include_directories(harken_studio_runtime_api_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${_studio_test_runtime_includes})
target_link_libraries(harken_studio_runtime_api_tests PRIVATE
    harken_studio_runtime harken_current_world_compiler GTest::gtest_main)
target_compile_definitions(harken_studio_runtime_api_tests PRIVATE
    HARKEN_TEST_PREVIEW_ENABLED=$<BOOL:${HARKEN_DEVELOPMENT_PREVIEW}>)
if(UNIX)
    target_link_libraries(harken_studio_runtime_api_tests PRIVATE ${CMAKE_DL_LIBS})
    target_compile_definitions(harken_studio_runtime_api_tests PRIVATE
        HARKEN_TEST_RUNTIME_LIBRARY="$<TARGET_FILE:harken_studio_runtime>")
endif()
gtest_discover_tests(harken_studio_runtime_api_tests DISCOVERY_MODE PRE_TEST)

add_executable(harken_studio_runtime_disabled_tests
    UnitTest/StudioRuntimeApiTest.cpp ${_studio_preview_source_dir}/src/PreviewProject.cpp)
target_include_directories(harken_studio_runtime_disabled_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${_studio_preview_source_dir}/include
    ${_studio_test_runtime_includes} ${HARKEN_RUNTIME_DIR}/api/src
    ${HARKEN_RUNTIME_DIR}/core/include ${HARKEN_RUNTIME_DIR}/media/include)
target_link_libraries(harken_studio_runtime_disabled_tests PRIVATE
    harken_core harken_current_world_compiler GTest::gtest_main)
target_compile_definitions(harken_studio_runtime_disabled_tests PRIVATE
    HARKEN_DEVELOPMENT_PREVIEW=0 HARKEN_TEST_PREVIEW_ENABLED=0)
gtest_discover_tests(harken_studio_runtime_disabled_tests
    TEST_PREFIX "Disabled." DISCOVERY_MODE PRE_TEST)

harken_add_ci_test_target(editor)
add_test(NAME studio_reference_slots_generated_check
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_SOURCE_DIR}/HarkenStudio/Source/Tooling/ReferenceSlots/slots/gen_reference_slots.py
        --check)
add_test(NAME studio_playable_properties_generated_check
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_SOURCE_DIR}/HarkenStudio/Source/Tooling/PlayableProperties/properties/gen_playable_properties.py
        --check)
