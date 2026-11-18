add_executable(harken_studio_tests
    ${CMAKE_SOURCE_DIR}/HarkenStudio/resources.qrc
    UnitTest/TestMain.cpp
    UnitTest/ThemePackageParserTest.cpp
    UnitTest/ThemePackageLoaderTest.cpp
    UnitTest/ThemeManagerTest.cpp
    UnitTest/ThemeAssetResolverTest.cpp
    UnitTest/TableWidgetTest.cpp
    UnitTest/ThemeMetricsMigrationTest.cpp
    UnitTest/ShortcutManagerTest.cpp
    UnitTest/ToastPresenterTest.cpp
    UnitTest/QtAdsTabLifecycleTest.cpp
    UnitTest/WorkspaceDockPinTest.cpp
    UnitTest/TimeSeriesChartModelTest.cpp
    UnitTest/TimeSeriesChartTest.cpp
    UnitTest/MonitorCursorModelTest.cpp
    UnitTest/MonitorStoreTest.cpp
    UnitTest/MonitorDecoderTest.cpp
    UnitTest/MonitorSessionFileTest.cpp
    UnitTest/MeterScaleTest.cpp
    UnitTest/MonitorStateCutTest.cpp

    # P2-5 availability
    # P2-6 session-file
    UnitTest/Monitor/support/MonitorFixtureLoader.cpp
    UnitTest/TimelineTicksTest.cpp
    UnitTest/SpectrogramMeterRendererTest.cpp
    UnitTest/PollReentryGuardTest.cpp
    UnitTest/StableIdTest.cpp
    UnitTest/AttenuationCurveEvalTest.cpp
    UnitTest/AttenuationCurveHandleGeometryTest.cpp
    UnitTest/AttenuationCurvePasteLimitTest.cpp
    UnitTest/AttenuationCurvePointCoordinateTest.cpp
    UnitTest/AttenuationCurveUndoTest.cpp
    UnitTest/AttenuationRuntimeSyncTest.cpp
    UnitTest/AudioEnginePreviewTest.cpp
    UnitTest/UndoHistoryTest.cpp
    UnitTest/AdvancedActionResultTest.cpp
    UnitTest/PreparedProjectAssetReconcileTest.cpp
    UnitTest/ProjectSerializationTest.cpp
    UnitTest/PlayablePropertyAuthoringTest.cpp
    UnitTest/FieldGroupProjectAdapterTest.cpp
    UnitTest/FieldGroupResolutionTest.cpp
    UnitTest/FieldGroupJsonTest.cpp
    UnitTest/PlatformOverrideSerializationTest.cpp
    UnitTest/ProjectPlatformCommandTest.cpp
    UnitTest/ProjectPlatformBuildTest.cpp
    UnitTest/PlatformFieldMergeTest.cpp
    UnitTest/PlayablePropertyOccurrenceLifecycleTest.cpp
    UnitTest/ProjectInstanceTest.cpp
    UnitTest/ProjectUpgradeManagerTest.cpp
    UnitTest/ProjectOpenApprovalTest.cpp
    UnitTest/SchemaArchiveTest.cpp
    UnitTest/ProjectDocumentRoundTripTest.cpp
    UnitTest/ProjectMigrationTest.cpp
    UnitTest/AudioFormatDecodeTest.cpp
    UnitTest/MediaPoolRefreshTest.cpp
    UnitTest/FileWatcherTest.cpp
    UnitTest/ProjectPackExporterManifestTest.cpp
    UnitTest/ExportedPackLifecycleTest.cpp
    UnitTest/CanonicalBusSendSnapshotParserTest.cpp
    UnitTest/DebugHonestyMapperTest.cpp
    UnitTest/WorkspaceWindowContractTest.cpp
    UnitTest/MonitorPanelModuleTest.cpp
    UnitTest/MonitorPanelTest.cpp
    UnitTest/MonitorSpectrumTest.cpp
    UnitTest/MonitorSharedCursorTest.cpp
    UnitTest/ParameterBindingTest.cpp
    UnitTest/ParameterControlHostTest.cpp
    UnitTest/PropertyEditorEngineTest.cpp
    UnitTest/InspectorRowSetGoldTest.cpp
    UnitTest/ResolveComponentTypedTest.cpp
    UnitTest/ListPropertyTest.cpp
    UnitTest/ObjectRefDropTest.cpp
    UnitTest/RefIdMimeTest.cpp
    UnitTest/ProjectExplorerSourcesTabTest.cpp
    UnitTest/ProjectTreeDragDropTest.cpp
    UnitTest/FolderAddressingTest.cpp
    UnitTest/BucketFolderStoreTest.cpp
    UnitTest/ProjectTreePanelAnchorTest.cpp
    UnitTest/BusTopologySyncTest.cpp
    UnitTest/CanonicalRuntimeProjectAdapterTest.cpp
    UnitTest/RuntimeSyncCoordinatorTest.cpp
    UnitTest/CurrentWorldProjectAdapterTest.cpp
    UnitTest/OutputSendExclusionTest.cpp
    UnitTest/SignalPathGraphTest.cpp
    UnitTest/ActionIdentityTest.cpp
    UnitTest/ActionCatalogExportTest.cpp
    UnitTest/ActionStepEditingTest.cpp
    UnitTest/PackGenerationRecordTest.cpp
    UnitTest/ComponentCatalogTest.cpp
    UnitTest/HelpCatalogKeyTest.cpp
    UnitTest/TimelineCoordinatesTest.cpp
    UnitTest/TimelineLedgerTest.cpp
    UnitTest/TimelineSemanticsTest.cpp
    UnitTest/TimelineSerializationTest.cpp
    UnitTest/VariableRehearsalInspectorTest.cpp
    UnitTest/SoundSourceSemanticsTest.cpp
    UnitTest/BindingSemanticsTest.cpp
    UnitTest/ContainerGridCopyTest.cpp
    UnitTest/SourceEditorPanelTest.cpp
    UnitTest/FileViewerPcmSliceTest.cpp
    UnitTest/StandaloneEditorDispatchTest.cpp
    UnitTest/TimelineEditorPanelTest.cpp
    UnitTest/ItemTypeIconAssetTest.cpp
    UnitTest/NewObjectNamingTest.cpp
    UnitTest/AICommandCatalogTest.cpp
    UnitTest/DesktopWidgetTargetsTest.cpp
    UnitTest/AIDispatchPolicyTest.cpp
    UnitTest/AIDurableMemoryStoreTest.cpp
    UnitTest/AIConversationStoreTest.cpp
    UnitTest/AIConversationSessionStoreTest.cpp
    UnitTest/AIMarkdownRendererTest.cpp
    UnitTest/ProjectSettingsPanelTest.cpp
    UnitTest/SettingsApplyContractTest.cpp
    UnitTest/MainWindowSettingsMenuTest.cpp
    UnitTest/Support/QtFatalToStderr.cpp
    UnitTest/Support/MockLLMProvider.h
    UnitTest/Support/MockLLMProvider.cpp
    UnitTest/Support/SyntheticHttpServer.h
    UnitTest/Support/SyntheticHttpServer.cpp
    UnitTest/OpenAICompatibleLLMProviderTest.cpp
    UnitTest/AIAssistantServiceTest.cpp
)
set_target_properties(harken_studio_tests PROPERTIES
    AUTOMOC ON
    AUTORCC ON
)

if(MSVC)
    target_compile_options(harken_studio_tests PRIVATE /FS)
endif()

if(WIN32)
    set_target_properties(harken_studio_tests PROPERTIES
        VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:harken_studio_tests>"
        VS_DEBUGGER_ENVIRONMENT "QT_QPA_PLATFORM=windows"
    )
endif()
target_link_libraries(harken_studio_tests PRIVATE
    harken_editor_lib
    harken_current_world_compiler
    Harken::EngineDev
    GTest::gtest
)
target_include_directories(harken_studio_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/HarkenStudio/Source
    ${CMAKE_SOURCE_DIR}/HarkenStudio/Source/Core
    ${CMAKE_SOURCE_DIR}/HarkenStudio/Source/Tooling/ProjectSchema/include
)
target_compile_definitions(harken_studio_tests PRIVATE
    HARKEN_TEMP_DIR="${PROJECT_SOURCE_DIR}/.temp"
    HARKEN_STUDIO_SOURCE_DIR="${CMAKE_SOURCE_DIR}/HarkenStudio"
    HARKEN_MONITOR_EDITOR_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/UnitTest/Monitor/fixtures")
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
target_compile_definitions(harken_studio_tests PRIVATE
    HARKEN_PROJECT_INSTANCE_PROCESS_PATH="$<TARGET_FILE:harken_project_instance_process>")
add_dependencies(harken_studio_tests harken_project_instance_process)
add_executable(harken_resource_compiler_process UnitTest/ResourceCompilerProcess.cpp)
target_link_libraries(harken_resource_compiler_process PRIVATE harkenizer_compiler_client Qt6::Core)
target_link_libraries(harken_studio_tests PRIVATE harkenizer_compiler_client)
target_compile_definitions(harken_studio_tests PRIVATE
    HARKEN_RESOURCE_COMPILER_PROCESS_PATH="$<TARGET_FILE:harken_resource_compiler_process>")
add_dependencies(harken_studio_tests harken_resource_compiler_process)
if(WIN32)
    add_custom_command(TARGET harken_resource_compiler_process POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:Qt6::Core>" "$<TARGET_FILE_DIR:harken_resource_compiler_process>")
endif()
harken_publish_studio_schema(harken_studio_tests)

target_compile_definitions(harken_studio_tests PRIVATE
    HARKEN_STUDIO_CLI_PATH="$<TARGET_FILE:harken_studio_cli>")
add_dependencies(harken_studio_tests harken_studio_cli)
add_test(NAME studio_cli_contract
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/.scripts/verify_studio_cli.py"
        --cli "$<TARGET_FILE:harken_studio_cli>"
        --work-dir "${CMAKE_BINARY_DIR}/cli-contract" --skip-audio-output)
set_tests_properties(studio_cli_contract PROPERTIES TIMEOUT 300)

add_executable(harken_timeline_benchmark EXCLUDE_FROM_ALL
    Benchmark/TimelineInteractionBenchmark.cpp
    ${CMAKE_SOURCE_DIR}/HarkenStudio/resources.qrc
)
set_target_properties(harken_timeline_benchmark PROPERTIES AUTORCC ON)
target_link_libraries(harken_timeline_benchmark PRIVATE harken_editor_lib)
target_compile_definitions(harken_timeline_benchmark PRIVATE
    HARKEN_BENCHMARK_CONFIGURATION="$<CONFIG>")
add_dependencies(harken_timeline_benchmark harken_studio)
harken_publish_studio_schema(harken_timeline_benchmark)
if(TARGET Qt6::QOffscreenIntegrationPlugin)
    add_custom_command(TARGET harken_timeline_benchmark POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:harken_timeline_benchmark>/platforms"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:Qt6::QOffscreenIntegrationPlugin>"
            "$<TARGET_FILE_DIR:harken_timeline_benchmark>/platforms/"
    )
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

    add_custom_command(TARGET harken_studio_tests POST_BUILD
        COMMAND ${WINDEPLOYQT_EXE}
            $<$<CONFIG:Debug>:--debug>
            $<$<NOT:$<CONFIG:Debug>>:--release>
            --no-translations
            --no-compiler-runtime
            --no-system-d3d-compiler
            --no-system-dxc-compiler
            --no-opengl-sw
            --no-quick-import
            $<TARGET_FILE:harken_studio_tests>
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
        add_custom_command(TARGET harken_studio_tests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:Qt6::QOffscreenIntegrationPlugin>"
                "$<TARGET_FILE_DIR:harken_studio_tests>/platforms/"
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
    Historical/ProjectDocumentVersionRoundTripTest.*)
list(JOIN _harken_editor_qualification_tests ":" _harken_editor_qualification_filter)

gtest_discover_tests(harken_studio_tests
    DISCOVERY_MODE PRE_TEST
    TEST_FILTER "-${_harken_editor_daily_filter}:${_harken_editor_qualification_filter}"
    TEST_LIST harken_editor_pr_tests
    PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
gtest_discover_tests(harken_studio_tests
    DISCOVERY_MODE PRE_TEST
    TEST_FILTER "${_harken_editor_daily_filter}-${_harken_editor_qualification_filter}"
    TEST_LIST harken_editor_daily_tests
    PROPERTIES LABELS harken-daily ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
gtest_discover_tests(harken_studio_tests
    DISCOVERY_MODE PRE_TEST
    TEST_FILTER "${_harken_editor_qualification_filter}"
    TEST_LIST harken_editor_qualification_tests
    PROPERTIES LABELS harken-qualification ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(harken_project_schema_tests
    UnitTest/ProjectSchemaFrontEndTest.cpp
    UnitTest/ProjectSchemaPlayablePropertyRegistryTest.cpp
    UnitTest/ProjectSchemaSignalGraphTest.cpp
    UnitTest/ProjectSchemaReferenceResolutionTest.cpp
    UnitTest/ProjectSchemaSendResolutionTest.cpp
    UnitTest/ProjectSchemaIrTest.cpp
    UnitTest/ProjectSchemaEstimationTest.cpp
    UnitTest/VariableContractArtifactTest.cpp
    UnitTest/ReferenceSlotResolverTest.cpp
)
target_link_libraries(harken_project_schema_tests PRIVATE
    harken_project_schema Harken::EngineDev GTest::gtest_main)
target_include_directories(harken_project_schema_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
gtest_discover_tests(harken_project_schema_tests
    TEST_PREFIX "ProjectSchema."
    PROPERTIES TIMEOUT 120)

add_executable(harken_bus_compiler_tests UnitTest/BusCompilerTest.cpp)
target_link_libraries(harken_bus_compiler_tests PRIVATE
    harken_bus_compiler Harken::EngineDev GTest::gtest_main)
target_include_directories(harken_bus_compiler_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
gtest_discover_tests(harken_bus_compiler_tests TEST_PREFIX "BusCompiler." PROPERTIES TIMEOUT 120)

harken_add_ci_test_target(studio)
harken_add_python_check_test(studio_schema_archive_unit
    "${CMAKE_SOURCE_DIR}/.scripts/tests/test_schema_archive.py")
harken_add_python_check_test(studio_schema_generated_check
    "${HARKEN_SCHEMA_TOOL}"
    --schema-root "${HARKEN_SCHEMA_SOURCE_DIR}"
    --check-generated "${HARKEN_SCHEMA_GENERATED_CORE_DIR}"
    --check-published "${HARKEN_SCHEMA_PUBLISHED_DIR}")
