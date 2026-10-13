#include <harken/Harken.h>

#include "DeviceBackend.h"
#include "Engine.h"
#include "harken/core/EngineConfig.h"
#if defined(HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE)
#include "TimelineCapacityOverrideForTest.h"
#endif

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <thread>

namespace {

VnConfig MakeConfig(std::uint32_t deviceMode, std::uint32_t sampleRate);

#if defined(HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE)
TEST(TimelineCapacityOverride, ResolvesBothPassesAndRestoresDefaultAfterInit) {
    (void)Vn_Term();
    const auto config = MakeConfig(VN_DEVICE_NULL, 48000);
    {
        harken::detail::ScopedTimelineCapacityOverrideForTest scope(4, 2, 2);
        ASSERT_TRUE(scope.Valid());
        harken::EngineConfig first{};
        harken::EngineConfig second{};
        ASSERT_EQ(VN_OK, harken::ResolveConfig(&config, &first));
        ASSERT_EQ(VN_OK, harken::ResolveConfigForInitialDevice(
            &config, 0, 0, &second));
        for (const auto* resolved : {&first, &second}) {
            EXPECT_EQ(4u, resolved->maxEventsPerQuantum);
            EXPECT_EQ(2u, resolved->maxPlan03GroupHeadersPerPage);
            EXPECT_EQ(2u, resolved->maxFrameBatchesPerQuantum);
        }
        ASSERT_EQ(VN_OK, Vn_Init(&config));
        auto* engine = harken::detail::ActiveEngine();
        ASSERT_NE(nullptr, engine);
        EXPECT_EQ(4u, engine->config.maxEventsPerQuantum);
        EXPECT_EQ(2u, engine->config.maxPlan03GroupHeadersPerPage);
        EXPECT_EQ(2u, engine->config.maxFrameBatchesPerQuantum);
        EXPECT_EQ(4u, engine->timelinePool.MaxEventsPerQuantum());
        EXPECT_EQ(2u, engine->timelinePool.MaxPlan03GroupHeadersPerPage());
        EXPECT_EQ(4u, engine->immediateActionLane.MaxEventsPerQuantum());
        EXPECT_EQ(8u, engine->timelineExecutor.DecodedEventCapacityForTest());
        EXPECT_EQ(2u, engine->timelineExecutor.PageGroupCapacityForTest());
        const auto sinkCapacity = engine->timelineSink.CaptureCapacityForTest();
        EXPECT_EQ(2u, sinkCapacity.pageGroups);
        EXPECT_EQ(8u, sinkCapacity.frontierEvents);
        EXPECT_EQ(4u, sinkCapacity.frontierGroups);
        EXPECT_EQ(4u, sinkCapacity.laneEvents);
        EXPECT_EQ(2u, sinkCapacity.laneGroups);
        EXPECT_EQ(VN_OK, Vn_Term());
    }
    ASSERT_EQ(VN_OK, Vn_Init(&config));
    auto* restored = harken::detail::ActiveEngine();
    ASSERT_NE(nullptr, restored);
    EXPECT_EQ(256u, restored->config.maxEventsPerQuantum);
    EXPECT_EQ(64u, restored->config.maxPlan03GroupHeadersPerPage);
    EXPECT_EQ(128u, restored->config.maxFrameBatchesPerQuantum);
    EXPECT_EQ(256u, restored->timelinePool.MaxEventsPerQuantum());
    EXPECT_EQ(64u, restored->timelinePool.MaxPlan03GroupHeadersPerPage());
    EXPECT_EQ(256u, restored->immediateActionLane.MaxEventsPerQuantum());
    EXPECT_EQ(512u, restored->timelineExecutor.DecodedEventCapacityForTest());
    EXPECT_EQ(64u, restored->timelineExecutor.PageGroupCapacityForTest());
    const auto restoredSink = restored->timelineSink.CaptureCapacityForTest();
    EXPECT_EQ(64u, restoredSink.pageGroups);
    EXPECT_EQ(512u, restoredSink.frontierEvents);
    EXPECT_EQ(128u, restoredSink.frontierGroups);
    EXPECT_EQ(256u, restoredSink.laneEvents);
    EXPECT_EQ(64u, restoredSink.laneGroups);
    EXPECT_EQ(VN_OK, Vn_Term());
}

TEST(TimelineCapacityOverride, InvalidCapacityNeverConstructsAnEngine) {
    (void)Vn_Term();
    const auto config = MakeConfig(VN_DEVICE_NULL, 48000);
    constexpr std::array<std::array<std::uint32_t, 3>, 8> invalid{{
        {{0, 2, 2}}, {{257, 2, 2}}, {{4, 0, 2}},
        {{4, 65, 2}}, {{4, 1025, 2}}, {{4, 2, 0}},
        {{4, 2, 129}}, {{2, 2, 3}},
    }};
    for (const auto& values : invalid) {
        SCOPED_TRACE(::testing::Message() << values[0] << '/' << values[1]
            << '/' << values[2]);
        {
            harken::detail::ScopedTimelineCapacityOverrideForTest scope(
                values[0], values[1], values[2]);
            ASSERT_TRUE(scope.Valid());
            harken::EngineConfig resolved{};
            EXPECT_EQ(VN_INVALID_CONFIG, harken::ResolveConfig(&config, &resolved));
            EXPECT_EQ(VN_INVALID_CONFIG, Vn_Init(&config));
            EXPECT_EQ(nullptr, harken::detail::ActiveEngine());
        }
        harken::EngineConfig restored{};
        ASSERT_EQ(VN_OK, harken::ResolveConfig(&config, &restored));
        EXPECT_EQ(256u, restored.maxEventsPerQuantum);
        EXPECT_EQ(64u, restored.maxPlan03GroupHeadersPerPage);
        EXPECT_EQ(128u, restored.maxFrameBatchesPerQuantum);
    }
}

TEST(TimelineCapacityOverride, RejectsOverlapRealDeviceAndForeignThread) {
    (void)Vn_Term();
    const auto nullConfig = MakeConfig(VN_DEVICE_NULL, 48000);
    const auto realConfig = MakeConfig(VN_DEVICE_REAL, 48000);
    {
        harken::detail::ScopedTimelineCapacityOverrideForTest scope(4, 2, 2);
        ASSERT_TRUE(scope.Valid());
        harken::detail::ScopedTimelineCapacityOverrideForTest overlapping(1, 1, 1);
        EXPECT_FALSE(overlapping.Valid());
        EXPECT_EQ(VN_INVALID_CONFIG, Vn_Init(&realConfig));
        EXPECT_EQ(nullptr, harken::detail::ActiveEngine());
        VnResult foreignResolve = VN_OK;
        VnResult foreignInit = VN_OK;
        std::thread foreign([&] {
            harken::EngineConfig resolved{};
            foreignResolve = harken::ResolveConfig(&nullConfig, &resolved);
            foreignInit = Vn_Init(&nullConfig);
        });
        foreign.join();
        EXPECT_EQ(VN_INVALID_CONFIG, foreignResolve);
        EXPECT_EQ(VN_INVALID_CONFIG, foreignInit);
        harken::EngineConfig ownerResolved{};
        ASSERT_EQ(VN_OK, harken::ResolveConfig(&nullConfig, &ownerResolved));
        EXPECT_EQ(4u, ownerResolved.maxEventsPerQuantum);
        ASSERT_EQ(VN_OK, Vn_Init(&nullConfig));
        harken::detail::ScopedTimelineCapacityOverrideForTest whileInitialized(1, 1, 1);
        EXPECT_FALSE(whileInitialized.Valid());
        EXPECT_EQ(VN_OK, Vn_Term());
    }
    const auto earlyReturn = [] {
        harken::detail::ScopedTimelineCapacityOverrideForTest scope(4, 2, 2);
        return scope.Valid();
    };
    ASSERT_TRUE(earlyReturn());
    ASSERT_EQ(VN_OK, Vn_Init(&nullConfig));
    EXPECT_EQ(256u, harken::detail::ActiveEngine()->config.maxEventsPerQuantum);
    EXPECT_EQ(VN_OK, Vn_Term());
}
#endif

VnConfig MakeConfig(std::uint32_t deviceMode, std::uint32_t sampleRate) {
    VnConfig config{};
    config.structSize = sizeof(VnConfig);
    config.deviceMode = deviceMode;
    config.sampleRate = sampleRate;
    config.deviceChannelLayout = VN_CHANNEL_LAYOUT_STEREO;
    return config;
}

class MixRateDeviceTest : public ::testing::Test {
protected:
    void SetUp() override {
        harken::SetDeviceBackendTestState(&deviceState);
    }

    void TearDown() override {
        if (harken::detail::ActiveEngine() != nullptr) {
            (void)Vn_Term();
        }
        harken::SetDeviceBackendTestState(nullptr);
    }

    harken::DeviceBackendTestState deviceState{};
};

TEST(MixRateConfig, ProductionDefaultAndSupportedOptInRates) {
    VnConfig config = MakeConfig(VN_DEVICE_NULL, 0);
    harken::EngineConfig resolved;
    ASSERT_EQ(VN_OK, harken::ResolveConfig(&config, &resolved));
    EXPECT_EQ(48000u, resolved.mixRate.SampleRate());
    EXPECT_EQ(harken::MixRateResolution::ProductionDefault,
              resolved.mixRate.Resolution());

    constexpr std::array<std::uint32_t, 3> kExplicitRates{
        44100u, 48000u, 96000u};
    for (const std::uint32_t rate : kExplicitRates) {
        config.sampleRate = rate;
        ASSERT_EQ(VN_OK, harken::ResolveConfig(&config, &resolved));
        EXPECT_EQ(rate, resolved.mixRate.SampleRate());
        EXPECT_EQ(harken::MixRateResolution::ExplicitRequest,
                  resolved.mixRate.Resolution());
    }
}

TEST(MixRateConfig,
     InitialDeviceRateRerunsDerivedBudgetsWithoutPollutingOutput) {
    VnConfig config = MakeConfig(VN_DEVICE_REAL, 0);
    config.delayEffectPoolCapacity = 64;

    harken::EngineConfig fallback;
    ASSERT_EQ(VN_OK, harken::ResolveConfig(&config, &fallback));
    ASSERT_EQ(48000u, fallback.mixRate.SampleRate());

    VnConfig sentinelConfig = MakeConfig(VN_DEVICE_NULL, 44100);
    harken::EngineConfig output;
    ASSERT_EQ(VN_OK, harken::ResolveConfig(&sentinelConfig, &output));
    output.topologyPlanPoolBytes = 987654321u;

    EXPECT_EQ(VN_INVALID_CONFIG,
              harken::ResolveConfigForInitialDevice(
                  &config, 2, 192000, &output));
    EXPECT_EQ(44100u, output.mixRate.SampleRate());
    EXPECT_EQ(harken::MixRateResolution::ExplicitRequest,
              output.mixRate.Resolution());
    EXPECT_EQ(987654321u, output.topologyPlanPoolBytes);
}

TEST(MixRateConfig,
     InitialDeviceRateRerunsHardLimitWithoutPollutingOutput) {
    const VnConfig config = MakeConfig(VN_DEVICE_REAL, 0);
    VnConfig sentinelConfig = MakeConfig(VN_DEVICE_NULL, 44100);
    harken::EngineConfig output;
    ASSERT_EQ(VN_OK, harken::ResolveConfig(&sentinelConfig, &output));
    output.topologyPlanPoolBytes = 987654321u;

    ASSERT_EQ(VN_OK, harken::ResolveConfigForInitialDevice(
                         &config, 2, 384000, &output));
    EXPECT_EQ(384000u, output.mixRate.SampleRate());

    ASSERT_EQ(VN_OK, harken::ResolveConfig(&sentinelConfig, &output));
    output.topologyPlanPoolBytes = 987654321u;
    EXPECT_EQ(VN_INVALID_CONFIG,
              harken::ResolveConfigForInitialDevice(
                  &config, 2, 384001, &output));
    EXPECT_EQ(44100u, output.mixRate.SampleRate());
    EXPECT_EQ(harken::MixRateResolution::ExplicitRequest,
              output.mixRate.Resolution());
    EXPECT_EQ(987654321u, output.topologyPlanPoolBytes);
}

TEST_F(MixRateDeviceTest,
       ConcreteInitialObservationAndUnknownFallbackPreserveLegacyBehavior) {
    VnConfig config = MakeConfig(VN_DEVICE_REAL, 0);

    deviceState.querySucceeds = true;
    deviceState.querySampleRate = 96000;
    deviceState.openedClientRate = 96000;
    deviceState.backendInternalRate = 96000;
    ASSERT_EQ(VN_OK, Vn_Init(&config));
    harken::Engine* engine = harken::detail::ActiveEngine();
    ASSERT_NE(nullptr, engine);
    EXPECT_EQ(96000u, engine->config.mixRate.SampleRate());
    EXPECT_EQ(harken::MixRateResolution::InitialDeviceObservation,
              engine->config.mixRate.Resolution());
    EXPECT_EQ(VN_OK, Vn_Term());

    deviceState.querySampleRate = 0;
    deviceState.openedClientRate = 0;
    deviceState.backendInternalRate = 0;
    ASSERT_EQ(VN_OK, Vn_Init(&config));
    engine = harken::detail::ActiveEngine();
    ASSERT_NE(nullptr, engine);
    EXPECT_EQ(48000u, engine->config.mixRate.SampleRate());
    EXPECT_EQ(harken::MixRateResolution::InitialDeviceFallback,
              engine->config.mixRate.Resolution());
    const harken::DeviceRateObservation nonConcrete =
        engine->device.RateObservation();
    EXPECT_EQ(harken::kDeviceRateRequestedKnown,
              nonConcrete.knownFields);
    EXPECT_EQ(VN_OK, Vn_Term());

    deviceState.querySucceeds = false;
    ASSERT_EQ(VN_OK, Vn_Init(&config));
    engine = harken::detail::ActiveEngine();
    ASSERT_NE(nullptr, engine);
    EXPECT_EQ(48000u, engine->config.mixRate.SampleRate());
    EXPECT_EQ(harken::MixRateResolution::InitialDeviceFallback,
              engine->config.mixRate.Resolution());
    EXPECT_EQ(VN_OK, Vn_Term());
    EXPECT_EQ(3u, deviceState.queryCount);
}

TEST_F(MixRateDeviceTest,
       OldestConfigVintageUsesInitialDeviceRateWithoutReadingPoisonedTail) {
    constexpr std::uint32_t kOldestConfigSize =
        static_cast<std::uint32_t>(offsetof(VnConfig, onNotification));
    VnConfig config;
    std::memset(&config, 0xA5, sizeof(config));
    std::memset(&config, 0, kOldestConfigSize);
    config.structSize = kOldestConfigSize;
    config.deviceMode = VN_DEVICE_REAL;

    deviceState.querySucceeds = true;
    deviceState.queryChannels = 2;
    deviceState.querySampleRate = 44100;
    deviceState.openedClientRate = 44100;
    deviceState.backendInternalRate = 44100;

    ASSERT_EQ(VN_OK, Vn_Init(&config));
    harken::Engine* const engine = harken::detail::ActiveEngine();
    ASSERT_NE(nullptr, engine);
    EXPECT_EQ(44100u, engine->config.mixRate.SampleRate());
    EXPECT_EQ(harken::MixRateResolution::InitialDeviceObservation,
              engine->config.mixRate.Resolution());
    EXPECT_EQ(kOldestConfigSize,
              engine->traceOrigin.requested.structSize);
    EXPECT_EQ(44100u, engine->traceOrigin.resolvedSampleRate);
    EXPECT_EQ(1u, deviceState.queryCount);
    EXPECT_EQ(1u, deviceState.createCount);
}

TEST_F(MixRateDeviceTest,
       InvalidObservedRateBudgetFailsBeforePublishingOrOpeningDevice) {
    deviceState.querySucceeds = true;
    deviceState.querySampleRate = 192000;
    VnConfig config = MakeConfig(VN_DEVICE_REAL, 0);
    config.delayEffectPoolCapacity = 64;

    EXPECT_EQ(VN_INVALID_CONFIG, Vn_Init(&config));
    EXPECT_EQ(nullptr, harken::detail::ActiveEngine());
    EXPECT_EQ(0u, deviceState.createCount);
    EXPECT_EQ(1u, deviceState.queryCount);
}

TEST_F(MixRateDeviceTest,
       ExplicitMixRateIgnores192kEndpointAndRemainsFixedAcrossResume) {
    deviceState.querySucceeds = true;
    deviceState.querySampleRate = 192000;
    deviceState.openedClientRate = 48000;
    deviceState.backendInternalRate = 192000;
    const VnConfig config = MakeConfig(VN_DEVICE_REAL, 48000);

    ASSERT_EQ(VN_OK, Vn_Init(&config));
    harken::Engine* const engine = harken::detail::ActiveEngine();
    ASSERT_NE(nullptr, engine);
    EXPECT_EQ(48000u, engine->config.mixRate.SampleRate());
    EXPECT_EQ(harken::MixRateResolution::ExplicitRequest,
              engine->config.mixRate.Resolution());

    harken::DeviceRateObservation observation =
        engine->device.RateObservation();
    EXPECT_EQ(48000u, observation.requestedDeviceRate);
    EXPECT_EQ(48000u, observation.openedClientRate);
    EXPECT_EQ(192000u, observation.backendInternalRate);
    EXPECT_EQ(192000u, observation.queriedDefaultEndpointRate);
    EXPECT_EQ(harken::kDeviceRateRequestedKnown |
                  harken::kDeviceRateOpenedClientKnown |
                  harken::kDeviceRateBackendInternalKnown |
                  harken::kDeviceRateQueriedDefaultEndpointKnown,
              observation.knownFields);

    ASSERT_EQ(VN_OK, Vn_Suspend());
    deviceState.querySampleRate = 96000;
    deviceState.openedClientRate = 44100;
    deviceState.backendInternalRate = 96000;
    ASSERT_EQ(VN_OK, Vn_Resume());

    EXPECT_EQ(48000u, engine->config.mixRate.SampleRate());
    observation = engine->device.RateObservation();
    EXPECT_EQ(48000u, observation.requestedDeviceRate);
    EXPECT_EQ(44100u, observation.openedClientRate);
    EXPECT_EQ(96000u, observation.backendInternalRate);
    EXPECT_EQ(96000u, observation.queriedDefaultEndpointRate);
    EXPECT_EQ(2u, deviceState.queryCount);
}

TEST_F(MixRateDeviceTest,
       InitialNativeMixRateIsNotReResolvedOnFailedOrSuccessfulResume) {
    deviceState.querySucceeds = true;
    deviceState.querySampleRate = 44100;
    deviceState.openedClientRate = 44100;
    deviceState.backendInternalRate = 48000;
    const VnConfig config = MakeConfig(VN_DEVICE_REAL, 0);

    ASSERT_EQ(VN_OK, Vn_Init(&config));
    harken::Engine* const engine = harken::detail::ActiveEngine();
    ASSERT_NE(nullptr, engine);
    ASSERT_EQ(44100u, engine->config.mixRate.SampleRate());
    ASSERT_EQ(VN_OK, Vn_Suspend());

    deviceState.querySampleRate = 192000;
    deviceState.openedClientRate = 0;
    deviceState.backendInternalRate = 0;
    deviceState.failCreateCount = 1;
    EXPECT_EQ(VN_DEVICE_ERROR, Vn_Resume());
    EXPECT_EQ(44100u, engine->config.mixRate.SampleRate());
    harken::DeviceRateObservation observation =
        engine->device.RateObservation();
    EXPECT_EQ(44100u, observation.requestedDeviceRate);
    EXPECT_EQ(192000u, observation.queriedDefaultEndpointRate);
    EXPECT_EQ(harken::kDeviceRateRequestedKnown |
                  harken::kDeviceRateQueriedDefaultEndpointKnown,
              observation.knownFields);

    deviceState.querySampleRate = 96000;
    deviceState.openedClientRate = 48000;
    deviceState.backendInternalRate = 96000;
    ASSERT_EQ(VN_OK, Vn_Resume());
    EXPECT_EQ(44100u, engine->config.mixRate.SampleRate());
    observation = engine->device.RateObservation();
    EXPECT_EQ(44100u, observation.requestedDeviceRate);
    EXPECT_EQ(48000u, observation.openedClientRate);
    EXPECT_EQ(96000u, observation.backendInternalRate);
    EXPECT_EQ(96000u, observation.queriedDefaultEndpointRate);
    EXPECT_EQ(3u, deviceState.queryCount);
}

} // namespace
