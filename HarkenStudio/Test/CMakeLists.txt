add_executable(harken_editor_tests
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
    UnitTest/MonitorDecoderTest.cpp
    UnitTest/MonitorStoreTest.cpp
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
    UnitTest/HubClientArgumentsTest.cpp
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
    UnitTest/RemoteConnectionTest.cpp
    UnitTest/DebugHonestyMapperTest.cpp
    UnitTest/WorkspaceWindowContractTest.cpp
    UnitTest/MonitorPanelModuleTest.cpp
    UnitTest/MonitorPanelTest.cpp
    UnitTest/MonitorSpectrumTest.cpp
    UnitTest/MeterPanelTest.cpp
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
    UnitTest/SourceEditorPcmSliceTest.cpp
    UnitTest/StandaloneEditorDispatchTest.cpp
    UnitTest/TimelineEditorPanelTest.cpp
    UnitTest/TransitionEditorPanelTest.cpp
    UnitTest/ItemTypeIconAssetTest.cpp
    UnitTest/NewObjectNamingTest.cpp
    UnitTest/AICommandCatalogTest.cpp
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
