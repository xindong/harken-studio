#include "MonitorAvailability.h"
#include "Monitor/MonitorTypes.h"

#include <cstddef>

namespace VNX {
namespace Monitor {
namespace {

constexpr uint32_t kKnownValidityFlags = kMonitorValidArmed |
                                          kMonitorValidNumeric |
                                          kMonitorValidWarmup |
                                          kMonitorValidUnsupported;

std::size_t indexOf(MonitorAvailabilityLayer layer)
{
    return static_cast<std::size_t>(layer);
}

MonitorAvailabilityLayerResult makeLayer(MonitorAvailabilityLayer layer,
                                          MonitorAvailabilityState state,
                                          MonitorAvailabilityReason reason,
                                          MonitorAvailabilitySource source)
{
    return {layer, state, reason, source};
}

void setContext(MonitorAvailabilityResult& result, MonitorAvailabilityReason reason)
{
    for (std::size_t i = 0; i < result.layers.size(); ++i)
    {
        const auto layer = static_cast<MonitorAvailabilityLayer>(i);
        result.layers[i] = makeLayer(layer,
                                     MonitorAvailabilityState::ContextUnavailable,
                                     reason,
                                     MonitorAvailabilitySource::Context);
    }
    result.displayPriority = MonitorAvailabilityLayer::Capability;
}

bool hasConsistentArray(const void* pointer, uint32_t count)
{
    return count == 0 || pointer != nullptr;
}

bool isLufs(uint32_t kind)
{
    return kind == kMonitorMeasureLufsMomentary ||
           kind == kMonitorMeasureLufsShortTerm ||
           kind == kMonitorMeasureLufsIntegrated;
}

bool lufsSupported(const MonitorCapabilities& capabilities, uint32_t kind)
{
    switch (kind)
    {
    case kMonitorMeasureLufsMomentary:
        return capabilities.lufsMomentarySupported != 0;
    case kMonitorMeasureLufsShortTerm:
        return capabilities.lufsShortTermSupported != 0;
    case kMonitorMeasureLufsIntegrated:
        return capabilities.lufsIntegratedSupported != 0;
    default:
        return true;
    }
}

bool isSupportedState(MonitorAvailabilityState state)
{
    return state == MonitorAvailabilityState::Available;
}

bool isCapabilityBlocking(MonitorAvailabilityState state)
{
    return state == MonitorAvailabilityState::CapabilityUnsupported ||
           state == MonitorAvailabilityState::CapabilityUnknown ||
           state == MonitorAvailabilityState::ContractIncompatible;
}

struct CapabilityLookup
{
    MonitorAvailabilityState state = MonitorAvailabilityState::CapabilityUnsupported;
    MonitorAvailabilityReason reason = MonitorAvailabilityReason::CapabilityMissing;
    const MonitorMeasurementCapability* row = nullptr;
};

CapabilityLookup findCapability(const MonitorAvailabilityInput& input)
{
    CapabilityLookup result;
    uint32_t matches = 0;
    bool supported = false;
    bool supportedSet = false;

    for (uint32_t i = 0; i < input.measurementCapabilityCount; ++i)
    {
        const auto& row = input.measurementCapabilities[i];
        if (row.kind != input.observation.kind)
            continue;

        ++matches;
        if (result.row == nullptr)
            result.row = &row;
        const bool rowSupported = row.supported != 0;
        if (supportedSet && supported != rowSupported)
        {
            result.state = MonitorAvailabilityState::ContractIncompatible;
            result.reason = MonitorAvailabilityReason::CapabilityDuplicateConflict;
            continue;
        }
        supported = rowSupported;
        supportedSet = true;
    }

    if (matches == 0)
    {
        result.state = MonitorAvailabilityState::CapabilityUnsupported;
        result.reason = MonitorAvailabilityReason::CapabilityMissing;
        return result;
    }
    if (result.state == MonitorAvailabilityState::ContractIncompatible)
        return result;
    if (result.state == MonitorAvailabilityState::CapabilityUnknown)
        return result;
    if (!supported)
    {
        result.state = MonitorAvailabilityState::CapabilityUnsupported;
        result.reason = MonitorAvailabilityReason::CapabilityUnsupported;
    }
    else
    {
        result.state = MonitorAvailabilityState::Available;
        result.reason = MonitorAvailabilityReason::CapabilitySupported;
    }
    return result;
}

MonitorAvailabilityReason contextError(const MonitorAvailabilityInput& input)
{
    if (!input.snapshot)
        return MonitorAvailabilityReason::MissingSnapshot;
    if (input.subscription == nullptr)
        return MonitorAvailabilityReason::MissingSubscription;
    if (input.subscription->state != MonitorSubscriptionProjectionState::Connected)
        return MonitorAvailabilityReason::SubscriptionNotConnected;
    if (!input.capabilities)
        return MonitorAvailabilityReason::MissingCapabilities;
    if (!input.effectivePlan)
        return MonitorAvailabilityReason::MissingPlan;
    if (input.dropState == MonitorDropProjectionState::Connected &&
        input.dropProjection == nullptr)
        return MonitorAvailabilityReason::MissingDropProjection;
    const auto& status = input.subscription->status;
    if (status.baselineCount > status.baselineCapacity ||
        !hasConsistentArray(status.baselines, status.baselineCount) ||
        !hasConsistentArray(input.measurementCapabilities,
                            input.measurementCapabilityCount) ||
        !hasConsistentArray(input.effectivePlan->accepted.taps,
                            input.effectivePlan->accepted.tapCount) ||
        !hasConsistentArray(input.effectivePlan->accepted.measurements,
                            input.effectivePlan->accepted.measurementCount))
        return MonitorAvailabilityReason::InvalidNestedPointer;
    if (input.observation.kind == 0 || input.observation.tapDescriptorId == 0)
        return MonitorAvailabilityReason::MissingMeasurementKey;
    if (input.subscription->status.monitorSessionEpoch != input.observation.sessionEpoch)
        return MonitorAvailabilityReason::EpochMismatch;
    return MonitorAvailabilityReason::None;
}

bool planContainsMeasurement(const MonitorAvailabilityInput& input)
{
    const auto& plan = input.effectivePlan->accepted;
    bool measurementMatch = false;
    for (uint32_t i = 0; i < plan.measurementCount; ++i)
    {
        const auto& measurement = plan.measurements[i];
        if (measurement.kind == input.observation.kind &&
            measurement.tapDescriptorId == input.observation.tapDescriptorId)
        {
            measurementMatch = true;
            break;
        }
    }
    if (!measurementMatch)
        return false;

    for (uint32_t i = 0; i < plan.tapCount; ++i)
    {
        if (plan.taps[i].tapDescriptorId == input.observation.tapDescriptorId)
            return true;
    }
    return false;
}

bool hasDropGap(const MonitorAvailabilityInput& input, uint32_t* ignoredCount)
{
    *ignoredCount = 0;
    if (input.dropState != MonitorDropProjectionState::Connected ||
        input.dropProjection == nullptr)
        return false;

    bool gap = false;
    for (const auto& reason : input.dropProjection->reasons)
    {
        if (reason.kind == MonitorDropReasonKind::UnauthorizedNotGap)
        {
            ++*ignoredCount;
            continue;
        }
        gap = true;
    }
    return gap;
}

} // namespace

MonitorAvailabilityResult projectAvailability(const MonitorAvailabilityInput& input)
{
    MonitorAvailabilityResult result;
    result.metadata.measurementKind = input.observation.kind;
    result.metadata.tapDescriptorId = input.observation.tapDescriptorId;
    result.metadata.dropProjectionState = input.dropState;

    const auto contextReason = contextError(input);
    if (contextReason != MonitorAvailabilityReason::None)
    {
        setContext(result, contextReason);
        return result;
    }

    const auto capability = findCapability(input);
    if (capability.row != nullptr)
    {
        result.metadata.unit = capability.row->unit;
        result.metadata.algorithmVersion = capability.row->algorithmVersion;
    }

    MonitorAvailabilityState capabilityState = capability.state;
    MonitorAvailabilityReason capabilityReason = capability.reason;
    if (isLufs(input.observation.kind) && !lufsSupported(*input.capabilities,
                                                          input.observation.kind))
    {
        capabilityState = MonitorAvailabilityState::CapabilityUnsupported;
        capabilityReason = MonitorAvailabilityReason::LufsGloballyUnsupported;
    }
    result.layers[indexOf(MonitorAvailabilityLayer::Capability)] = makeLayer(
        MonitorAvailabilityLayer::Capability,
        capabilityState,
        capabilityReason,
        MonitorAvailabilitySource::Capabilities);

    const auto& status = input.subscription->status;
    const bool permissionAccepted =
        (status.acceptedPermissionMask & input.requiredPermissionMask) ==
        input.requiredPermissionMask;
    const bool streamSubscribed =
        (status.resolvedStreamKindMask & input.requiredStreamKindMask) ==
        input.requiredStreamKindMask;
    result.layers[indexOf(MonitorAvailabilityLayer::Subscription)] = makeLayer(
        MonitorAvailabilityLayer::Subscription,
        permissionAccepted && streamSubscribed ? MonitorAvailabilityState::Available
                                               : MonitorAvailabilityState::Unauthorized,
        !permissionAccepted
            ? MonitorAvailabilityReason::PermissionDenied
            : (!streamSubscribed ? MonitorAvailabilityReason::StreamNotSubscribed
                                  : MonitorAvailabilityReason::PermissionAccepted),
        MonitorAvailabilitySource::Subscription);

    const bool armed = planContainsMeasurement(input);
    result.layers[indexOf(MonitorAvailabilityLayer::Plan)] = makeLayer(
        MonitorAvailabilityLayer::Plan,
        !input.requiresArm
            ? MonitorAvailabilityState::NotApplicable
            : (armed ? MonitorAvailabilityState::Available
                     : MonitorAvailabilityState::NotArmed),
        !input.requiresArm
            ? MonitorAvailabilityReason::PlanNotApplicable
            : (armed ? MonitorAvailabilityReason::PlanContainsMeasurement
                     : MonitorAvailabilityReason::PlanMissingMeasurement),
        MonitorAvailabilitySource::EffectivePlan);

    uint32_t ignoredDropReasons = 0;
    const bool dropGap = hasDropGap(input, &ignoredDropReasons);
    result.metadata.ignoredDropReasonCount = ignoredDropReasons;
    if (input.dropState == MonitorDropProjectionState::NotConnected)
    {
        result.layers[indexOf(MonitorAvailabilityLayer::Drop)] = makeLayer(
            MonitorAvailabilityLayer::Drop,
            MonitorAvailabilityState::NotEvaluated,
            MonitorAvailabilityReason::DropProjectionNotConnected,
            MonitorAvailabilitySource::DropProjection);
    }
    else if (dropGap)
    {
        result.layers[indexOf(MonitorAvailabilityLayer::Drop)] = makeLayer(
            MonitorAvailabilityLayer::Drop,
            MonitorAvailabilityState::DropGap,
            MonitorAvailabilityReason::DropReasonPresent,
            MonitorAvailabilitySource::DropProjection);
    }
    else if (ignoredDropReasons != 0)
    {
        result.layers[indexOf(MonitorAvailabilityLayer::Drop)] = makeLayer(
            MonitorAvailabilityLayer::Drop,
            MonitorAvailabilityState::NotEvaluated,
            MonitorAvailabilityReason::UnauthorizedNotGap,
            MonitorAvailabilitySource::DropProjection);
    }
    else
    {
        result.layers[indexOf(MonitorAvailabilityLayer::Drop)] = makeLayer(
            MonitorAvailabilityLayer::Drop,
            MonitorAvailabilityState::Available,
            MonitorAvailabilityReason::DropNone,
            MonitorAvailabilitySource::DropProjection);
    }

    const uint32_t unknownFlags = input.observation.validityFlags & ~kKnownValidityFlags;
    MonitorAvailabilityState validityState = MonitorAvailabilityState::ArmedInvalid;
    MonitorAvailabilityReason validityReason = MonitorAvailabilityReason::ValidityNotArmed;
    if (unknownFlags != 0)
    {
        validityState = MonitorAvailabilityState::ContractIncompatible;
        validityReason = MonitorAvailabilityReason::ValidityUnknownBits;
    }
    else if ((input.observation.validityFlags & kMonitorValidUnsupported) != 0)
    {
        validityState = MonitorAvailabilityState::CapabilityUnsupported;
        validityReason = MonitorAvailabilityReason::ValidityUnsupported;
    }
    else if ((input.observation.validityFlags & kMonitorValidWarmup) != 0)
    {
        validityState = MonitorAvailabilityState::ArmedInvalid;
        validityReason = MonitorAvailabilityReason::ValidityWarmup;
    }
    else if ((input.observation.validityFlags & kMonitorValidArmed) == 0 &&
             (input.observation.validityFlags & kMonitorValidNumeric) != 0)
    {
        validityState = MonitorAvailabilityState::ArmedInvalid;
        validityReason = MonitorAvailabilityReason::ValidityNotArmed;
    }
    else if ((input.observation.validityFlags & kMonitorValidArmed) != 0 &&
             (input.observation.validityFlags & kMonitorValidNumeric) != 0)
    {
        validityState = MonitorAvailabilityState::Available;
        validityReason = MonitorAvailabilityReason::ValidityNumeric;
    }
    result.layers[indexOf(MonitorAvailabilityLayer::Validity)] = makeLayer(
        MonitorAvailabilityLayer::Validity,
        validityState,
        validityReason,
        MonitorAvailabilitySource::RecordValidity);
    if (validityState == MonitorAvailabilityState::CapabilityUnsupported &&
        capabilityState == MonitorAvailabilityState::Available)
    {
        result.layers[indexOf(MonitorAvailabilityLayer::Capability)] = makeLayer(
            MonitorAvailabilityLayer::Capability,
            MonitorAvailabilityState::CapabilityUnsupported,
            MonitorAvailabilityReason::ValidityUnsupported,
            MonitorAvailabilitySource::RecordValidity);
        capabilityState = MonitorAvailabilityState::CapabilityUnsupported;
    }

    const bool valueUsable = isSupportedState(capabilityState) &&
                             validityState == MonitorAvailabilityState::Available;
    const bool valueIsZero = input.observation.numericValue == 0.0f;
    if (valueUsable && valueIsZero)
    {
        result.layers[indexOf(MonitorAvailabilityLayer::Value)] = makeLayer(
            MonitorAvailabilityLayer::Value,
            MonitorAvailabilityState::ValidZero,
            MonitorAvailabilityReason::ValueZero,
            MonitorAvailabilitySource::PayloadValue);
    }
    else if (valueUsable)
    {
        result.layers[indexOf(MonitorAvailabilityLayer::Value)] = makeLayer(
            MonitorAvailabilityLayer::Value,
            MonitorAvailabilityState::ValidNonZero,
            MonitorAvailabilityReason::ValueNonZero,
            MonitorAvailabilitySource::PayloadValue);
    }
    else
    {
        result.layers[indexOf(MonitorAvailabilityLayer::Value)] = makeLayer(
            MonitorAvailabilityLayer::Value,
            MonitorAvailabilityState::NotEvaluated,
            MonitorAvailabilityReason::ValueIgnored,
            MonitorAvailabilitySource::PayloadValue);
    }

    result.displayPriority = MonitorAvailabilityLayer::Value;
    if (isCapabilityBlocking(capabilityState))
        result.displayPriority = MonitorAvailabilityLayer::Capability;
    else if (result.layers[indexOf(MonitorAvailabilityLayer::Subscription)].state ==
             MonitorAvailabilityState::Unauthorized)
        result.displayPriority = MonitorAvailabilityLayer::Subscription;
    else if (result.layers[indexOf(MonitorAvailabilityLayer::Plan)].state ==
                 MonitorAvailabilityState::NotArmed ||
             result.layers[indexOf(MonitorAvailabilityLayer::Plan)].state ==
                 MonitorAvailabilityState::NotApplicable)
        result.displayPriority = MonitorAvailabilityLayer::Plan;
    else if (result.layers[indexOf(MonitorAvailabilityLayer::Drop)].state ==
             MonitorAvailabilityState::DropGap)
        result.displayPriority = MonitorAvailabilityLayer::Drop;
    else if (result.layers[indexOf(MonitorAvailabilityLayer::Validity)].state ==
                 MonitorAvailabilityState::CapabilityUnsupported ||
             result.layers[indexOf(MonitorAvailabilityLayer::Validity)].state ==
                 MonitorAvailabilityState::ContractIncompatible ||
             result.layers[indexOf(MonitorAvailabilityLayer::Validity)].state ==
                 MonitorAvailabilityState::ArmedInvalid)
        result.displayPriority = MonitorAvailabilityLayer::Validity;

    return result;
}

const MonitorAvailabilityLayerResult& availabilityLayer(
    const MonitorAvailabilityResult& result,
    MonitorAvailabilityLayer layer)
{
    return result.layers[indexOf(layer)];
}

} // namespace Monitor
} // namespace VNX
