find_package(Qt6 REQUIRED COMPONENTS Widgets Network Test)
add_executable(harken_hub_studio_fixture UnitTest/Support/StudioProcessFixture.cpp)
target_link_libraries(harken_hub_studio_fixture PRIVATE Qt6::Core Qt6::Network)
add_executable(harken_hub_tests
    "${CMAKE_SOURCE_DIR}/HarkenHub/resources.qrc"
    UnitTest/TestMain.cpp
    UnitTest/Support/ManifestTestUtils.h
    UnitTest/Support/RangeHttpServer.h
    UnitTest/Support/RangeHttpServer.cpp
    UnitTest/AclHardeningTest.cpp
    UnitTest/BackoffPolicyTest.cpp
    UnitTest/ConsentStoreTest.cpp
    UnitTest/ConsentDialogTest.cpp
    UnitTest/DownloaderTest.cpp
    UnitTest/FileLogTest.cpp
    UnitTest/HandoffAreaTest.cpp
    UnitTest/InstallExecutorTest.cpp
    UnitTest/InstallIndexTest.cpp
    UnitTest/InstallRequestTest.cpp
    UnitTest/IntegrationStoreTest.cpp
    UnitTest/EngineIntegrationsPageTest.cpp
    UnitTest/IntegrityAndRepairTest.cpp
    UnitTest/LaunchDiagnosticsTest.cpp
    UnitTest/LocalBuildRegistryTest.cpp
    UnitTest/LaunchPreflightTest.cpp
    UnitTest/ProjectOpenAnalyzerTest.cpp
    UnitTest/ProjectOpenFlowTest.cpp
    UnitTest/ProjectCreateFlowTest.cpp
    UnitTest/ProjectsPageTest.cpp
    UnitTest/StudioPackageIntegrationTest.cpp
    UnitTest/LogTailerTest.cpp
    UnitTest/HubSelfUpdateTest.cpp
    UnitTest/HubLocalizationTest.cpp
    UnitTest/HubSettingsTest.cpp
    UnitTest/MaintenanceTest.cpp
    UnitTest/ManifestVerifierTest.cpp
    UnitTest/PackageReaderTest.cpp
    UnitTest/PackageVerifierTest.cpp
    UnitTest/SelfUpdateHealthCheckTest.cpp
    UnitTest/ServerEndpointsTest.cpp
    UnitTest/ServerVersionCatalogTest.cpp
    UnitTest/StudioLaunchMonitorTest.cpp
    UnitTest/StudioLauncherTest.cpp
    UnitTest/TransactionReclaimerTest.cpp
    UnitTest/TrustAnchorsTest.cpp
    UnitTest/UnityIntegrationServiceTest.cpp
    UnitTest/UninstallExecutorTest.cpp
    UnitTest/UpdateBridgeConsumerTest.cpp
    UnitTest/UpdateCheckerTest.cpp
    UnitTest/UpdateCoordinatorTest.cpp
    UnitTest/UpdateRecoveryTest.cpp
    UnitTest/UpdateNotifierTest.cpp
    UnitTest/UpdateQueueTest.cpp
    UnitTest/UpdateStateStoreTest.cpp
    UnitTest/BuildPinRegistryTest.cpp
    UnitTest/ProjectStoreTest.cpp
    UnitTest/ProjectStoreLaunchFieldsTest.cpp
    UnitTest/ProjectProbeTest.cpp
    UnitTest/ProjectIntakeConsumerTest.cpp
    UnitTest/ProjectIntegrationExecutorTest.cpp
    UnitTest/ProjectTemplateCatalogTest.cpp
    UnitTest/ReleaseNotesCatalogTest.cpp
    UnitTest/SingleInstanceGuardTest.cpp
    UnitTest/VersionScannerTest.cpp
    UnitTest/VersionsPageWatchTest.cpp
    UnitTest/StudioLaunchTargetTest.cpp
)
set_target_properties(harken_hub_tests PROPERTIES AUTORCC ON)
harken_add_hub_legal_resources(harken_hub_tests)
if(WIN32)
    target_sources(harken_hub_tests PRIVATE
        UnitTest/UpdateBridgeDeploymentTest.cpp)
endif()
target_link_libraries(harken_hub_tests PRIVATE
    harken_hub_test_lib
    MINIZIP::minizip
    harken_ed25519
    GTest::gtest
    Qt6::Test
)
add_dependencies(harken_hub_tests harken_hub_lrelease)
add_dependencies(harken_hub_tests harken_hub_studio_fixture)
if(WIN32)
    add_custom_command(TARGET harken_hub_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:Qt6::Test>" "$<TARGET_FILE_DIR:harken_hub_tests>")
endif()
add_custom_command(TARGET harken_hub_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:harken_hub_tests>/platforms"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:Qt6::QOffscreenIntegrationPlugin>"
        "$<TARGET_FILE_DIR:harken_hub_tests>/platforms/")
target_compile_definitions(harken_hub_tests PRIVATE
    HARKEN_HUB_STUDIO_FIXTURE="$<TARGET_FILE:harken_hub_studio_fixture>"
    HARKEN_HUB_TRANSLATION_FILE="${CMAKE_BINARY_DIR}/HarkenHub/harken_hub_zh_CN.qm"
    HARKEN_HUB_RELEASE_NOTES_FILE="${CMAKE_SOURCE_DIR}/HarkenHub/Resources/release-notes.json")
gtest_discover_tests(harken_hub_tests DISCOVERY_MODE PRE_TEST)

add_executable(harken_hub_integration_guidance_tests UnitTest/IntegrationGuidanceTest.cpp)
set_target_properties(harken_hub_integration_guidance_tests PROPERTIES AUTOMOC ON)
target_link_libraries(harken_hub_integration_guidance_tests PRIVATE harken_hub_test_lib Qt6::Test)
target_compile_definitions(harken_hub_integration_guidance_tests PRIVATE
    HARKEN_HUB_TRANSLATION_FILE="${CMAKE_BINARY_DIR}/HarkenHub/harken_hub_zh_CN.qm")
add_dependencies(harken_hub_integration_guidance_tests harken_hub_lrelease)
add_test(NAME HubIntegrationGuidance COMMAND harken_hub_integration_guidance_tests)
set_tests_properties(HubIntegrationGuidance PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_FORCE_STDERR_LOGGING=1")

harken_add_ci_test_target(hub)
