#include <harken/Harken.h>
#include "UnitTest/StudioObjectIdTestSupport.h"
#include "Monitor/MonitorAvailability.h"

#include <gtest/gtest.h>

#include <fstream>
#include <iterator>
#include <memory>
#include <string>

using VNX::Monitor::MonitorAvailabilityInput;
using VNX::Monitor::MonitorAvailabilityLayer;
using VNX::Monitor::MonitorAvailabilityReason;
using VNX::Monitor::MonitorAvailabilityState;
using VNX::Monitor::MonitorDropProjection;
using VNX::Monitor::MonitorDropProjectionState;
using VNX::Monitor::MonitorDropReasonKind;
using VNX::Monitor::MonitorSubscriptionProjection;
using VNX::Monitor::MonitorSubscriptionProjectionState;
using VNX::Monitor::availabilityLayer;
using VNX::Monitor::projectAvailability;

namespace {

class MonitorAvailability : public ::testing::Test
{
protected:
    void SetUp() override
    {
        snapshot = std::make_shared<VNX::Monitor::MonitorStoreSnapshot>();

        capabilities.lufsMomentarySupported = 1;
        capabilities.lufsShortTermSupported = 1;
        capabilities.lufsIntegratedSupported = 1;

        capability.kind = VN_MONITOR_MEASURE_RMS;
        capability.supported = 1;
        capability.algorithmVersion = 3;
        capability.unit = 17;

        tap.tapDescriptorId = 44;

        measurement.kind = VN_MONITOR_MEASURE_RMS;
        measurement.tapDescriptorId = tap.tapDescriptorId;

        plan.accepted.tapCount = 1;
        plan.accepted.taps = &tap;
        plan.accepted.measurementCount = 1;
        plan.accepted.measurements = &measurement;

        subscription.state = MonitorSubscriptionProjectionState::Connected;
        subscription.status.acceptedPermissionMask =
            VN_MONITOR_PERM_OBSERVE_MEASUREMENTS;
        subscription.status.monitorSessionEpoch = 7;

        input.snapshot = snapshot;
        input.subscription = &subscription;
        input.capabilities = &capabilities;
        input.measurementCapabilities = &capability;
        input.measurementCapabilityCount = 1;
        input.effectivePlan = &plan;
        input.observation.kind = VN_MONITOR_MEASURE_RMS;
        input.observation.tapDescriptorId = tap.tapDescriptorId;
        input.observation.sessionEpoch = subscription.status.monitorSessionEpoch;
        input.observation.validityFlags = VN_MONITOR_VALID_ARMED |
                                          VN_MONITOR_VALID_NUMERIC;
    }

    VNX::Monitor::MonitorCapabilities capabilities {};
    VNX::Monitor::MonitorMeasurementCapability capability {};
    VNX::Monitor::MonitorTapDescriptor tap {};
    VNX::Monitor::MonitorMeasurementRequest measurement {};
    VNX::Monitor::MonitorEffectiveCapturePlan plan {};
    MonitorSubscriptionProjection subscription {};
    std::shared_ptr<VNX::Monitor::MonitorStoreSnapshot> snapshot;
    MonitorAvailabilityInput input {};
};

} // namespace

TEST_F(MonitorAvailability, ValidZeroAndNonZeroKeepEveryLayer)
{
    auto out = projectAvailability(input);
    EXPECT_EQ(out.displayPriority, MonitorAvailabilityLayer::Value);
    EXPECT_EQ(availabilityLayer(out, MonitorAvailabilityLayer::Capability).state,
              MonitorAvailabilityState::Available);
    EXPECT_EQ(availabilityLayer(out, MonitorAvailabilityLayer::Subscription).state,
              MonitorAvailabilityState::Available);
    EXPECT_EQ(availabilityLayer(out, MonitorAvailabilityLayer::Plan).state,
              MonitorAvailabilityState::Available);
    EXPECT_EQ(availabilityLayer(out, MonitorAvailabilityLayer::Drop).state,
              MonitorAvailabilityState::NotEvaluated);
    EXPECT_EQ(availabilityLayer(out, MonitorAvailabilityLayer::Validity).state,
              MonitorAvailabilityState::Available);
    EXPECT_EQ(availabilityLayer(out, MonitorAvailabilityLayer::Value).state,
              MonitorAvailabilityState::ValidZero);
    EXPECT_EQ(out.metadata.unit, capability.unit);
    EXPECT_EQ(out.metadata.algorithmVersion, capability.algorithmVersion);

    input.observation.numericValue = 1.0f;
    out = projectAvailability(input);
    EXPECT_EQ(availabilityLayer(out, MonitorAvailabilityLayer::Value).state,
              MonitorAvailabilityState::ValidNonZero);
}
TEST_F(MonitorAvailability, UnauthorizedIsNotDropGap)
{
    subscription.status.acceptedPermissionMask = 0;
    MonitorDropProjection drops;
    drops.reasons.push_back({MonitorDropReasonKind::UnauthorizedNotGap});
    input.dropState = MonitorDropProjectionState::Connected;
    input.dropProjection = &drops;

    const auto out = projectAvailability(input);
    EXPECT_EQ(out.displayPriority, MonitorAvailabilityLayer::Subscription);
    EXPECT_EQ(availabilityLayer(out, MonitorAvailabilityLayer::Subscription).state,
              MonitorAvailabilityState::Unauthorized);
    EXPECT_EQ(availabilityLayer(out, MonitorAvailabilityLayer::Drop).state,
              MonitorAvailabilityState::NotEvaluated);
    EXPECT_EQ(availabilityLayer(out, MonitorAvailabilityLayer::Drop).reason,
              MonitorAvailabilityReason::UnauthorizedNotGap);
}

TEST_F(MonitorAvailability, ConnectedWithoutDropProjectionIsContextUnavailable)
{
    input.dropState = MonitorDropProjectionState::Connected;
    input.dropProjection = nullptr;

    const auto out = projectAvailability(input);
    for (const auto& layer : out.layers)
    {
        EXPECT_EQ(layer.state, MonitorAvailabilityState::ContextUnavailable);
        EXPECT_EQ(layer.reason, MonitorAvailabilityReason::MissingDropProjection);
    }
    EXPECT_EQ(out.displayPriority, MonitorAvailabilityLayer::Capability);
    EXPECT_NE(availabilityLayer(out, MonitorAvailabilityLayer::Drop).state,
              MonitorAvailabilityState::ValidZero);
    EXPECT_NE(availabilityLayer(out, MonitorAvailabilityLayer::Drop).reason,
              MonitorAvailabilityReason::DropNone);
}

TEST_F(MonitorAvailability, ConnectedDropGapWinsAfterPlan)
{
    MonitorDropProjection drops;
    drops.reasons.push_back({MonitorDropReasonKind::StageGap});
    drops.reasons.push_back({MonitorDropReasonKind::UnauthorizedNotGap});
    input.dropState = MonitorDropProjectionState::Connected;
    input.dropProjection = &drops;

    const auto out = projectAvailability(input);
    EXPECT_EQ(out.displayPriority, MonitorAvailabilityLayer::Drop);
    EXPECT_EQ(availabilityLayer(out, MonitorAvailabilityLayer::Drop).state,
              MonitorAvailabilityState::DropGap);
    EXPECT_EQ(out.metadata.ignoredDropReasonCount, 1u);
}

TEST_F(MonitorAvailability, EveryConnectedDropReasonExceptUnauthorizedIsGap)
{
    const MonitorDropReasonKind kinds[] = {
        MonitorDropReasonKind::BaselineMissing,
        MonitorDropReasonKind::MalformedDrop,
        MonitorDropReasonKind::StoreLocalSaturation,
        MonitorDropReasonKind::DerivedInput,
    };
    for (const auto kind : kinds)
    {
        MonitorDropProjection drops;
        drops.reasons.push_back({kind});
        input.dropState = MonitorDropProjectionState::Connected;
        input.dropProjection = &drops;
        const auto out = projectAvailability(input);
        EXPECT_EQ(availabilityLayer(out, MonitorAvailabilityLayer::Drop).state,
                  MonitorAvailabilityState::DropGap);
        EXPECT_EQ(out.displayPriority, MonitorAvailabilityLayer::Drop);
    }
}

TEST_F(MonitorAvailability, NotConnectedSubscriptionMakesEveryLayerContextUnavailable)
{
    subscription.state = MonitorSubscriptionProjectionState::NotConnected;
    const auto out = projectAvailability(input);
    for (const auto& layer : out.layers)
        EXPECT_EQ(layer.state, MonitorAvailabilityState::ContextUnavailable);
    EXPECT_EQ(out.displayPriority, MonitorAvailabilityLayer::Capability);
}

TEST_F(MonitorAvailability, EpochMismatchMakesEveryLayerContextUnavailable)
{
    input.observation.sessionEpoch = 8;
    const auto out = projectAvailability(input);
    for (const auto& layer : out.layers)
    {
        EXPECT_EQ(layer.state, MonitorAvailabilityState::ContextUnavailable);
        EXPECT_EQ(layer.reason, MonitorAvailabilityReason::EpochMismatch);
    }
}

TEST_F(MonitorAvailability, NullPointerWithNonZeroCountIsContextUnavailable)
{
    input.measurementCapabilities = nullptr;
    input.measurementCapabilityCount = 1;
    const auto out = projectAvailability(input);
    for (const auto& layer : out.layers)
        EXPECT_EQ(layer.state, MonitorAvailabilityState::ContextUnavailable);
}

TEST_F(MonitorAvailability, ZeroMatchingBaselinesWithCapacityIsNotContextUnavailable)
{
    subscription.status.baselineCapacity = 1;
    subscription.status.baselineCount = 0;
    subscription.status.baselines = nullptr;
    const auto out = projectAvailability(input);
    EXPECT_NE(availabilityLayer(out, MonitorAvailabilityLayer::Capability).state,
              MonitorAvailabilityState::ContextUnavailable);
    EXPECT_EQ(out.displayPriority, MonitorAvailabilityLayer::Value);
}

TEST_F(MonitorAvailability, ZeroCountWithNonNullBaselineBufferIsNotContextUnavailable)
{
    VNX::Monitor::MonitorStreamBaseline leftover {};
    leftover.structSize = sizeof(leftover);
    subscription.status.baselineCapacity = 1;
    subscription.status.baselineCount = 0;
    subscription.status.baselines = &leftover;
    const auto out = projectAvailability(input);
    EXPECT_NE(availabilityLayer(out, MonitorAvailabilityLayer::Capability).state,
              MonitorAvailabilityState::ContextUnavailable);
    EXPECT_EQ(out.displayPriority, MonitorAvailabilityLayer::Value);
}

TEST_F(MonitorAvailability, SubscriptionBaselinePointerMismatchIsContextUnavailable)
{
    subscription.status.baselineCapacity = 1;
    subscription.status.baselineCount = 1;
    subscription.status.baselines = nullptr;
    const auto out = projectAvailability(input);
    for (const auto& layer : out.layers)
        EXPECT_EQ(layer.state, MonitorAvailabilityState::ContextUnavailable);
}

TEST_F(MonitorAvailability, EmptyCapabilityRowsAreUnsupported)
{
    input.measurementCapabilities = nullptr;
    input.measurementCapabilityCount = 0;

    const auto out = projectAvailability(input);
    EXPECT_EQ(out.displayPriority, MonitorAvailabilityLayer::Capability);
    EXPECT_EQ(availabilityLayer(out, MonitorAvailabilityLayer::Capability).state,
              MonitorAvailabilityState::CapabilityUnsupported);
    EXPECT_EQ(availabilityLayer(out, MonitorAvailabilityLayer::Value).state,
              MonitorAvailabilityState::NotEvaluated);
}

TEST_F(MonitorAvailability, DuplicateCapabilityRowsWithDifferentSupportAreIncompatible)
{
    VNX::Monitor::MonitorMeasurementCapability rows[2] = {capability, capability};
    rows[1].supported = 0;
    input.measurementCapabilities = rows;
    input.measurementCapabilityCount = 2;

    const auto out = projectAvailability(input);
    EXPECT_EQ(out.displayPriority, MonitorAvailabilityLayer::Capability);
    EXPECT_EQ(availabilityLayer(out, MonitorAvailabilityLayer::Capability).state,
              MonitorAvailabilityState::ContractIncompatible);
}

TEST_F(MonitorAvailability, WrongPlanTargetIsNotArmed)
{
    input.observation.tapDescriptorId = 45;
    const auto out = projectAvailability(input);
    EXPECT_EQ(out.displayPriority, MonitorAvailabilityLayer::Plan);
    EXPECT_EQ(availabilityLayer(out, MonitorAvailabilityLayer::Plan).state,
              MonitorAvailabilityState::NotArmed);
}

TEST_F(MonitorAvailability, LufsGlobalUnsupportedOverridesNumericRms)
{
    input.observation.kind = VN_MONITOR_MEASURE_LUFS_MOMENTARY;
    capability.kind = input.observation.kind;
    capabilities.lufsMomentarySupported = 0;
    measurement.kind = input.observation.kind;

    const auto out = projectAvailability(input);
    EXPECT_EQ(out.displayPriority, MonitorAvailabilityLayer::Capability);
    EXPECT_EQ(availabilityLayer(out, MonitorAvailabilityLayer::Capability).state,
              MonitorAvailabilityState::CapabilityUnsupported);
    EXPECT_EQ(availabilityLayer(out, MonitorAvailabilityLayer::Value).state,
              MonitorAvailabilityState::NotEvaluated);
}

TEST_F(MonitorAvailability, UnknownValidityBitsOverrideNumeric)
{
    input.observation.validityFlags |= 1u << 31;
    const auto out = projectAvailability(input);
    EXPECT_EQ(out.displayPriority, MonitorAvailabilityLayer::Validity);
    EXPECT_EQ(availabilityLayer(out, MonitorAvailabilityLayer::Validity).state,
              MonitorAvailabilityState::ContractIncompatible);
    EXPECT_EQ(availabilityLayer(out, MonitorAvailabilityLayer::Value).state,
              MonitorAvailabilityState::NotEvaluated);
}

TEST_F(MonitorAvailability, WarmupAndUnarmedNumericIgnoreZero)
{
    input.observation.validityFlags = VN_MONITOR_VALID_ARMED |
                                      VN_MONITOR_VALID_NUMERIC |
                                      VN_MONITOR_VALID_WARMUP;
    auto out = projectAvailability(input);
    EXPECT_EQ(availabilityLayer(out, MonitorAvailabilityLayer::Validity).state,
              MonitorAvailabilityState::ArmedInvalid);
    EXPECT_EQ(availabilityLayer(out, MonitorAvailabilityLayer::Value).state,
              MonitorAvailabilityState::NotEvaluated);

    input.observation.validityFlags = VN_MONITOR_VALID_NUMERIC;
    out = projectAvailability(input);
    EXPECT_EQ(availabilityLayer(out, MonitorAvailabilityLayer::Validity).state,
              MonitorAvailabilityState::ArmedInvalid);
    EXPECT_EQ(availabilityLayer(out, MonitorAvailabilityLayer::Value).state,
              MonitorAvailabilityState::NotEvaluated);
}

TEST_F(MonitorAvailability, ValidUnsupportedFlagOverridesArmedNumeric)
{
    input.observation.validityFlags = VN_MONITOR_VALID_ARMED |
                                      VN_MONITOR_VALID_NUMERIC |
                                      VN_MONITOR_VALID_UNSUPPORTED;
    const auto out = projectAvailability(input);
    EXPECT_EQ(availabilityLayer(out, MonitorAvailabilityLayer::Capability).state,
              MonitorAvailabilityState::CapabilityUnsupported);
    EXPECT_EQ(availabilityLayer(out, MonitorAvailabilityLayer::Validity).state,
              MonitorAvailabilityState::CapabilityUnsupported);
    EXPECT_EQ(availabilityLayer(out, MonitorAvailabilityLayer::Value).state,
              MonitorAvailabilityState::NotEvaluated);
}

TEST_F(MonitorAvailability, NoArmPlanIsNotApplicable)
{
    input.requiresArm = false;
    const auto out = projectAvailability(input);
    EXPECT_EQ(out.displayPriority, MonitorAvailabilityLayer::Plan);
    EXPECT_EQ(availabilityLayer(out, MonitorAvailabilityLayer::Plan).state,
              MonitorAvailabilityState::NotApplicable);
}

TEST_F(MonitorAvailability, SourcesStayInDataLayer)
{
    const char* files[] = {
        "Source/Monitor/MonitorAvailability.cpp",
        "Source/Monitor/MonitorAvailability.h",
        "Source/Monitor/CMakeLists.txt",
    };
    for (const char* file : files)
    {
        const std::string path = std::string(HARKEN_STUDIO_SOURCE_DIR) + "/" + file;
        std::ifstream in(path);
        ASSERT_TRUE(in) << path;
        const std::string text((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        EXPECT_EQ(text.find("Qt6::Widgets"), std::string::npos) << path;
        EXPECT_EQ(text.find("Vn_MonitorApplyCapturePlan"), std::string::npos) << path;
        EXPECT_EQ(text.find("Vn_MonitorAcquireCaptureLease"), std::string::npos) << path;
        EXPECT_EQ(text.find("Vn_MonitorReleaseCaptureLease"), std::string::npos) << path;
    }
}
