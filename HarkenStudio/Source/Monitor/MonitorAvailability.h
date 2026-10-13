#pragma once

#include "MonitorProjectionTypes.h"

#include "Monitor/MonitorTypes.h"

#include <array>
#include <cstdint>
#include <memory>

namespace VNX {
namespace Monitor {

enum class MonitorDropProjectionState
{
    NotConnected = 0,
    Connected,
};

enum class MonitorAvailabilityLayer
{
    Capability = 0,
    Subscription,
    Plan,
    Drop,
    Validity,
    Value,
};

enum class MonitorAvailabilityState
{
    ContextUnavailable = 0,
    Available,
    CapabilityUnsupported,
    CapabilityUnknown,
    Unauthorized,
    NotApplicable,
    NotArmed,
    DropGap,
    ArmedInvalid,
    ValidZero,
    ValidNonZero,
    ContractIncompatible,
    NotEvaluated,
};

enum class MonitorAvailabilityReason
{
    None = 0,
    MissingSnapshot,
    MissingSubscription,
    SubscriptionNotConnected,
    MissingCapabilities,
    MissingPlan,
    MissingDropProjection,
    InvalidStructure,
    InvalidNestedPointer,
    EpochMismatch,
    MissingMeasurementKey,
    CapabilitySupported,
    CapabilityUnsupported,
    CapabilityMissing,
    CapabilityUnknown,
    CapabilityRowTruncated,
    CapabilityDuplicateConflict,
    LufsGloballyUnsupported,
    PermissionAccepted,
    PermissionDenied,
    StreamSubscribed,
    StreamNotSubscribed,
    PlanContainsMeasurement,
    PlanMissingMeasurement,
    PlanNotApplicable,
    DropNone,
    DropProjectionNotConnected,
    DropReasonPresent,
    UnauthorizedNotGap,
    ValidityNumeric,
    ValidityUnsupported,
    ValidityUnknownBits,
    ValidityWarmup,
    ValidityNotArmed,
    ValueZero,
    ValueNonZero,
    ValueIgnored,
};

enum class MonitorAvailabilitySource
{
    Context = 0,
    Capabilities,
    Subscription,
    EffectivePlan,
    DropProjection,
    RecordValidity,
    PayloadValue,
};

struct MonitorAvailabilityObservation
{
    uint32_t kind = 0;
    uint64_t tapDescriptorId = 0;
    uint64_t sessionEpoch = 0;
    uint32_t validityFlags = 0;
    float numericValue = 0.0f;
};

struct MonitorAvailabilityInput
{
    std::shared_ptr<const MonitorStoreSnapshot> snapshot;
    const MonitorSubscriptionProjection* subscription = nullptr;
    const MonitorCapabilities* capabilities = nullptr;
    const MonitorMeasurementCapability* measurementCapabilities = nullptr;
    uint32_t measurementCapabilityCount = 0;
    const MonitorEffectiveCapturePlan* effectivePlan = nullptr;
    uint32_t requiredPermissionMask = kMonitorPermObserveMeasurements;
    uint32_t requiredStreamKindMask = 0;
    MonitorAvailabilityObservation observation;
    MonitorDropProjectionState dropState = MonitorDropProjectionState::NotConnected;
    const MonitorDropProjection* dropProjection = nullptr;
    bool requiresArm = true;
};

struct MonitorAvailabilityLayerResult
{
    MonitorAvailabilityLayer layer = MonitorAvailabilityLayer::Capability;
    MonitorAvailabilityState state = MonitorAvailabilityState::NotEvaluated;
    MonitorAvailabilityReason reason = MonitorAvailabilityReason::None;
    MonitorAvailabilitySource source = MonitorAvailabilitySource::Context;
};

struct MonitorAvailabilityMetadata
{
    uint32_t measurementKind = 0;
    uint64_t tapDescriptorId = 0;
    uint32_t unit = 0;
    uint32_t algorithmVersion = 0;
    MonitorDropProjectionState dropProjectionState = MonitorDropProjectionState::NotConnected;
    uint32_t ignoredDropReasonCount = 0;
};

struct MonitorAvailabilityResult
{
    std::array<MonitorAvailabilityLayerResult, 6> layers {};
    MonitorAvailabilityLayer displayPriority = MonitorAvailabilityLayer::Value;
    MonitorAvailabilityMetadata metadata {};
};

MonitorAvailabilityResult projectAvailability(const MonitorAvailabilityInput& input);

const MonitorAvailabilityLayerResult& availabilityLayer(
    const MonitorAvailabilityResult& result,
    MonitorAvailabilityLayer layer);

} // namespace Monitor
} // namespace VNX
