if(HARKEN_BUILD_RUNTIME_UNIT_TESTS)

add_subdirectory(UnitTest/ProgramRegistration ProgramRegistration)

add_executable(harken_object_identity_tests
    UnitTest/ObjectIdTest.cpp
    UnitTest/ObjectIdCContract.c)
target_include_directories(harken_object_identity_tests PRIVATE
    ${HARKEN_RUNTIME_DIR}/api/include
    ${HARKEN_RUNTIME_DIR}/core/include
    ${HARKEN_RUNTIME_DIR}/core/contract/include
    ${HARKEN_RUNTIME_DIR}/dsp/include
    ${HARKEN_RUNTIME_DIR}/media/include
    ${HARKEN_RUNTIME_DIR}/param/include
    ${HARKEN_RUNTIME_DIR}/remote/include
    ${HARKEN_RUNTIME_DIR}/render/include
    ${CMAKE_SOURCE_DIR}/HarkenEngine/Tool/Remote/include
    ${CMAKE_SOURCE_DIR}/HarkenEngine/Tool/Remote/generated)
target_link_libraries(harken_object_identity_tests PRIVATE harken_core harken_buildinfo GTest::gtest_main)
if(WIN32 AND TARGET harken_host)
    add_dependencies(harken_object_identity_tests harken_host)
    target_compile_definitions(harken_object_identity_tests PRIVATE
        HARKEN_HOST_PATH="$<TARGET_FILE:harken_host>"
        HARKEN_TEMP_DIR="${CMAKE_SOURCE_DIR}/.temp")
endif()
gtest_discover_tests(harken_object_identity_tests)

add_executable(harken_remote_runtime_tests
    UnitTest/RuntimeRemoteTest.cpp
    UnitTest/monitor_fixture_decode.cpp)
target_link_libraries(harken_remote_runtime_tests PRIVATE harken_core harken_buildinfo harken_remote_protocol harken_preview GTest::gtest_main)
target_include_directories(harken_remote_runtime_tests PRIVATE
    ${HARKEN_RUNTIME_DIR}/api/src
    ${HARKEN_RUNTIME_DIR}/core/include
    ${HARKEN_RUNTIME_DIR}/core/contract/include
    ${HARKEN_RUNTIME_DIR}/dsp/include
    ${HARKEN_RUNTIME_DIR}/media/include
    ${HARKEN_RUNTIME_DIR}/media/contract/include
    ${HARKEN_RUNTIME_DIR}/param/include
    ${HARKEN_RUNTIME_DIR}/render/include
    ${HARKEN_THIRD_PARTY_DIR}/miniaudio
    ${CMAKE_CURRENT_SOURCE_DIR})
gtest_discover_tests(harken_remote_runtime_tests)

add_executable(harken_monitor_runtime_tests
    UnitTest/MonitorExecutionCodecTest.cpp
    UnitTest/MonitorExecutionProducerTest.cpp
    UnitTest/MonitorExecutionIdentityTest.cpp
    UnitTest/MonitorHubSessionTest.cpp
    UnitTest/MonitorHubFanoutTest.cpp
    UnitTest/MonitorHubSubscriptionTest.cpp
    UnitTest/MonitorHubPlanTest.cpp
    UnitTest/MonitorHubTimeSeriesTest.cpp
    UnitTest/MonitorHubConformanceTest.cpp
    UnitTest/MonitorSessionRecordTest.cpp
    UnitTest/MonitorSessionTraceTest.cpp
    UnitTest/MonitorRemoteEndpointTest.cpp
    UnitTest/MonitorRemoteAuthTest.cpp
    UnitTest/monitor_fixture_decode.cpp
    UnitTest/monitor_session_codec.cpp
    UnitTest/monitor_remote_codec.cpp)
target_link_libraries(harken_monitor_runtime_tests PRIVATE harken_core harken_buildinfo GTest::gtest_main)
target_include_directories(harken_monitor_runtime_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/HarkenEngine/Tool/Remote/generated
    ${HARKEN_RUNTIME_DIR}/api/src
    ${HARKEN_RUNTIME_DIR}/core/include
    ${HARKEN_RUNTIME_DIR}/core/contract/include
    ${HARKEN_RUNTIME_DIR}/dsp/include
    ${HARKEN_RUNTIME_DIR}/media/include
    ${HARKEN_RUNTIME_DIR}/media/contract/include
    ${HARKEN_RUNTIME_DIR}/param/include
    ${HARKEN_RUNTIME_DIR}/render/include
    ${HARKEN_THIRD_PARTY_DIR}/miniaudio
    ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(harken_monitor_runtime_tests PRIVATE
    HARKEN_MONITOR_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/UnitTest/monitor"
    _CRT_SECURE_NO_WARNINGS)
if(HARKEN_ENABLE_STAGE_TAPS)
    target_compile_definitions(harken_monitor_runtime_tests PRIVATE HARKEN_ENABLE_STAGE_TAPS)
endif()
gtest_discover_tests(harken_monitor_runtime_tests TEST_PREFIX "MonitorRuntime." PROPERTIES TIMEOUT 120)

add_executable(harken_pack_storage_tests
    UnitTest/AllocationProbe.cpp
    UnitTest/CanonicalPackSetStorageTest.cpp
    UnitTest/CanonicalAttenuationGenerationTest.cpp)
target_link_libraries(harken_pack_storage_tests PRIVATE harken_core harken_buildinfo GTest::gtest_main)
target_include_directories(harken_pack_storage_tests PRIVATE
    ${HARKEN_RUNTIME_DIR}/core/include
    ${HARKEN_RUNTIME_DIR}/core/contract/include
    ${HARKEN_RUNTIME_DIR}/media/include
    ${HARKEN_RUNTIME_DIR}/param/include
    ${HARKEN_RUNTIME_DIR}/render/include
    ${HARKEN_RUNTIME_DIR}/dsp/include
    ${CMAKE_CURRENT_SOURCE_DIR})
gtest_discover_tests(harken_pack_storage_tests PROPERTIES TIMEOUT 120)

add_executable(harken_runtime_tests
    UnitTest/AllocationProbe.cpp
    UnitTest/AbiExceptionFirewallSweepTest.cpp
    UnitTest/AbiVintageContractTest.cpp
    UnitTest/LifecycleTest.cpp
    UnitTest/MixRateConfigTest.cpp
    UnitTest/BusTreeTest.cpp
    UnitTest/BusTopologySnapshotTest.cpp
    UnitTest/BusTopologyBuilderTest.cpp
    UnitTest/CallbackTermTest.cpp
    UnitTest/CascadeProtocolTest.cpp
    UnitTest/AuthoredStopAudioTest.cpp
    UnitTest/LogRingTest.cpp
    UnitTest/NotificationQueueTest.cpp
    UnitTest/ParamCombineTest.cpp
    UnitTest/PcmRegistryTest.cpp
    UnitTest/BlockArenaTest.cpp
    UnitTest/SnapshotTest.cpp
    UnitTest/ChannelPeakMeterTest.cpp
    UnitTest/SourceCursorMirrorTest.cpp
    UnitTest/TransportTest.cpp
    UnitTest/VoicePoolTest.cpp
    UnitTest/VoiceCapacityTest.cpp
    UnitTest/CommandBudgetTest.cpp
    UnitTest/OriginalSourceSubmissionTest.cpp
    UnitTest/CapacityContractTest.cpp
    UnitTest/VoiceSetterTest.cpp
    UnitTest/StagedVoiceStartTest.cpp
    UnitTest/SpatialApiTest.cpp
    UnitTest/SpatialCombineTest.cpp
    UnitTest/SpatialValidationTest.cpp
    UnitTest/RenderSliceTest.cpp
    UnitTest/MemguardScopeTest.cpp
    UnitTest/FloatModeTest.cpp
    UnitTest/HostOptionsTest.cpp
    UnitTest/JsonReaderTest.cpp
    UnitTest/PackTest.cpp
    UnitTest/LoopPlaybackTest.cpp
    UnitTest/EffectChainTest.cpp
    UnitTest/BuiltinEffectTest.cpp
    UnitTest/EffectAbiFreezeTest.cpp
    UnitTest/EffectContractSeamTest.cpp
    UnitTest/EffectTypeStatsSeamTest.cpp
    UnitTest/BusLifecycleContractSeamTest.cpp
    UnitTest/GainEffectTest.cpp
    UnitTest/LimiterEffectTest.cpp
    UnitTest/EffectKernelCharacterizationTest.cpp
    UnitTest/EffectRecycleCleanTest.cpp
    UnitTest/EffectParamLocatorTest.cpp
    UnitTest/EffectParamMetadataTest.cpp
    UnitTest/CapabilityDiscoveryTest.cpp
    UnitTest/EffectParamTargetStoreTest.cpp
    UnitTest/EffectParamBindingTableTest.cpp
    UnitTest/EffectParamBindingE2ETest.cpp
    UnitTest/StreamFormatTest.cpp
    UnitTest/StreamRegisterTest.cpp
    UnitTest/StreamPlaybackTest.cpp
    UnitTest/StreamWorkerLifecycleTest.cpp
    UnitTest/StreamConfigTest.cpp
    UnitTest/Utf8PathTest.cpp
    UnitTest/VoiceLedgerTest.cpp
    UnitTest/VoiceManagerTest.cpp
    UnitTest/StealTest.cpp
    UnitTest/EmitterTest.cpp
    UnitTest/RenameAbiPinTest.cpp
    UnitTest/MonitorAbiPinTest.cpp
    UnitTest/MonitorAbiPinC.c
    UnitTest/MonitorModelOnlyTest.cpp
    UnitTest/monitor_fixture_decode.cpp
    UnitTest/monitor_session_codec.cpp
    UnitTest/monitor_remote_codec.cpp
    UnitTest/MonitorFixtureRecordTest.cpp
    UnitTest/MonitorFixturePlanTest.cpp
    UnitTest/MonitorFixtureSessionTest.cpp
    UnitTest/MonitorFixtureRemoteTest.cpp
    UnitTest/MonitorObserveLifecycleTest.cpp
    UnitTest/MonitorObserveLineageTest.cpp
    UnitTest/MonitorObserveVoiceTest.cpp
    UnitTest/MonitorObserveBusTest.cpp
    UnitTest/MonitorObserveDspTest.cpp
    UnitTest/MonitorObserveEffectTest.cpp
    UnitTest/MonitorMeasureRegistryTest.cpp
    UnitTest/MonitorMeasureWaveformTest.cpp
    UnitTest/MonitorMeasureScopeTest.cpp
    UnitTest/MonitorMeasureSpectrumTest.cpp
    UnitTest/MonitorMeasureMeterTest.cpp
    UnitTest/MonitorMeasureBudgetTest.cpp
    UnitTest/MonitorMeasureSegmentTest.cpp
    UnitTest/MonitorSessionRecoverTest.cpp
    UnitTest/MonitorSessionCompareTest.cpp
    UnitTest/ParkTest.cpp
    UnitTest/HotReloadTest.cpp
    UnitTest/BusSendTest.cpp
    UnitTest/BusFilterTest.cpp
    UnitTest/BusMuteSoloTest.cpp
    UnitTest/BusReparentTest.cpp
    UnitTest/BusDestroyTest.cpp
    UnitTest/BusEffectTest.cpp
    UnitTest/BiquadGoldenTest.cpp
    UnitTest/OutputGainTest.cpp
    UnitTest/BusMixGoldenTest.cpp
    UnitTest/BusRampGoldenTest.cpp
    UnitTest/CascadeGainGoldenTest.cpp
    UnitTest/CurveMemoryPinTest.cpp
    UnitTest/CurveGoldenTest.cpp
    UnitTest/PassthroughGoldenTest.cpp
    UnitTest/PitchContinuityGoldenTest.cpp
    UnitTest/ResamplerGoldenTest.cpp
    UnitTest/ResamplerHighRatioTest.cpp
    UnitTest/StageTapGoldenTest.cpp
    UnitTest/VariableSweepGoldenTest.cpp
    UnitTest/VoiceRampGoldenTest.cpp
    UnitTest/PanLawGoldenTest.cpp
    UnitTest/SpatialMatrixTest.cpp
    UnitTest/StereoBalanceGoldenTest.cpp
    UnitTest/MultichannelPanGoldenTest.cpp
    UnitTest/DownmixGoldenTest.cpp
    UnitTest/BusDownmixTest.cpp
    UnitTest/ChannelLayoutAbiPinTest.cpp
    UnitTest/BindingAbiPinTest.cpp
    UnitTest/AttenuationRolloffGoldenTest.cpp
    UnitTest/AirAbsorptionGoldenTest.cpp
    UnitTest/WidthCollapseGoldenTest.cpp
    UnitTest/NearfieldShelfGoldenTest.cpp
    UnitTest/ProximityFrequencyRenderedTest.cpp
    UnitTest/ConeGoldenTest.cpp
    UnitTest/AttenuationOverrideGoldenTest.cpp
    UnitTest/PackRoundtripTest.cpp
    UnitTest/PackV2FormatTest.cpp
    UnitTest/PackCanonicalFormatTest.cpp
    UnitTest/CanonicalPackSetActivateTest.cpp
    UnitTest/ActionCatalogFormatTest.cpp
    UnitTest/ActionCatalogOwnershipTest.cpp
    UnitTest/ActionNamePostTest.cpp
    UnitTest/PackCompositionTest.cpp
    UnitTest/PackcPipelineTest.cpp
    UnitTest/StreamRoundtripTest.cpp
    UnitTest/EffectChainGoldenTest.cpp
    UnitTest/LoopSeamGoldenTest.cpp
    UnitTest/BuiltinEffectGoldenTest.cpp
    UnitTest/RemoteWireFormatTest.cpp
    UnitTest/RemoteDispatchTest.cpp
    UnitTest/ProfilingTimingTest.cpp
    UnitTest/RenderWorkerTest.cpp
    UnitTest/PlatformBackendTest.cpp
    UnitTest/TraceTest.cpp
    UnitTest/VariableWorldTest.cpp
    UnitTest/VariableBatchWriteTest.cpp
    UnitTest/VariableCatalogTest.cpp
    UnitTest/VariableQueryTest.cpp
    UnitTest/VariableCapacityTest.cpp
    UnitTest/VariableExecutionFingerprintTest.cpp
    UnitTest/VariableCurrentSliceTest.cpp
    UnitTest/VariableSliceTableTest.cpp
    UnitTest/PropertyPublicationTest.cpp
    UnitTest/PropertyPressureGoldenTest.cpp
    UnitTest/TargetMetadataTest.cpp
    UnitTest/BusTargetStoreTest.cpp
    UnitTest/BindingPoolTest.cpp
    UnitTest/BindingLivenessTest.cpp
    UnitTest/BindingApiTest.cpp
    UnitTest/MeterBlockRingTest.cpp
    UnitTest/MeterProbeTest.cpp
    UnitTest/MeterBallisticsGoldenTest.cpp
    UnitTest/BusMeterSidechainTest.cpp
    UnitTest/PropertyCaptureTest.cpp
    UnitTest/PackLogicLoadTest.cpp
    UnitTest/PackReplaceTest.cpp
     UnitTest/PlayableInstanceTableTest.cpp
    UnitTest/PlayableEntryConvergenceTest.cpp
     UnitTest/OccurrenceStatePoolTest.cpp
    UnitTest/CompletionProgressTest.cpp
    UnitTest/ResamplerAdvanceTest.cpp
    UnitTest/CompletionProgressRenderTest.cpp
    UnitTest/KnownCompletionTest.cpp
    UnitTest/CompletionOwnerTest.cpp
    UnitTest/CompletionMutationGuardTest.cpp
    UnitTest/DecisionGroupTest.cpp
    UnitTest/TransitionInstancePoolTest.cpp
    UnitTest/QueueRuntimeTest.cpp
    UnitTest/VariableControlGridTest.cpp
    UnitTest/VariableControlRuntimeTest.cpp
    UnitTest/ProgramGenerationRegistryTest.cpp
    UnitTest/CurrentSelectionTest.cpp
    UnitTest/CurrentWorldInstallTest.cpp
    UnitTest/PlayablePropertyCutTest.cpp
    UnitTest/OccurrencePathBindingTest.cpp
    UnitTest/PlayableBindingGateTest.cpp
    UnitTest/TimelineVoiceBindingInstallTest.cpp
    UnitTest/PlayableStartTest.cpp
    UnitTest/BusAdmissionStartTest.cpp
    UnitTest/CurrentWorldCompilerTest.cpp
    UnitTest/CurrentWorldHistoricalDecodeTest.cpp
    UnitTest/ResourceFactsTest.cpp
    UnitTest/ResolvedSlotTicketTest.cpp
    UnitTest/LogicCommitBundleTest.cpp
    UnitTest/ActionFiringTableTest.cpp
    UnitTest/RuntimeWorkIndexTest.cpp
    UnitTest/MonitorRequestTest.cpp
    UnitTest/ActionSeekTableTest.cpp
    UnitTest/ActionCooldownTableTest.cpp
    UnitTest/ActionDeferredRecordTableTest.cpp
    UnitTest/ActionFiringAbiTest.cpp
    UnitTest/ActionFiringApiTest.cpp
    UnitTest/ImmediateActionLaneTest.cpp
    UnitTest/DeferredIntentPoolTest.cpp
    UnitTest/LogicTimeAndStampTest.cpp
    UnitTest/ClockKernelTest.cpp
    UnitTest/ScheduledTransactionPoolTest.cpp
    UnitTest/MarkerIndexTest.cpp
    UnitTest/SamePositionResolverTest.cpp
    UnitTest/TimelinePlannerTest.cpp
    UnitTest/TimelineExecutorTest.cpp
    UnitTest/TimelineConvergenceTest.cpp
    UnitTest/TransportEpochOwnerTest.cpp
    UnitTest/ProgramConvergenceTest.cpp
    UnitTest/PreviewAbiTest.cpp
    UnitTest/ProgramRegistrationInstallTest.cpp
    UnitTest/EffectDescriptorWriterTest.cpp
    E2E/BaselineRegressionTest.cpp
    E2E/RemoteChannelTest.cpp
    E2E/TraceReplayTest.cpp
    E2E/PackLogicRoundtripTest.cpp
)
# ResourceFactsTest.cpp includes harken/media/ResourceFacts.h, which now pulls in
# the engine-free harken/media/ResourceFactsValue.h from a separate include root.
# The interface target carries that root (and no linked code), so the test can
# resolve it without this target learning the whole media include layout.
target_link_libraries(harken_runtime_tests PRIVATE harken_core harken_buildinfo harken_remote
                      harken_reference_slots harken_resource_facts_contract
                      harken_program_registration harken_current_world_compiler
                      harken_preview
                      GTest::gtest_main)
if(HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE)
    target_compile_definitions(harken_runtime_tests PRIVATE
        HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE)
endif()
if(MSVC)
    target_compile_options(harken_runtime_tests PRIVATE /FS /MP1 /bigobj)
endif()
# Matches harken_core's stage-tap opt-in: test assertions branch on the
# same macro and MUST agree with the library about whether measurement
# is live — an unconditional define here against an undefined library
# would make Release assert live per-voice taps the library never
# measures. Debug measures everywhere regardless; the Release-off shape
# is itself under test (the section-11.7 contract: bus output peaks
# honest, per-voice taps structurally zero).
if(HARKEN_ENABLE_STAGE_TAPS)
    target_compile_definitions(harken_runtime_tests PRIVATE HARKEN_ENABLE_STAGE_TAPS)
endif()
# Recorded-baseline comparisons resolve their files in the source tree,
# so recording and verifying agree on the location from any build
# directory (both generators, both configurations).
target_compile_definitions(harken_runtime_tests PRIVATE
    HARKEN_BASELINE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/Baselines"
    HARKEN_MONITOR_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/UnitTest/monitor"
    HARKEN_MONITOR_PLAN_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/UnitTest/monitor/plan"
    HARKEN_MONITOR_SESSION_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/UnitTest/monitor/session"
    HARKEN_MONITOR_REMOTE_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/UnitTest/monitor/remote")
# Tests read process env vars (baseline recorder switch) via standard
# std::getenv; silence the MS CRT deprecation aimed at it.
target_compile_definitions(harken_runtime_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
# The per-type effect stats table and refusal counters are published in
# harken/Harken.h, so activate the effect-type-stats contract seam: its frozen
# static_asserts pin the real header layout and its skip/red facets become live
# runtime tests.
target_compile_definitions(harken_runtime_tests PRIVATE HARKEN_EFFECT_TYPE_STATS_PUBLISHED)
# Tests drive internals directly (null backend pump, guard state); they see
# module-internal headers the public API deliberately does not expose.
target_include_directories(harken_runtime_tests PRIVATE
    ${HARKEN_RUNTIME_DIR}/api/src
    ${HARKEN_RUNTIME_DIR}/core/include
    ${HARKEN_RUNTIME_DIR}/core/contract/include
    ${HARKEN_RUNTIME_DIR}/dsp/include
    ${HARKEN_RUNTIME_DIR}/media/include
    ${HARKEN_RUNTIME_DIR}/param/include
    ${HARKEN_RUNTIME_DIR}/render/include
    ${CMAKE_CURRENT_SOURCE_DIR}
    # The host tool's pure parsing helpers are unit-tested here.
    ${CMAKE_SOURCE_DIR}/HarkenEngine/Tool/Host
    ${CMAKE_SOURCE_DIR}/HarkenEngine/Source/Tooling/Common
    # The packc authoring-range predicate (RecipeParamInAuthoredRange) is pinned
    # here against the runtime effect-parameter validator for accept-domain
    # equality.
    ${CMAKE_SOURCE_DIR}/HarkenStudio/Source/Tooling/PackC
    # The generated C# binding's C++ ABI-pin header (compiled by
    # BindingAbiPinTest.cpp to make table<->Harken.h drift a compile error).
    ${CMAKE_SOURCE_DIR}/HarkenEngine/Source/Tooling/Binding/generated
)
set_source_files_properties(UnitTest/MonitorAbiPinC.c PROPERTIES LANGUAGE C)

# The pack round-trip cases drive the real compiler tool as a subprocess
# and write their scratch files under the repository's .temp/ directory
# (the one sanctioned scratch location, gitignored).
target_compile_definitions(harken_runtime_tests PRIVATE
    HARKEN_PACKC_PATH="$<TARGET_FILE:harken_packc>"
    HARKEN_TEMP_DIR="${CMAKE_SOURCE_DIR}/.temp")
add_dependencies(harken_runtime_tests harken_packc)

# The remote-channel end-to-end cases drive harken_host remote-serve as a
# subprocess over a named pipe (the packc-subprocess precedent).
target_compile_definitions(harken_runtime_tests PRIVATE
    HARKEN_HOST_PATH="$<TARGET_FILE:harken_host>")
add_dependencies(harken_runtime_tests harken_host)

set(_harken_runtime_daily_suites
    ActionFiringApi.*
    */ImmediateConsumerDebtTest.*
    PackComposition.*
    NativeSlicePack.*
    NestedFrontierRefusal.*
    */NestedFrontierMatrix.*
    */ChildFrontierMatrix.*
    PlayableStart.*
    */NaturalEndVariableControlCancellationTest.*
    */VariableControlAudibleLockBoundaryTest.*
    */LateAppliedVariableControlSupersedeTest.*
    */ClaimedVariableControlSliceCancellationTest.*
    */ClaimedInitialVariableControlSliceCancellationTest.*
    */NaturalEndQueueCancellationTest.*
    */ClaimedQueueSliceCancellationTest.*
    */CompositeVariableControlCancellationTest.*
    TraceReplayE2E.*
    RemoteChannelE2E.*
    PackLogicRoundtrip.*)
list(JOIN _harken_runtime_daily_suites ":" _harken_runtime_daily_filter)
gtest_discover_tests(harken_runtime_tests
    TEST_FILTER "-${_harken_runtime_daily_filter}"
    TEST_LIST harken_runtime_pr_tests)
gtest_discover_tests(harken_runtime_tests
    TEST_FILTER "${_harken_runtime_daily_filter}"
    TEST_LIST harken_runtime_daily_tests
    PROPERTIES LABELS harken-daily)

add_executable(harken_variable_name_set_test
    UnitTest/VariableNameSetTest.cpp)
target_link_libraries(harken_variable_name_set_test PRIVATE harken_core harken_buildinfo GTest::gtest_main)
target_include_directories(harken_variable_name_set_test PRIVATE
    ${HARKEN_RUNTIME_DIR}/api/src
    ${HARKEN_RUNTIME_DIR}/core/include
    ${HARKEN_RUNTIME_DIR}/core/contract/include
    ${HARKEN_RUNTIME_DIR}/dsp/include
    ${HARKEN_RUNTIME_DIR}/media/include
    ${HARKEN_RUNTIME_DIR}/param/include
    ${HARKEN_RUNTIME_DIR}/render/include
    ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(harken_variable_name_set_test PRIVATE _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    target_compile_options(harken_variable_name_set_test PRIVATE /FS /bigobj)
endif()
gtest_discover_tests(harken_variable_name_set_test)

add_executable(harken_pack_schema_tests
    UnitTest/PackFoundationQualificationTest.cpp)
target_link_libraries(harken_pack_schema_tests PRIVATE
    harken_core harken_buildinfo harken_resource_facts_contract GTest::gtest_main)
target_include_directories(harken_pack_schema_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${HARKEN_RUNTIME_DIR}/api/src
    ${HARKEN_RUNTIME_DIR}/core/include
    ${HARKEN_RUNTIME_DIR}/core/contract/include
    ${HARKEN_RUNTIME_DIR}/dsp/include
    ${HARKEN_RUNTIME_DIR}/media/include
    ${HARKEN_RUNTIME_DIR}/param/include
    ${HARKEN_RUNTIME_DIR}/render/include)
target_compile_definitions(harken_pack_schema_tests PRIVATE
    HARKEN_TEMP_DIR="${CMAKE_SOURCE_DIR}/.temp")
if(HARKEN_ENABLE_STAGE_TAPS)
    target_compile_definitions(harken_pack_schema_tests PRIVATE HARKEN_ENABLE_STAGE_TAPS)
endif()
gtest_discover_tests(harken_pack_schema_tests TEST_PREFIX "PackSchema.")

add_executable(harken_monitor_public_api_probe
    UnitTest/MonitorPublicApiProbe.c
    UnitTest/monitor_fixture_decode.cpp
    UnitTest/monitor_session_codec.cpp)
set_source_files_properties(UnitTest/MonitorPublicApiProbe.c PROPERTIES LANGUAGE C)
target_include_directories(harken_monitor_public_api_probe PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/UnitTest
    ${HARKEN_RUNTIME_DIR}/api/include)
target_compile_definitions(harken_monitor_public_api_probe PRIVATE
    HARKEN_MONITOR_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/UnitTest/monitor"
    HARKEN_MONITOR_SESSION_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/UnitTest/monitor/session"
    _CRT_SECURE_NO_WARNINGS)
add_test(NAME monitor_public_api_probe
         COMMAND "$<TARGET_FILE:harken_monitor_public_api_probe>")

add_executable(harken_runtime_tests_hub_absent
    UnitTest/PassthroughGoldenTest.cpp
)
target_link_libraries(harken_runtime_tests_hub_absent PRIVATE
    harken_core_hub_absent harken_buildinfo GTest::gtest_main)
target_include_directories(harken_runtime_tests_hub_absent PRIVATE
    ${HARKEN_RUNTIME_DIR}/api/src
    ${HARKEN_RUNTIME_DIR}/core/include
    ${HARKEN_RUNTIME_DIR}/core/contract/include
    ${HARKEN_RUNTIME_DIR}/dsp/include
    ${HARKEN_RUNTIME_DIR}/media/include
    ${HARKEN_RUNTIME_DIR}/param/include
    ${HARKEN_RUNTIME_DIR}/render/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_definitions(harken_runtime_tests_hub_absent PRIVATE
    HARKEN_MONITOR_HUB_ABSENT
    _CRT_SECURE_NO_WARNINGS)
if(HARKEN_ENABLE_STAGE_TAPS)
    target_compile_definitions(harken_runtime_tests_hub_absent PRIVATE HARKEN_ENABLE_STAGE_TAPS)
endif()
gtest_discover_tests(harken_runtime_tests_hub_absent
    TEST_PREFIX "HubAbsent."
    PROPERTIES TIMEOUT 120 LABELS harken-artifact-contract)

# The forced fallback executable owns a separately compiled worker-pool object.
# It must not link harken_core because the private wake strategy changes the
# RenderWorkerPool layout for this target.
add_executable(harken_render_worker_wake_fallback_tests
    UnitTest/RenderWorkerWakeFallbackTest.cpp
    ${HARKEN_RUNTIME_DIR}/render/src/RenderWorkerPool.cpp
    ${HARKEN_RUNTIME_DIR}/core/src/AllocGuard.cpp
    ${HARKEN_RUNTIME_DIR}/core/src/CpuClock.cpp
)
target_link_libraries(harken_render_worker_wake_fallback_tests PRIVATE
    GTest::gtest_main)
target_include_directories(harken_render_worker_wake_fallback_tests PRIVATE
    ${HARKEN_RUNTIME_DIR}/core/include
    ${HARKEN_RUNTIME_DIR}/core/contract/include
    ${HARKEN_RUNTIME_DIR}/render/include
)
target_compile_definitions(harken_render_worker_wake_fallback_tests PRIVATE
    HARKEN_RENDER_WORKER_POOL_FORCE_WAKE_FALLBACK
    HARKEN_RENDER_WORKER_POOL_TEST_HOOKS
)
gtest_discover_tests(harken_render_worker_wake_fallback_tests
    TEST_PREFIX "RenderWorkerWakeFallback."
    PROPERTIES TIMEOUT 120)

# The rendezvous-deadline executable: the same separately compiled worker-pool
# object, with the test gates but WITHOUT the forced wake fallback, so the pool
# under test uses the platform's shipping wake strategy. The gate macro only adds
# friend declarations and a free-function namespace, so unlike the wake macro it
# leaves the RenderWorkerPool layout alone; the deadline and abandonment evidence
# is therefore taken on the configuration that ships. It does not link harken_core
# for the same reason the fallback target does not: this object is compiled with
# definitions the library's own copy was not.
add_executable(harken_render_worker_deadline_tests
    UnitTest/RenderWorkerDeadlineTest.cpp
    ${HARKEN_RUNTIME_DIR}/render/src/RenderWorkerPool.cpp
    ${HARKEN_RUNTIME_DIR}/core/src/AllocGuard.cpp
    ${HARKEN_RUNTIME_DIR}/core/src/CpuClock.cpp
)
target_link_libraries(harken_render_worker_deadline_tests PRIVATE
    GTest::gtest_main)
target_include_directories(harken_render_worker_deadline_tests PRIVATE
    ${HARKEN_RUNTIME_DIR}/core/include
    ${HARKEN_RUNTIME_DIR}/core/contract/include
    ${HARKEN_RUNTIME_DIR}/render/include
)
target_compile_definitions(harken_render_worker_deadline_tests PRIVATE
    HARKEN_RENDER_WORKER_POOL_TEST_HOOKS
)
gtest_discover_tests(harken_render_worker_deadline_tests
    TEST_PREFIX "RenderWorkerDeadlineTarget."
    PROPERTIES TIMEOUT 120)

# The real-time qualification probe checks, built in the configuration the daily
# channels and the gate already use.
#
# It cannot be a suite inside harken_runtime_tests. That executable links
# harken_core, whose RenderWorkerPool object is compiled with no probe control at
# all — the control adds a member, so it decides the class layout — and
# RtProbe.cpp is not part of the shipping library in the first place. The
# instrumentation under test would be compiled out of everything the suite links.
# Same reason the wake-fallback and deadline targets carry their own pool object.
# The source brings its own main() and reports through exit status, so it links
# no test framework.
#
# The control is target-private and the top-level HARKEN_RT_PROBE_* options stay
# off, so the configure-time check that refuses probe definitions outside a
# qualification build keeps doing its job: harken_core and harken_core_shared are
# still built without any probe control, and this executable links neither.
#
# The qualification presets are Visual Studio and MSVC only, so this is also the
# first time these sources are compiled by clang-cl.
add_executable(harken_rt_probe_worker_tests
    UnitTest/RtProbeTest.cpp
    ${HARKEN_RUNTIME_DIR}/core/src/RtProbe.cpp
    ${HARKEN_RUNTIME_DIR}/render/src/RenderWorkerPool.cpp
    ${HARKEN_RUNTIME_DIR}/core/src/AllocGuard.cpp
    ${HARKEN_RUNTIME_DIR}/core/src/CpuClock.cpp
)
target_include_directories(harken_rt_probe_worker_tests PRIVATE
    ${HARKEN_RUNTIME_DIR}/core/include
    ${HARKEN_RUNTIME_DIR}/core/contract/include
    ${HARKEN_RUNTIME_DIR}/render/include
)
target_compile_definitions(harken_rt_probe_worker_tests PRIVATE
    HARKEN_RT_PROBE_WORKER_BARRIER
)
add_test(NAME harken_rt_probe_worker
         COMMAND "$<TARGET_FILE:harken_rt_probe_worker_tests>")
set_tests_properties(harken_rt_probe_worker PROPERTIES TIMEOUT 120)

if(TARGET harken_core_shared)
    add_executable(harken_native_variable_sdk_smoke UnitTest/NativeVariableSdkSmoke.cpp)
    target_link_libraries(harken_native_variable_sdk_smoke PRIVATE harken_core_shared)
    add_test(NAME harken_native_variable_sdk_smoke COMMAND harken_native_variable_sdk_smoke)
endif()

if(WIN32 AND TARGET harken_core_shared)
    add_executable(harken_shared_load_smoke
        UnitTest/SharedLibraryLoadSmoke.cpp)
    add_dependencies(harken_shared_load_smoke harken_core_shared)
    add_test(NAME harken_shared_load_smoke
             COMMAND "$<TARGET_FILE:harken_shared_load_smoke>"
                     "$<TARGET_FILE:harken_core_shared>")
endif()

# Generated-artifact and export-surface contract checks. Each re-derives a
# checked-in product from its source of truth and fails on any drift: the
# built-in effect registry against the public effect ids, the C# binding and the
# remote wire layer against the wire table, the public ABI manifest against the
# declared contract, and the Windows export allowlist against the header plus the
# linked DLL. Uses the same discovered Python 3 interpreter as the generator
# custom targets; WORKING_DIRECTORY is the repo root.
#
# Every one of them is registered whether or not an interpreter was found.
# Registering them conditionally made a missing interpreter delete the checks
# rather than fail them, and ctest exits 0 for a selection matching nothing --
# with other tests still present the selection was not even empty, so losing just
# these stayed green. A placeholder that always fails keeps the gate red for the
# one reason that matters: the check did not run.
function(_harken_add_python_check_test test_name)
    if(Python3_Interpreter_FOUND)
        add_test(NAME ${test_name}
                 COMMAND "${Python3_EXECUTABLE}" ${ARGN}
                 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
        set_tests_properties(${test_name} PROPERTIES
            LABELS harken-source-contract TIMEOUT 120)
        return()
    endif()
    set(_stub "${CMAKE_CURRENT_BINARY_DIR}/python_check_unavailable_${test_name}.cmake")
    file(WRITE "${_stub}"
         "message(FATAL_ERROR
"
         "    \"${test_name} could not run: no Python 3 interpreter was found at \"
"
         "    \"configure time, so this contract check was never registered. The \"
"
         "    \"gate reports failure rather than skipping silently, because an \"
"
         "    \"unverified contract is not a verified one.\")
")
    add_test(NAME ${test_name}
             COMMAND "${CMAKE_COMMAND}" -P "${_stub}")
    set_tests_properties(${test_name} PROPERTIES
        LABELS harken-source-contract TIMEOUT 120)
endfunction()

_harken_add_python_check_test(effect_registry_manifest_check
    "${CMAKE_SOURCE_DIR}/HarkenEngine/Source/Tooling/Effects/gen_effect_registry.py"
    --check)
_harken_add_python_check_test(binding_generated_check
    "${CMAKE_SOURCE_DIR}/HarkenEngine/Source/Tooling/Binding/gen_binding.py"
    --check)
_harken_add_python_check_test(managed_generation_policy_check
    "${CMAKE_SOURCE_DIR}/HarkenEngine/Test/UnitTest/test_managed_generation_policy.py")
_harken_add_python_check_test(remote_wire_generated_check
    "${CMAKE_SOURCE_DIR}/HarkenEngine/Source/Tooling/Remote/wire/gen_wire.py"
    --check)
# The reference-slot vocabulary, whose compile-side and runtime-side definitions
# are emitted from one table. This check is what replaces the pin test that used
# to compare the two hand-written numberings: with both generated, comparing them
# to each other is a tautology, while comparing the checked-in headers to the
# table still catches a hand edit or a table change nobody regenerated.
_harken_add_python_check_test(reference_slots_generated_check
    "${CMAKE_SOURCE_DIR}/HarkenStudio/Source/Tooling/ReferenceSlots/slots/gen_reference_slots.py"
    --check)
_harken_add_python_check_test(playable_properties_generated_check
    "${CMAKE_SOURCE_DIR}/HarkenStudio/Source/Tooling/PlayableProperties/properties/gen_playable_properties.py"
    --check)
_harken_add_python_check_test(variable_catalog_generated_check
    "${CMAKE_SOURCE_DIR}/HarkenEngine/Source/Tooling/VariableCatalog/gen_variable_catalog.py"
    --check)
_harken_add_python_check_test(public_abi_manifest_check
    "${CMAKE_SOURCE_DIR}/HarkenEngine/Source/Tooling/Abi/gen_abi_manifest.py"
    --check)
_harken_add_python_check_test(monitor_record_fixture_check
    "${CMAKE_SOURCE_DIR}/HarkenEngine/Source/Runtime/api/monitor/fixtures/generate_record_fixtures.py"
    --check)
_harken_add_python_check_test(monitor_plan_fixture_check
    "${CMAKE_SOURCE_DIR}/HarkenEngine/Source/Runtime/api/monitor/fixtures/generate_plan_fixtures.py"
    --check)
_harken_add_python_check_test(monitor_session_fixture_check
    "${CMAKE_SOURCE_DIR}/HarkenEngine/Source/Runtime/api/monitor/fixtures/generate_session_fixtures.py"
    --check)
_harken_add_python_check_test(monitor_remote_fixture_check
    "${CMAKE_SOURCE_DIR}/HarkenEngine/Source/Runtime/api/monitor/fixtures/generate_remote_fixtures.py"
    --check)
_harken_add_python_check_test(monitor_appendix_a_check
    "${CMAKE_SOURCE_DIR}/HarkenEngine/Source/Runtime/api/monitor/fixtures/check_appendix_a.py"
    --check)
_harken_add_python_check_test(monitor_public_api_probe_includes_check
    "${CMAKE_SOURCE_DIR}/HarkenEngine/Source/Runtime/api/monitor/fixtures/check_monitor_public_api_probe_includes.py"
    --check)
_harken_add_python_check_test(public_abi_manifest_unit
    "${CMAKE_CURRENT_SOURCE_DIR}/UnitTest/test_runtime_abi_manifest.py")
_harken_add_python_check_test(public_abi_history_unit
    "${CMAKE_SOURCE_DIR}/.scripts/tests/test_runtime_repository_history.py")
_harken_add_python_check_test(monitor_record_fixture_unit
    "${CMAKE_SOURCE_DIR}/.scripts/tests/test_monitor_record_fixture_check.py")
_harken_add_python_check_test(monitor_remote_fixture_unit
    "${CMAKE_SOURCE_DIR}/.scripts/tests/test_monitor_remote_fixture_check.py")

# The export allowlist is checked in two independent steps. The first compares
# the checked-in .def against the entry points declared in Harken.h, so a new
# public function nobody allowlisted fails here instead of silently not being
# exported. The second re-reads the linked DLL's PE export directory and compares
# it to that same allowlist, which is what proves the build honored /DEF: -- a
# reintroduced WINDOWS_EXPORT_ALL_SYMBOLS would republish thousands of internal
# symbols while the first check still passed.
_harken_add_python_check_test(abi_export_allowlist_check
    "${CMAKE_SOURCE_DIR}/HarkenEngine/Source/Tooling/Abi/gen_abi_exports.py"
    --check)
# The parser behind that check, against declaration shapes it must recognize and
# shapes it must refuse. A shape it silently fails to model would omit an entry
# point from the DLL with every other check still green.
_harken_add_python_check_test(abi_export_parser_unit
    "${CMAKE_SOURCE_DIR}/.scripts/tests/test_abi_exports.py")
# The development preview surface's isolation from the gameplay face. The export
# allowlist and the C# binding are generated from the engine's own declarations
# and never read the preview header, so a preview symbol cannot reach either --
# structurally, with no filter to configure. This check is what makes that
# structure load-bearing: declaring a preview entry in the public header or naming
# one in the wire table fails here at once, before anything is regenerated, and it
# holds the preview export manifest in step with its header in both directions.
_harken_add_python_check_test(preview_surface_isolation_check
    "${CMAKE_SOURCE_DIR}/HarkenEngine/Source/Tooling/Preview/abi/check_preview_isolation.py")
# The preview allocation table against the header it documents. A status code the
# header allocates and the table omits is the drift that makes a table read as a
# specification for something the build does not do.
_harken_add_python_check_test(preview_allocation_table_check
    "${CMAKE_SOURCE_DIR}/HarkenEngine/Source/Tooling/Preview/abi/check_preview_allocation.py")
if(WIN32 AND TARGET harken_core_shared)
    _harken_add_python_check_test(abi_export_surface_check
        "${CMAKE_SOURCE_DIR}/HarkenEngine/Source/Tooling/Abi/gen_abi_exports.py"
        --verify-dll "$<TARGET_FILE:harken_core_shared>")
    set_tests_properties(abi_export_surface_check PROPERTIES LABELS harken-artifact-contract)
endif()
if(TARGET harken_host_selection_copy_test)
    add_test(NAME harken_host_selection_copy_test
             COMMAND "$<TARGET_FILE:harken_host_selection_copy_test>")
    set_tests_properties(harken_host_selection_copy_test PROPERTIES TIMEOUT 120)
endif()
harken_add_ci_test_target(runtime)
endif()

# Performance benchmarks live under Benchmark/, kept separate from the
# correctness suite (repository convention). This target is a plain executable
# with its own main() that prints a throughput table; it is deliberately NOT
# registered with ctest, so the correctness run never depends on timing. Run it
# by hand from the build tree (bin/<config>/harken_benchmarks).
if(HARKEN_BUILD_RUNTIME_BENCHMARKS)
add_executable(harken_benchmarks
    Benchmark/CurveUpdateBenchmark.cpp
    Benchmark/LimiterReleaseBenchmark.cpp
    Benchmark/RenderWorkerScalingBenchmark.cpp
)
target_link_libraries(harken_benchmarks PRIVATE harken_core harken_buildinfo)
# Same library/consumer macro agreement as harken_runtime_tests (the benchmark's
# own sources do not read taps, but the discipline is uniform).
if(HARKEN_ENABLE_STAGE_TAPS)
    target_compile_definitions(harken_benchmarks PRIVATE
        HARKEN_ENABLE_STAGE_TAPS)
endif()
# Drives the null backend pump directly, like the tests, so it needs the same
# module-internal headers (Engine.h and the runtime module includes).
target_include_directories(harken_benchmarks PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${HARKEN_RUNTIME_DIR}/api/src
    ${HARKEN_RUNTIME_DIR}/core/include
    ${HARKEN_RUNTIME_DIR}/core/contract/include
    ${HARKEN_RUNTIME_DIR}/dsp/include
    ${HARKEN_RUNTIME_DIR}/media/include
    ${HARKEN_RUNTIME_DIR}/param/include
    ${HARKEN_RUNTIME_DIR}/render/include
)

# Standalone resampler stretch characterization. It uses the same null-backend
# harness as harken_benchmarks but keeps its repetitions and report independent;
# it is deliberately not registered with ctest.
add_executable(harken_resample_benchmark
    Benchmark/ResampleStretchBenchmark.cpp
)
target_link_libraries(harken_resample_benchmark PRIVATE harken_core harken_buildinfo)
if(HARKEN_ENABLE_STAGE_TAPS)
    target_compile_definitions(harken_resample_benchmark PRIVATE
        HARKEN_ENABLE_STAGE_TAPS)
endif()
target_include_directories(harken_resample_benchmark PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${HARKEN_RUNTIME_DIR}/api/src
    ${HARKEN_RUNTIME_DIR}/core/include
    ${HARKEN_RUNTIME_DIR}/core/contract/include
    ${HARKEN_RUNTIME_DIR}/dsp/include
    ${HARKEN_RUNTIME_DIR}/media/include
    ${HARKEN_RUNTIME_DIR}/param/include
    ${HARKEN_RUNTIME_DIR}/render/include
)

# Standalone M11 capacity characterizer. It keeps capacity/resource evidence
# separate from the M10 resampler tool and is driven one scenario at a time by
# the bounded test runner; it is deliberately not registered with ctest.
add_executable(harken_runtime_capacity_benchmark
    Benchmark/RuntimeCapacityBenchmark.cpp
    UnitTest/AllocationProbe.cpp
)
target_link_libraries(harken_runtime_capacity_benchmark PRIVATE harken_core harken_buildinfo)
if(WIN32)
    target_link_libraries(harken_runtime_capacity_benchmark PRIVATE Psapi)
endif()
if(HARKEN_ENABLE_STAGE_TAPS)
    target_compile_definitions(harken_runtime_capacity_benchmark PRIVATE
        HARKEN_ENABLE_STAGE_TAPS)
endif()
target_include_directories(harken_runtime_capacity_benchmark PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${HARKEN_RUNTIME_DIR}/api/src
    ${HARKEN_RUNTIME_DIR}/core/include
    ${HARKEN_RUNTIME_DIR}/core/contract/include
    ${HARKEN_RUNTIME_DIR}/dsp/include
    ${HARKEN_RUNTIME_DIR}/media/include
    ${HARKEN_RUNTIME_DIR}/param/include
    ${HARKEN_RUNTIME_DIR}/render/include
)

add_executable(harken_pack_foundation_qualification
    Benchmark/PackFoundationQualification.cpp
)
target_link_libraries(harken_pack_foundation_qualification PRIVATE
    harken_core
    harken_buildinfo)
target_compile_definitions(harken_pack_foundation_qualification PRIVATE
    HARKEN_QUALIFICATION_CONFIGURATION="$<CONFIG>")
if(WIN32)
    target_link_libraries(harken_pack_foundation_qualification PRIVATE Psapi)
endif()
target_include_directories(harken_pack_foundation_qualification PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${HARKEN_RUNTIME_DIR}/api/src
    ${HARKEN_RUNTIME_DIR}/core/include
    ${HARKEN_RUNTIME_DIR}/core/contract/include
    ${HARKEN_RUNTIME_DIR}/dsp/include
    ${HARKEN_RUNTIME_DIR}/media/include
    ${HARKEN_RUNTIME_DIR}/param/include
    ${HARKEN_RUNTIME_DIR}/render/include
)

add_executable(harken_exported_pack_qualification
    Benchmark/ExportedPackQualification.cpp
)
target_link_libraries(harken_exported_pack_qualification PRIVATE
    harken_core
    harken_buildinfo)
target_compile_definitions(harken_exported_pack_qualification PRIVATE
    HARKEN_QUALIFICATION_CONFIGURATION="$<CONFIG>")
if(WIN32)
    target_link_libraries(harken_exported_pack_qualification PRIVATE Psapi)
endif()
target_include_directories(harken_exported_pack_qualification PRIVATE
    ${CMAKE_SOURCE_DIR}/HarkenEngine/Tool/Host
    ${CMAKE_SOURCE_DIR}/HarkenEngine/Source/Tooling/Common
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${HARKEN_RUNTIME_DIR}/api/src
    ${HARKEN_RUNTIME_DIR}/core/include
    ${HARKEN_RUNTIME_DIR}/core/contract/include
    ${HARKEN_RUNTIME_DIR}/dsp/include
    ${HARKEN_RUNTIME_DIR}/media/include
    ${HARKEN_RUNTIME_DIR}/param/include
    ${HARKEN_RUNTIME_DIR}/render/include
)

# Standalone denormal timing probe. It compares the caller thread's default
# floating-point mode with explicit FTZ+DAZ while pumping the same null-backend
# feedback scene. Timing is reported only, so it is not registered with ctest.
add_executable(harken_denormal_probe
    Benchmark/DenormalProbeBenchmark.cpp
)
target_link_libraries(harken_denormal_probe PRIVATE harken_core harken_buildinfo)
if(HARKEN_ENABLE_STAGE_TAPS)
    target_compile_definitions(harken_denormal_probe PRIVATE
        HARKEN_ENABLE_STAGE_TAPS)
endif()
target_include_directories(harken_denormal_probe PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${HARKEN_RUNTIME_DIR}/api/src
    ${HARKEN_RUNTIME_DIR}/core/include
    ${HARKEN_RUNTIME_DIR}/core/contract/include
    ${HARKEN_RUNTIME_DIR}/dsp/include
    ${HARKEN_RUNTIME_DIR}/media/include
    ${HARKEN_RUNTIME_DIR}/param/include
    ${HARKEN_RUNTIME_DIR}/render/include
)

# Release metering benchmark (spec section 11.7 / acceptance item 11).
# Deliberately carries no stage-tap define of its own: with the
# HARKEN_ENABLE_STAGE_TAPS option OFF (the default), a Release build of
# this target times exactly the always-on bus output peak scan a
# shipping Release build runs. (With the option ON the library also
# scans per-voice taps — a diagnostic build, not the section-11.7
# shape; take the acceptance numbers with the option OFF.) Not
# registered with ctest, like harken_benchmarks.
add_executable(harken_meter_benchmark
    Benchmark/MeterScanBenchmark.cpp
)
target_link_libraries(harken_meter_benchmark PRIVATE harken_core harken_buildinfo)
target_include_directories(harken_meter_benchmark PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${HARKEN_RUNTIME_DIR}/api/src
    ${HARKEN_RUNTIME_DIR}/core/include
    ${HARKEN_RUNTIME_DIR}/core/contract/include
    ${HARKEN_RUNTIME_DIR}/dsp/include
    ${HARKEN_RUNTIME_DIR}/media/include
    ${HARKEN_RUNTIME_DIR}/param/include
    ${HARKEN_RUNTIME_DIR}/render/include
)
endif()
