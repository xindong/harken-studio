#ifndef HARKEN_MEDIA_PACK_WIRE_H
#define HARKEN_MEDIA_PACK_WIRE_H

#include <harken_pack/HarkenPack.h>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <span>
#include <type_traits>

namespace harken {

static_assert(std::endian::native == std::endian::little);

template <class Wire>
inline constexpr std::uint64_t kPackBytes = sizeof(Wire);

[[nodiscard]] inline HkPackObjectId ToPackId(VnObjectId value) noexcept {
    HkPackObjectId id;
    const auto bytes = ObjectIdBytes(value);
    std::memcpy(id.bytes, bytes.data(), sizeof id.bytes);
    return id;
}

[[nodiscard]] inline VnObjectId FromPackId(const HkPackObjectId& id) noexcept {
    return ObjectIdFromBytes(std::span<const std::uint8_t, 16>(id.bytes, 16));
}

template <class Target, class Source>
inline void AssignPack(Target& target, Source value) noexcept {
    target = static_cast<Target>(value);
}

template <class Value>
[[nodiscard]] inline bool PackZero(const Value& value) noexcept {
    unsigned char bytes[sizeof(Value)];
    std::memcpy(bytes, &value, sizeof bytes);
    return std::all_of(std::begin(bytes), std::end(bytes), [](unsigned char byte) { return byte == 0; });
}

inline void ToWire(const VnAttenuationCurvePoint& row, HkPackAttenuationCurvePoint& wire) noexcept {
    wire.x = static_cast<float>(row.x);
    wire.y = static_cast<float>(row.y);
}

[[nodiscard]] inline bool FromWire(const HkPackAttenuationCurvePoint& wire, VnAttenuationCurvePoint& out) noexcept {
    AssignPack(out.x, wire.x);
    AssignPack(out.y, wire.y);
    return true;
}

inline void ToWire(const VnCurveHandlePair& row, HkPackCurveHandles& wire) noexcept {
    wire.h1x = static_cast<float>(row.h1x);
    wire.h1y = static_cast<float>(row.h1y);
    wire.h2x = static_cast<float>(row.h2x);
    wire.h2y = static_cast<float>(row.h2y);
}

[[nodiscard]] inline bool FromWire(const HkPackCurveHandles& wire, VnCurveHandlePair& out) noexcept {
    AssignPack(out.h1x, wire.h1x);
    AssignPack(out.h1y, wire.h1y);
    AssignPack(out.h2x, wire.h2x);
    AssignPack(out.h2y, wire.h2y);
    return true;
}

inline void ToWire(const VnAttenuationCurve& row, HkPackAttenuationCurve& wire) noexcept {
    wire.enabled = static_cast<std::uint32_t>(row.enabled);
    wire.pointCount = static_cast<std::uint32_t>(row.pointCount);
    static_assert(std::size(std::remove_cvref_t<decltype(row.points)>{}) == 8);
    for (std::uint64_t i = 0; i < 8; ++i) {
        if (i < row.pointCount) {
            ToWire(row.points[i], wire.points[i]);
        }
    }
    static_assert(std::size(std::remove_cvref_t<decltype(row.handles)>{}) == 7);
    const auto handlesLiveCount = std::min<std::uint64_t>(row.pointCount, 8);
    for (std::uint64_t i = 0; i < 7; ++i) {
        if (handlesLiveCount > 1 && i < handlesLiveCount - 1) {
            ToWire(row.handles[i], wire.handles[i]);
        }
    }
}

[[nodiscard]] inline bool FromWire(const HkPackAttenuationCurve& wire, VnAttenuationCurve& out) noexcept {
    AssignPack(out.enabled, wire.enabled);
    AssignPack(out.pointCount, wire.pointCount);
    static_assert(std::size(std::remove_cvref_t<decltype(out.points)>{}) == 8);
    for (std::uint64_t i = 0; i < 8; ++i) {
        if (i < out.pointCount) {
            if (!FromWire(wire.points[i], out.points[i])) return false;
        } else {
            if (!PackZero(wire.points[i])) return false;
            out.points[i] = {};
        }
    }
    static_assert(std::size(std::remove_cvref_t<decltype(out.handles)>{}) == 7);
    const auto handlesLiveCount = std::min<std::uint64_t>(out.pointCount, 8);
    for (std::uint64_t i = 0; i < 7; ++i) {
        if (handlesLiveCount > 1 && i < handlesLiveCount - 1) {
            if (!FromWire(wire.handles[i], out.handles[i])) return false;
        } else {
            if (!PackZero(wire.handles[i])) return false;
            out.handles[i] = {};
        }
    }
    return true;
}

inline void ToWire(const VnAttenuationDesc& row, HkPackAttenuation& wire) noexcept {
    wire.rolloff = static_cast<std::uint32_t>(row.rolloff);
    wire.minDistance = static_cast<float>(row.minDistance);
    wire.maxDistance = static_cast<float>(row.maxDistance);
    wire.airAbsorptionNearHz = static_cast<float>(row.airAbsorptionNearHz);
    wire.airAbsorptionFarHz = static_cast<float>(row.airAbsorptionFarHz);
    wire.proximityRadius = static_cast<float>(row.proximityRadius);
    wire.proximityMaxBoostDb = static_cast<float>(row.proximityMaxBoostDb);
    wire.coneEnabled = static_cast<std::uint32_t>(row.coneEnabled);
    wire.coneInnerDeg = static_cast<float>(row.coneInnerDeg);
    wire.coneOuterDeg = static_cast<float>(row.coneOuterDeg);
    wire.coneOuterGainDb = static_cast<float>(row.coneOuterGainDb);
    wire.coneOuterLowpassHz = static_cast<float>(row.coneOuterLowpassHz);
    ToWire(row.volumeCurve, wire.volumeCurve);
    ToWire(row.lowpassCurve, wire.lowpassCurve);
    ToWire(row.widthCurve, wire.widthCurve);
    wire.proximityFrequency = static_cast<float>(row.proximityFrequency);
}

[[nodiscard]] inline bool FromWire(const HkPackAttenuation& wire, VnAttenuationDesc& out) noexcept {
    AssignPack(out.rolloff, wire.rolloff);
    AssignPack(out.minDistance, wire.minDistance);
    AssignPack(out.maxDistance, wire.maxDistance);
    AssignPack(out.airAbsorptionNearHz, wire.airAbsorptionNearHz);
    AssignPack(out.airAbsorptionFarHz, wire.airAbsorptionFarHz);
    AssignPack(out.proximityRadius, wire.proximityRadius);
    AssignPack(out.proximityMaxBoostDb, wire.proximityMaxBoostDb);
    AssignPack(out.coneEnabled, wire.coneEnabled);
    AssignPack(out.coneInnerDeg, wire.coneInnerDeg);
    AssignPack(out.coneOuterDeg, wire.coneOuterDeg);
    AssignPack(out.coneOuterGainDb, wire.coneOuterGainDb);
    AssignPack(out.coneOuterLowpassHz, wire.coneOuterLowpassHz);
    if (!FromWire(wire.volumeCurve, out.volumeCurve)) return false;
    if (!FromWire(wire.lowpassCurve, out.lowpassCurve)) return false;
    if (!FromWire(wire.widthCurve, out.widthCurve)) return false;
    AssignPack(out.proximityFrequency, wire.proximityFrequency);
    if (wire.reserved604 != 0) return false;
    return true;
}

inline void ToWire(const PackBindingAttachmentEntry& row, HkPackBindingAttachment& wire) noexcept {
    wire.bindingId = ToPackId(row.bindingId);
    wire.scopeKind = static_cast<std::uint32_t>(row.scopeKind);
    wire.objectId = ToPackId(row.objectId);
}

[[nodiscard]] inline bool FromWire(const HkPackBindingAttachment& wire, PackBindingAttachmentEntry& out) noexcept {
    AssignPack(out.bindingId, FromPackId(wire.bindingId));
    AssignPack(out.scopeKind, wire.scopeKind);
    if (wire.reserved20 != 0) return false;
    AssignPack(out.objectId, FromPackId(wire.objectId));
    return true;
}

inline void ToWire(const VnCurvePoint& row, HkPackCurvePoint& wire) noexcept {
    wire.x = static_cast<float>(row.x);
    wire.y = static_cast<float>(row.y);
}

[[nodiscard]] inline bool FromWire(const HkPackCurvePoint& wire, VnCurvePoint& out) noexcept {
    AssignPack(out.x, wire.x);
    AssignPack(out.y, wire.y);
    return true;
}

inline void ToWire(const VnCurveDesc& row, HkPackBindingCurve& wire) noexcept {
    wire.pointCount = static_cast<std::uint32_t>(row.pointCount);
    static_assert(std::size(std::remove_cvref_t<decltype(row.points)>{}) == 8);
    for (std::uint64_t i = 0; i < 8; ++i) {
        if (i < row.pointCount) {
            ToWire(row.points[i], wire.points[i]);
        }
    }
    static_assert(std::size(std::remove_cvref_t<decltype(row.handles)>{}) == 7);
    const auto handlesLiveCount = std::min<std::uint64_t>(row.pointCount, 8);
    for (std::uint64_t i = 0; i < 7; ++i) {
        if (handlesLiveCount > 1 && i < handlesLiveCount - 1) {
            ToWire(row.handles[i], wire.handles[i]);
        }
    }
}

[[nodiscard]] inline bool FromWire(const HkPackBindingCurve& wire, VnCurveDesc& out) noexcept {
    AssignPack(out.pointCount, wire.pointCount);
    static_assert(std::size(std::remove_cvref_t<decltype(out.points)>{}) == 8);
    for (std::uint64_t i = 0; i < 8; ++i) {
        if (i < out.pointCount) {
            if (!FromWire(wire.points[i], out.points[i])) return false;
        } else {
            if (!PackZero(wire.points[i])) return false;
            out.points[i] = {};
        }
    }
    static_assert(std::size(std::remove_cvref_t<decltype(out.handles)>{}) == 7);
    const auto handlesLiveCount = std::min<std::uint64_t>(out.pointCount, 8);
    for (std::uint64_t i = 0; i < 7; ++i) {
        if (handlesLiveCount > 1 && i < handlesLiveCount - 1) {
            if (!FromWire(wire.handles[i], out.handles[i])) return false;
        } else {
            if (!PackZero(wire.handles[i])) return false;
            out.handles[i] = {};
        }
    }
    return true;
}

inline void ToWire(const PackBindingDefinitionEntry& row, HkPackBindingDefinition& wire) noexcept {
    wire.bindingId = ToPackId(row.bindingId);
    wire.variableId = ToPackId(row.definition.variableId);
    wire.targetScopeKind = static_cast<std::uint32_t>(row.definition.targetScopeKind);
    wire.targetId = static_cast<std::uint32_t>(row.definition.targetId);
    wire.transitionMs = static_cast<std::uint32_t>(row.definition.transitionMs);
    ToWire(row.definition.curve, wire.curve);
}

[[nodiscard]] inline bool FromWire(const HkPackBindingDefinition& wire, PackBindingDefinitionEntry& out) noexcept {
    AssignPack(out.bindingId, FromPackId(wire.bindingId));
    AssignPack(out.definition.variableId, FromPackId(wire.variableId));
    AssignPack(out.definition.targetScopeKind, wire.targetScopeKind);
    AssignPack(out.definition.targetId, wire.targetId);
    AssignPack(out.definition.transitionMs, wire.transitionMs);
    if (wire.reserved44 != 0) return false;
    if (!FromWire(wire.curve, out.definition.curve)) return false;
    return true;
}

inline void ToWire(const EffectEdgeV1& row, HkPackBodyPayloadEffectEdge& wire) noexcept {
    wire.fromNodeId = static_cast<std::uint32_t>(row.fromNodeId);
    wire.toNodeId = static_cast<std::uint32_t>(row.toNodeId);
}

[[nodiscard]] inline bool FromWire(const HkPackBodyPayloadEffectEdge& wire, EffectEdgeV1& out) noexcept {
    AssignPack(out.fromNodeId, wire.fromNodeId);
    AssignPack(out.toNodeId, wire.toNodeId);
    return true;
}

inline void ToWire(const EffectNodeV1& row, HkPackBodyPayloadEffectNodeHeader& wire) noexcept {
    wire.nodeId = static_cast<std::uint32_t>(row.nodeId);
    wire.effectId = ToPackId(row.effectId);
    wire.effectType = static_cast<std::uint32_t>(row.effectType);
    wire.paramCount = static_cast<std::uint32_t>(row.paramCount);
    wire.flags = static_cast<std::uint32_t>(row.flags);
}

[[nodiscard]] inline bool FromWire(const HkPackBodyPayloadEffectNodeHeader& wire, EffectNodeV1& out) noexcept {
    AssignPack(out.nodeId, wire.nodeId);
    AssignPack(out.effectId, FromPackId(wire.effectId));
    AssignPack(out.effectType, wire.effectType);
    AssignPack(out.paramCount, wire.paramCount);
    AssignPack(out.flags, wire.flags);
    return true;
}

inline void ToWire(const EffectNodeV1& row, HkPackBodyPayloadEffectPresetReference& wire) noexcept {
    wire.presetId = ToPackId(row.presetId);
}

[[nodiscard]] inline bool FromWire(const HkPackBodyPayloadEffectPresetReference& wire, EffectNodeV1& out) noexcept {
    AssignPack(out.presetId, FromPackId(wire.presetId));
    return true;
}

inline void ToWire(const EffectParamV1& row, HkPackBodyPayloadEffectParam& wire) noexcept {
    wire.paramId = static_cast<std::uint32_t>(row.paramId);
    wire.valueBits = static_cast<std::uint32_t>(row.valueBits);
}

[[nodiscard]] inline bool FromWire(const HkPackBodyPayloadEffectParam& wire, EffectParamV1& out) noexcept {
    AssignPack(out.paramId, wire.paramId);
    AssignPack(out.valueBits, wire.valueBits);
    return true;
}

inline void ToWire(const PlayablePropertyPayloadViewV1& row, HkPackBodyPayloadHeader& wire) noexcept {
    wire.presentMask = static_cast<std::uint64_t>(row.presentMask);
    wire.volume = static_cast<std::uint32_t>(row.values[0].bits);
    wire.pitch = static_cast<std::uint32_t>(row.values[1].bits);
    wire.outputBusVolume = static_cast<std::uint32_t>(row.values[5].bits);
    wire.streamingMode = static_cast<std::uint32_t>(row.values[4].bits);
    wire.loopCount = static_cast<std::uint32_t>(row.values[3].bits);
    wire.spatialIs3DEnabled = static_cast<std::uint8_t>(row.values[10].bits);
    wire.spatialPanning = static_cast<std::uint32_t>(row.values[8].bits);
    wire.spatialSpread = static_cast<std::uint32_t>(row.values[9].bits);
    wire.spatialAttenuationDistanceScaling = static_cast<std::uint32_t>(row.values[11].bits);
    wire.legacySpatialValue = static_cast<std::uint32_t>(row.values[12].bits);
    wire.outputBusRef = ToPackId(row.values[6].objectId);
    wire.spatialAttenuationRef = ToPackId(row.values[13].objectId);
    wire.legacyValue = static_cast<std::uint64_t>(row.values[14].bits);
    wire.spatializationMix = static_cast<std::uint32_t>(row.values[16].bits);
    wire.sendCount = static_cast<std::uint32_t>(row.sendCount);
    wire.effectNodeCount = static_cast<std::uint32_t>(row.effectNodeCount);
    wire.effectEdgeCount = static_cast<std::uint32_t>(row.effectEdgeCount);
}

[[nodiscard]] inline bool FromWire(const HkPackBodyPayloadHeader& wire, PlayablePropertyPayloadViewV1& out) noexcept {
    AssignPack(out.presentMask, wire.presentMask);
    AssignPack(out.values[0].bits, wire.volume);
    AssignPack(out.values[1].bits, wire.pitch);
    AssignPack(out.values[5].bits, wire.outputBusVolume);
    AssignPack(out.values[4].bits, wire.streamingMode);
    AssignPack(out.values[3].bits, wire.loopCount);
    AssignPack(out.values[10].bits, wire.spatialIs3DEnabled);
    if (wire.reserved != 0) return false;
    AssignPack(out.values[8].bits, wire.spatialPanning);
    AssignPack(out.values[9].bits, wire.spatialSpread);
    AssignPack(out.values[11].bits, wire.spatialAttenuationDistanceScaling);
    AssignPack(out.values[12].bits, wire.legacySpatialValue);
    AssignPack(out.values[6].objectId, FromPackId(wire.outputBusRef));
    AssignPack(out.values[13].objectId, FromPackId(wire.spatialAttenuationRef));
    AssignPack(out.values[14].bits, wire.legacyValue);
    AssignPack(out.values[16].bits, wire.spatializationMix);
    AssignPack(out.sendCount, wire.sendCount);
    if (wire.reservedCount1 != 0) return false;
    if (wire.reservedCount2 != 0) return false;
    AssignPack(out.effectNodeCount, wire.effectNodeCount);
    AssignPack(out.effectEdgeCount, wire.effectEdgeCount);
    return true;
}

inline void ToWire(const SendV1& row, HkPackBodyPayloadSend& wire) noexcept {
    wire.stableSendOrdinal = static_cast<std::uint32_t>(row.stableSendOrdinal);
    wire.reserved = static_cast<std::uint32_t>(row.reserved);
    wire.targetBusId = ToPackId(row.targetBusId);
    wire.sendLevel = static_cast<float>(row.sendLevel);
    wire.reserved2 = static_cast<std::uint32_t>(row.reserved2);
}

[[nodiscard]] inline bool FromWire(const HkPackBodyPayloadSend& wire, SendV1& out) noexcept {
    AssignPack(out.stableSendOrdinal, wire.stableSendOrdinal);
    AssignPack(out.reserved, wire.reserved);
    AssignPack(out.targetBusId, FromPackId(wire.targetBusId));
    AssignPack(out.sendLevel, wire.sendLevel);
    AssignPack(out.reserved2, wire.reserved2);
    return true;
}

inline void ToWire(const ContainerWorldInstallViewV1& row, HkPackBodyWorldActionCapacity& wire) noexcept {
    wire.actionCount = static_cast<std::uint32_t>(row.actionCount);
    wire.maxActionDefinitions = static_cast<std::uint32_t>(row.capacity.maxActionDefinitions);
    wire.maxActionStepsPerDefinition = static_cast<std::uint32_t>(row.capacity.maxActionStepsPerDefinition);
    wire.maxActionStepsTotal = static_cast<std::uint32_t>(row.capacity.maxActionStepsTotal);
    wire.maxActionTriggerDepth = static_cast<std::uint32_t>(row.capacity.maxActionTriggerDepth);
    wire.maxActionExpandedWork = static_cast<std::uint32_t>(row.capacity.maxActionExpandedWork);
}

[[nodiscard]] inline bool FromWire(const HkPackBodyWorldActionCapacity& wire, ContainerWorldInstallViewV1& out) noexcept {
    AssignPack(out.actionCount, wire.actionCount);
    AssignPack(out.capacity.maxActionDefinitions, wire.maxActionDefinitions);
    AssignPack(out.capacity.maxActionStepsPerDefinition, wire.maxActionStepsPerDefinition);
    AssignPack(out.capacity.maxActionStepsTotal, wire.maxActionStepsTotal);
    AssignPack(out.capacity.maxActionTriggerDepth, wire.maxActionTriggerDepth);
    AssignPack(out.capacity.maxActionExpandedWork, wire.maxActionExpandedWork);
    return true;
}

inline void ToWire(const ActionProgramViewV1& row, HkPackBodyWorldActionHeader& wire) noexcept {
    wire.actionStableId = ToPackId(row.actionStableId);
    wire.programSemanticFingerprint = static_cast<std::uint64_t>(row.programSemanticFingerprint);
    wire.stepCount = static_cast<std::uint32_t>(row.stepCount);
    wire.directFeatureMask = static_cast<std::uint32_t>(row.directFeatureMask);
    wire.transitiveFeatureMask = static_cast<std::uint32_t>(row.transitiveFeatureMask);
    wire.expandedStepOccurrenceCount = static_cast<std::uint32_t>(row.expandedStepOccurrenceCount);
    wire.expandedFiringCount = static_cast<std::uint32_t>(row.expandedFiringCount);
}

[[nodiscard]] inline bool FromWire(const HkPackBodyWorldActionHeader& wire, ActionProgramViewV1& out) noexcept {
    AssignPack(out.actionStableId, FromPackId(wire.actionStableId));
    AssignPack(out.programSemanticFingerprint, wire.programSemanticFingerprint);
    AssignPack(out.stepCount, wire.stepCount);
    AssignPack(out.directFeatureMask, wire.directFeatureMask);
    AssignPack(out.transitiveFeatureMask, wire.transitiveFeatureMask);
    AssignPack(out.expandedStepOccurrenceCount, wire.expandedStepOccurrenceCount);
    AssignPack(out.expandedFiringCount, wire.expandedFiringCount);
    return true;
}

inline void ToWire(const ActionObjectRefV1& row, HkPackBodyWorldActionRef& wire) noexcept {
    wire.kind = static_cast<std::uint32_t>(row.kind);
    wire.reserved = static_cast<std::uint32_t>(row.reserved);
    wire.stableId = ToPackId(row.stableId);
    wire.expectedProgramSemanticFingerprint = static_cast<std::uint64_t>(row.expectedProgramSemanticFingerprint);
}

[[nodiscard]] inline bool FromWire(const HkPackBodyWorldActionRef& wire, ActionObjectRefV1& out) noexcept {
    AssignPack(out.kind, wire.kind);
    AssignPack(out.reserved, wire.reserved);
    AssignPack(out.stableId, FromPackId(wire.stableId));
    AssignPack(out.expectedProgramSemanticFingerprint, wire.expectedProgramSemanticFingerprint);
    return true;
}

inline void ToWire(const AuthoredTimeRationalV1& row, HkPackBodyWorldTime& wire) noexcept {
    wire.domain = static_cast<std::uint32_t>(row.domain);
    wire.numerator = static_cast<std::uint64_t>(row.numerator);
    wire.denominator = static_cast<std::uint32_t>(row.denominator);
}

[[nodiscard]] inline bool FromWire(const HkPackBodyWorldTime& wire, AuthoredTimeRationalV1& out) noexcept {
    AssignPack(out.domain, wire.domain);
    AssignPack(out.numerator, wire.numerator);
    AssignPack(out.denominator, wire.denominator);
    return true;
}

inline void ToWire(const ActionStepProgramViewV1& row, HkPackBodyWorldActionStep& wire) noexcept {
    wire.opcode = static_cast<std::uint32_t>(row.opcode);
    wire.authoredEventIndex = static_cast<std::uint32_t>(row.authoredEventIndex);
    ToWire(row.target, wire.target);
    ToWire(row.delay, wire.delay);
    ToWire(row.fade, wire.fade);
    ToWire(row.quantizeReference, wire.quantizeReference);
    wire.quantizeTiming = static_cast<std::uint32_t>(row.quantizeTiming);
    wire.cooldownScope = static_cast<std::uint32_t>(row.cooldownScope);
    wire.ifSilent = static_cast<std::uint32_t>(row.ifSilent);
    wire.scope = static_cast<std::uint32_t>(row.scope);
    ToWire(row.cooldown, wire.cooldown);
    wire.valueBits = static_cast<std::uint32_t>(row.valueBits);
    wire.reserved0 = static_cast<std::uint32_t>(row.reserved0);
}

[[nodiscard]] inline bool FromWire(const HkPackBodyWorldActionStep& wire, ActionStepProgramViewV1& out) noexcept {
    AssignPack(out.opcode, wire.opcode);
    AssignPack(out.authoredEventIndex, wire.authoredEventIndex);
    if (!FromWire(wire.target, out.target)) return false;
    if (!FromWire(wire.delay, out.delay)) return false;
    if (!FromWire(wire.fade, out.fade)) return false;
    if (!FromWire(wire.quantizeReference, out.quantizeReference)) return false;
    AssignPack(out.quantizeTiming, wire.quantizeTiming);
    AssignPack(out.cooldownScope, wire.cooldownScope);
    AssignPack(out.ifSilent, wire.ifSilent);
    AssignPack(out.scope, wire.scope);
    if (!FromWire(wire.cooldown, out.cooldown)) return false;
    AssignPack(out.valueBits, wire.valueBits);
    AssignPack(out.reserved0, wire.reserved0);
    return true;
}

inline void ToWire(const WorldCapacitySummaryV1& row, HkPackBodyWorldCapacity& wire) noexcept {
    wire.maxVariableDefinitions = static_cast<std::uint32_t>(row.maxVariableDefinitions);
    wire.maxContainerPrograms = static_cast<std::uint32_t>(row.maxContainerPrograms);
    wire.maxPlayablePropertyPrograms = static_cast<std::uint32_t>(row.maxPlayablePropertyPrograms);
    wire.maxPlayableOccurrencePropertyRows = static_cast<std::uint32_t>(row.maxPlayableOccurrencePropertyRows);
    wire.maxPlayablePropertySendsTotal = static_cast<std::uint32_t>(row.maxPlayablePropertySendsTotal);
    wire.maxPlayablePropertyLayerAncestorRefsTotal = static_cast<std::uint32_t>(row.maxPlayablePropertyLayerAncestorRefsTotal);
    wire.maxPlayablePropertyEffectNodesTotal = static_cast<std::uint32_t>(row.maxPlayablePropertyEffectNodesTotal);
    wire.maxPlayablePropertyEffectParamsTotal = static_cast<std::uint32_t>(row.maxPlayablePropertyEffectParamsTotal);
    wire.maxPlayablePropertyEffectEdgesTotal = static_cast<std::uint32_t>(row.maxPlayablePropertyEffectEdgesTotal);
    wire.maxContainerDepth = static_cast<std::uint32_t>(row.maxContainerDepth);
    wire.maxChildrenPerContainer = static_cast<std::uint32_t>(row.maxChildrenPerContainer);
    wire.maxAvoidLastN = static_cast<std::uint32_t>(row.maxAvoidLastN);
    wire.maxDiscreteAxes = static_cast<std::uint32_t>(row.maxDiscreteAxes);
    wire.maxDiscreteRows = static_cast<std::uint32_t>(row.maxDiscreteRows);
    wire.maxDiscreteTrieNodes = static_cast<std::uint32_t>(row.maxDiscreteTrieNodes);
    wire.maxContinuousAxes = static_cast<std::uint32_t>(row.maxContinuousAxes);
    wire.maxContinuousRows = static_cast<std::uint32_t>(row.maxContinuousRows);
    wire.maxContinuousCandidates = static_cast<std::uint32_t>(row.maxContinuousCandidates);
    wire.maxContinuousActiveRows = static_cast<std::uint32_t>(row.maxContinuousActiveRows);
    wire.maxContinuousLiveRows = static_cast<std::uint32_t>(row.maxContinuousLiveRows);
    wire.maxTransitionRules = static_cast<std::uint32_t>(row.maxTransitionRules);
    wire.maxCurvePoints = static_cast<std::uint32_t>(row.maxCurvePoints);
    wire.maxKnownScheduleLeaves = static_cast<std::uint32_t>(row.maxKnownScheduleLeaves);
    wire.maxTrackSchedulePrograms = static_cast<std::uint32_t>(row.maxTrackSchedulePrograms);
    wire.maxSegmentSchedulePrograms = static_cast<std::uint32_t>(row.maxSegmentSchedulePrograms);
    wire.maxScheduleTrackRefsPerSegment = static_cast<std::uint32_t>(row.maxScheduleTrackRefsPerSegment);
    wire.maxScheduleTrackRefsTotal = static_cast<std::uint32_t>(row.maxScheduleTrackRefsTotal);
    wire.maxSchedulePlacementsPerTrack = static_cast<std::uint32_t>(row.maxSchedulePlacementsPerTrack);
    wire.maxSchedulePlacementsTotal = static_cast<std::uint32_t>(row.maxSchedulePlacementsTotal);
    wire.maxScheduleMarkersPerSegment = static_cast<std::uint32_t>(row.maxScheduleMarkersPerSegment);
    wire.maxScheduleMarkersTotal = static_cast<std::uint32_t>(row.maxScheduleMarkersTotal);
    wire.maxTrackSegmentScheduleDepth = static_cast<std::uint32_t>(row.maxTrackSegmentScheduleDepth);
    wire.maxActionDefinitions = static_cast<std::uint32_t>(row.maxActionDefinitions);
    wire.maxActionStepsPerDefinition = static_cast<std::uint32_t>(row.maxActionStepsPerDefinition);
    wire.maxActionStepsTotal = static_cast<std::uint32_t>(row.maxActionStepsTotal);
    wire.maxActionTriggerDepth = static_cast<std::uint32_t>(row.maxActionTriggerDepth);
    wire.maxActionExpandedWork = static_cast<std::uint32_t>(row.maxActionExpandedWork);
}

[[nodiscard]] inline bool FromWire(const HkPackBodyWorldCapacity& wire, WorldCapacitySummaryV1& out) noexcept {
    AssignPack(out.maxVariableDefinitions, wire.maxVariableDefinitions);
    AssignPack(out.maxContainerPrograms, wire.maxContainerPrograms);
    AssignPack(out.maxPlayablePropertyPrograms, wire.maxPlayablePropertyPrograms);
    AssignPack(out.maxPlayableOccurrencePropertyRows, wire.maxPlayableOccurrencePropertyRows);
    AssignPack(out.maxPlayablePropertySendsTotal, wire.maxPlayablePropertySendsTotal);
    AssignPack(out.maxPlayablePropertyLayerAncestorRefsTotal, wire.maxPlayablePropertyLayerAncestorRefsTotal);
    AssignPack(out.maxPlayablePropertyEffectNodesTotal, wire.maxPlayablePropertyEffectNodesTotal);
    AssignPack(out.maxPlayablePropertyEffectParamsTotal, wire.maxPlayablePropertyEffectParamsTotal);
    AssignPack(out.maxPlayablePropertyEffectEdgesTotal, wire.maxPlayablePropertyEffectEdgesTotal);
    AssignPack(out.maxContainerDepth, wire.maxContainerDepth);
    AssignPack(out.maxChildrenPerContainer, wire.maxChildrenPerContainer);
    AssignPack(out.maxAvoidLastN, wire.maxAvoidLastN);
    AssignPack(out.maxDiscreteAxes, wire.maxDiscreteAxes);
    AssignPack(out.maxDiscreteRows, wire.maxDiscreteRows);
    AssignPack(out.maxDiscreteTrieNodes, wire.maxDiscreteTrieNodes);
    AssignPack(out.maxContinuousAxes, wire.maxContinuousAxes);
    AssignPack(out.maxContinuousRows, wire.maxContinuousRows);
    AssignPack(out.maxContinuousCandidates, wire.maxContinuousCandidates);
    AssignPack(out.maxContinuousActiveRows, wire.maxContinuousActiveRows);
    AssignPack(out.maxContinuousLiveRows, wire.maxContinuousLiveRows);
    AssignPack(out.maxTransitionRules, wire.maxTransitionRules);
    AssignPack(out.maxCurvePoints, wire.maxCurvePoints);
    AssignPack(out.maxKnownScheduleLeaves, wire.maxKnownScheduleLeaves);
    AssignPack(out.maxTrackSchedulePrograms, wire.maxTrackSchedulePrograms);
    AssignPack(out.maxSegmentSchedulePrograms, wire.maxSegmentSchedulePrograms);
    AssignPack(out.maxScheduleTrackRefsPerSegment, wire.maxScheduleTrackRefsPerSegment);
    AssignPack(out.maxScheduleTrackRefsTotal, wire.maxScheduleTrackRefsTotal);
    AssignPack(out.maxSchedulePlacementsPerTrack, wire.maxSchedulePlacementsPerTrack);
    AssignPack(out.maxSchedulePlacementsTotal, wire.maxSchedulePlacementsTotal);
    AssignPack(out.maxScheduleMarkersPerSegment, wire.maxScheduleMarkersPerSegment);
    AssignPack(out.maxScheduleMarkersTotal, wire.maxScheduleMarkersTotal);
    AssignPack(out.maxTrackSegmentScheduleDepth, wire.maxTrackSegmentScheduleDepth);
    AssignPack(out.maxActionDefinitions, wire.maxActionDefinitions);
    AssignPack(out.maxActionStepsPerDefinition, wire.maxActionStepsPerDefinition);
    AssignPack(out.maxActionStepsTotal, wire.maxActionStepsTotal);
    AssignPack(out.maxActionTriggerDepth, wire.maxActionTriggerDepth);
    AssignPack(out.maxActionExpandedWork, wire.maxActionExpandedWork);
    return true;
}

inline void ToWire(const PlayableRefV1& row, HkPackBodyWorldPlayableRef& wire) noexcept {
    wire.kind = static_cast<std::uint32_t>(row.kind);
    wire.reserved = static_cast<std::uint32_t>(row.reserved);
    wire.stableId = ToPackId(row.stableId);
    wire.expectedProgramSemanticFingerprint = static_cast<std::uint64_t>(row.expectedProgramSemanticFingerprint);
    wire.contentRevision = static_cast<std::uint64_t>(row.contentRevision);
    wire.routeStableId = ToPackId(row.routeStableId);
    wire.compatibilityFingerprint = static_cast<std::uint64_t>(row.compatibilityFingerprint);
}

[[nodiscard]] inline bool FromWire(const HkPackBodyWorldPlayableRef& wire, PlayableRefV1& out) noexcept {
    AssignPack(out.kind, wire.kind);
    AssignPack(out.reserved, wire.reserved);
    AssignPack(out.stableId, FromPackId(wire.stableId));
    AssignPack(out.expectedProgramSemanticFingerprint, wire.expectedProgramSemanticFingerprint);
    AssignPack(out.contentRevision, wire.contentRevision);
    AssignPack(out.routeStableId, FromPackId(wire.routeStableId));
    AssignPack(out.compatibilityFingerprint, wire.compatibilityFingerprint);
    return true;
}

inline void ToWire(const ContainerChildProgramViewV2& row, HkPackBodyWorldChildHeader& wire) noexcept {
    ToWire(row.playable, wire.playable);
    wire.stableChildOrdinal = static_cast<std::uint32_t>(row.stableChildOrdinal);
    wire.authoredStableOrder = static_cast<std::uint32_t>(row.authoredStableOrder);
    wire.frequencyWeight = static_cast<std::uint32_t>(row.frequencyWeight);
    wire.orderWeight = static_cast<std::uint32_t>(row.orderWeight);
    wire.queueStopPolicy = static_cast<std::uint32_t>(row.queueStopPolicy);
    wire.reserved = static_cast<std::uint32_t>(row.reserved);
    wire.timingKind = static_cast<std::uint32_t>(row.timingHint.kind);
    wire.timingReserved = static_cast<std::uint32_t>(row.timingHint.reserved);
}

[[nodiscard]] inline bool FromWire(const HkPackBodyWorldChildHeader& wire, ContainerChildProgramViewV2& out) noexcept {
    if (!FromWire(wire.playable, out.playable)) return false;
    AssignPack(out.stableChildOrdinal, wire.stableChildOrdinal);
    AssignPack(out.authoredStableOrder, wire.authoredStableOrder);
    AssignPack(out.frequencyWeight, wire.frequencyWeight);
    AssignPack(out.orderWeight, wire.orderWeight);
    AssignPack(out.queueStopPolicy, wire.queueStopPolicy);
    AssignPack(out.reserved, wire.reserved);
    AssignPack(out.timingHint.kind, wire.timingKind);
    AssignPack(out.timingHint.reserved, wire.timingReserved);
    return true;
}

inline void ToWire(const ContainerProgramViewV3& row, HkPackBodyWorldContainerHeader& wire) noexcept {
    wire.containerStableId = ToPackId(row.containerStableId);
    wire.programSemanticFingerprint = static_cast<std::uint64_t>(row.programSemanticFingerprint);
    wire.mode = static_cast<std::uint32_t>(row.mode);
    wire.randomDiscipline = static_cast<std::uint32_t>(row.randomDiscipline);
    wire.memoryScope = static_cast<std::uint32_t>(row.memoryScope);
    wire.retriggerPolicy = static_cast<std::uint32_t>(row.retriggerPolicy);
    wire.resetPolicy = static_cast<std::uint32_t>(row.resetPolicy);
    wire.sequenceEndPolicy = static_cast<std::uint32_t>(row.sequenceEndPolicy);
    wire.queueCycle = static_cast<std::uint32_t>(row.queueCycle);
    wire.queueSplice = static_cast<std::uint32_t>(row.queueSplice);
    wire.queueCycleCount = static_cast<std::uint32_t>(row.queueCycleCount);
    wire.resumePosition = static_cast<std::uint8_t>(row.resumePosition);
    wire.avoidLastN = static_cast<std::uint32_t>(row.avoidLastN);
    wire.initialSequenceCursor = static_cast<std::uint32_t>(row.initialSequenceCursor);
    wire.discreteTiming = static_cast<std::uint32_t>(row.discreteTiming);
    wire.continuousTiming = static_cast<std::uint32_t>(row.continuousTiming);
    wire.continuousNormalization = static_cast<std::uint32_t>(row.continuousNormalization);
    wire.evaluationPeriodFrames = static_cast<std::uint32_t>(row.evaluationPeriodFrames);
    wire.envelopeFrames = static_cast<std::uint32_t>(row.envelopeFrames);
    wire.maxActiveRows = static_cast<std::uint32_t>(row.maxActiveRows);
    wire.maxLiveRows = static_cast<std::uint32_t>(row.maxLiveRows);
    wire.childCount = static_cast<std::uint32_t>(row.childCount);
    wire.curveCount = static_cast<std::uint32_t>(row.curveCount);
    wire.discreteAxisCount = static_cast<std::uint32_t>(row.discreteAxisCount);
    wire.discreteRowCount = static_cast<std::uint32_t>(row.discreteRowCount);
    wire.continuousAxisCount = static_cast<std::uint32_t>(row.continuousAxisCount);
    wire.continuousRowCount = static_cast<std::uint32_t>(row.continuousRowCount);
    wire.transitionProgramIndex = static_cast<std::uint32_t>(row.transitionProgramIndex);
    wire.authoredStableOrder = static_cast<std::uint32_t>(row.authoredStableOrder);
}

[[nodiscard]] inline bool FromWire(const HkPackBodyWorldContainerHeader& wire, ContainerProgramViewV3& out) noexcept {
    AssignPack(out.containerStableId, FromPackId(wire.containerStableId));
    AssignPack(out.programSemanticFingerprint, wire.programSemanticFingerprint);
    AssignPack(out.mode, wire.mode);
    AssignPack(out.randomDiscipline, wire.randomDiscipline);
    AssignPack(out.memoryScope, wire.memoryScope);
    AssignPack(out.retriggerPolicy, wire.retriggerPolicy);
    AssignPack(out.resetPolicy, wire.resetPolicy);
    AssignPack(out.sequenceEndPolicy, wire.sequenceEndPolicy);
    AssignPack(out.queueCycle, wire.queueCycle);
    AssignPack(out.queueSplice, wire.queueSplice);
    AssignPack(out.queueCycleCount, wire.queueCycleCount);
    AssignPack(out.resumePosition, wire.resumePosition);
    if (!PackZero(wire.reserved)) return false;
    AssignPack(out.avoidLastN, wire.avoidLastN);
    AssignPack(out.initialSequenceCursor, wire.initialSequenceCursor);
    AssignPack(out.discreteTiming, wire.discreteTiming);
    AssignPack(out.continuousTiming, wire.continuousTiming);
    AssignPack(out.continuousNormalization, wire.continuousNormalization);
    AssignPack(out.evaluationPeriodFrames, wire.evaluationPeriodFrames);
    AssignPack(out.envelopeFrames, wire.envelopeFrames);
    AssignPack(out.maxActiveRows, wire.maxActiveRows);
    AssignPack(out.maxLiveRows, wire.maxLiveRows);
    AssignPack(out.childCount, wire.childCount);
    AssignPack(out.curveCount, wire.curveCount);
    AssignPack(out.discreteAxisCount, wire.discreteAxisCount);
    AssignPack(out.discreteRowCount, wire.discreteRowCount);
    AssignPack(out.continuousAxisCount, wire.continuousAxisCount);
    AssignPack(out.continuousRowCount, wire.continuousRowCount);
    AssignPack(out.transitionProgramIndex, wire.transitionProgramIndex);
    AssignPack(out.authoredStableOrder, wire.authoredStableOrder);
    return true;
}

inline void ToWire(const ContinuousGridCellV1& row, HkPackBodyWorldContinuousCell& wire) noexcept {
    wire.kind = static_cast<std::uint32_t>(row.kind);
    wire.lower = static_cast<float>(row.lower);
    wire.upper = static_cast<float>(row.upper);
    wire.lowerInclusive = static_cast<std::uint8_t>(row.lowerInclusive);
    wire.upperInclusive = static_cast<std::uint8_t>(row.upperInclusive);
    wire.reserved = static_cast<std::uint16_t>(row.reserved);
    wire.leftCurveLocalId = static_cast<std::uint32_t>(row.leftCurveLocalId);
    wire.rightCurveLocalId = static_cast<std::uint32_t>(row.rightCurveLocalId);
}

[[nodiscard]] inline bool FromWire(const HkPackBodyWorldContinuousCell& wire, ContinuousGridCellV1& out) noexcept {
    AssignPack(out.kind, wire.kind);
    AssignPack(out.lower, wire.lower);
    AssignPack(out.upper, wire.upper);
    AssignPack(out.lowerInclusive, wire.lowerInclusive);
    AssignPack(out.upperInclusive, wire.upperInclusive);
    AssignPack(out.reserved, wire.reserved);
    AssignPack(out.leftCurveLocalId, wire.leftCurveLocalId);
    AssignPack(out.rightCurveLocalId, wire.rightCurveLocalId);
    return true;
}

inline void ToWire(const ContinuousGridRowViewV1& row, HkPackBodyWorldContinuousGridHeader& wire) noexcept {
    wire.stableRowOrdinal = static_cast<std::uint32_t>(row.stableRowOrdinal);
    wire.authoredStableOrder = static_cast<std::uint32_t>(row.authoredStableOrder);
    wire.childStableOrdinal = static_cast<std::uint32_t>(row.childStableOrdinal);
    wire.cellCount = static_cast<std::uint32_t>(row.cellCount);
}

[[nodiscard]] inline bool FromWire(const HkPackBodyWorldContinuousGridHeader& wire, ContinuousGridRowViewV1& out) noexcept {
    AssignPack(out.stableRowOrdinal, wire.stableRowOrdinal);
    AssignPack(out.authoredStableOrder, wire.authoredStableOrder);
    AssignPack(out.childStableOrdinal, wire.childStableOrdinal);
    AssignPack(out.cellCount, wire.cellCount);
    return true;
}

inline void ToWire(const CurveHandlePair& row, HkPackBodyWorldCurveHandle& wire) noexcept {
    wire.h1x = static_cast<float>(row.h1x);
    wire.h1y = static_cast<float>(row.h1y);
    wire.h2x = static_cast<float>(row.h2x);
    wire.h2y = static_cast<float>(row.h2y);
}

[[nodiscard]] inline bool FromWire(const HkPackBodyWorldCurveHandle& wire, CurveHandlePair& out) noexcept {
    AssignPack(out.h1x, wire.h1x);
    AssignPack(out.h1y, wire.h1y);
    AssignPack(out.h2x, wire.h2x);
    AssignPack(out.h2y, wire.h2y);
    return true;
}

inline void ToWire(const CurveV1& row, HkPackBodyWorldCurveHeader& wire) noexcept {
    wire.curveLocalId = static_cast<std::uint32_t>(row.curveLocalId);
    wire.pointCount = static_cast<std::uint32_t>(row.curve.pointCount);
}

[[nodiscard]] inline bool FromWire(const HkPackBodyWorldCurveHeader& wire, CurveV1& out) noexcept {
    AssignPack(out.curveLocalId, wire.curveLocalId);
    AssignPack(out.curve.pointCount, wire.pointCount);
    return true;
}

inline void ToWire(const CurvePoint& row, HkPackBodyWorldCurvePoint& wire) noexcept {
    wire.x = static_cast<float>(row.x);
    wire.y = static_cast<float>(row.y);
}

[[nodiscard]] inline bool FromWire(const HkPackBodyWorldCurvePoint& wire, CurvePoint& out) noexcept {
    AssignPack(out.x, wire.x);
    AssignPack(out.y, wire.y);
    return true;
}

inline void ToWire(const DiscreteGridCellV1& row, HkPackBodyWorldDiscreteCell& wire) noexcept {
    wire.kind = static_cast<std::uint32_t>(row.kind);
    wire.exactValue = static_cast<std::int32_t>(row.exactValue);
}

[[nodiscard]] inline bool FromWire(const HkPackBodyWorldDiscreteCell& wire, DiscreteGridCellV1& out) noexcept {
    AssignPack(out.kind, wire.kind);
    AssignPack(out.exactValue, wire.exactValue);
    return true;
}

inline void ToWire(const DiscreteGridRowViewV1& row, HkPackBodyWorldDiscreteGridHeader& wire) noexcept {
    wire.stableRowOrdinal = static_cast<std::uint32_t>(row.stableRowOrdinal);
    wire.authoredStableOrder = static_cast<std::uint32_t>(row.authoredStableOrder);
    wire.childStableOrdinal = static_cast<std::uint32_t>(row.childStableOrdinal);
    wire.cellCount = static_cast<std::uint32_t>(row.cellCount);
}

[[nodiscard]] inline bool FromWire(const HkPackBodyWorldDiscreteGridHeader& wire, DiscreteGridRowViewV1& out) noexcept {
    AssignPack(out.stableRowOrdinal, wire.stableRowOrdinal);
    AssignPack(out.authoredStableOrder, wire.authoredStableOrder);
    AssignPack(out.childStableOrdinal, wire.childStableOrdinal);
    AssignPack(out.cellCount, wire.cellCount);
    return true;
}

inline void ToWire(const TransitionFadeV1& row, HkPackBodyWorldFade& wire) noexcept {
    wire.active = static_cast<std::uint8_t>(row.active);
    static_assert(std::size(std::remove_cvref_t<decltype(row.reserved)>{}) == 3);
    for (std::uint64_t i = 0; i < 3; ++i) {
        wire.reserved[i] = static_cast<std::uint8_t>(row.reserved[i]);
    }
    wire.durationFrames = static_cast<std::uint32_t>(row.durationFrames);
    wire.curveLocalId = static_cast<std::uint32_t>(row.curveLocalId);
}

[[nodiscard]] inline bool FromWire(const HkPackBodyWorldFade& wire, TransitionFadeV1& out) noexcept {
    AssignPack(out.active, wire.active);
    static_assert(std::size(std::remove_cvref_t<decltype(out.reserved)>{}) == 3);
    for (std::uint64_t i = 0; i < 3; ++i) {
        AssignPack(out.reserved[i], wire.reserved[i]);
    }
    AssignPack(out.durationFrames, wire.durationFrames);
    AssignPack(out.curveLocalId, wire.curveLocalId);
    return true;
}

inline void ToWire(const GridAxisV1& row, HkPackBodyWorldGridAxis& wire) noexcept {
    wire.stableAxisOrdinal = static_cast<std::uint32_t>(row.stableAxisOrdinal);
    wire.authoredColumnOrder = static_cast<std::uint32_t>(row.authoredColumnOrder);
    wire.variableId = ToPackId(row.variableId);
}

[[nodiscard]] inline bool FromWire(const HkPackBodyWorldGridAxis& wire, GridAxisV1& out) noexcept {
    AssignPack(out.stableAxisOrdinal, wire.stableAxisOrdinal);
    AssignPack(out.authoredColumnOrder, wire.authoredColumnOrder);
    AssignPack(out.variableId, FromPackId(wire.variableId));
    return true;
}

inline void ToWire(const InlineSoundTimingHintV2& row, HkPackBodyWorldInlineSound& wire) noexcept {
    ToWire(row.startOffset, wire.startOffset);
    ToWire(row.duration, wire.duration);
    wire.loopCount = static_cast<std::uint32_t>(row.loopCount);
    wire.loopRegionPresent = static_cast<std::uint8_t>(row.loopRegionPresent);
    static_assert(std::size(std::remove_cvref_t<decltype(row.reserved)>{}) == 3);
    for (std::uint64_t i = 0; i < 3; ++i) {
        wire.reserved[i] = static_cast<std::uint8_t>(row.reserved[i]);
    }
    ToWire(row.loopStart, wire.loopStart);
    ToWire(row.loopEnd, wire.loopEnd);
}

[[nodiscard]] inline bool FromWire(const HkPackBodyWorldInlineSound& wire, InlineSoundTimingHintV2& out) noexcept {
    if (!FromWire(wire.startOffset, out.startOffset)) return false;
    if (!FromWire(wire.duration, out.duration)) return false;
    AssignPack(out.loopCount, wire.loopCount);
    AssignPack(out.loopRegionPresent, wire.loopRegionPresent);
    static_assert(std::size(std::remove_cvref_t<decltype(out.reserved)>{}) == 3);
    for (std::uint64_t i = 0; i < 3; ++i) {
        AssignPack(out.reserved[i], wire.reserved[i]);
    }
    if (!FromWire(wire.loopStart, out.loopStart)) return false;
    if (!FromWire(wire.loopEnd, out.loopEnd)) return false;
    return true;
}

inline void ToWire(const LayerAncestorRefV1& row, HkPackBodyWorldLayerAncestor& wire) noexcept {
    wire.layerStableId = ToPackId(row.layerStableId);
    wire.layerProgramSemanticFingerprint = static_cast<std::uint64_t>(row.layerProgramSemanticFingerprint);
}

[[nodiscard]] inline bool FromWire(const HkPackBodyWorldLayerAncestor& wire, LayerAncestorRefV1& out) noexcept {
    AssignPack(out.layerStableId, FromPackId(wire.layerStableId));
    AssignPack(out.layerProgramSemanticFingerprint, wire.layerProgramSemanticFingerprint);
    return true;
}

inline void ToWire(const ScheduleMarkerV1& row, HkPackBodyWorldMarker& wire) noexcept {
    wire.stableMarkerId = ToPackId(row.stableMarkerId);
    wire.markerId = static_cast<std::uint32_t>(row.markerId);
    wire.markerKind = static_cast<std::uint32_t>(row.markerKind);
    wire.position = static_cast<std::uint64_t>(row.position);
}

[[nodiscard]] inline bool FromWire(const HkPackBodyWorldMarker& wire, ScheduleMarkerV1& out) noexcept {
    AssignPack(out.stableMarkerId, FromPackId(wire.stableMarkerId));
    AssignPack(out.markerId, wire.markerId);
    AssignPack(out.markerKind, wire.markerKind);
    AssignPack(out.position, wire.position);
    return true;
}

inline void ToWire(const PlayableOccurrencePropertyViewV1& row, HkPackBodyWorldOccurrenceHeader& wire) noexcept {
    wire.parentKind = static_cast<std::uint32_t>(row.parentKind);
    wire.stableOccurrenceOrdinal = static_cast<std::uint32_t>(row.stableOccurrenceOrdinal);
    wire.parentStableId = ToPackId(row.parentStableId);
    wire.parentProgramSemanticFingerprint = static_cast<std::uint64_t>(row.parentProgramSemanticFingerprint);
    ToWire(row.childPlayable, wire.childPlayable);
    wire.occurrenceSemanticFingerprint = static_cast<std::uint64_t>(row.occurrenceSemanticFingerprint);
}

[[nodiscard]] inline bool FromWire(const HkPackBodyWorldOccurrenceHeader& wire, PlayableOccurrencePropertyViewV1& out) noexcept {
    AssignPack(out.parentKind, wire.parentKind);
    AssignPack(out.stableOccurrenceOrdinal, wire.stableOccurrenceOrdinal);
    AssignPack(out.parentStableId, FromPackId(wire.parentStableId));
    AssignPack(out.parentProgramSemanticFingerprint, wire.parentProgramSemanticFingerprint);
    if (!FromWire(wire.childPlayable, out.childPlayable)) return false;
    AssignPack(out.occurrenceSemanticFingerprint, wire.occurrenceSemanticFingerprint);
    return true;
}

inline void ToWire(const StableAuthoredPathStepV1& row, HkPackBodyWorldPathStep& wire) noexcept {
    ToWire(row.playable, wire.playable);
    wire.stableChildOrdinal = static_cast<std::uint32_t>(row.stableChildOrdinal);
    wire.reserved = static_cast<std::uint32_t>(row.reserved);
    wire.trackId = ToPackId(row.trackId);
    wire.clipId = ToPackId(row.clipId);
    wire.clipGuidHigh = static_cast<std::uint64_t>(row.clipGuidHigh);
    wire.clipGuidLow = static_cast<std::uint64_t>(row.clipGuidLow);
}

[[nodiscard]] inline bool FromWire(const HkPackBodyWorldPathStep& wire, StableAuthoredPathStepV1& out) noexcept {
    if (!FromWire(wire.playable, out.playable)) return false;
    AssignPack(out.stableChildOrdinal, wire.stableChildOrdinal);
    AssignPack(out.reserved, wire.reserved);
    AssignPack(out.trackId, FromPackId(wire.trackId));
    AssignPack(out.clipId, FromPackId(wire.clipId));
    AssignPack(out.clipGuidHigh, wire.clipGuidHigh);
    AssignPack(out.clipGuidLow, wire.clipGuidLow);
    return true;
}

inline void ToWire(const ScheduleClipPlacementV1& row, HkPackBodyWorldPlacement& wire) noexcept {
    wire.stablePlacementOrdinal = static_cast<std::uint32_t>(row.stablePlacementOrdinal);
    wire.stableOccurrenceOrdinal = static_cast<std::uint32_t>(row.stableOccurrenceOrdinal);
    ToWire(row.content, wire.content);
    wire.authoredStart = static_cast<std::uint64_t>(row.authoredStart);
    wire.authoredDuration = static_cast<std::uint64_t>(row.authoredDuration);
    wire.authoredFadeIn = static_cast<std::uint64_t>(row.authoredFadeIn);
    wire.authoredFadeOut = static_cast<std::uint64_t>(row.authoredFadeOut);
    wire.clipId = ToPackId(row.clipId);
    wire.clipGuidHigh = static_cast<std::uint64_t>(row.clipGuidHigh);
    wire.clipGuidLow = static_cast<std::uint64_t>(row.clipGuidLow);
}

[[nodiscard]] inline bool FromWire(const HkPackBodyWorldPlacement& wire, ScheduleClipPlacementV1& out) noexcept {
    AssignPack(out.stablePlacementOrdinal, wire.stablePlacementOrdinal);
    AssignPack(out.stableOccurrenceOrdinal, wire.stableOccurrenceOrdinal);
    if (!FromWire(wire.content, out.content)) return false;
    AssignPack(out.authoredStart, wire.authoredStart);
    AssignPack(out.authoredDuration, wire.authoredDuration);
    AssignPack(out.authoredFadeIn, wire.authoredFadeIn);
    AssignPack(out.authoredFadeOut, wire.authoredFadeOut);
    AssignPack(out.clipId, FromPackId(wire.clipId));
    AssignPack(out.clipGuidHigh, wire.clipGuidHigh);
    AssignPack(out.clipGuidLow, wire.clipGuidLow);
    return true;
}

inline void ToWire(const CurrentWorldV3PrefixValues& row, HkPackBodyWorldPrefix& wire) noexcept {
    wire.schema = static_cast<std::uint32_t>(row.schema);
    wire.compiler = static_cast<std::uint32_t>(row.compiler);
    wire.selectionAlgorithm = static_cast<std::uint32_t>(row.selectionAlgorithm);
    wire.gridAlgorithm = static_cast<std::uint32_t>(row.gridAlgorithm);
    wire.transitionAlgorithm = static_cast<std::uint32_t>(row.transitionAlgorithm);
    wire.publicationProtocol = static_cast<std::uint32_t>(row.publicationProtocol);
    wire.runtimeSemanticFingerprint = static_cast<std::uint64_t>(row.runtimeSemanticFingerprint);
    wire.capabilityMask = static_cast<std::uint64_t>(row.capabilityMask);
    wire.variableCount = static_cast<std::uint32_t>(row.variableCount);
    wire.playablePropertyProgramCount = static_cast<std::uint32_t>(row.playablePropertyProgramCount);
    wire.playableOccurrencePropertyCount = static_cast<std::uint32_t>(row.playableOccurrencePropertyCount);
    wire.playablePropertySendCount = static_cast<std::uint32_t>(row.playablePropertySendCount);
    wire.reservedChildCount1 = static_cast<std::uint32_t>(row.reservedChildCount1);
    wire.reservedChildCount2 = static_cast<std::uint32_t>(row.reservedChildCount2);
    wire.playablePropertyLayerAncestorRefCount = static_cast<std::uint32_t>(row.playablePropertyLayerAncestorRefCount);
    wire.playablePropertyEffectNodeCount = static_cast<std::uint32_t>(row.playablePropertyEffectNodeCount);
    wire.playablePropertyEffectParamCount = static_cast<std::uint32_t>(row.playablePropertyEffectParamCount);
    wire.playablePropertyEffectEdgeCount = static_cast<std::uint32_t>(row.playablePropertyEffectEdgeCount);
    wire.trackScheduleCount = static_cast<std::uint32_t>(row.trackScheduleCount);
    wire.segmentScheduleCount = static_cast<std::uint32_t>(row.segmentScheduleCount);
    wire.scheduleTrackRefCount = static_cast<std::uint32_t>(row.scheduleTrackRefCount);
    wire.schedulePlacementCount = static_cast<std::uint32_t>(row.schedulePlacementCount);
    wire.scheduleMarkerCount = static_cast<std::uint32_t>(row.scheduleMarkerCount);
    wire.containerCount = static_cast<std::uint32_t>(row.containerCount);
    wire.transitionProgramCount = static_cast<std::uint32_t>(row.transitionProgramCount);
    wire.maxContainerDepth = static_cast<std::uint32_t>(row.maxContainerDepth);
    wire.maxChildrenPerContainer = static_cast<std::uint32_t>(row.maxChildrenPerContainer);
    wire.maxAvoidLastN = static_cast<std::uint32_t>(row.maxAvoidLastN);
    wire.maxDiscreteAxes = static_cast<std::uint32_t>(row.maxDiscreteAxes);
    wire.maxDiscreteRows = static_cast<std::uint32_t>(row.maxDiscreteRows);
    wire.maxDiscreteTrieNodes = static_cast<std::uint32_t>(row.maxDiscreteTrieNodes);
    wire.maxContinuousAxes = static_cast<std::uint32_t>(row.maxContinuousAxes);
    wire.maxContinuousRows = static_cast<std::uint32_t>(row.maxContinuousRows);
    wire.maxContinuousCandidates = static_cast<std::uint32_t>(row.maxContinuousCandidates);
    wire.maxContinuousActiveRows = static_cast<std::uint32_t>(row.maxContinuousActiveRows);
    wire.maxContinuousLiveRows = static_cast<std::uint32_t>(row.maxContinuousLiveRows);
    wire.maxTransitionRules = static_cast<std::uint32_t>(row.maxTransitionRules);
    wire.maxCurvePoints = static_cast<std::uint32_t>(row.maxCurvePoints);
    wire.maxKnownScheduleLeaves = static_cast<std::uint32_t>(row.maxKnownScheduleLeaves);
    wire.maxTrackSchedulePrograms = static_cast<std::uint32_t>(row.maxTrackSchedulePrograms);
    wire.maxSegmentSchedulePrograms = static_cast<std::uint32_t>(row.maxSegmentSchedulePrograms);
    wire.maxScheduleTrackRefsPerSegment = static_cast<std::uint32_t>(row.maxScheduleTrackRefsPerSegment);
    wire.maxScheduleTrackRefsTotal = static_cast<std::uint32_t>(row.maxScheduleTrackRefsTotal);
    wire.maxSchedulePlacementsPerTrack = static_cast<std::uint32_t>(row.maxSchedulePlacementsPerTrack);
    wire.maxSchedulePlacementsTotal = static_cast<std::uint32_t>(row.maxSchedulePlacementsTotal);
    wire.maxScheduleMarkersPerSegment = static_cast<std::uint32_t>(row.maxScheduleMarkersPerSegment);
    wire.maxScheduleMarkersTotal = static_cast<std::uint32_t>(row.maxScheduleMarkersTotal);
    wire.maxTrackSegmentScheduleDepth = static_cast<std::uint32_t>(row.maxTrackSegmentScheduleDepth);
}

[[nodiscard]] inline bool FromWire(const HkPackBodyWorldPrefix& wire, CurrentWorldV3PrefixValues& out) noexcept {
    AssignPack(out.schema, wire.schema);
    AssignPack(out.compiler, wire.compiler);
    AssignPack(out.selectionAlgorithm, wire.selectionAlgorithm);
    AssignPack(out.gridAlgorithm, wire.gridAlgorithm);
    AssignPack(out.transitionAlgorithm, wire.transitionAlgorithm);
    AssignPack(out.publicationProtocol, wire.publicationProtocol);
    AssignPack(out.runtimeSemanticFingerprint, wire.runtimeSemanticFingerprint);
    AssignPack(out.capabilityMask, wire.capabilityMask);
    AssignPack(out.variableCount, wire.variableCount);
    AssignPack(out.playablePropertyProgramCount, wire.playablePropertyProgramCount);
    AssignPack(out.playableOccurrencePropertyCount, wire.playableOccurrencePropertyCount);
    AssignPack(out.playablePropertySendCount, wire.playablePropertySendCount);
    AssignPack(out.reservedChildCount1, wire.reservedChildCount1);
    AssignPack(out.reservedChildCount2, wire.reservedChildCount2);
    AssignPack(out.playablePropertyLayerAncestorRefCount, wire.playablePropertyLayerAncestorRefCount);
    AssignPack(out.playablePropertyEffectNodeCount, wire.playablePropertyEffectNodeCount);
    AssignPack(out.playablePropertyEffectParamCount, wire.playablePropertyEffectParamCount);
    AssignPack(out.playablePropertyEffectEdgeCount, wire.playablePropertyEffectEdgeCount);
    AssignPack(out.trackScheduleCount, wire.trackScheduleCount);
    AssignPack(out.segmentScheduleCount, wire.segmentScheduleCount);
    AssignPack(out.scheduleTrackRefCount, wire.scheduleTrackRefCount);
    AssignPack(out.schedulePlacementCount, wire.schedulePlacementCount);
    AssignPack(out.scheduleMarkerCount, wire.scheduleMarkerCount);
    AssignPack(out.containerCount, wire.containerCount);
    AssignPack(out.transitionProgramCount, wire.transitionProgramCount);
    AssignPack(out.maxContainerDepth, wire.maxContainerDepth);
    AssignPack(out.maxChildrenPerContainer, wire.maxChildrenPerContainer);
    AssignPack(out.maxAvoidLastN, wire.maxAvoidLastN);
    AssignPack(out.maxDiscreteAxes, wire.maxDiscreteAxes);
    AssignPack(out.maxDiscreteRows, wire.maxDiscreteRows);
    AssignPack(out.maxDiscreteTrieNodes, wire.maxDiscreteTrieNodes);
    AssignPack(out.maxContinuousAxes, wire.maxContinuousAxes);
    AssignPack(out.maxContinuousRows, wire.maxContinuousRows);
    AssignPack(out.maxContinuousCandidates, wire.maxContinuousCandidates);
    AssignPack(out.maxContinuousActiveRows, wire.maxContinuousActiveRows);
    AssignPack(out.maxContinuousLiveRows, wire.maxContinuousLiveRows);
    AssignPack(out.maxTransitionRules, wire.maxTransitionRules);
    AssignPack(out.maxCurvePoints, wire.maxCurvePoints);
    AssignPack(out.maxKnownScheduleLeaves, wire.maxKnownScheduleLeaves);
    AssignPack(out.maxTrackSchedulePrograms, wire.maxTrackSchedulePrograms);
    AssignPack(out.maxSegmentSchedulePrograms, wire.maxSegmentSchedulePrograms);
    AssignPack(out.maxScheduleTrackRefsPerSegment, wire.maxScheduleTrackRefsPerSegment);
    AssignPack(out.maxScheduleTrackRefsTotal, wire.maxScheduleTrackRefsTotal);
    AssignPack(out.maxSchedulePlacementsPerTrack, wire.maxSchedulePlacementsPerTrack);
    AssignPack(out.maxSchedulePlacementsTotal, wire.maxSchedulePlacementsTotal);
    AssignPack(out.maxScheduleMarkersPerSegment, wire.maxScheduleMarkersPerSegment);
    AssignPack(out.maxScheduleMarkersTotal, wire.maxScheduleMarkersTotal);
    AssignPack(out.maxTrackSegmentScheduleDepth, wire.maxTrackSegmentScheduleDepth);
    return true;
}

inline void ToWire(const VnObjectId& row, HkPackBodyWorldProjectId& wire) noexcept {
    wire.value = ToPackId(row);
}

[[nodiscard]] inline bool FromWire(const HkPackBodyWorldProjectId& wire, VnObjectId& out) noexcept {
    AssignPack(out, FromPackId(wire.value));
    return true;
}

inline void ToWire(const PlayablePropertyProgramViewV1& row, HkPackBodyWorldPropertyHeader& wire) noexcept {
    wire.contentPropertyKind = static_cast<std::uint32_t>(row.contentPropertyKind);
    wire.authoredStableOrder = static_cast<std::uint32_t>(row.authoredStableOrder);
    wire.stableId = ToPackId(row.stableId);
    wire.programSemanticFingerprint = static_cast<std::uint64_t>(row.programSemanticFingerprint);
    wire.contentRevision = static_cast<std::uint64_t>(row.contentRevision);
    wire.routeStableId = ToPackId(row.routeStableId);
    wire.compatibilityFingerprint = static_cast<std::uint64_t>(row.compatibilityFingerprint);
    wire.organizationLayerAncestorCount = static_cast<std::uint32_t>(row.organizationLayerAncestorCount);
}

[[nodiscard]] inline bool FromWire(const HkPackBodyWorldPropertyHeader& wire, PlayablePropertyProgramViewV1& out) noexcept {
    AssignPack(out.contentPropertyKind, wire.contentPropertyKind);
    AssignPack(out.authoredStableOrder, wire.authoredStableOrder);
    AssignPack(out.stableId, FromPackId(wire.stableId));
    AssignPack(out.programSemanticFingerprint, wire.programSemanticFingerprint);
    AssignPack(out.contentRevision, wire.contentRevision);
    AssignPack(out.routeStableId, FromPackId(wire.routeStableId));
    AssignPack(out.compatibilityFingerprint, wire.compatibilityFingerprint);
    AssignPack(out.organizationLayerAncestorCount, wire.organizationLayerAncestorCount);
    if (wire.reserved != 0) return false;
    return true;
}

inline void ToWire(const TrackSegmentScheduleKeyV1& row, HkPackBodyWorldScheduleKey& wire) noexcept {
    wire.kind = static_cast<std::uint32_t>(row.kind);
    wire.reserved = static_cast<std::uint32_t>(row.reserved);
    wire.stableId = ToPackId(row.stableId);
    wire.scheduleSemanticFingerprint = static_cast<std::uint64_t>(row.scheduleSemanticFingerprint);
}

[[nodiscard]] inline bool FromWire(const HkPackBodyWorldScheduleKey& wire, TrackSegmentScheduleKeyV1& out) noexcept {
    AssignPack(out.kind, wire.kind);
    AssignPack(out.reserved, wire.reserved);
    AssignPack(out.stableId, FromPackId(wire.stableId));
    AssignPack(out.scheduleSemanticFingerprint, wire.scheduleSemanticFingerprint);
    return true;
}

inline void ToWire(const PackSegmentEntryTimeline& row, HkPackBodyWorldSegmentEntryTimeline& wire) noexcept {
    wire.stableMarkerId = ToPackId(row.stableMarkerId);
    wire.position = static_cast<std::uint64_t>(row.position);
    wire.contentStart = static_cast<std::uint64_t>(row.contentStart);
    wire.contentEnd = static_cast<std::uint64_t>(row.contentEnd);
    wire.nominalStart = static_cast<std::uint64_t>(row.nominalStart);
    wire.nominalEnd = static_cast<std::uint64_t>(row.nominalEnd);
}

[[nodiscard]] inline bool FromWire(const HkPackBodyWorldSegmentEntryTimeline& wire, PackSegmentEntryTimeline& out) noexcept {
    AssignPack(out.stableMarkerId, FromPackId(wire.stableMarkerId));
    AssignPack(out.position, wire.position);
    AssignPack(out.contentStart, wire.contentStart);
    AssignPack(out.contentEnd, wire.contentEnd);
    AssignPack(out.nominalStart, wire.nominalStart);
    AssignPack(out.nominalEnd, wire.nominalEnd);
    return true;
}

inline void ToWire(const SegmentScheduleProgramViewV1& row, HkPackBodyWorldSegmentHeader& wire) noexcept {
    ToWire(row.rootDefinition, wire.rootDefinition);
    wire.scheduleSemanticFingerprint = static_cast<std::uint64_t>(row.scheduleSemanticFingerprint);
    wire.timeKind = static_cast<std::uint32_t>(row.timeKind);
    wire.startPointKind = static_cast<std::uint32_t>(row.startPointKind);
    wire.startMarkerId = static_cast<std::uint32_t>(row.startMarkerId);
    wire.reserved = static_cast<std::uint32_t>(row.reserved);
    wire.microsPerQuarter = static_cast<std::uint64_t>(row.microsPerQuarter);
    wire.meterNumerator = static_cast<std::uint32_t>(row.meterNumerator);
    wire.meterDenominator = static_cast<std::uint32_t>(row.meterDenominator);
    wire.trackRefCount = static_cast<std::uint32_t>(row.trackRefCount);
    wire.markerCount = static_cast<std::uint32_t>(row.markerCount);
}

[[nodiscard]] inline bool FromWire(const HkPackBodyWorldSegmentHeader& wire, SegmentScheduleProgramViewV1& out) noexcept {
    if (!FromWire(wire.rootDefinition, out.rootDefinition)) return false;
    AssignPack(out.scheduleSemanticFingerprint, wire.scheduleSemanticFingerprint);
    AssignPack(out.timeKind, wire.timeKind);
    AssignPack(out.startPointKind, wire.startPointKind);
    AssignPack(out.startMarkerId, wire.startMarkerId);
    AssignPack(out.reserved, wire.reserved);
    AssignPack(out.microsPerQuarter, wire.microsPerQuarter);
    AssignPack(out.meterNumerator, wire.meterNumerator);
    AssignPack(out.meterDenominator, wire.meterDenominator);
    AssignPack(out.trackRefCount, wire.trackRefCount);
    AssignPack(out.markerCount, wire.markerCount);
    return true;
}

inline void ToWire(const ContainerWorldInstallViewV1& row, HkPackBodyWorldSignalCounts& wire) noexcept {
    wire.signalRelationCount = static_cast<std::uint32_t>(row.signalRelationCount);
    wire.signalReceiverCount = static_cast<std::uint32_t>(row.signalReceiverCount);
}

[[nodiscard]] inline bool FromWire(const HkPackBodyWorldSignalCounts& wire, ContainerWorldInstallViewV1& out) noexcept {
    AssignPack(out.signalRelationCount, wire.signalRelationCount);
    AssignPack(out.signalReceiverCount, wire.signalReceiverCount);
    return true;
}

inline void ToWire(const SignalEndpointRefV1& row, HkPackBodyWorldSignalEndpoint& wire) noexcept {
    wire.kind = static_cast<std::uint32_t>(row.kind);
    wire.reserved = static_cast<std::uint32_t>(row.reserved);
    wire.stableId = ToPackId(row.stableId);
    wire.expectedPropertyFingerprint = static_cast<std::uint64_t>(row.expectedPropertyFingerprint);
    wire.expectedProgramFingerprint = static_cast<std::uint64_t>(row.expectedProgramFingerprint);
}

[[nodiscard]] inline bool FromWire(const HkPackBodyWorldSignalEndpoint& wire, SignalEndpointRefV1& out) noexcept {
    AssignPack(out.kind, wire.kind);
    AssignPack(out.reserved, wire.reserved);
    AssignPack(out.stableId, FromPackId(wire.stableId));
    AssignPack(out.expectedPropertyFingerprint, wire.expectedPropertyFingerprint);
    AssignPack(out.expectedProgramFingerprint, wire.expectedProgramFingerprint);
    return true;
}

inline void ToWire(const SignalReceiverV1& row, HkPackBodyWorldSignalReceiver& wire) noexcept {
    ToWire(row.target, wire.target);
    wire.algorithm = static_cast<std::uint32_t>(row.algorithm);
    wire.attackMs = static_cast<float>(row.attackMs);
    wire.releaseMs = static_cast<float>(row.releaseMs);
    wire.reserved = static_cast<std::uint32_t>(row.reserved);
}

[[nodiscard]] inline bool FromWire(const HkPackBodyWorldSignalReceiver& wire, SignalReceiverV1& out) noexcept {
    if (!FromWire(wire.target, out.target)) return false;
    AssignPack(out.algorithm, wire.algorithm);
    AssignPack(out.attackMs, wire.attackMs);
    AssignPack(out.releaseMs, wire.releaseMs);
    AssignPack(out.reserved, wire.reserved);
    return true;
}

inline void ToWire(const SignalRelationV1& row, HkPackBodyWorldSignalRelation& wire) noexcept {
    wire.stableId = ToPackId(row.stableId);
    ToWire(row.source, wire.source);
    ToWire(row.target, wire.target);
    wire.consumer = static_cast<std::uint32_t>(row.consumer);
    wire.sendGainDb = static_cast<float>(row.sendGainDb);
}

[[nodiscard]] inline bool FromWire(const HkPackBodyWorldSignalRelation& wire, SignalRelationV1& out) noexcept {
    AssignPack(out.stableId, FromPackId(wire.stableId));
    if (!FromWire(wire.source, out.source)) return false;
    if (!FromWire(wire.target, out.target)) return false;
    AssignPack(out.consumer, wire.consumer);
    AssignPack(out.sendGainDb, wire.sendGainDb);
    return true;
}

inline void ToWire(const StructuralAncestorRefV1& row, HkPackBodyWorldStructuralAncestorHeader& wire) noexcept {
    wire.contentPropertyKind = static_cast<std::uint32_t>(row.contentPropertyKind);
    wire.reserved = static_cast<std::uint32_t>(row.reserved);
    wire.stableId = ToPackId(row.stableId);
    wire.programSemanticFingerprint = static_cast<std::uint64_t>(row.programSemanticFingerprint);
}

[[nodiscard]] inline bool FromWire(const HkPackBodyWorldStructuralAncestorHeader& wire, StructuralAncestorRefV1& out) noexcept {
    AssignPack(out.contentPropertyKind, wire.contentPropertyKind);
    AssignPack(out.reserved, wire.reserved);
    AssignPack(out.stableId, FromPackId(wire.stableId));
    AssignPack(out.programSemanticFingerprint, wire.programSemanticFingerprint);
    return true;
}

inline void ToWire(const std::uint32_t& row, HkPackBodyWorldStructuralCount& wire) noexcept {
    wire.value = static_cast<std::uint32_t>(row);
}

[[nodiscard]] inline bool FromWire(const HkPackBodyWorldStructuralCount& wire, std::uint32_t& out) noexcept {
    AssignPack(out, wire.value);
    return true;
}

inline void ToWire(const PlayablePropertyProgramViewV1& row, HkPackBodyWorldStructuralHeader& wire) noexcept {
    wire.structuralAncestorCount = static_cast<std::uint32_t>(row.structuralAncestorCount);
}

[[nodiscard]] inline bool FromWire(const HkPackBodyWorldStructuralHeader& wire, PlayablePropertyProgramViewV1& out) noexcept {
    AssignPack(out.structuralAncestorCount, wire.structuralAncestorCount);
    if (wire.reserved != 0) return false;
    return true;
}

inline void ToWire(const TrackScheduleProgramViewV1& row, HkPackBodyWorldTrackHeader& wire) noexcept {
    ToWire(row.rootDefinition, wire.rootDefinition);
    wire.scheduleSemanticFingerprint = static_cast<std::uint64_t>(row.scheduleSemanticFingerprint);
    wire.placementCount = static_cast<std::uint32_t>(row.placementCount);
    wire.reserved = static_cast<std::uint32_t>(row.reserved);
}

[[nodiscard]] inline bool FromWire(const HkPackBodyWorldTrackHeader& wire, TrackScheduleProgramViewV1& out) noexcept {
    if (!FromWire(wire.rootDefinition, out.rootDefinition)) return false;
    AssignPack(out.scheduleSemanticFingerprint, wire.scheduleSemanticFingerprint);
    AssignPack(out.placementCount, wire.placementCount);
    AssignPack(out.reserved, wire.reserved);
    return true;
}

inline void ToWire(const ScheduleTrackRefV1& row, HkPackBodyWorldTrackRef& wire) noexcept {
    ToWire(row.trackDefinition, wire.trackDefinition);
    wire.trackScheduleSemanticFingerprint = static_cast<std::uint64_t>(row.trackScheduleSemanticFingerprint);
    wire.stableTrackOrdinal = static_cast<std::uint32_t>(row.stableTrackOrdinal);
    wire.reserved = static_cast<std::uint32_t>(row.reserved);
}

[[nodiscard]] inline bool FromWire(const HkPackBodyWorldTrackRef& wire, ScheduleTrackRefV1& out) noexcept {
    if (!FromWire(wire.trackDefinition, out.trackDefinition)) return false;
    AssignPack(out.trackScheduleSemanticFingerprint, wire.trackScheduleSemanticFingerprint);
    AssignPack(out.stableTrackOrdinal, wire.stableTrackOrdinal);
    AssignPack(out.reserved, wire.reserved);
    return true;
}

inline void ToWire(const TransitionProgramViewV1& row, HkPackBodyWorldTransitionHeader& wire) noexcept {
    wire.ownerContainerStableId = ToPackId(row.ownerContainerStableId);
    wire.ruleCount = static_cast<std::uint32_t>(row.ruleCount);
    wire.defaultRuleIndex = static_cast<std::uint32_t>(row.defaultRuleIndex);
    wire.downwardPathStepCount = static_cast<std::uint32_t>(row.downwardPathStepCount);
    wire.maxContainerDescent = static_cast<std::uint32_t>(row.maxContainerDescent);
    wire.maxOwnerAscent = static_cast<std::uint32_t>(row.maxOwnerAscent);
    wire.watchFallback = static_cast<std::uint32_t>(row.watchFallback);
}

[[nodiscard]] inline bool FromWire(const HkPackBodyWorldTransitionHeader& wire, TransitionProgramViewV1& out) noexcept {
    AssignPack(out.ownerContainerStableId, FromPackId(wire.ownerContainerStableId));
    AssignPack(out.ruleCount, wire.ruleCount);
    AssignPack(out.defaultRuleIndex, wire.defaultRuleIndex);
    AssignPack(out.downwardPathStepCount, wire.downwardPathStepCount);
    AssignPack(out.maxContainerDescent, wire.maxContainerDescent);
    AssignPack(out.maxOwnerAscent, wire.maxOwnerAscent);
    AssignPack(out.watchFallback, wire.watchFallback);
    return true;
}

inline void ToWire(const TransitionRuleViewV1& row, HkPackBodyWorldTransitionRule& wire) noexcept {
    wire.stableRuleOrdinal = static_cast<std::uint32_t>(row.stableRuleOrdinal);
    wire.sourceAny = static_cast<std::uint8_t>(row.sourceAny);
    wire.targetAny = static_cast<std::uint8_t>(row.targetAny);
    wire.bridgePresent = static_cast<std::uint8_t>(row.bridgePresent);
    wire.reserved = static_cast<std::uint8_t>(row.reserved);
    ToWire(row.source, wire.source);
    ToWire(row.target, wire.target);
    wire.sourceExit = static_cast<std::uint32_t>(row.sourceExit);
    wire.targetEntry = static_cast<std::uint32_t>(row.targetEntry);
    ToWire(row.bridge, wire.bridge);
    ToWire(row.sourceFade, wire.sourceFade);
    ToWire(row.bridgeFadeIn, wire.bridgeFadeIn);
    ToWire(row.bridgeFadeOut, wire.bridgeFadeOut);
    ToWire(row.targetFade, wire.targetFade);
    wire.requiredClockFingerprint = static_cast<std::uint64_t>(row.requiredClockFingerprint);
    wire.requiredMarkerStableId = ToPackId(row.requiredMarkerStableId);
    wire.samePositionCompatibilityFingerprint = static_cast<std::uint64_t>(row.samePositionCompatibilityFingerprint);
    wire.authoredStableOrder = static_cast<std::uint32_t>(row.authoredStableOrder);
    wire.reserved2 = static_cast<std::uint32_t>(row.reserved2);
}

[[nodiscard]] inline bool FromWire(const HkPackBodyWorldTransitionRule& wire, TransitionRuleViewV1& out) noexcept {
    AssignPack(out.stableRuleOrdinal, wire.stableRuleOrdinal);
    AssignPack(out.sourceAny, wire.sourceAny);
    AssignPack(out.targetAny, wire.targetAny);
    AssignPack(out.bridgePresent, wire.bridgePresent);
    AssignPack(out.reserved, wire.reserved);
    if (!FromWire(wire.source, out.source)) return false;
    if (!FromWire(wire.target, out.target)) return false;
    AssignPack(out.sourceExit, wire.sourceExit);
    AssignPack(out.targetEntry, wire.targetEntry);
    if (!FromWire(wire.bridge, out.bridge)) return false;
    if (!FromWire(wire.sourceFade, out.sourceFade)) return false;
    if (!FromWire(wire.bridgeFadeIn, out.bridgeFadeIn)) return false;
    if (!FromWire(wire.bridgeFadeOut, out.bridgeFadeOut)) return false;
    if (!FromWire(wire.targetFade, out.targetFade)) return false;
    AssignPack(out.requiredClockFingerprint, wire.requiredClockFingerprint);
    AssignPack(out.requiredMarkerStableId, FromPackId(wire.requiredMarkerStableId));
    AssignPack(out.samePositionCompatibilityFingerprint, wire.samePositionCompatibilityFingerprint);
    AssignPack(out.authoredStableOrder, wire.authoredStableOrder);
    AssignPack(out.reserved2, wire.reserved2);
    return true;
}

inline void ToWire(const PackWorldVariableWire& row, HkPackBodyWorldVariableHeader& wire) noexcept {
    wire.variableId = ToPackId(row.variableId);
    wire.kind = static_cast<std::uint32_t>(row.kind);
    wire.scope = HK_PACK_BODY_WORLD_VARIABLE_HEADER_SCOPE;
    wire.rangeMin = static_cast<float>(row.rangeMin);
    wire.rangeMax = static_cast<float>(row.rangeMax);
    wire.defaultValue = static_cast<float>(row.defaultValue);
}

[[nodiscard]] inline bool FromWire(const HkPackBodyWorldVariableHeader& wire, PackWorldVariableWire& out) noexcept {
    AssignPack(out.variableId, FromPackId(wire.variableId));
    AssignPack(out.kind, wire.kind);
    if (wire.scope != HK_PACK_BODY_WORLD_VARIABLE_HEADER_SCOPE) return false;
    AssignPack(out.rangeMin, wire.rangeMin);
    AssignPack(out.rangeMax, wire.rangeMax);
    AssignPack(out.defaultValue, wire.defaultValue);
    if (wire.reserved != 0) return false;
    return true;
}

inline void ToWire(const PackBusEntry& row, HkPackBus& wire) noexcept {
    wire.busId = ToPackId(row.busId);
    wire.parentBusId = ToPackId(row.parentBusId);
    wire.volume = static_cast<float>(row.volume);
    wire.outputBusVolume = static_cast<float>(row.outputBusVolume);
}

[[nodiscard]] inline bool FromWire(const HkPackBus& wire, PackBusEntry& out) noexcept {
    AssignPack(out.busId, FromPackId(wire.busId));
    AssignPack(out.parentBusId, FromPackId(wire.parentBusId));
    AssignPack(out.volume, wire.volume);
    AssignPack(out.outputBusVolume, wire.outputBusVolume);
    return true;
}

inline void ToWire(const PackBusDefinitionBus& row, HkPackBusDefinitionBus& wire) noexcept {
    wire.busId = ToPackId(row.busId);
    wire.parentId = ToPackId(row.parentId);
    wire.feedsFirstOrdinal = static_cast<std::uint32_t>(row.feedsFirstOrdinal);
    wire.faderDb = static_cast<float>(row.faderDb);
    wire.outputBusVolumeDb = static_cast<float>(row.outputBusVolumeDb);
    wire.faderAfterSlot = static_cast<std::uint32_t>(row.faderAfterSlot);
    wire.outputLayout = static_cast<std::uint32_t>(row.outputLayout);
    wire.effectFirst = static_cast<std::uint32_t>(row.effectFirst);
    wire.effectCount = static_cast<std::uint32_t>(row.effectCount);
    wire.downmixFirst = static_cast<std::uint32_t>(row.downmixFirst);
    wire.downmixCount = static_cast<std::uint32_t>(row.downmixCount);
}

[[nodiscard]] inline bool FromWire(const HkPackBusDefinitionBus& wire, PackBusDefinitionBus& out) noexcept {
    AssignPack(out.busId, FromPackId(wire.busId));
    AssignPack(out.parentId, FromPackId(wire.parentId));
    AssignPack(out.feedsFirstOrdinal, wire.feedsFirstOrdinal);
    AssignPack(out.faderDb, wire.faderDb);
    AssignPack(out.outputBusVolumeDb, wire.outputBusVolumeDb);
    AssignPack(out.faderAfterSlot, wire.faderAfterSlot);
    AssignPack(out.outputLayout, wire.outputLayout);
    AssignPack(out.effectFirst, wire.effectFirst);
    AssignPack(out.effectCount, wire.effectCount);
    AssignPack(out.downmixFirst, wire.downmixFirst);
    AssignPack(out.downmixCount, wire.downmixCount);
    if (wire.reserved0 != 0) return false;
    return true;
}

inline void ToWire(const float& row, HkPackBusDefinitionCoefficient& wire) noexcept {
    wire.value = static_cast<float>(row);
}

[[nodiscard]] inline bool FromWire(const HkPackBusDefinitionCoefficient& wire, float& out) noexcept {
    AssignPack(out, wire.value);
    return true;
}

inline void ToWire(const PackBusDefinitionEffect& row, HkPackBusDefinitionEffect& wire) noexcept {
    wire.busId = ToPackId(row.busId);
    wire.effectId = ToPackId(row.effectId);
    wire.slot = static_cast<std::uint32_t>(row.slot);
    wire.effectType = static_cast<std::uint32_t>(row.effectType);
    wire.descriptorVintage = static_cast<std::uint32_t>(row.descriptorVintage);
    wire.flags = static_cast<std::uint32_t>(row.flags);
    wire.parameterFirst = static_cast<std::uint32_t>(row.parameterFirst);
    wire.parameterCount = static_cast<std::uint32_t>(row.parameterCount);
}

[[nodiscard]] inline bool FromWire(const HkPackBusDefinitionEffect& wire, PackBusDefinitionEffect& out) noexcept {
    AssignPack(out.busId, FromPackId(wire.busId));
    AssignPack(out.effectId, FromPackId(wire.effectId));
    AssignPack(out.slot, wire.slot);
    AssignPack(out.effectType, wire.effectType);
    AssignPack(out.descriptorVintage, wire.descriptorVintage);
    AssignPack(out.flags, wire.flags);
    AssignPack(out.parameterFirst, wire.parameterFirst);
    AssignPack(out.parameterCount, wire.parameterCount);
    return true;
}

inline void ToWire(const PackBusDefinitionEffectParameter& row, HkPackBusDefinitionEffectParameter& wire) noexcept {
    wire.index = static_cast<std::uint32_t>(row.index);
    wire.value = static_cast<float>(row.value);
}

[[nodiscard]] inline bool FromWire(const HkPackBusDefinitionEffectParameter& wire, PackBusDefinitionEffectParameter& out) noexcept {
    AssignPack(out.index, wire.index);
    AssignPack(out.value, wire.value);
    return true;
}

inline void ToWire(const PackBusDefinitionDownmix& row, HkPackBusDefinitionDownmix& wire) noexcept {
    wire.busId = ToPackId(row.busId);
    wire.sourceLayout = static_cast<std::uint32_t>(row.sourceLayout);
    wire.destinationLayout = static_cast<std::uint32_t>(row.destinationLayout);
    wire.coefficientFirst = static_cast<std::uint32_t>(row.coefficientFirst);
    wire.coefficientCount = static_cast<std::uint32_t>(row.coefficientCount);
}

[[nodiscard]] inline bool FromWire(const HkPackBusDefinitionDownmix& wire, PackBusDefinitionDownmix& out) noexcept {
    AssignPack(out.busId, FromPackId(wire.busId));
    AssignPack(out.sourceLayout, wire.sourceLayout);
    AssignPack(out.destinationLayout, wire.destinationLayout);
    AssignPack(out.coefficientFirst, wire.coefficientFirst);
    AssignPack(out.coefficientCount, wire.coefficientCount);
    if (wire.reserved0 != 0) return false;
    return true;
}

inline void ToWire(const PackBusDefinitionSend& row, HkPackBusDefinitionSend& wire) noexcept {
    wire.source = ToPackId(row.source);
    wire.target = ToPackId(row.target);
    wire.levelDb = static_cast<float>(row.levelDb);
}

[[nodiscard]] inline bool FromWire(const HkPackBusDefinitionSend& wire, PackBusDefinitionSend& out) noexcept {
    AssignPack(out.source, FromPackId(wire.source));
    AssignPack(out.target, FromPackId(wire.target));
    AssignPack(out.levelDb, wire.levelDb);
    if (wire.reserved0 != 0) return false;
    if (wire.reserved1 != 0) return false;
    return true;
}

inline void ToWire(const PackBusDefinitionSnapshotHeader& row, HkPackBusDefinitionSnapshotHeader& wire) noexcept {
    wire.schemaVersion = static_cast<std::uint32_t>(row.schemaVersion);
    wire.storageModelVersion = static_cast<std::uint32_t>(row.storageModelVersion);
    wire.headerBytes = static_cast<std::uint32_t>(row.headerBytes);
    wire.publicationCapabilityVintage = static_cast<std::uint32_t>(row.publicationCapabilityVintage);
    wire.contentGeneration = static_cast<std::uint64_t>(row.contentGeneration);
    wire.publishedBusFingerprint = static_cast<std::uint64_t>(row.publishedBusFingerprint);
    wire.busCount = static_cast<std::uint32_t>(row.busCount);
    wire.sendCount = static_cast<std::uint32_t>(row.sendCount);
    wire.effectCount = static_cast<std::uint32_t>(row.effectCount);
    wire.effectParameterCount = static_cast<std::uint32_t>(row.effectParameterCount);
    wire.downmixCount = static_cast<std::uint32_t>(row.downmixCount);
    wire.coefficientCount = static_cast<std::uint32_t>(row.coefficientCount);
    wire.busOffset = static_cast<std::uint64_t>(row.busOffset);
    wire.sendOffset = static_cast<std::uint64_t>(row.sendOffset);
    wire.effectOffset = static_cast<std::uint64_t>(row.effectOffset);
    wire.effectParameterOffset = static_cast<std::uint64_t>(row.effectParameterOffset);
    wire.downmixOffset = static_cast<std::uint64_t>(row.downmixOffset);
    wire.coefficientOffset = static_cast<std::uint64_t>(row.coefficientOffset);
    wire.payloadBytes = static_cast<std::uint64_t>(row.payloadBytes);
    wire.checksum = static_cast<std::uint64_t>(row.checksum);
    wire.compileTargetProfileFingerprint = static_cast<std::uint64_t>(row.compileTargetProfileFingerprint);
    wire.profileVintage = static_cast<std::uint32_t>(row.profileVintage);
    wire.renderSampleRateHz = static_cast<std::uint32_t>(row.renderSampleRateHz);
    wire.mixLayout = static_cast<std::uint32_t>(row.mixLayout);
    wire.deviceLayout = static_cast<std::uint32_t>(row.deviceLayout);
    wire.runtimeCapabilityMask = static_cast<std::uint64_t>(row.runtimeCapabilityMask);
    wire.reserved0 = static_cast<std::uint64_t>(row.reserved0);
    wire.busRowBytes = static_cast<std::uint32_t>(row.busRowBytes);
    wire.sendRowBytes = static_cast<std::uint32_t>(row.sendRowBytes);
    wire.downmixRowBytes = static_cast<std::uint32_t>(row.downmixRowBytes);
    wire.coefficientRowBytes = static_cast<std::uint32_t>(row.coefficientRowBytes);
    wire.reserved1 = static_cast<std::uint64_t>(row.reserved1);
}

[[nodiscard]] inline bool FromWire(const HkPackBusDefinitionSnapshotHeader& wire, PackBusDefinitionSnapshotHeader& out) noexcept {
    AssignPack(out.schemaVersion, wire.schemaVersion);
    AssignPack(out.storageModelVersion, wire.storageModelVersion);
    AssignPack(out.headerBytes, wire.headerBytes);
    AssignPack(out.publicationCapabilityVintage, wire.publicationCapabilityVintage);
    AssignPack(out.contentGeneration, wire.contentGeneration);
    AssignPack(out.publishedBusFingerprint, wire.publishedBusFingerprint);
    AssignPack(out.busCount, wire.busCount);
    AssignPack(out.sendCount, wire.sendCount);
    AssignPack(out.effectCount, wire.effectCount);
    AssignPack(out.effectParameterCount, wire.effectParameterCount);
    AssignPack(out.downmixCount, wire.downmixCount);
    AssignPack(out.coefficientCount, wire.coefficientCount);
    AssignPack(out.busOffset, wire.busOffset);
    AssignPack(out.sendOffset, wire.sendOffset);
    AssignPack(out.effectOffset, wire.effectOffset);
    AssignPack(out.effectParameterOffset, wire.effectParameterOffset);
    AssignPack(out.downmixOffset, wire.downmixOffset);
    AssignPack(out.coefficientOffset, wire.coefficientOffset);
    AssignPack(out.payloadBytes, wire.payloadBytes);
    AssignPack(out.checksum, wire.checksum);
    AssignPack(out.compileTargetProfileFingerprint, wire.compileTargetProfileFingerprint);
    AssignPack(out.profileVintage, wire.profileVintage);
    AssignPack(out.renderSampleRateHz, wire.renderSampleRateHz);
    AssignPack(out.mixLayout, wire.mixLayout);
    AssignPack(out.deviceLayout, wire.deviceLayout);
    AssignPack(out.runtimeCapabilityMask, wire.runtimeCapabilityMask);
    AssignPack(out.reserved0, wire.reserved0);
    AssignPack(out.busRowBytes, wire.busRowBytes);
    AssignPack(out.sendRowBytes, wire.sendRowBytes);
    AssignPack(out.downmixRowBytes, wire.downmixRowBytes);
    AssignPack(out.coefficientRowBytes, wire.coefficientRowBytes);
    AssignPack(out.reserved1, wire.reserved1);
    return true;
}

inline void ToWire(const PackBusEffectEntry& row, HkPackBusEffect& wire) noexcept {
    wire.busId = ToPackId(row.busId);
    wire.slot = static_cast<std::uint32_t>(row.slot);
    wire.effectType = static_cast<std::uint32_t>(row.effectType);
    wire.descriptorVintage = static_cast<std::uint32_t>(row.descriptorVintage);
    wire.bypass = static_cast<std::uint32_t>(row.bypass);
    wire.faderPointGuard = static_cast<std::uint32_t>(row.faderPointGuard);
    wire.paramCount = static_cast<std::uint32_t>(row.paramCount);
    wire.paramBytes = static_cast<std::uint32_t>(row.paramBytes);
    static_assert(std::size(std::remove_cvref_t<decltype(row.params)>{}) == HK_PACK_VOICE_RECIPE_MAX_PARAMS);
    for (std::uint64_t i = 0; i < HK_PACK_VOICE_RECIPE_MAX_PARAMS; ++i) {
        if (i < row.paramCount) {
            wire.params[i] = static_cast<float>(row.params[i]);
        }
    }
    wire.effectId = ToPackId(row.effectId);
}

[[nodiscard]] inline bool FromWire(const HkPackBusEffect& wire, PackBusEffectEntry& out) noexcept {
    AssignPack(out.busId, FromPackId(wire.busId));
    AssignPack(out.slot, wire.slot);
    AssignPack(out.effectType, wire.effectType);
    AssignPack(out.descriptorVintage, wire.descriptorVintage);
    AssignPack(out.bypass, wire.bypass);
    AssignPack(out.faderPointGuard, wire.faderPointGuard);
    AssignPack(out.paramCount, wire.paramCount);
    AssignPack(out.paramBytes, wire.paramBytes);
    if (wire.reserved44 != 0) return false;
    static_assert(std::size(std::remove_cvref_t<decltype(out.params)>{}) == HK_PACK_VOICE_RECIPE_MAX_PARAMS);
    for (std::uint64_t i = 0; i < HK_PACK_VOICE_RECIPE_MAX_PARAMS; ++i) {
        if (i < out.paramCount) {
            AssignPack(out.params[i], wire.params[i]);
        } else {
            if (!PackZero(wire.params[i])) return false;
            out.params[i] = {};
        }
    }
    AssignPack(out.effectId, FromPackId(wire.effectId));
    return true;
}

inline void ToWire(const PackBusSendEntry& row, HkPackBusSend& wire) noexcept {
    wire.fromBusId = ToPackId(row.fromBusId);
    wire.toBusId = ToPackId(row.toBusId);
    wire.levelDb = static_cast<float>(row.levelDb);
}

[[nodiscard]] inline bool FromWire(const HkPackBusSend& wire, PackBusSendEntry& out) noexcept {
    AssignPack(out.fromBusId, FromPackId(wire.fromBusId));
    AssignPack(out.toBusId, FromPackId(wire.toBusId));
    AssignPack(out.levelDb, wire.levelDb);
    if (wire.reserved0 != 0) return false;
    return true;
}

inline void ToWire(const PackCanonicalPartitionHeader& row, HkPackCanonicalPartitionHeader& wire) noexcept {
    std::memcpy(wire.magic, HK_PACK_CANONICAL_PARTITION_HEADER_MAGIC, sizeof wire.magic);
    wire.headerBytes = static_cast<std::uint32_t>(row.headerBytes);
    wire.bodyVintage = static_cast<std::uint32_t>(row.bodyVintage);
    wire.bodyBytes = static_cast<std::uint64_t>(row.bodyBytes);
    wire.bodyChecksum = static_cast<std::uint64_t>(row.bodyChecksum);
    wire.packStableId = static_cast<std::uint64_t>(row.packStableId);
    wire.partitionRevision = static_cast<std::uint64_t>(row.partitionRevision);
    wire.ownedDefinitionCount = static_cast<std::uint64_t>(row.ownedDefinitionCount);
    wire.busContentGeneration = static_cast<std::uint64_t>(row.busContentGeneration);
    wire.publishedBusFingerprint = static_cast<std::uint64_t>(row.publishedBusFingerprint);
    wire.compileTargetProfileFingerprint = static_cast<std::uint64_t>(row.compileTargetProfileFingerprint);
    wire.headerChecksum = static_cast<std::uint64_t>(row.headerChecksum);
}

[[nodiscard]] inline bool FromWire(const HkPackCanonicalPartitionHeader& wire, PackCanonicalPartitionHeader& out) noexcept {
    if (std::memcmp(wire.magic, HK_PACK_CANONICAL_PARTITION_HEADER_MAGIC, sizeof wire.magic) != 0) return false;
    AssignPack(out.headerBytes, wire.headerBytes);
    AssignPack(out.bodyVintage, wire.bodyVintage);
    AssignPack(out.bodyBytes, wire.bodyBytes);
    AssignPack(out.bodyChecksum, wire.bodyChecksum);
    AssignPack(out.packStableId, wire.packStableId);
    AssignPack(out.partitionRevision, wire.partitionRevision);
    AssignPack(out.ownedDefinitionCount, wire.ownedDefinitionCount);
    AssignPack(out.busContentGeneration, wire.busContentGeneration);
    AssignPack(out.publishedBusFingerprint, wire.publishedBusFingerprint);
    AssignPack(out.compileTargetProfileFingerprint, wire.compileTargetProfileFingerprint);
    AssignPack(out.headerChecksum, wire.headerChecksum);
    return true;
}

inline void ToWire(const PackContainerChild& row, HkPackContainerChild& wire) noexcept {
    wire.playableId = ToPackId(row.playableId);
    wire.weight = static_cast<std::uint32_t>(row.weight);
    wire.matchValue = static_cast<std::uint32_t>(row.matchValue);
    wire.delayMs = static_cast<std::uint32_t>(row.delayMs);
    wire.delayJitterMs = static_cast<std::uint32_t>(row.delayJitterMs);
}

[[nodiscard]] inline bool FromWire(const HkPackContainerChild& wire, PackContainerChild& out) noexcept {
    AssignPack(out.playableId, FromPackId(wire.playableId));
    AssignPack(out.weight, wire.weight);
    AssignPack(out.matchValue, wire.matchValue);
    AssignPack(out.delayMs, wire.delayMs);
    AssignPack(out.delayJitterMs, wire.delayJitterMs);
    return true;
}

inline void ToWire(const PackContainerEntryHeader& row, HkPackContainerHeader& wire) noexcept {
    wire.containerId = ToPackId(row.containerId);
    wire.selectionOrder = static_cast<std::uint32_t>(row.selectionOrder);
    wire.avoidRepeatWindow = static_cast<std::uint32_t>(row.avoidRepeatWindow);
    wire.selectionVariableId = ToPackId(row.selectionVariableId);
    wire.defaultIndex = static_cast<std::uint32_t>(row.defaultIndex);
    wire.childCount = static_cast<std::uint32_t>(row.childCount);
}

[[nodiscard]] inline bool FromWire(const HkPackContainerHeader& wire, PackContainerEntryHeader& out) noexcept {
    AssignPack(out.containerId, FromPackId(wire.containerId));
    AssignPack(out.selectionOrder, wire.selectionOrder);
    AssignPack(out.avoidRepeatWindow, wire.avoidRepeatWindow);
    AssignPack(out.selectionVariableId, FromPackId(wire.selectionVariableId));
    AssignPack(out.defaultIndex, wire.defaultIndex);
    AssignPack(out.childCount, wire.childCount);
    return true;
}

inline void ToWire(const PackDependencyEdge& row, HkPackDependencyEdge& wire) noexcept {
    wire.prerequisitePackStableId = static_cast<std::uint64_t>(row.prerequisitePackStableId);
    wire.edgeKind = static_cast<std::uint32_t>(row.edgeKind);
    wire.required01 = static_cast<std::uint32_t>(row.required01);
    wire.expectedPrerequisiteContentRevision = static_cast<std::uint64_t>(row.expectedPrerequisiteContentRevision);
    wire.expectedBusContentGeneration = static_cast<std::uint64_t>(row.expectedBusContentGeneration);
    wire.expectedPublishedBusFingerprint = static_cast<std::uint64_t>(row.expectedPublishedBusFingerprint);
    wire.expectedCompileTargetProfileFingerprint = static_cast<std::uint64_t>(row.expectedCompileTargetProfileFingerprint);
}

[[nodiscard]] inline bool FromWire(const HkPackDependencyEdge& wire, PackDependencyEdge& out) noexcept {
    AssignPack(out.prerequisitePackStableId, wire.prerequisitePackStableId);
    AssignPack(out.edgeKind, wire.edgeKind);
    AssignPack(out.required01, wire.required01);
    AssignPack(out.expectedPrerequisiteContentRevision, wire.expectedPrerequisiteContentRevision);
    AssignPack(out.expectedBusContentGeneration, wire.expectedBusContentGeneration);
    AssignPack(out.expectedPublishedBusFingerprint, wire.expectedPublishedBusFingerprint);
    AssignPack(out.expectedCompileTargetProfileFingerprint, wire.expectedCompileTargetProfileFingerprint);
    return true;
}

inline void ToWire(const PackDirectoryEntry& row, HkPackDirectoryEntry& wire) noexcept {
    wire.kind = static_cast<std::uint32_t>(row.kind);
    wire.vintage = static_cast<std::uint32_t>(row.vintage);
    wire.id = ToPackId(row.id);
    wire.offset = static_cast<std::uint64_t>(row.offset);
    wire.size = static_cast<std::uint64_t>(row.size);
    wire.payloadChecksum = static_cast<std::uint64_t>(row.payloadChecksum);
    wire.relationOrdinal = static_cast<std::uint64_t>(row.relationOrdinal);
}

[[nodiscard]] inline bool FromWire(const HkPackDirectoryEntry& wire, PackDirectoryEntry& out) noexcept {
    AssignPack(out.kind, wire.kind);
    AssignPack(out.vintage, wire.vintage);
    AssignPack(out.id, FromPackId(wire.id));
    AssignPack(out.offset, wire.offset);
    AssignPack(out.size, wire.size);
    AssignPack(out.payloadChecksum, wire.payloadChecksum);
    AssignPack(out.relationOrdinal, wire.relationOrdinal);
    return true;
}

inline void ToWire(const PackEffectDynamicRouteEntry& row, HkPackEffectDynamicRoute& wire) noexcept {
    wire.targetBusId = ToPackId(row.targetBusId);
    wire.effectSlot = static_cast<std::uint32_t>(row.effectSlot);
    wire.routeSlot = static_cast<std::uint32_t>(row.routeSlot);
    wire.expectedEffectType = static_cast<std::uint32_t>(row.expectedEffectType);
    wire.signalForm = static_cast<std::uint32_t>(row.signalForm);
    wire.sourceKind = static_cast<std::uint32_t>(row.sourceKind);
    wire.tap = static_cast<std::uint32_t>(row.tap);
    wire.sourceId = ToPackId(row.sourceId);
    wire.levelDb = static_cast<float>(row.levelDb);
}

[[nodiscard]] inline bool FromWire(const HkPackEffectDynamicRoute& wire, PackEffectDynamicRouteEntry& out) noexcept {
    AssignPack(out.targetBusId, FromPackId(wire.targetBusId));
    AssignPack(out.effectSlot, wire.effectSlot);
    AssignPack(out.routeSlot, wire.routeSlot);
    AssignPack(out.expectedEffectType, wire.expectedEffectType);
    AssignPack(out.signalForm, wire.signalForm);
    AssignPack(out.sourceKind, wire.sourceKind);
    AssignPack(out.tap, wire.tap);
    AssignPack(out.sourceId, FromPackId(wire.sourceId));
    AssignPack(out.levelDb, wire.levelDb);
    if (wire.reserved60 != 0) return false;
    if (wire.reserved64 != 0) return false;
    return true;
}

inline void ToWire(const PackEffectPresetHeader& row, HkPackEffectPresetHeader& wire) noexcept {
    wire.presetId = ToPackId(row.presetId);
    wire.effectType = static_cast<std::uint32_t>(row.effectType);
    wire.descriptorVintage = static_cast<std::uint32_t>(row.descriptorVintage);
    wire.paramCount = static_cast<std::uint32_t>(row.paramCount);
}

[[nodiscard]] inline bool FromWire(const HkPackEffectPresetHeader& wire, PackEffectPresetHeader& out) noexcept {
    AssignPack(out.presetId, FromPackId(wire.presetId));
    AssignPack(out.effectType, wire.effectType);
    AssignPack(out.descriptorVintage, wire.descriptorVintage);
    AssignPack(out.paramCount, wire.paramCount);
    return true;
}

inline void ToWire(const PackEffectPresetParameter& row, HkPackEffectPresetParameter& wire) noexcept {
    wire.paramId = static_cast<std::uint32_t>(row.paramId);
    wire.valueBits = static_cast<std::uint32_t>(row.valueBits);
}

[[nodiscard]] inline bool FromWire(const HkPackEffectPresetParameter& wire, PackEffectPresetParameter& out) noexcept {
    AssignPack(out.paramId, wire.paramId);
    AssignPack(out.valueBits, wire.valueBits);
    return true;
}

inline void ToWire(const PackFileHeader& row, HkPackFileHeader& wire) noexcept {
    std::memcpy(wire.magic, HK_PACK_MAGIC, sizeof wire.magic);
    wire.formatVersion = static_cast<std::uint32_t>(row.formatVersion);
    wire.headerBytes = static_cast<std::uint32_t>(row.headerBytes);
    wire.fileBytes = static_cast<std::uint64_t>(row.fileBytes);
    wire.packKind = static_cast<std::uint32_t>(row.packKind);
    wire.entryCount = static_cast<std::uint32_t>(row.entryCount);
    wire.directoryOffset = static_cast<std::uint64_t>(row.directoryOffset);
    wire.directoryChecksum = static_cast<std::uint64_t>(row.directoryChecksum);
}

[[nodiscard]] inline bool FromWire(const HkPackFileHeader& wire, PackFileHeader& out) noexcept {
    if (std::memcmp(wire.magic, HK_PACK_MAGIC, sizeof wire.magic) != 0) return false;
    AssignPack(out.formatVersion, wire.formatVersion);
    AssignPack(out.headerBytes, wire.headerBytes);
    AssignPack(out.fileBytes, wire.fileBytes);
    AssignPack(out.packKind, wire.packKind);
    AssignPack(out.entryCount, wire.entryCount);
    AssignPack(out.directoryOffset, wire.directoryOffset);
    AssignPack(out.directoryChecksum, wire.directoryChecksum);
    return true;
}

[[nodiscard]] inline bool FromWire(const HkPackFilePrefix& wire, PackFilePrefix& out) noexcept {
    if (std::memcmp(wire.magic, HK_PACK_MAGIC, sizeof wire.magic) != 0) return false;
    AssignPack(out.formatVersion, wire.formatVersion);
    AssignPack(out.headerBytes, wire.headerBytes);
    return true;
}

inline void ToWire(const PackTocEntry& row, HkPackMedia& wire) noexcept {
    wire.soundId = ToPackId(row.soundId);
    wire.sampleRate = static_cast<std::uint32_t>(row.sampleRate);
    wire.channelCount = static_cast<std::uint16_t>(row.channelCount);
    wire.sampleFormat = static_cast<std::uint16_t>(row.sampleFormat);
    wire.frameCount = static_cast<std::uint64_t>(row.frameCount);
    wire.blobOffset = static_cast<std::uint64_t>(row.blobOffset);
    wire.blobBytes = static_cast<std::uint64_t>(row.blobBytes);
    wire.blobChecksum = static_cast<std::uint64_t>(row.blobChecksum);
    wire.flags = static_cast<std::uint32_t>(row.flags);
    wire.loopStartFrame = static_cast<std::uint64_t>(row.loopStartFrame);
    wire.loopEndFrame = static_cast<std::uint64_t>(row.loopEndFrame);
}

[[nodiscard]] inline bool FromWire(const HkPackMedia& wire, PackTocEntry& out) noexcept {
    AssignPack(out.soundId, FromPackId(wire.soundId));
    AssignPack(out.sampleRate, wire.sampleRate);
    AssignPack(out.channelCount, wire.channelCount);
    AssignPack(out.sampleFormat, wire.sampleFormat);
    AssignPack(out.frameCount, wire.frameCount);
    AssignPack(out.blobOffset, wire.blobOffset);
    AssignPack(out.blobBytes, wire.blobBytes);
    AssignPack(out.blobChecksum, wire.blobChecksum);
    AssignPack(out.flags, wire.flags);
    AssignPack(out.loopStartFrame, wire.loopStartFrame);
    AssignPack(out.loopEndFrame, wire.loopEndFrame);
    return true;
}

inline void ToWire(const PackMediaFileHeader& row, HkPackMediaFileHeader& wire) noexcept {
    std::memcpy(wire.magic, HK_PACK_MAGIC, sizeof wire.magic);
    wire.formatVersion = static_cast<std::uint32_t>(row.formatVersion);
    wire.headerBytes = static_cast<std::uint32_t>(row.headerBytes);
    wire.fileBytes = static_cast<std::uint64_t>(row.fileBytes);
    wire.entryCount = static_cast<std::uint32_t>(row.entryCount);
    wire.tocOffset = static_cast<std::uint32_t>(row.tocOffset);
    wire.tocChecksum = static_cast<std::uint64_t>(row.tocChecksum);
}

[[nodiscard]] inline bool FromWire(const HkPackMediaFileHeader& wire, PackMediaFileHeader& out) noexcept {
    if (std::memcmp(wire.magic, HK_PACK_MAGIC, sizeof wire.magic) != 0) return false;
    AssignPack(out.formatVersion, wire.formatVersion);
    AssignPack(out.headerBytes, wire.headerBytes);
    AssignPack(out.fileBytes, wire.fileBytes);
    AssignPack(out.entryCount, wire.entryCount);
    AssignPack(out.tocOffset, wire.tocOffset);
    AssignPack(out.tocChecksum, wire.tocChecksum);
    return true;
}

inline void ToWire(const StreamHeader& row, HkPackStreamFileHeader& wire) noexcept {
    std::memcpy(wire.magic, HK_PACK_STREAM_FILE_MAGIC, sizeof wire.magic);
    wire.formatVersion = static_cast<std::uint32_t>(row.formatVersion);
    wire.headerBytes = static_cast<std::uint32_t>(row.headerBytes);
    wire.fileBytes = static_cast<std::uint64_t>(row.fileBytes);
    wire.entryCount = static_cast<std::uint32_t>(row.entryCount);
    wire.tocOffset = static_cast<std::uint32_t>(row.tocOffset);
    wire.chunkFrames = static_cast<std::uint32_t>(row.chunkFrames);
    wire.tocChecksum = static_cast<std::uint64_t>(row.tocChecksum);
}

[[nodiscard]] inline bool FromWire(const HkPackStreamFileHeader& wire, StreamHeader& out) noexcept {
    if (std::memcmp(wire.magic, HK_PACK_STREAM_FILE_MAGIC, sizeof wire.magic) != 0) return false;
    AssignPack(out.formatVersion, wire.formatVersion);
    AssignPack(out.headerBytes, wire.headerBytes);
    AssignPack(out.fileBytes, wire.fileBytes);
    AssignPack(out.entryCount, wire.entryCount);
    AssignPack(out.tocOffset, wire.tocOffset);
    AssignPack(out.chunkFrames, wire.chunkFrames);
    AssignPack(out.tocChecksum, wire.tocChecksum);
    return true;
}

inline void ToWire(const StreamTocEntry& row, HkPackStreamFileTocEntry& wire) noexcept {
    wire.soundId = ToPackId(row.soundId);
    wire.sampleRate = static_cast<std::uint32_t>(row.sampleRate);
    wire.channelCount = static_cast<std::uint16_t>(row.channelCount);
    wire.sampleFormat = static_cast<std::uint16_t>(row.sampleFormat);
    wire.frameCount = static_cast<std::uint64_t>(row.frameCount);
    wire.payloadOffset = static_cast<std::uint64_t>(row.payloadOffset);
    wire.payloadBytes = static_cast<std::uint64_t>(row.payloadBytes);
    wire.chunkChecksumOffset = static_cast<std::uint64_t>(row.chunkChecksumOffset);
    wire.chunkCount = static_cast<std::uint32_t>(row.chunkCount);
    wire.flags = static_cast<std::uint32_t>(row.flags);
    wire.loopStartFrame = static_cast<std::uint64_t>(row.loopStartFrame);
    wire.loopEndFrame = static_cast<std::uint64_t>(row.loopEndFrame);
}

[[nodiscard]] inline bool FromWire(const HkPackStreamFileTocEntry& wire, StreamTocEntry& out) noexcept {
    AssignPack(out.soundId, FromPackId(wire.soundId));
    AssignPack(out.sampleRate, wire.sampleRate);
    AssignPack(out.channelCount, wire.channelCount);
    AssignPack(out.sampleFormat, wire.sampleFormat);
    AssignPack(out.frameCount, wire.frameCount);
    AssignPack(out.payloadOffset, wire.payloadOffset);
    AssignPack(out.payloadBytes, wire.payloadBytes);
    AssignPack(out.chunkChecksumOffset, wire.chunkChecksumOffset);
    AssignPack(out.chunkCount, wire.chunkCount);
    AssignPack(out.flags, wire.flags);
    AssignPack(out.loopStartFrame, wire.loopStartFrame);
    AssignPack(out.loopEndFrame, wire.loopEndFrame);
    return true;
}

inline void ToWire(const PackRequirementBlock& row, HkPackRequirementBlock& wire) noexcept {
    wire.schemaVersion = static_cast<std::uint32_t>(row.schemaVersion);
    wire.storageModelVersion = static_cast<std::uint32_t>(row.storageModelVersion);
    wire.packStableId = static_cast<std::uint64_t>(row.packStableId);
    wire.packContentRevision = static_cast<std::uint64_t>(row.packContentRevision);
    wire.definitionPartitionRevision = static_cast<std::uint64_t>(row.definitionPartitionRevision);
    wire.resourceClosureRevision = static_cast<std::uint64_t>(row.resourceClosureRevision);
    wire.semanticFingerprint = static_cast<std::uint64_t>(row.semanticFingerprint);
    wire.lineageFingerprint = static_cast<std::uint64_t>(row.lineageFingerprint);
    wire.ownedDefinitionCount = static_cast<std::uint32_t>(row.ownedDefinitionCount);
    wire.ownedResourceCount = static_cast<std::uint32_t>(row.ownedResourceCount);
    wire.installedDefinitionBytes = static_cast<std::uint64_t>(row.installedDefinitionBytes);
    wire.installedResourceBytes = static_cast<std::uint64_t>(row.installedResourceBytes);
    wire.replacementPeakBytes = static_cast<std::uint64_t>(row.replacementPeakBytes);
    wire.decodeStagingBytes = static_cast<std::uint64_t>(row.decodeStagingBytes);
    wire.requiredCapabilityMask = static_cast<std::uint32_t>(row.requiredCapabilityMask);
    wire.reserved0 = static_cast<std::uint32_t>(row.reserved0);
    wire.payloadChecksumCoverage = static_cast<std::uint64_t>(row.payloadChecksumCoverage);
    wire.busContentGeneration = static_cast<std::uint64_t>(row.busContentGeneration);
    wire.publishedBusFingerprint = static_cast<std::uint64_t>(row.publishedBusFingerprint);
    wire.compileTargetProfileFingerprint = static_cast<std::uint64_t>(row.compileTargetProfileFingerprint);
}

[[nodiscard]] inline bool FromWire(const HkPackRequirementBlock& wire, PackRequirementBlock& out) noexcept {
    AssignPack(out.schemaVersion, wire.schemaVersion);
    AssignPack(out.storageModelVersion, wire.storageModelVersion);
    AssignPack(out.packStableId, wire.packStableId);
    AssignPack(out.packContentRevision, wire.packContentRevision);
    AssignPack(out.definitionPartitionRevision, wire.definitionPartitionRevision);
    AssignPack(out.resourceClosureRevision, wire.resourceClosureRevision);
    AssignPack(out.semanticFingerprint, wire.semanticFingerprint);
    AssignPack(out.lineageFingerprint, wire.lineageFingerprint);
    AssignPack(out.ownedDefinitionCount, wire.ownedDefinitionCount);
    AssignPack(out.ownedResourceCount, wire.ownedResourceCount);
    AssignPack(out.installedDefinitionBytes, wire.installedDefinitionBytes);
    AssignPack(out.installedResourceBytes, wire.installedResourceBytes);
    AssignPack(out.replacementPeakBytes, wire.replacementPeakBytes);
    AssignPack(out.decodeStagingBytes, wire.decodeStagingBytes);
    AssignPack(out.requiredCapabilityMask, wire.requiredCapabilityMask);
    AssignPack(out.reserved0, wire.reserved0);
    AssignPack(out.payloadChecksumCoverage, wire.payloadChecksumCoverage);
    AssignPack(out.busContentGeneration, wire.busContentGeneration);
    AssignPack(out.publishedBusFingerprint, wire.publishedBusFingerprint);
    AssignPack(out.compileTargetProfileFingerprint, wire.compileTargetProfileFingerprint);
    return true;
}

inline void ToWire(const std::int32_t& row, HkPackVariableAnchor& wire) noexcept {
    wire.anchor = static_cast<std::int32_t>(row);
}

[[nodiscard]] inline bool FromWire(const HkPackVariableAnchor& wire, std::int32_t& out) noexcept {
    AssignPack(out, wire.anchor);
    return true;
}

inline void ToWire(const PackVariableAnchorHeader& row, HkPackVariableAnchorCount& wire) noexcept {
    wire.count = static_cast<std::uint32_t>(row.count);
}

[[nodiscard]] inline bool FromWire(const HkPackVariableAnchorCount& wire, PackVariableAnchorHeader& out) noexcept {
    AssignPack(out.count, wire.count);
    return true;
}

inline void ToWire(const PackVariableLabel& row, HkPackVariableLabel& wire) noexcept {
    wire.labelId = ToPackId(row.labelId);
    wire.anchor = static_cast<std::int32_t>(row.anchor);
}

[[nodiscard]] inline bool FromWire(const HkPackVariableLabel& wire, PackVariableLabel& out) noexcept {
    AssignPack(out.labelId, FromPackId(wire.labelId));
    AssignPack(out.anchor, wire.anchor);
    if (wire.reserved != 0) return false;
    return true;
}

inline void ToWire(const PackVariableLabelHeader& row, HkPackVariableLabelHeader& wire) noexcept {
    wire.labelMappingFingerprint = static_cast<std::uint64_t>(row.labelMappingFingerprint);
    wire.labelCount = static_cast<std::uint32_t>(row.labelCount);
}

[[nodiscard]] inline bool FromWire(const HkPackVariableLabelHeader& wire, PackVariableLabelHeader& out) noexcept {
    AssignPack(out.labelMappingFingerprint, wire.labelMappingFingerprint);
    AssignPack(out.labelCount, wire.labelCount);
    if (wire.reserved != 0) return false;
    return true;
}

inline void ToWire(const PackVariablePolicyHeader& row, HkPackVariablePolicyHeader& wire) noexcept {
    wire.rowCount = static_cast<std::uint32_t>(row.rowCount);
    wire.format = HK_PACK_VARIABLE_POLICY_HEADER_FORMAT;
}

[[nodiscard]] inline bool FromWire(const HkPackVariablePolicyHeader& wire, PackVariablePolicyHeader& out) noexcept {
    AssignPack(out.rowCount, wire.rowCount);
    if (wire.format != HK_PACK_VARIABLE_POLICY_HEADER_FORMAT) return false;
    if (wire.reserved != 0) return false;
    return true;
}

inline void ToWire(const PackVariablePolicyRow& row, HkPackVariablePolicyRow& wire) noexcept {
    wire.variableId = ToPackId(row.variableId);
    wire.writers = static_cast<std::uint8_t>(row.writers);
    wire.executionFingerprint = static_cast<std::uint64_t>(row.executionFingerprint);
}

[[nodiscard]] inline bool FromWire(const HkPackVariablePolicyRow& wire, PackVariablePolicyRow& out) noexcept {
    AssignPack(out.variableId, FromPackId(wire.variableId));
    AssignPack(out.writers, wire.writers);
    if (!PackZero(wire.reserved)) return false;
    AssignPack(out.executionFingerprint, wire.executionFingerprint);
    return true;
}

inline void ToWire(const PackVariableRequirement& row, HkPackVariableRequirement& wire) noexcept {
    wire.identityClass = HK_PACK_VARIABLE_REQUIREMENT_IDENTITY_CLASS;
    wire.stableId = ToPackId(row.stableId);
    wire.expectedExecutionFingerprint = static_cast<std::uint64_t>(row.expectedExecutionFingerprint);
    wire.expectedLabelMappingFingerprint = static_cast<std::uint64_t>(row.expectedLabelMappingFingerprint);
}

[[nodiscard]] inline bool FromWire(const HkPackVariableRequirement& wire, PackVariableRequirement& out) noexcept {
    if (wire.identityClass != HK_PACK_VARIABLE_REQUIREMENT_IDENTITY_CLASS) return false;
    if (!PackZero(wire.reserved)) return false;
    AssignPack(out.stableId, FromPackId(wire.stableId));
    AssignPack(out.expectedExecutionFingerprint, wire.expectedExecutionFingerprint);
    AssignPack(out.expectedLabelMappingFingerprint, wire.expectedLabelMappingFingerprint);
    return true;
}

inline void ToWire(const PackVariableRequirementsHeader& row, HkPackVariableRequirementsHeader& wire) noexcept {
    wire.rowCount = static_cast<std::uint32_t>(row.rowCount);
    wire.format = HK_PACK_VARIABLE_REQUIREMENTS_HEADER_FORMAT;
}

[[nodiscard]] inline bool FromWire(const HkPackVariableRequirementsHeader& wire, PackVariableRequirementsHeader& out) noexcept {
    AssignPack(out.rowCount, wire.rowCount);
    if (wire.format != HK_PACK_VARIABLE_REQUIREMENTS_HEADER_FORMAT) return false;
    if (wire.reserved != 0) return false;
    return true;
}

inline void ToWire(const VnBindingId& row, HkPackVoiceRecipeBinding& wire) noexcept {
    wire.bindingId = ToPackId(row);
}

[[nodiscard]] inline bool FromWire(const HkPackVoiceRecipeBinding& wire, VnBindingId& out) noexcept {
    AssignPack(out, FromPackId(wire.bindingId));
    return true;
}

inline void ToWire(const PackVoiceRecipeEntryHeader& row, HkPackVoiceRecipeHeader& wire) noexcept {
    wire.playableId = ToPackId(row.playableId);
    wire.installCount = static_cast<std::uint32_t>(row.installCount);
    wire.bindingCount = static_cast<std::uint32_t>(row.bindingCount);
}

[[nodiscard]] inline bool FromWire(const HkPackVoiceRecipeHeader& wire, PackVoiceRecipeEntryHeader& out) noexcept {
    AssignPack(out.playableId, FromPackId(wire.playableId));
    AssignPack(out.installCount, wire.installCount);
    AssignPack(out.bindingCount, wire.bindingCount);
    return true;
}

inline void ToWire(const PackVoiceRecipeInstallWire& row, HkPackVoiceRecipeInstall& wire) noexcept {
    wire.slot = static_cast<std::uint32_t>(row.slot);
    wire.effectType = static_cast<std::uint32_t>(row.effectType);
    wire.descriptorVintage = static_cast<std::uint32_t>(row.descriptorVintage);
    wire.bypass = static_cast<std::uint32_t>(row.bypass);
    wire.paramCount = static_cast<std::uint32_t>(row.paramCount);
    wire.paramBytes = static_cast<std::uint32_t>(row.paramBytes);
    static_assert(std::size(std::remove_cvref_t<decltype(row.params)>{}) == HK_PACK_VOICE_RECIPE_MAX_PARAMS);
    for (std::uint64_t i = 0; i < HK_PACK_VOICE_RECIPE_MAX_PARAMS; ++i) {
        if (i < row.paramCount) {
            wire.params[i] = static_cast<float>(row.params[i]);
        }
    }
    wire.effectId = ToPackId(row.effectId);
}

[[nodiscard]] inline bool FromWire(const HkPackVoiceRecipeInstall& wire, PackVoiceRecipeInstallWire& out) noexcept {
    AssignPack(out.slot, wire.slot);
    AssignPack(out.effectType, wire.effectType);
    AssignPack(out.descriptorVintage, wire.descriptorVintage);
    AssignPack(out.bypass, wire.bypass);
    AssignPack(out.paramCount, wire.paramCount);
    AssignPack(out.paramBytes, wire.paramBytes);
    if (wire.reserved24 != 0) return false;
    if (wire.reserved28 != 0) return false;
    static_assert(std::size(std::remove_cvref_t<decltype(out.params)>{}) == HK_PACK_VOICE_RECIPE_MAX_PARAMS);
    for (std::uint64_t i = 0; i < HK_PACK_VOICE_RECIPE_MAX_PARAMS; ++i) {
        if (i < out.paramCount) {
            AssignPack(out.params[i], wire.params[i]);
        } else {
            if (!PackZero(wire.params[i])) return false;
            out.params[i] = {};
        }
    }
    AssignPack(out.effectId, FromPackId(wire.effectId));
    return true;
}

inline void ToWire(const PackVoiceRecipePlayback& row, HkPackVoiceRecipePlayback& wire) noexcept {
    wire.present = static_cast<std::uint32_t>(row.present);
    wire.loopCount = static_cast<std::uint32_t>(row.loopCount);
}

[[nodiscard]] inline bool FromWire(const HkPackVoiceRecipePlayback& wire, PackVoiceRecipePlayback& out) noexcept {
    AssignPack(out.present, wire.present);
    AssignPack(out.loopCount, wire.loopCount);
    return true;
}

inline void ToWire(const PackVoiceRecipeSpatial& row, HkPackVoiceRecipeSpatial& wire) noexcept {
    wire.attenuationId = ToPackId(row.attenuationId);
    wire.distanceScale = static_cast<float>(row.distanceScale);
}

[[nodiscard]] inline bool FromWire(const HkPackVoiceRecipeSpatial& wire, PackVoiceRecipeSpatial& out) noexcept {
    AssignPack(out.attenuationId, FromPackId(wire.attenuationId));
    AssignPack(out.distanceScale, wire.distanceScale);
    if (wire.reserved20 != 0) return false;
    return true;
}

inline void ToWire(const PackVoiceRecipeTone& row, HkPackVoiceRecipeTone& wire) noexcept {
    wire.present = static_cast<std::uint32_t>(row.present);
    wire.sectionEnableMask = static_cast<std::uint32_t>(row.sectionEnableMask);
    wire.lowpassFrequencyHz = static_cast<float>(row.lowpassFrequencyHz);
    wire.lowpassQ = static_cast<float>(row.lowpassQ);
    wire.highpassFrequencyHz = static_cast<float>(row.highpassFrequencyHz);
    wire.highpassQ = static_cast<float>(row.highpassQ);
    wire.lowShelfFrequencyHz = static_cast<float>(row.lowShelfFrequencyHz);
    wire.lowShelfQ = static_cast<float>(row.lowShelfQ);
    wire.lowShelfGainDb = static_cast<float>(row.lowShelfGainDb);
    wire.highShelfFrequencyHz = static_cast<float>(row.highShelfFrequencyHz);
    wire.highShelfQ = static_cast<float>(row.highShelfQ);
    wire.highShelfGainDb = static_cast<float>(row.highShelfGainDb);
}

[[nodiscard]] inline bool FromWire(const HkPackVoiceRecipeTone& wire, PackVoiceRecipeTone& out) noexcept {
    AssignPack(out.present, wire.present);
    AssignPack(out.sectionEnableMask, wire.sectionEnableMask);
    AssignPack(out.lowpassFrequencyHz, wire.lowpassFrequencyHz);
    AssignPack(out.lowpassQ, wire.lowpassQ);
    AssignPack(out.highpassFrequencyHz, wire.highpassFrequencyHz);
    AssignPack(out.highpassQ, wire.highpassQ);
    AssignPack(out.lowShelfFrequencyHz, wire.lowShelfFrequencyHz);
    AssignPack(out.lowShelfQ, wire.lowShelfQ);
    AssignPack(out.lowShelfGainDb, wire.lowShelfGainDb);
    AssignPack(out.highShelfFrequencyHz, wire.highShelfFrequencyHz);
    AssignPack(out.highShelfQ, wire.highShelfQ);
    AssignPack(out.highShelfGainDb, wire.highShelfGainDb);
    return true;
}

template <class Wire, class Row>
inline void SerializePackRow(const Row& row, std::uint8_t* out) noexcept {
    Wire wire{};
    ToWire(row, wire);
    std::memcpy(out, &wire, sizeof wire);
}

template <class Wire, class Row>
[[nodiscard]] inline bool ParsePackRow(const std::uint8_t* in, std::uint64_t size, Row* out) noexcept {
    if (size != sizeof(Wire)) return false;
    Wire wire;
    std::memcpy(&wire, in, sizeof wire);
    return FromWire(wire, *out);
}

template <class Wire, class Buffer, class Row>
[[nodiscard]] bool AppendPackRow(Buffer& buffer, const Row& row) noexcept {
    static_assert(noexcept(buffer.Take(kPackBytes<Wire>)));
    auto* bytes = buffer.Take(kPackBytes<Wire>);
    if (!bytes) return false;
    SerializePackRow<Wire>(row, bytes);
    return true;
}

template <class Wire, class Buffer, class Row>
[[nodiscard]] bool ReadPackRow(Buffer& buffer, Row& row) noexcept {
    static_assert(noexcept(buffer.Take(kPackBytes<Wire>)));
    const auto* bytes = buffer.Take(kPackBytes<Wire>);
    return bytes && ParsePackRow<Wire>(bytes, kPackBytes<Wire>, &row);
}

template <class Wire, class Row>
[[nodiscard]] inline bool MeasurePackRow(const Row&, std::uint64_t& size) noexcept {
    if (size > UINT64_MAX - kPackBytes<Wire>) return false;
    size += kPackBytes<Wire>;
    return true;
}

[[nodiscard]] inline bool PackEntryKindSupported(std::uint32_t kind) noexcept {
    switch (kind) {
    case HK_PACK_ENTRY_KIND_MEDIA:
    case HK_PACK_ENTRY_KIND_BUS:
    case HK_PACK_ENTRY_KIND_BUS_SEND:
    case HK_PACK_ENTRY_KIND_ATTENUATION:
    case HK_PACK_ENTRY_KIND_BINDING_DEFINITION:
    case HK_PACK_ENTRY_KIND_BINDING_ATTACHMENT:
    case HK_PACK_ENTRY_KIND_CONTAINER:
    case HK_PACK_ENTRY_KIND_VOICE_RECIPE:
    case HK_PACK_ENTRY_KIND_BUS_EFFECT:
    case HK_PACK_ENTRY_KIND_EFFECT_DYNAMIC_ROUTE:
    case HK_PACK_ENTRY_KIND_CANONICAL_DEFINITION_PARTITION:
    case HK_PACK_ENTRY_KIND_PACK_REQUIREMENT_BLOCK:
    case HK_PACK_ENTRY_KIND_PACK_DEPENDENCY_EDGE:
    case HK_PACK_ENTRY_KIND_BUS_DEFINITION_SNAPSHOT:
    case HK_PACK_ENTRY_KIND_VARIABLE_POLICY_OVERLAY:
    case HK_PACK_ENTRY_KIND_VARIABLE_REQUIREMENTS:
    case HK_PACK_ENTRY_KIND_EFFECT_PRESET:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] inline constexpr std::uint32_t PackEntryVintage(std::uint32_t kind) noexcept {
    switch (kind) {
    case HK_PACK_ENTRY_KIND_MEDIA: return HK_PACK_MEDIA_VINTAGE;
    case HK_PACK_ENTRY_KIND_BUS: return HK_PACK_BUS_VINTAGE;
    case HK_PACK_ENTRY_KIND_BUS_SEND: return HK_PACK_BUS_SEND_VINTAGE;
    case HK_PACK_ENTRY_KIND_ATTENUATION: return HK_PACK_ATTENUATION_VINTAGE;
    case HK_PACK_ENTRY_KIND_BINDING_DEFINITION: return HK_PACK_BINDING_DEFINITION_VINTAGE;
    case HK_PACK_ENTRY_KIND_BINDING_ATTACHMENT: return HK_PACK_BINDING_ATTACHMENT_VINTAGE;
    case HK_PACK_ENTRY_KIND_CONTAINER: return HK_PACK_CONTAINER_VINTAGE;
    case HK_PACK_ENTRY_KIND_VOICE_RECIPE: return HK_PACK_VOICE_RECIPE_VINTAGE;
    case HK_PACK_ENTRY_KIND_BUS_EFFECT: return HK_PACK_BUS_EFFECT_VINTAGE;
    case HK_PACK_ENTRY_KIND_EFFECT_DYNAMIC_ROUTE: return HK_PACK_EFFECT_DYNAMIC_ROUTE_VINTAGE;
    case HK_PACK_ENTRY_KIND_CANONICAL_DEFINITION_PARTITION: return HK_PACK_CANONICAL_DEFINITION_PARTITION_VINTAGE;
    case HK_PACK_ENTRY_KIND_PACK_REQUIREMENT_BLOCK: return HK_PACK_PACK_REQUIREMENT_BLOCK_VINTAGE;
    case HK_PACK_ENTRY_KIND_PACK_DEPENDENCY_EDGE: return HK_PACK_PACK_DEPENDENCY_EDGE_VINTAGE;
    case HK_PACK_ENTRY_KIND_BUS_DEFINITION_SNAPSHOT: return HK_PACK_BUS_DEFINITION_SNAPSHOT_VINTAGE;
    case HK_PACK_ENTRY_KIND_VARIABLE_POLICY_OVERLAY: return HK_PACK_VARIABLE_POLICY_OVERLAY_VINTAGE;
    case HK_PACK_ENTRY_KIND_VARIABLE_REQUIREMENTS: return HK_PACK_VARIABLE_REQUIREMENTS_VINTAGE;
    case HK_PACK_ENTRY_KIND_EFFECT_PRESET: return HK_PACK_EFFECT_PRESET_VINTAGE;
    default: return 0;
    }
}

[[nodiscard]] inline bool VintageLegalForKind(std::uint32_t kind, std::uint32_t vintage) noexcept {
    return PackEntryKindSupported(kind) && vintage == PackEntryVintage(kind);
}

inline void StorePackBusDefinitionSnapshotHeaderChecksum(std::uint8_t* out, std::uint64_t value) noexcept {
    std::memcpy(out + offsetof(HkPackBusDefinitionSnapshotHeader, checksum), &value, sizeof value);
}

inline void StorePackRequirementBlockPayloadChecksumCoverage(std::uint8_t* out, std::uint64_t value) noexcept {
    std::memcpy(out + offsetof(HkPackRequirementBlock, payloadChecksumCoverage), &value, sizeof value);
}

inline constexpr std::uint64_t kPackBodyWorldBodyHeaderBytes = kPackBytes<HkPackBodyWorldPrefix> + kPackBytes<HkPackBodyWorldActionCapacity> + kPackBytes<HkPackBodyWorldStructuralCount> + kPackBytes<HkPackBodyWorldCapacity> + kPackBytes<HkPackBodyWorldProjectId>;

template <class Buffer, class Visitor>
[[nodiscard]] bool AppendPackBodyPayloadEffectNode(Buffer& buffer, Visitor& visitor) noexcept {
    static_assert(noexcept(visitor.Value()));
    auto& value = visitor.Value();
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(value)>, EffectNodeV1>);
    {
        static_assert(noexcept(visitor.Header()));
        auto&& row = visitor.Header();
        if (!AppendPackRow<HkPackBodyPayloadEffectNodeHeader>(buffer, row)) return false;
    }
    if (static_cast<std::uint64_t>(value.effectType) == HK_PACK_EFFECT_PRESET_REFERENCE_TYPE) {
        static_assert(noexcept(visitor.PresetReference()));
        auto&& row = visitor.PresetReference();
        if (!AppendPackRow<HkPackBodyPayloadEffectPresetReference>(buffer, row)) return false;
    }
    const std::uint64_t paramsCount = static_cast<std::uint64_t>(value.paramCount);
    static_assert(noexcept(visitor.PrepareParams(paramsCount)));
    if (!visitor.PrepareParams(paramsCount)) return false;
    for (std::uint64_t i = 0; i < paramsCount; ++i) {
        static_assert(noexcept(visitor.Params(i)));
        auto&& row = visitor.Params(i);
        if (!AppendPackRow<HkPackBodyPayloadEffectParam>(buffer, row)) return false;
    }
    static_assert(noexcept(visitor.Finish()));
    return visitor.Finish();
}

template <class Buffer, class Visitor>
[[nodiscard]] bool ReadPackBodyPayloadEffectNode(Buffer& buffer, Visitor& visitor) noexcept {
    static_assert(noexcept(visitor.Value()));
    auto& value = visitor.Value();
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(value)>, EffectNodeV1>);
    {
        static_assert(noexcept(visitor.Header()));
        auto&& row = visitor.Header();
        if (!ReadPackRow<HkPackBodyPayloadEffectNodeHeader>(buffer, row)) return false;
    }
    if (static_cast<std::uint64_t>(value.effectType) == HK_PACK_EFFECT_PRESET_REFERENCE_TYPE) {
        static_assert(noexcept(visitor.PresetReference()));
        auto&& row = visitor.PresetReference();
        if (!ReadPackRow<HkPackBodyPayloadEffectPresetReference>(buffer, row)) return false;
    }
    const std::uint64_t paramsCount = static_cast<std::uint64_t>(value.paramCount);
    static_assert(noexcept(buffer.Remaining()));
    if (paramsCount > buffer.Remaining() / kPackBytes<HkPackBodyPayloadEffectParam>) return false;
    static_assert(noexcept(visitor.PrepareParams(paramsCount)));
    if (!visitor.PrepareParams(paramsCount)) return false;
    for (std::uint64_t i = 0; i < paramsCount; ++i) {
        static_assert(noexcept(visitor.Params(i)));
        auto&& row = visitor.Params(i);
        if (!ReadPackRow<HkPackBodyPayloadEffectParam>(buffer, row)) return false;
    }
    static_assert(noexcept(visitor.Finish()));
    return visitor.Finish();
}

template <class Visitor>
[[nodiscard]] bool MeasurePackBodyPayloadEffectNode(Visitor& visitor, std::uint64_t& size) noexcept {
    static_assert(noexcept(visitor.Value()));
    auto& value = visitor.Value();
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(value)>, EffectNodeV1>);
    if (size > UINT64_MAX - kPackBytes<HkPackBodyPayloadEffectNodeHeader>) return false;
    size += kPackBytes<HkPackBodyPayloadEffectNodeHeader>;
    if (static_cast<std::uint64_t>(value.effectType) == HK_PACK_EFFECT_PRESET_REFERENCE_TYPE) {
        if (size > UINT64_MAX - kPackBytes<HkPackBodyPayloadEffectPresetReference>) return false;
        size += kPackBytes<HkPackBodyPayloadEffectPresetReference>;
    }
    const std::uint64_t paramsCount = static_cast<std::uint64_t>(value.paramCount);
    if (paramsCount > (UINT64_MAX - size) / kPackBytes<HkPackBodyPayloadEffectParam>) return false;
    size += paramsCount * kPackBytes<HkPackBodyPayloadEffectParam>;
    return true;
}

template <class Buffer, class Visitor>
[[nodiscard]] bool AppendPackBodyPayloadBody(Buffer& buffer, Visitor& visitor) noexcept {
    static_assert(noexcept(visitor.Value()));
    auto& value = visitor.Value();
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(value)>, PlayablePropertyPayloadViewV1>);
    const std::uint64_t sendsCount = static_cast<std::uint64_t>(value.sendCount);
    static_assert(noexcept(visitor.PrepareSends(sendsCount)));
    if (!visitor.PrepareSends(sendsCount)) return false;
    for (std::uint64_t i = 0; i < sendsCount; ++i) {
        static_assert(noexcept(visitor.Sends(i)));
        auto&& row = visitor.Sends(i);
        if (!AppendPackRow<HkPackBodyPayloadSend>(buffer, row)) return false;
    }
    const std::uint64_t effectNodesCount = static_cast<std::uint64_t>(value.effectNodeCount);
    static_assert(noexcept(visitor.PrepareEffectNodes(effectNodesCount)));
    if (!visitor.PrepareEffectNodes(effectNodesCount)) return false;
    for (std::uint64_t i = 0; i < effectNodesCount; ++i) {
        static_assert(noexcept(visitor.EffectNodes(i)));
        auto&& row = visitor.EffectNodes(i);
        if (!AppendPackBodyPayloadEffectNode(buffer, row)) return false;
    }
    const std::uint64_t effectEdgesCount = static_cast<std::uint64_t>(value.effectEdgeCount);
    static_assert(noexcept(visitor.PrepareEffectEdges(effectEdgesCount)));
    if (!visitor.PrepareEffectEdges(effectEdgesCount)) return false;
    for (std::uint64_t i = 0; i < effectEdgesCount; ++i) {
        static_assert(noexcept(visitor.EffectEdges(i)));
        auto&& row = visitor.EffectEdges(i);
        if (!AppendPackRow<HkPackBodyPayloadEffectEdge>(buffer, row)) return false;
    }
    static_assert(noexcept(visitor.Finish()));
    return visitor.Finish();
}

template <class Buffer, class Visitor>
[[nodiscard]] bool ReadPackBodyPayloadBody(Buffer& buffer, Visitor& visitor) noexcept {
    static_assert(noexcept(visitor.Value()));
    auto& value = visitor.Value();
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(value)>, PlayablePropertyPayloadViewV1>);
    const std::uint64_t sendsCount = static_cast<std::uint64_t>(value.sendCount);
    static_assert(noexcept(buffer.Remaining()));
    if (sendsCount > buffer.Remaining() / kPackBytes<HkPackBodyPayloadSend>) return false;
    static_assert(noexcept(visitor.PrepareSends(sendsCount)));
    if (!visitor.PrepareSends(sendsCount)) return false;
    for (std::uint64_t i = 0; i < sendsCount; ++i) {
        static_assert(noexcept(visitor.Sends(i)));
        auto&& row = visitor.Sends(i);
        if (!ReadPackRow<HkPackBodyPayloadSend>(buffer, row)) return false;
    }
    const std::uint64_t effectNodesCount = static_cast<std::uint64_t>(value.effectNodeCount);
    static_assert(noexcept(buffer.Remaining()));
    if (effectNodesCount > buffer.Remaining() / kPackBytes<HkPackBodyPayloadEffectNodeHeader>) return false;
    static_assert(noexcept(visitor.PrepareEffectNodes(effectNodesCount)));
    if (!visitor.PrepareEffectNodes(effectNodesCount)) return false;
    for (std::uint64_t i = 0; i < effectNodesCount; ++i) {
        static_assert(noexcept(visitor.EffectNodes(i)));
        auto&& row = visitor.EffectNodes(i);
        if (!ReadPackBodyPayloadEffectNode(buffer, row)) return false;
    }
    const std::uint64_t effectEdgesCount = static_cast<std::uint64_t>(value.effectEdgeCount);
    static_assert(noexcept(buffer.Remaining()));
    if (effectEdgesCount > buffer.Remaining() / kPackBytes<HkPackBodyPayloadEffectEdge>) return false;
    static_assert(noexcept(visitor.PrepareEffectEdges(effectEdgesCount)));
    if (!visitor.PrepareEffectEdges(effectEdgesCount)) return false;
    for (std::uint64_t i = 0; i < effectEdgesCount; ++i) {
        static_assert(noexcept(visitor.EffectEdges(i)));
        auto&& row = visitor.EffectEdges(i);
        if (!ReadPackRow<HkPackBodyPayloadEffectEdge>(buffer, row)) return false;
    }
    static_assert(noexcept(visitor.Finish()));
    return visitor.Finish();
}

template <class Visitor>
[[nodiscard]] bool MeasurePackBodyPayloadBody(Visitor& visitor, std::uint64_t& size) noexcept {
    static_assert(noexcept(visitor.Value()));
    auto& value = visitor.Value();
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(value)>, PlayablePropertyPayloadViewV1>);
    const std::uint64_t sendsCount = static_cast<std::uint64_t>(value.sendCount);
    if (sendsCount > (UINT64_MAX - size) / kPackBytes<HkPackBodyPayloadSend>) return false;
    size += sendsCount * kPackBytes<HkPackBodyPayloadSend>;
    const std::uint64_t effectNodesCount = static_cast<std::uint64_t>(value.effectNodeCount);
    static_assert(noexcept(visitor.PrepareEffectNodes(effectNodesCount)));
    if (!visitor.PrepareEffectNodes(effectNodesCount)) return false;
    for (std::uint64_t i = 0; i < effectNodesCount; ++i) {
        static_assert(noexcept(visitor.EffectNodes(i)));
        auto&& row = visitor.EffectNodes(i);
        if (!MeasurePackBodyPayloadEffectNode(row, size)) return false;
    }
    const std::uint64_t effectEdgesCount = static_cast<std::uint64_t>(value.effectEdgeCount);
    if (effectEdgesCount > (UINT64_MAX - size) / kPackBytes<HkPackBodyPayloadEffectEdge>) return false;
    size += effectEdgesCount * kPackBytes<HkPackBodyPayloadEffectEdge>;
    return true;
}

template <class Buffer, class Visitor>
[[nodiscard]] bool AppendPackBodyPayload(Buffer& buffer, Visitor& visitor) noexcept {
    {
        static_assert(noexcept(visitor.Header()));
        auto&& row = visitor.Header();
        if (!AppendPackRow<HkPackBodyPayloadHeader>(buffer, row)) return false;
    }
    {
        static_assert(noexcept(visitor.Body()));
        auto&& row = visitor.Body();
        if (!AppendPackBodyPayloadBody(buffer, row)) return false;
    }
    static_assert(noexcept(visitor.Finish()));
    return visitor.Finish();
}

template <class Buffer, class Visitor>
[[nodiscard]] bool ReadPackBodyPayload(Buffer& buffer, Visitor& visitor) noexcept {
    {
        static_assert(noexcept(visitor.Header()));
        auto&& row = visitor.Header();
        if (!ReadPackRow<HkPackBodyPayloadHeader>(buffer, row)) return false;
    }
    {
        static_assert(noexcept(visitor.Body()));
        auto&& row = visitor.Body();
        if (!ReadPackBodyPayloadBody(buffer, row)) return false;
    }
    static_assert(noexcept(visitor.Finish()));
    return visitor.Finish();
}

template <class Visitor>
[[nodiscard]] bool MeasurePackBodyPayload(Visitor& visitor, std::uint64_t& size) noexcept {
    if (size > UINT64_MAX - kPackBytes<HkPackBodyPayloadHeader>) return false;
    size += kPackBytes<HkPackBodyPayloadHeader>;
    {
        static_assert(noexcept(visitor.Body()));
        auto&& row = visitor.Body();
        if (!MeasurePackBodyPayloadBody(row, size)) return false;
    }
    return true;
}

template <class Buffer, class Visitor>
[[nodiscard]] bool AppendPackBodyWorldAction(Buffer& buffer, Visitor& visitor) noexcept {
    static_assert(noexcept(visitor.Value()));
    auto& value = visitor.Value();
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(value)>, ActionProgramViewV1>);
    {
        static_assert(noexcept(visitor.Header()));
        auto&& row = visitor.Header();
        if (!AppendPackRow<HkPackBodyWorldActionHeader>(buffer, row)) return false;
    }
    const std::uint64_t stepsCount = static_cast<std::uint64_t>(value.stepCount);
    static_assert(noexcept(visitor.PrepareSteps(stepsCount)));
    if (!visitor.PrepareSteps(stepsCount)) return false;
    for (std::uint64_t i = 0; i < stepsCount; ++i) {
        static_assert(noexcept(visitor.Steps(i)));
        auto&& row = visitor.Steps(i);
        if (!AppendPackRow<HkPackBodyWorldActionStep>(buffer, row)) return false;
    }
    static_assert(noexcept(visitor.Finish()));
    return visitor.Finish();
}

template <class Buffer, class Visitor>
[[nodiscard]] bool ReadPackBodyWorldAction(Buffer& buffer, Visitor& visitor) noexcept {
    static_assert(noexcept(visitor.Value()));
    auto& value = visitor.Value();
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(value)>, ActionProgramViewV1>);
    {
        static_assert(noexcept(visitor.Header()));
        auto&& row = visitor.Header();
        if (!ReadPackRow<HkPackBodyWorldActionHeader>(buffer, row)) return false;
    }
    const std::uint64_t stepsCount = static_cast<std::uint64_t>(value.stepCount);
    static_assert(noexcept(buffer.Remaining()));
    if (stepsCount > buffer.Remaining() / kPackBytes<HkPackBodyWorldActionStep>) return false;
    static_assert(noexcept(visitor.PrepareSteps(stepsCount)));
    if (!visitor.PrepareSteps(stepsCount)) return false;
    for (std::uint64_t i = 0; i < stepsCount; ++i) {
        static_assert(noexcept(visitor.Steps(i)));
        auto&& row = visitor.Steps(i);
        if (!ReadPackRow<HkPackBodyWorldActionStep>(buffer, row)) return false;
    }
    static_assert(noexcept(visitor.Finish()));
    return visitor.Finish();
}

template <class Visitor>
[[nodiscard]] bool MeasurePackBodyWorldAction(Visitor& visitor, std::uint64_t& size) noexcept {
    static_assert(noexcept(visitor.Value()));
    auto& value = visitor.Value();
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(value)>, ActionProgramViewV1>);
    if (size > UINT64_MAX - kPackBytes<HkPackBodyWorldActionHeader>) return false;
    size += kPackBytes<HkPackBodyWorldActionHeader>;
    const std::uint64_t stepsCount = static_cast<std::uint64_t>(value.stepCount);
    if (stepsCount > (UINT64_MAX - size) / kPackBytes<HkPackBodyWorldActionStep>) return false;
    size += stepsCount * kPackBytes<HkPackBodyWorldActionStep>;
    return true;
}

template <class Buffer, class Visitor>
[[nodiscard]] bool AppendPackBodyWorldBodyHeader(Buffer& buffer, Visitor& visitor) noexcept {
    {
        static_assert(noexcept(visitor.Prefix()));
        auto&& row = visitor.Prefix();
        if (!AppendPackRow<HkPackBodyWorldPrefix>(buffer, row)) return false;
    }
    {
        static_assert(noexcept(visitor.ActionCapacity()));
        auto&& row = visitor.ActionCapacity();
        if (!AppendPackRow<HkPackBodyWorldActionCapacity>(buffer, row)) return false;
    }
    {
        static_assert(noexcept(visitor.StructuralCount()));
        auto&& row = visitor.StructuralCount();
        if (!AppendPackRow<HkPackBodyWorldStructuralCount>(buffer, row)) return false;
    }
    {
        static_assert(noexcept(visitor.Capacity()));
        auto&& row = visitor.Capacity();
        if (!AppendPackRow<HkPackBodyWorldCapacity>(buffer, row)) return false;
    }
    {
        static_assert(noexcept(visitor.ProjectId()));
        auto&& row = visitor.ProjectId();
        if (!AppendPackRow<HkPackBodyWorldProjectId>(buffer, row)) return false;
    }
    static_assert(noexcept(visitor.Finish()));
    return visitor.Finish();
}

template <class Buffer, class Visitor>
[[nodiscard]] bool ReadPackBodyWorldBodyHeader(Buffer& buffer, Visitor& visitor) noexcept {
    {
        static_assert(noexcept(visitor.Prefix()));
        auto&& row = visitor.Prefix();
        if (!ReadPackRow<HkPackBodyWorldPrefix>(buffer, row)) return false;
    }
    {
        static_assert(noexcept(visitor.ActionCapacity()));
        auto&& row = visitor.ActionCapacity();
        if (!ReadPackRow<HkPackBodyWorldActionCapacity>(buffer, row)) return false;
    }
    {
        static_assert(noexcept(visitor.StructuralCount()));
        auto&& row = visitor.StructuralCount();
        if (!ReadPackRow<HkPackBodyWorldStructuralCount>(buffer, row)) return false;
    }
    {
        static_assert(noexcept(visitor.Capacity()));
        auto&& row = visitor.Capacity();
        if (!ReadPackRow<HkPackBodyWorldCapacity>(buffer, row)) return false;
    }
    {
        static_assert(noexcept(visitor.ProjectId()));
        auto&& row = visitor.ProjectId();
        if (!ReadPackRow<HkPackBodyWorldProjectId>(buffer, row)) return false;
    }
    static_assert(noexcept(visitor.Finish()));
    return visitor.Finish();
}

template <class Buffer, class Visitor>
[[nodiscard]] bool AppendPackBodyWorldVariable(Buffer& buffer, Visitor& visitor) noexcept {
    {
        static_assert(noexcept(visitor.Header()));
        auto&& row = visitor.Header();
        if (!AppendPackRow<HkPackBodyWorldVariableHeader>(buffer, row)) return false;
    }
    static_assert(noexcept(visitor.Finish()));
    return visitor.Finish();
}

template <class Buffer, class Visitor>
[[nodiscard]] bool ReadPackBodyWorldVariable(Buffer& buffer, Visitor& visitor) noexcept {
    {
        static_assert(noexcept(visitor.Header()));
        auto&& row = visitor.Header();
        if (!ReadPackRow<HkPackBodyWorldVariableHeader>(buffer, row)) return false;
    }
    static_assert(noexcept(visitor.Finish()));
    return visitor.Finish();
}

template <class Buffer, class Visitor>
[[nodiscard]] bool AppendPackBodyWorldStructuralAncestor(Buffer& buffer, Visitor& visitor) noexcept {
    {
        static_assert(noexcept(visitor.Header()));
        auto&& row = visitor.Header();
        if (!AppendPackRow<HkPackBodyWorldStructuralAncestorHeader>(buffer, row)) return false;
    }
    {
        static_assert(noexcept(visitor.Payload()));
        auto&& row = visitor.Payload();
        if (!AppendPackBodyPayload(buffer, row)) return false;
    }
    static_assert(noexcept(visitor.Finish()));
    return visitor.Finish();
}

template <class Buffer, class Visitor>
[[nodiscard]] bool ReadPackBodyWorldStructuralAncestor(Buffer& buffer, Visitor& visitor) noexcept {
    {
        static_assert(noexcept(visitor.Header()));
        auto&& row = visitor.Header();
        if (!ReadPackRow<HkPackBodyWorldStructuralAncestorHeader>(buffer, row)) return false;
    }
    {
        static_assert(noexcept(visitor.Payload()));
        auto&& row = visitor.Payload();
        if (!ReadPackBodyPayload(buffer, row)) return false;
    }
    static_assert(noexcept(visitor.Finish()));
    return visitor.Finish();
}

template <class Visitor>
[[nodiscard]] bool MeasurePackBodyWorldStructuralAncestor(Visitor& visitor, std::uint64_t& size) noexcept {
    if (size > UINT64_MAX - kPackBytes<HkPackBodyWorldStructuralAncestorHeader>) return false;
    size += kPackBytes<HkPackBodyWorldStructuralAncestorHeader>;
    {
        static_assert(noexcept(visitor.Payload()));
        auto&& row = visitor.Payload();
        if (!MeasurePackBodyPayload(row, size)) return false;
    }
    return true;
}

template <class Buffer, class Visitor>
[[nodiscard]] bool AppendPackBodyWorldProperty(Buffer& buffer, Visitor& visitor) noexcept {
    static_assert(noexcept(visitor.Value()));
    auto& value = visitor.Value();
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(value)>, PlayablePropertyProgramViewV1>);
    {
        static_assert(noexcept(visitor.Header()));
        auto&& row = visitor.Header();
        if (!AppendPackRow<HkPackBodyWorldPropertyHeader>(buffer, row)) return false;
    }
    {
        static_assert(noexcept(visitor.PayloadHeader()));
        auto&& row = visitor.PayloadHeader();
        if (!AppendPackRow<HkPackBodyPayloadHeader>(buffer, row)) return false;
    }
    const std::uint64_t ancestorsCount = static_cast<std::uint64_t>(value.organizationLayerAncestorCount);
    static_assert(noexcept(visitor.PrepareAncestors(ancestorsCount)));
    if (!visitor.PrepareAncestors(ancestorsCount)) return false;
    for (std::uint64_t i = 0; i < ancestorsCount; ++i) {
        static_assert(noexcept(visitor.Ancestors(i)));
        auto&& row = visitor.Ancestors(i);
        if (!AppendPackRow<HkPackBodyWorldLayerAncestor>(buffer, row)) return false;
    }
    {
        static_assert(noexcept(visitor.StructuralHeader()));
        auto&& row = visitor.StructuralHeader();
        if (!AppendPackRow<HkPackBodyWorldStructuralHeader>(buffer, row)) return false;
    }
    const std::uint64_t structuralAncestorsCount = static_cast<std::uint64_t>(value.structuralAncestorCount);
    static_assert(noexcept(visitor.PrepareStructuralAncestors(structuralAncestorsCount)));
    if (!visitor.PrepareStructuralAncestors(structuralAncestorsCount)) return false;
    for (std::uint64_t i = 0; i < structuralAncestorsCount; ++i) {
        static_assert(noexcept(visitor.StructuralAncestors(i)));
        auto&& row = visitor.StructuralAncestors(i);
        if (!AppendPackBodyWorldStructuralAncestor(buffer, row)) return false;
    }
    {
        static_assert(noexcept(visitor.PayloadBody()));
        auto&& row = visitor.PayloadBody();
        if (!AppendPackBodyPayloadBody(buffer, row)) return false;
    }
    static_assert(noexcept(visitor.Finish()));
    return visitor.Finish();
}

template <class Buffer, class Visitor>
[[nodiscard]] bool ReadPackBodyWorldProperty(Buffer& buffer, Visitor& visitor) noexcept {
    static_assert(noexcept(visitor.Value()));
    auto& value = visitor.Value();
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(value)>, PlayablePropertyProgramViewV1>);
    {
        static_assert(noexcept(visitor.Header()));
        auto&& row = visitor.Header();
        if (!ReadPackRow<HkPackBodyWorldPropertyHeader>(buffer, row)) return false;
    }
    {
        static_assert(noexcept(visitor.PayloadHeader()));
        auto&& row = visitor.PayloadHeader();
        if (!ReadPackRow<HkPackBodyPayloadHeader>(buffer, row)) return false;
    }
    const std::uint64_t ancestorsCount = static_cast<std::uint64_t>(value.organizationLayerAncestorCount);
    static_assert(noexcept(buffer.Remaining()));
    if (ancestorsCount > buffer.Remaining() / kPackBytes<HkPackBodyWorldLayerAncestor>) return false;
    static_assert(noexcept(visitor.PrepareAncestors(ancestorsCount)));
    if (!visitor.PrepareAncestors(ancestorsCount)) return false;
    for (std::uint64_t i = 0; i < ancestorsCount; ++i) {
        static_assert(noexcept(visitor.Ancestors(i)));
        auto&& row = visitor.Ancestors(i);
        if (!ReadPackRow<HkPackBodyWorldLayerAncestor>(buffer, row)) return false;
    }
    {
        static_assert(noexcept(visitor.StructuralHeader()));
        auto&& row = visitor.StructuralHeader();
        if (!ReadPackRow<HkPackBodyWorldStructuralHeader>(buffer, row)) return false;
    }
    const std::uint64_t structuralAncestorsCount = static_cast<std::uint64_t>(value.structuralAncestorCount);
    static_assert(noexcept(buffer.Remaining()));
    if (structuralAncestorsCount > buffer.Remaining() / (kPackBytes<HkPackBodyWorldStructuralAncestorHeader> + kPackBytes<HkPackBodyPayloadHeader>)) return false;
    static_assert(noexcept(visitor.PrepareStructuralAncestors(structuralAncestorsCount)));
    if (!visitor.PrepareStructuralAncestors(structuralAncestorsCount)) return false;
    for (std::uint64_t i = 0; i < structuralAncestorsCount; ++i) {
        static_assert(noexcept(visitor.StructuralAncestors(i)));
        auto&& row = visitor.StructuralAncestors(i);
        if (!ReadPackBodyWorldStructuralAncestor(buffer, row)) return false;
    }
    {
        static_assert(noexcept(visitor.PayloadBody()));
        auto&& row = visitor.PayloadBody();
        if (!ReadPackBodyPayloadBody(buffer, row)) return false;
    }
    static_assert(noexcept(visitor.Finish()));
    return visitor.Finish();
}

template <class Visitor>
[[nodiscard]] bool MeasurePackBodyWorldProperty(Visitor& visitor, std::uint64_t& size) noexcept {
    static_assert(noexcept(visitor.Value()));
    auto& value = visitor.Value();
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(value)>, PlayablePropertyProgramViewV1>);
    if (size > UINT64_MAX - kPackBytes<HkPackBodyWorldPropertyHeader>) return false;
    size += kPackBytes<HkPackBodyWorldPropertyHeader>;
    if (size > UINT64_MAX - kPackBytes<HkPackBodyPayloadHeader>) return false;
    size += kPackBytes<HkPackBodyPayloadHeader>;
    const std::uint64_t ancestorsCount = static_cast<std::uint64_t>(value.organizationLayerAncestorCount);
    if (ancestorsCount > (UINT64_MAX - size) / kPackBytes<HkPackBodyWorldLayerAncestor>) return false;
    size += ancestorsCount * kPackBytes<HkPackBodyWorldLayerAncestor>;
    if (size > UINT64_MAX - kPackBytes<HkPackBodyWorldStructuralHeader>) return false;
    size += kPackBytes<HkPackBodyWorldStructuralHeader>;
    const std::uint64_t structuralAncestorsCount = static_cast<std::uint64_t>(value.structuralAncestorCount);
    static_assert(noexcept(visitor.PrepareStructuralAncestors(structuralAncestorsCount)));
    if (!visitor.PrepareStructuralAncestors(structuralAncestorsCount)) return false;
    for (std::uint64_t i = 0; i < structuralAncestorsCount; ++i) {
        static_assert(noexcept(visitor.StructuralAncestors(i)));
        auto&& row = visitor.StructuralAncestors(i);
        if (!MeasurePackBodyWorldStructuralAncestor(row, size)) return false;
    }
    {
        static_assert(noexcept(visitor.PayloadBody()));
        auto&& row = visitor.PayloadBody();
        if (!MeasurePackBodyPayloadBody(row, size)) return false;
    }
    return true;
}

template <class Buffer, class Visitor>
[[nodiscard]] bool AppendPackBodyWorldOccurrence(Buffer& buffer, Visitor& visitor) noexcept {
    {
        static_assert(noexcept(visitor.Header()));
        auto&& row = visitor.Header();
        if (!AppendPackRow<HkPackBodyWorldOccurrenceHeader>(buffer, row)) return false;
    }
    {
        static_assert(noexcept(visitor.Payload()));
        auto&& row = visitor.Payload();
        if (!AppendPackBodyPayload(buffer, row)) return false;
    }
    static_assert(noexcept(visitor.Finish()));
    return visitor.Finish();
}

template <class Buffer, class Visitor>
[[nodiscard]] bool ReadPackBodyWorldOccurrence(Buffer& buffer, Visitor& visitor) noexcept {
    {
        static_assert(noexcept(visitor.Header()));
        auto&& row = visitor.Header();
        if (!ReadPackRow<HkPackBodyWorldOccurrenceHeader>(buffer, row)) return false;
    }
    {
        static_assert(noexcept(visitor.Payload()));
        auto&& row = visitor.Payload();
        if (!ReadPackBodyPayload(buffer, row)) return false;
    }
    static_assert(noexcept(visitor.Finish()));
    return visitor.Finish();
}

template <class Visitor>
[[nodiscard]] bool MeasurePackBodyWorldOccurrence(Visitor& visitor, std::uint64_t& size) noexcept {
    if (size > UINT64_MAX - kPackBytes<HkPackBodyWorldOccurrenceHeader>) return false;
    size += kPackBytes<HkPackBodyWorldOccurrenceHeader>;
    {
        static_assert(noexcept(visitor.Payload()));
        auto&& row = visitor.Payload();
        if (!MeasurePackBodyPayload(row, size)) return false;
    }
    return true;
}

template <class Buffer, class Visitor>
[[nodiscard]] bool AppendPackBodyWorldTrack(Buffer& buffer, Visitor& visitor) noexcept {
    static_assert(noexcept(visitor.Value()));
    auto& value = visitor.Value();
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(value)>, TrackScheduleProgramViewV1>);
    {
        static_assert(noexcept(visitor.Header()));
        auto&& row = visitor.Header();
        if (!AppendPackRow<HkPackBodyWorldTrackHeader>(buffer, row)) return false;
    }
    const std::uint64_t placementsCount = static_cast<std::uint64_t>(value.placementCount);
    static_assert(noexcept(visitor.PreparePlacements(placementsCount)));
    if (!visitor.PreparePlacements(placementsCount)) return false;
    for (std::uint64_t i = 0; i < placementsCount; ++i) {
        static_assert(noexcept(visitor.Placements(i)));
        auto&& row = visitor.Placements(i);
        if (!AppendPackRow<HkPackBodyWorldPlacement>(buffer, row)) return false;
    }
    static_assert(noexcept(visitor.Finish()));
    return visitor.Finish();
}

template <class Buffer, class Visitor>
[[nodiscard]] bool ReadPackBodyWorldTrack(Buffer& buffer, Visitor& visitor) noexcept {
    static_assert(noexcept(visitor.Value()));
    auto& value = visitor.Value();
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(value)>, TrackScheduleProgramViewV1>);
    {
        static_assert(noexcept(visitor.Header()));
        auto&& row = visitor.Header();
        if (!ReadPackRow<HkPackBodyWorldTrackHeader>(buffer, row)) return false;
    }
    const std::uint64_t placementsCount = static_cast<std::uint64_t>(value.placementCount);
    static_assert(noexcept(buffer.Remaining()));
    if (placementsCount > buffer.Remaining() / kPackBytes<HkPackBodyWorldPlacement>) return false;
    static_assert(noexcept(visitor.PreparePlacements(placementsCount)));
    if (!visitor.PreparePlacements(placementsCount)) return false;
    for (std::uint64_t i = 0; i < placementsCount; ++i) {
        static_assert(noexcept(visitor.Placements(i)));
        auto&& row = visitor.Placements(i);
        if (!ReadPackRow<HkPackBodyWorldPlacement>(buffer, row)) return false;
    }
    static_assert(noexcept(visitor.Finish()));
    return visitor.Finish();
}

template <class Visitor>
[[nodiscard]] bool MeasurePackBodyWorldTrack(Visitor& visitor, std::uint64_t& size) noexcept {
    static_assert(noexcept(visitor.Value()));
    auto& value = visitor.Value();
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(value)>, TrackScheduleProgramViewV1>);
    if (size > UINT64_MAX - kPackBytes<HkPackBodyWorldTrackHeader>) return false;
    size += kPackBytes<HkPackBodyWorldTrackHeader>;
    const std::uint64_t placementsCount = static_cast<std::uint64_t>(value.placementCount);
    if (placementsCount > (UINT64_MAX - size) / kPackBytes<HkPackBodyWorldPlacement>) return false;
    size += placementsCount * kPackBytes<HkPackBodyWorldPlacement>;
    return true;
}

template <class Buffer, class Visitor>
[[nodiscard]] bool AppendPackBodyWorldSegment(Buffer& buffer, Visitor& visitor) noexcept {
    static_assert(noexcept(visitor.Value()));
    auto& value = visitor.Value();
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(value)>, SegmentScheduleProgramViewV1>);
    {
        static_assert(noexcept(visitor.Header()));
        auto&& row = visitor.Header();
        if (!AppendPackRow<HkPackBodyWorldSegmentHeader>(buffer, row)) return false;
    }
    const std::uint64_t trackRefsCount = static_cast<std::uint64_t>(value.trackRefCount);
    static_assert(noexcept(visitor.PrepareTrackRefs(trackRefsCount)));
    if (!visitor.PrepareTrackRefs(trackRefsCount)) return false;
    for (std::uint64_t i = 0; i < trackRefsCount; ++i) {
        static_assert(noexcept(visitor.TrackRefs(i)));
        auto&& row = visitor.TrackRefs(i);
        if (!AppendPackRow<HkPackBodyWorldTrackRef>(buffer, row)) return false;
    }
    const std::uint64_t markersCount = static_cast<std::uint64_t>(value.markerCount);
    static_assert(noexcept(visitor.PrepareMarkers(markersCount)));
    if (!visitor.PrepareMarkers(markersCount)) return false;
    for (std::uint64_t i = 0; i < markersCount; ++i) {
        static_assert(noexcept(visitor.Markers(i)));
        auto&& row = visitor.Markers(i);
        if (!AppendPackRow<HkPackBodyWorldMarker>(buffer, row)) return false;
    }
    static_assert(noexcept(visitor.Finish()));
    return visitor.Finish();
}

template <class Buffer, class Visitor>
[[nodiscard]] bool ReadPackBodyWorldSegment(Buffer& buffer, Visitor& visitor) noexcept {
    static_assert(noexcept(visitor.Value()));
    auto& value = visitor.Value();
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(value)>, SegmentScheduleProgramViewV1>);
    {
        static_assert(noexcept(visitor.Header()));
        auto&& row = visitor.Header();
        if (!ReadPackRow<HkPackBodyWorldSegmentHeader>(buffer, row)) return false;
    }
    const std::uint64_t trackRefsCount = static_cast<std::uint64_t>(value.trackRefCount);
    static_assert(noexcept(buffer.Remaining()));
    if (trackRefsCount > buffer.Remaining() / kPackBytes<HkPackBodyWorldTrackRef>) return false;
    static_assert(noexcept(visitor.PrepareTrackRefs(trackRefsCount)));
    if (!visitor.PrepareTrackRefs(trackRefsCount)) return false;
    for (std::uint64_t i = 0; i < trackRefsCount; ++i) {
        static_assert(noexcept(visitor.TrackRefs(i)));
        auto&& row = visitor.TrackRefs(i);
        if (!ReadPackRow<HkPackBodyWorldTrackRef>(buffer, row)) return false;
    }
    const std::uint64_t markersCount = static_cast<std::uint64_t>(value.markerCount);
    static_assert(noexcept(buffer.Remaining()));
    if (markersCount > buffer.Remaining() / kPackBytes<HkPackBodyWorldMarker>) return false;
    static_assert(noexcept(visitor.PrepareMarkers(markersCount)));
    if (!visitor.PrepareMarkers(markersCount)) return false;
    for (std::uint64_t i = 0; i < markersCount; ++i) {
        static_assert(noexcept(visitor.Markers(i)));
        auto&& row = visitor.Markers(i);
        if (!ReadPackRow<HkPackBodyWorldMarker>(buffer, row)) return false;
    }
    static_assert(noexcept(visitor.Finish()));
    return visitor.Finish();
}

template <class Visitor>
[[nodiscard]] bool MeasurePackBodyWorldSegment(Visitor& visitor, std::uint64_t& size) noexcept {
    static_assert(noexcept(visitor.Value()));
    auto& value = visitor.Value();
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(value)>, SegmentScheduleProgramViewV1>);
    if (size > UINT64_MAX - kPackBytes<HkPackBodyWorldSegmentHeader>) return false;
    size += kPackBytes<HkPackBodyWorldSegmentHeader>;
    const std::uint64_t trackRefsCount = static_cast<std::uint64_t>(value.trackRefCount);
    if (trackRefsCount > (UINT64_MAX - size) / kPackBytes<HkPackBodyWorldTrackRef>) return false;
    size += trackRefsCount * kPackBytes<HkPackBodyWorldTrackRef>;
    const std::uint64_t markersCount = static_cast<std::uint64_t>(value.markerCount);
    if (markersCount > (UINT64_MAX - size) / kPackBytes<HkPackBodyWorldMarker>) return false;
    size += markersCount * kPackBytes<HkPackBodyWorldMarker>;
    return true;
}

template <class Buffer, class Visitor>
[[nodiscard]] bool AppendPackBodyWorldCurve(Buffer& buffer, Visitor& visitor) noexcept {
    static_assert(noexcept(visitor.Value()));
    auto& value = visitor.Value();
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(value)>, CurveV1>);
    {
        static_assert(noexcept(visitor.Header()));
        auto&& row = visitor.Header();
        if (!AppendPackRow<HkPackBodyWorldCurveHeader>(buffer, row)) return false;
    }
    const std::uint64_t pointsCount = static_cast<std::uint64_t>(value.curve.pointCount);
    static_assert(noexcept(visitor.PreparePoints(pointsCount)));
    if (!visitor.PreparePoints(pointsCount)) return false;
    for (std::uint64_t i = 0; i < pointsCount; ++i) {
        static_assert(noexcept(visitor.Points(i)));
        auto&& row = visitor.Points(i);
        if (!AppendPackRow<HkPackBodyWorldCurvePoint>(buffer, row)) return false;
    }
    const std::uint64_t handlesCount = value.curve.pointCount > 1 ? static_cast<std::uint64_t>(value.curve.pointCount) - 1 : 0;
    static_assert(noexcept(visitor.PrepareHandles(handlesCount)));
    if (!visitor.PrepareHandles(handlesCount)) return false;
    for (std::uint64_t i = 0; i < handlesCount; ++i) {
        static_assert(noexcept(visitor.Handles(i)));
        auto&& row = visitor.Handles(i);
        if (!AppendPackRow<HkPackBodyWorldCurveHandle>(buffer, row)) return false;
    }
    static_assert(noexcept(visitor.Finish()));
    return visitor.Finish();
}

template <class Buffer, class Visitor>
[[nodiscard]] bool ReadPackBodyWorldCurve(Buffer& buffer, Visitor& visitor) noexcept {
    static_assert(noexcept(visitor.Value()));
    auto& value = visitor.Value();
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(value)>, CurveV1>);
    {
        static_assert(noexcept(visitor.Header()));
        auto&& row = visitor.Header();
        if (!ReadPackRow<HkPackBodyWorldCurveHeader>(buffer, row)) return false;
    }
    const std::uint64_t pointsCount = static_cast<std::uint64_t>(value.curve.pointCount);
    static_assert(noexcept(buffer.Remaining()));
    if (pointsCount > buffer.Remaining() / kPackBytes<HkPackBodyWorldCurvePoint>) return false;
    static_assert(noexcept(visitor.PreparePoints(pointsCount)));
    if (!visitor.PreparePoints(pointsCount)) return false;
    for (std::uint64_t i = 0; i < pointsCount; ++i) {
        static_assert(noexcept(visitor.Points(i)));
        auto&& row = visitor.Points(i);
        if (!ReadPackRow<HkPackBodyWorldCurvePoint>(buffer, row)) return false;
    }
    const std::uint64_t handlesCount = value.curve.pointCount > 1 ? static_cast<std::uint64_t>(value.curve.pointCount) - 1 : 0;
    static_assert(noexcept(buffer.Remaining()));
    if (handlesCount > buffer.Remaining() / kPackBytes<HkPackBodyWorldCurveHandle>) return false;
    static_assert(noexcept(visitor.PrepareHandles(handlesCount)));
    if (!visitor.PrepareHandles(handlesCount)) return false;
    for (std::uint64_t i = 0; i < handlesCount; ++i) {
        static_assert(noexcept(visitor.Handles(i)));
        auto&& row = visitor.Handles(i);
        if (!ReadPackRow<HkPackBodyWorldCurveHandle>(buffer, row)) return false;
    }
    static_assert(noexcept(visitor.Finish()));
    return visitor.Finish();
}

template <class Visitor>
[[nodiscard]] bool MeasurePackBodyWorldCurve(Visitor& visitor, std::uint64_t& size) noexcept {
    static_assert(noexcept(visitor.Value()));
    auto& value = visitor.Value();
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(value)>, CurveV1>);
    if (size > UINT64_MAX - kPackBytes<HkPackBodyWorldCurveHeader>) return false;
    size += kPackBytes<HkPackBodyWorldCurveHeader>;
    const std::uint64_t pointsCount = static_cast<std::uint64_t>(value.curve.pointCount);
    if (pointsCount > (UINT64_MAX - size) / kPackBytes<HkPackBodyWorldCurvePoint>) return false;
    size += pointsCount * kPackBytes<HkPackBodyWorldCurvePoint>;
    const std::uint64_t handlesCount = value.curve.pointCount > 1 ? static_cast<std::uint64_t>(value.curve.pointCount) - 1 : 0;
    if (handlesCount > (UINT64_MAX - size) / kPackBytes<HkPackBodyWorldCurveHandle>) return false;
    size += handlesCount * kPackBytes<HkPackBodyWorldCurveHandle>;
    return true;
}

template <class Buffer, class Visitor>
[[nodiscard]] bool AppendPackBodyWorldChild(Buffer& buffer, Visitor& visitor) noexcept {
    static_assert(noexcept(visitor.Value()));
    auto& value = visitor.Value();
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(value)>, ContainerChildProgramViewV2>);
    {
        static_assert(noexcept(visitor.Header()));
        auto&& row = visitor.Header();
        if (!AppendPackRow<HkPackBodyWorldChildHeader>(buffer, row)) return false;
    }
    if (static_cast<std::uint64_t>(value.timingHint.kind) == 1) {
        {
            static_assert(noexcept(visitor.InlineSound()));
            auto&& row = visitor.InlineSound();
            if (!AppendPackRow<HkPackBodyWorldInlineSound>(buffer, row)) return false;
        }
    }
    if (static_cast<std::uint64_t>(value.timingHint.kind) == 2) {
        {
            static_assert(noexcept(visitor.Schedule()));
            auto&& row = visitor.Schedule();
            if (!AppendPackRow<HkPackBodyWorldScheduleKey>(buffer, row)) return false;
        }
    }
    static_assert(noexcept(visitor.Finish()));
    return visitor.Finish();
}

template <class Buffer, class Visitor>
[[nodiscard]] bool ReadPackBodyWorldChild(Buffer& buffer, Visitor& visitor) noexcept {
    static_assert(noexcept(visitor.Value()));
    auto& value = visitor.Value();
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(value)>, ContainerChildProgramViewV2>);
    {
        static_assert(noexcept(visitor.Header()));
        auto&& row = visitor.Header();
        if (!ReadPackRow<HkPackBodyWorldChildHeader>(buffer, row)) return false;
    }
    if (static_cast<std::uint64_t>(value.timingHint.kind) == 1) {
        {
            static_assert(noexcept(visitor.InlineSound()));
            auto&& row = visitor.InlineSound();
            if (!ReadPackRow<HkPackBodyWorldInlineSound>(buffer, row)) return false;
        }
    }
    if (static_cast<std::uint64_t>(value.timingHint.kind) == 2) {
        {
            static_assert(noexcept(visitor.Schedule()));
            auto&& row = visitor.Schedule();
            if (!ReadPackRow<HkPackBodyWorldScheduleKey>(buffer, row)) return false;
        }
    }
    static_assert(noexcept(visitor.Finish()));
    return visitor.Finish();
}

template <class Visitor>
[[nodiscard]] bool MeasurePackBodyWorldChild(Visitor& visitor, std::uint64_t& size) noexcept {
    static_assert(noexcept(visitor.Value()));
    auto& value = visitor.Value();
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(value)>, ContainerChildProgramViewV2>);
    if (size > UINT64_MAX - kPackBytes<HkPackBodyWorldChildHeader>) return false;
    size += kPackBytes<HkPackBodyWorldChildHeader>;
    if (static_cast<std::uint64_t>(value.timingHint.kind) == 1) {
        if (size > UINT64_MAX - kPackBytes<HkPackBodyWorldInlineSound>) return false;
        size += kPackBytes<HkPackBodyWorldInlineSound>;
    }
    if (static_cast<std::uint64_t>(value.timingHint.kind) == 2) {
        if (size > UINT64_MAX - kPackBytes<HkPackBodyWorldScheduleKey>) return false;
        size += kPackBytes<HkPackBodyWorldScheduleKey>;
    }
    return true;
}

template <class Buffer, class Visitor>
[[nodiscard]] bool AppendPackBodyWorldDiscreteGrid(Buffer& buffer, Visitor& visitor) noexcept {
    static_assert(noexcept(visitor.Value()));
    auto& value = visitor.Value();
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(value)>, DiscreteGridRowViewV1>);
    {
        static_assert(noexcept(visitor.Header()));
        auto&& row = visitor.Header();
        if (!AppendPackRow<HkPackBodyWorldDiscreteGridHeader>(buffer, row)) return false;
    }
    const std::uint64_t cellsCount = static_cast<std::uint64_t>(value.cellCount);
    static_assert(noexcept(visitor.PrepareCells(cellsCount)));
    if (!visitor.PrepareCells(cellsCount)) return false;
    for (std::uint64_t i = 0; i < cellsCount; ++i) {
        static_assert(noexcept(visitor.Cells(i)));
        auto&& row = visitor.Cells(i);
        if (!AppendPackRow<HkPackBodyWorldDiscreteCell>(buffer, row)) return false;
    }
    static_assert(noexcept(visitor.Finish()));
    return visitor.Finish();
}

template <class Buffer, class Visitor>
[[nodiscard]] bool ReadPackBodyWorldDiscreteGrid(Buffer& buffer, Visitor& visitor) noexcept {
    static_assert(noexcept(visitor.Value()));
    auto& value = visitor.Value();
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(value)>, DiscreteGridRowViewV1>);
    {
        static_assert(noexcept(visitor.Header()));
        auto&& row = visitor.Header();
        if (!ReadPackRow<HkPackBodyWorldDiscreteGridHeader>(buffer, row)) return false;
    }
    const std::uint64_t cellsCount = static_cast<std::uint64_t>(value.cellCount);
    static_assert(noexcept(buffer.Remaining()));
    if (cellsCount > buffer.Remaining() / kPackBytes<HkPackBodyWorldDiscreteCell>) return false;
    static_assert(noexcept(visitor.PrepareCells(cellsCount)));
    if (!visitor.PrepareCells(cellsCount)) return false;
    for (std::uint64_t i = 0; i < cellsCount; ++i) {
        static_assert(noexcept(visitor.Cells(i)));
        auto&& row = visitor.Cells(i);
        if (!ReadPackRow<HkPackBodyWorldDiscreteCell>(buffer, row)) return false;
    }
    static_assert(noexcept(visitor.Finish()));
    return visitor.Finish();
}

template <class Visitor>
[[nodiscard]] bool MeasurePackBodyWorldDiscreteGrid(Visitor& visitor, std::uint64_t& size) noexcept {
    static_assert(noexcept(visitor.Value()));
    auto& value = visitor.Value();
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(value)>, DiscreteGridRowViewV1>);
    if (size > UINT64_MAX - kPackBytes<HkPackBodyWorldDiscreteGridHeader>) return false;
    size += kPackBytes<HkPackBodyWorldDiscreteGridHeader>;
    const std::uint64_t cellsCount = static_cast<std::uint64_t>(value.cellCount);
    if (cellsCount > (UINT64_MAX - size) / kPackBytes<HkPackBodyWorldDiscreteCell>) return false;
    size += cellsCount * kPackBytes<HkPackBodyWorldDiscreteCell>;
    return true;
}

template <class Buffer, class Visitor>
[[nodiscard]] bool AppendPackBodyWorldContinuousGrid(Buffer& buffer, Visitor& visitor) noexcept {
    static_assert(noexcept(visitor.Value()));
    auto& value = visitor.Value();
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(value)>, ContinuousGridRowViewV1>);
    {
        static_assert(noexcept(visitor.Header()));
        auto&& row = visitor.Header();
        if (!AppendPackRow<HkPackBodyWorldContinuousGridHeader>(buffer, row)) return false;
    }
    const std::uint64_t cellsCount = static_cast<std::uint64_t>(value.cellCount);
    static_assert(noexcept(visitor.PrepareCells(cellsCount)));
    if (!visitor.PrepareCells(cellsCount)) return false;
    for (std::uint64_t i = 0; i < cellsCount; ++i) {
        static_assert(noexcept(visitor.Cells(i)));
        auto&& row = visitor.Cells(i);
        if (!AppendPackRow<HkPackBodyWorldContinuousCell>(buffer, row)) return false;
    }
    static_assert(noexcept(visitor.Finish()));
    return visitor.Finish();
}

template <class Buffer, class Visitor>
[[nodiscard]] bool ReadPackBodyWorldContinuousGrid(Buffer& buffer, Visitor& visitor) noexcept {
    static_assert(noexcept(visitor.Value()));
    auto& value = visitor.Value();
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(value)>, ContinuousGridRowViewV1>);
    {
        static_assert(noexcept(visitor.Header()));
        auto&& row = visitor.Header();
        if (!ReadPackRow<HkPackBodyWorldContinuousGridHeader>(buffer, row)) return false;
    }
    const std::uint64_t cellsCount = static_cast<std::uint64_t>(value.cellCount);
    static_assert(noexcept(buffer.Remaining()));
    if (cellsCount > buffer.Remaining() / kPackBytes<HkPackBodyWorldContinuousCell>) return false;
    static_assert(noexcept(visitor.PrepareCells(cellsCount)));
    if (!visitor.PrepareCells(cellsCount)) return false;
    for (std::uint64_t i = 0; i < cellsCount; ++i) {
        static_assert(noexcept(visitor.Cells(i)));
        auto&& row = visitor.Cells(i);
        if (!ReadPackRow<HkPackBodyWorldContinuousCell>(buffer, row)) return false;
    }
    static_assert(noexcept(visitor.Finish()));
    return visitor.Finish();
}

template <class Visitor>
[[nodiscard]] bool MeasurePackBodyWorldContinuousGrid(Visitor& visitor, std::uint64_t& size) noexcept {
    static_assert(noexcept(visitor.Value()));
    auto& value = visitor.Value();
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(value)>, ContinuousGridRowViewV1>);
    if (size > UINT64_MAX - kPackBytes<HkPackBodyWorldContinuousGridHeader>) return false;
    size += kPackBytes<HkPackBodyWorldContinuousGridHeader>;
    const std::uint64_t cellsCount = static_cast<std::uint64_t>(value.cellCount);
    if (cellsCount > (UINT64_MAX - size) / kPackBytes<HkPackBodyWorldContinuousCell>) return false;
    size += cellsCount * kPackBytes<HkPackBodyWorldContinuousCell>;
    return true;
}

template <class Buffer, class Visitor>
[[nodiscard]] bool AppendPackBodyWorldContainer(Buffer& buffer, Visitor& visitor) noexcept {
    static_assert(noexcept(visitor.Value()));
    auto& value = visitor.Value();
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(value)>, ContainerProgramViewV3>);
    {
        static_assert(noexcept(visitor.Header()));
        auto&& row = visitor.Header();
        if (!AppendPackRow<HkPackBodyWorldContainerHeader>(buffer, row)) return false;
    }
    const std::uint64_t curvesCount = static_cast<std::uint64_t>(value.curveCount);
    static_assert(noexcept(visitor.PrepareCurves(curvesCount)));
    if (!visitor.PrepareCurves(curvesCount)) return false;
    for (std::uint64_t i = 0; i < curvesCount; ++i) {
        static_assert(noexcept(visitor.Curves(i)));
        auto&& row = visitor.Curves(i);
        if (!AppendPackBodyWorldCurve(buffer, row)) return false;
    }
    const std::uint64_t childrenCount = static_cast<std::uint64_t>(value.childCount);
    static_assert(noexcept(visitor.PrepareChildren(childrenCount)));
    if (!visitor.PrepareChildren(childrenCount)) return false;
    for (std::uint64_t i = 0; i < childrenCount; ++i) {
        static_assert(noexcept(visitor.Children(i)));
        auto&& row = visitor.Children(i);
        if (!AppendPackBodyWorldChild(buffer, row)) return false;
    }
    const std::uint64_t discreteAxesCount = static_cast<std::uint64_t>(value.discreteAxisCount);
    static_assert(noexcept(visitor.PrepareDiscreteAxes(discreteAxesCount)));
    if (!visitor.PrepareDiscreteAxes(discreteAxesCount)) return false;
    for (std::uint64_t i = 0; i < discreteAxesCount; ++i) {
        static_assert(noexcept(visitor.DiscreteAxes(i)));
        auto&& row = visitor.DiscreteAxes(i);
        if (!AppendPackRow<HkPackBodyWorldGridAxis>(buffer, row)) return false;
    }
    const std::uint64_t discreteRowsCount = static_cast<std::uint64_t>(value.discreteRowCount);
    static_assert(noexcept(visitor.PrepareDiscreteRows(discreteRowsCount)));
    if (!visitor.PrepareDiscreteRows(discreteRowsCount)) return false;
    for (std::uint64_t i = 0; i < discreteRowsCount; ++i) {
        static_assert(noexcept(visitor.DiscreteRows(i)));
        auto&& row = visitor.DiscreteRows(i);
        if (!AppendPackBodyWorldDiscreteGrid(buffer, row)) return false;
    }
    const std::uint64_t continuousAxesCount = static_cast<std::uint64_t>(value.continuousAxisCount);
    static_assert(noexcept(visitor.PrepareContinuousAxes(continuousAxesCount)));
    if (!visitor.PrepareContinuousAxes(continuousAxesCount)) return false;
    for (std::uint64_t i = 0; i < continuousAxesCount; ++i) {
        static_assert(noexcept(visitor.ContinuousAxes(i)));
        auto&& row = visitor.ContinuousAxes(i);
        if (!AppendPackRow<HkPackBodyWorldGridAxis>(buffer, row)) return false;
    }
    const std::uint64_t continuousRowsCount = static_cast<std::uint64_t>(value.continuousRowCount);
    static_assert(noexcept(visitor.PrepareContinuousRows(continuousRowsCount)));
    if (!visitor.PrepareContinuousRows(continuousRowsCount)) return false;
    for (std::uint64_t i = 0; i < continuousRowsCount; ++i) {
        static_assert(noexcept(visitor.ContinuousRows(i)));
        auto&& row = visitor.ContinuousRows(i);
        if (!AppendPackBodyWorldContinuousGrid(buffer, row)) return false;
    }
    static_assert(noexcept(visitor.Finish()));
    return visitor.Finish();
}

template <class Buffer, class Visitor>
[[nodiscard]] bool ReadPackBodyWorldContainer(Buffer& buffer, Visitor& visitor) noexcept {
    static_assert(noexcept(visitor.Value()));
    auto& value = visitor.Value();
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(value)>, ContainerProgramViewV3>);
    {
        static_assert(noexcept(visitor.Header()));
        auto&& row = visitor.Header();
        if (!ReadPackRow<HkPackBodyWorldContainerHeader>(buffer, row)) return false;
    }
    const std::uint64_t curvesCount = static_cast<std::uint64_t>(value.curveCount);
    static_assert(noexcept(buffer.Remaining()));
    if (curvesCount > buffer.Remaining() / kPackBytes<HkPackBodyWorldCurveHeader>) return false;
    static_assert(noexcept(visitor.PrepareCurves(curvesCount)));
    if (!visitor.PrepareCurves(curvesCount)) return false;
    for (std::uint64_t i = 0; i < curvesCount; ++i) {
        static_assert(noexcept(visitor.Curves(i)));
        auto&& row = visitor.Curves(i);
        if (!ReadPackBodyWorldCurve(buffer, row)) return false;
    }
    const std::uint64_t childrenCount = static_cast<std::uint64_t>(value.childCount);
    static_assert(noexcept(buffer.Remaining()));
    if (childrenCount > buffer.Remaining() / kPackBytes<HkPackBodyWorldChildHeader>) return false;
    static_assert(noexcept(visitor.PrepareChildren(childrenCount)));
    if (!visitor.PrepareChildren(childrenCount)) return false;
    for (std::uint64_t i = 0; i < childrenCount; ++i) {
        static_assert(noexcept(visitor.Children(i)));
        auto&& row = visitor.Children(i);
        if (!ReadPackBodyWorldChild(buffer, row)) return false;
    }
    const std::uint64_t discreteAxesCount = static_cast<std::uint64_t>(value.discreteAxisCount);
    static_assert(noexcept(buffer.Remaining()));
    if (discreteAxesCount > buffer.Remaining() / kPackBytes<HkPackBodyWorldGridAxis>) return false;
    static_assert(noexcept(visitor.PrepareDiscreteAxes(discreteAxesCount)));
    if (!visitor.PrepareDiscreteAxes(discreteAxesCount)) return false;
    for (std::uint64_t i = 0; i < discreteAxesCount; ++i) {
        static_assert(noexcept(visitor.DiscreteAxes(i)));
        auto&& row = visitor.DiscreteAxes(i);
        if (!ReadPackRow<HkPackBodyWorldGridAxis>(buffer, row)) return false;
    }
    const std::uint64_t discreteRowsCount = static_cast<std::uint64_t>(value.discreteRowCount);
    static_assert(noexcept(buffer.Remaining()));
    if (discreteRowsCount > buffer.Remaining() / kPackBytes<HkPackBodyWorldDiscreteGridHeader>) return false;
    static_assert(noexcept(visitor.PrepareDiscreteRows(discreteRowsCount)));
    if (!visitor.PrepareDiscreteRows(discreteRowsCount)) return false;
    for (std::uint64_t i = 0; i < discreteRowsCount; ++i) {
        static_assert(noexcept(visitor.DiscreteRows(i)));
        auto&& row = visitor.DiscreteRows(i);
        if (!ReadPackBodyWorldDiscreteGrid(buffer, row)) return false;
    }
    const std::uint64_t continuousAxesCount = static_cast<std::uint64_t>(value.continuousAxisCount);
    static_assert(noexcept(buffer.Remaining()));
    if (continuousAxesCount > buffer.Remaining() / kPackBytes<HkPackBodyWorldGridAxis>) return false;
    static_assert(noexcept(visitor.PrepareContinuousAxes(continuousAxesCount)));
    if (!visitor.PrepareContinuousAxes(continuousAxesCount)) return false;
    for (std::uint64_t i = 0; i < continuousAxesCount; ++i) {
        static_assert(noexcept(visitor.ContinuousAxes(i)));
        auto&& row = visitor.ContinuousAxes(i);
        if (!ReadPackRow<HkPackBodyWorldGridAxis>(buffer, row)) return false;
    }
    const std::uint64_t continuousRowsCount = static_cast<std::uint64_t>(value.continuousRowCount);
    static_assert(noexcept(buffer.Remaining()));
    if (continuousRowsCount > buffer.Remaining() / kPackBytes<HkPackBodyWorldContinuousGridHeader>) return false;
    static_assert(noexcept(visitor.PrepareContinuousRows(continuousRowsCount)));
    if (!visitor.PrepareContinuousRows(continuousRowsCount)) return false;
    for (std::uint64_t i = 0; i < continuousRowsCount; ++i) {
        static_assert(noexcept(visitor.ContinuousRows(i)));
        auto&& row = visitor.ContinuousRows(i);
        if (!ReadPackBodyWorldContinuousGrid(buffer, row)) return false;
    }
    static_assert(noexcept(visitor.Finish()));
    return visitor.Finish();
}

template <class Visitor>
[[nodiscard]] bool MeasurePackBodyWorldContainer(Visitor& visitor, std::uint64_t& size) noexcept {
    static_assert(noexcept(visitor.Value()));
    auto& value = visitor.Value();
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(value)>, ContainerProgramViewV3>);
    if (size > UINT64_MAX - kPackBytes<HkPackBodyWorldContainerHeader>) return false;
    size += kPackBytes<HkPackBodyWorldContainerHeader>;
    const std::uint64_t curvesCount = static_cast<std::uint64_t>(value.curveCount);
    static_assert(noexcept(visitor.PrepareCurves(curvesCount)));
    if (!visitor.PrepareCurves(curvesCount)) return false;
    for (std::uint64_t i = 0; i < curvesCount; ++i) {
        static_assert(noexcept(visitor.Curves(i)));
        auto&& row = visitor.Curves(i);
        if (!MeasurePackBodyWorldCurve(row, size)) return false;
    }
    const std::uint64_t childrenCount = static_cast<std::uint64_t>(value.childCount);
    static_assert(noexcept(visitor.PrepareChildren(childrenCount)));
    if (!visitor.PrepareChildren(childrenCount)) return false;
    for (std::uint64_t i = 0; i < childrenCount; ++i) {
        static_assert(noexcept(visitor.Children(i)));
        auto&& row = visitor.Children(i);
        if (!MeasurePackBodyWorldChild(row, size)) return false;
    }
    const std::uint64_t discreteAxesCount = static_cast<std::uint64_t>(value.discreteAxisCount);
    if (discreteAxesCount > (UINT64_MAX - size) / kPackBytes<HkPackBodyWorldGridAxis>) return false;
    size += discreteAxesCount * kPackBytes<HkPackBodyWorldGridAxis>;
    const std::uint64_t discreteRowsCount = static_cast<std::uint64_t>(value.discreteRowCount);
    static_assert(noexcept(visitor.PrepareDiscreteRows(discreteRowsCount)));
    if (!visitor.PrepareDiscreteRows(discreteRowsCount)) return false;
    for (std::uint64_t i = 0; i < discreteRowsCount; ++i) {
        static_assert(noexcept(visitor.DiscreteRows(i)));
        auto&& row = visitor.DiscreteRows(i);
        if (!MeasurePackBodyWorldDiscreteGrid(row, size)) return false;
    }
    const std::uint64_t continuousAxesCount = static_cast<std::uint64_t>(value.continuousAxisCount);
    if (continuousAxesCount > (UINT64_MAX - size) / kPackBytes<HkPackBodyWorldGridAxis>) return false;
    size += continuousAxesCount * kPackBytes<HkPackBodyWorldGridAxis>;
    const std::uint64_t continuousRowsCount = static_cast<std::uint64_t>(value.continuousRowCount);
    static_assert(noexcept(visitor.PrepareContinuousRows(continuousRowsCount)));
    if (!visitor.PrepareContinuousRows(continuousRowsCount)) return false;
    for (std::uint64_t i = 0; i < continuousRowsCount; ++i) {
        static_assert(noexcept(visitor.ContinuousRows(i)));
        auto&& row = visitor.ContinuousRows(i);
        if (!MeasurePackBodyWorldContinuousGrid(row, size)) return false;
    }
    return true;
}

template <class Buffer, class Visitor>
[[nodiscard]] bool AppendPackBodyWorldTransition(Buffer& buffer, Visitor& visitor) noexcept {
    static_assert(noexcept(visitor.Value()));
    auto& value = visitor.Value();
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(value)>, TransitionProgramViewV1>);
    {
        static_assert(noexcept(visitor.Header()));
        auto&& row = visitor.Header();
        if (!AppendPackRow<HkPackBodyWorldTransitionHeader>(buffer, row)) return false;
    }
    const std::uint64_t pathCount = static_cast<std::uint64_t>(value.downwardPathStepCount);
    static_assert(noexcept(visitor.PreparePath(pathCount)));
    if (!visitor.PreparePath(pathCount)) return false;
    for (std::uint64_t i = 0; i < pathCount; ++i) {
        static_assert(noexcept(visitor.Path(i)));
        auto&& row = visitor.Path(i);
        if (!AppendPackRow<HkPackBodyWorldPathStep>(buffer, row)) return false;
    }
    const std::uint64_t rulesCount = static_cast<std::uint64_t>(value.ruleCount);
    static_assert(noexcept(visitor.PrepareRules(rulesCount)));
    if (!visitor.PrepareRules(rulesCount)) return false;
    for (std::uint64_t i = 0; i < rulesCount; ++i) {
        static_assert(noexcept(visitor.Rules(i)));
        auto&& row = visitor.Rules(i);
        if (!AppendPackRow<HkPackBodyWorldTransitionRule>(buffer, row)) return false;
    }
    static_assert(noexcept(visitor.Finish()));
    return visitor.Finish();
}

template <class Buffer, class Visitor>
[[nodiscard]] bool ReadPackBodyWorldTransition(Buffer& buffer, Visitor& visitor) noexcept {
    static_assert(noexcept(visitor.Value()));
    auto& value = visitor.Value();
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(value)>, TransitionProgramViewV1>);
    {
        static_assert(noexcept(visitor.Header()));
        auto&& row = visitor.Header();
        if (!ReadPackRow<HkPackBodyWorldTransitionHeader>(buffer, row)) return false;
    }
    const std::uint64_t pathCount = static_cast<std::uint64_t>(value.downwardPathStepCount);
    static_assert(noexcept(buffer.Remaining()));
    if (pathCount > buffer.Remaining() / kPackBytes<HkPackBodyWorldPathStep>) return false;
    static_assert(noexcept(visitor.PreparePath(pathCount)));
    if (!visitor.PreparePath(pathCount)) return false;
    for (std::uint64_t i = 0; i < pathCount; ++i) {
        static_assert(noexcept(visitor.Path(i)));
        auto&& row = visitor.Path(i);
        if (!ReadPackRow<HkPackBodyWorldPathStep>(buffer, row)) return false;
    }
    const std::uint64_t rulesCount = static_cast<std::uint64_t>(value.ruleCount);
    static_assert(noexcept(buffer.Remaining()));
    if (rulesCount > buffer.Remaining() / kPackBytes<HkPackBodyWorldTransitionRule>) return false;
    static_assert(noexcept(visitor.PrepareRules(rulesCount)));
    if (!visitor.PrepareRules(rulesCount)) return false;
    for (std::uint64_t i = 0; i < rulesCount; ++i) {
        static_assert(noexcept(visitor.Rules(i)));
        auto&& row = visitor.Rules(i);
        if (!ReadPackRow<HkPackBodyWorldTransitionRule>(buffer, row)) return false;
    }
    static_assert(noexcept(visitor.Finish()));
    return visitor.Finish();
}

template <class Visitor>
[[nodiscard]] bool MeasurePackBodyWorldTransition(Visitor& visitor, std::uint64_t& size) noexcept {
    static_assert(noexcept(visitor.Value()));
    auto& value = visitor.Value();
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(value)>, TransitionProgramViewV1>);
    if (size > UINT64_MAX - kPackBytes<HkPackBodyWorldTransitionHeader>) return false;
    size += kPackBytes<HkPackBodyWorldTransitionHeader>;
    const std::uint64_t pathCount = static_cast<std::uint64_t>(value.downwardPathStepCount);
    if (pathCount > (UINT64_MAX - size) / kPackBytes<HkPackBodyWorldPathStep>) return false;
    size += pathCount * kPackBytes<HkPackBodyWorldPathStep>;
    const std::uint64_t rulesCount = static_cast<std::uint64_t>(value.ruleCount);
    if (rulesCount > (UINT64_MAX - size) / kPackBytes<HkPackBodyWorldTransitionRule>) return false;
    size += rulesCount * kPackBytes<HkPackBodyWorldTransitionRule>;
    return true;
}

template <class Buffer, class Visitor>
[[nodiscard]] bool AppendPackBodyWorldBody(Buffer& buffer, Visitor& visitor) noexcept {
    static_assert(noexcept(visitor.Value()));
    auto& value = visitor.Value();
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(value)>, ContainerWorldInstallViewV1>);
    {
        static_assert(noexcept(visitor.Header()));
        auto&& row = visitor.Header();
        if (!AppendPackBodyWorldBodyHeader(buffer, row)) return false;
    }
    const std::uint64_t variablesCount = static_cast<std::uint64_t>(value.variableCount);
    static_assert(noexcept(visitor.PrepareVariables(variablesCount)));
    if (!visitor.PrepareVariables(variablesCount)) return false;
    for (std::uint64_t i = 0; i < variablesCount; ++i) {
        static_assert(noexcept(visitor.Variables(i)));
        auto&& row = visitor.Variables(i);
        if (!AppendPackBodyWorldVariable(buffer, row)) return false;
    }
    const std::uint64_t propertiesCount = static_cast<std::uint64_t>(value.playablePropertyCount);
    static_assert(noexcept(visitor.PrepareProperties(propertiesCount)));
    if (!visitor.PrepareProperties(propertiesCount)) return false;
    for (std::uint64_t i = 0; i < propertiesCount; ++i) {
        static_assert(noexcept(visitor.Properties(i)));
        auto&& row = visitor.Properties(i);
        if (!AppendPackBodyWorldProperty(buffer, row)) return false;
    }
    const std::uint64_t occurrencesCount = static_cast<std::uint64_t>(value.playableOccurrenceCount);
    static_assert(noexcept(visitor.PrepareOccurrences(occurrencesCount)));
    if (!visitor.PrepareOccurrences(occurrencesCount)) return false;
    for (std::uint64_t i = 0; i < occurrencesCount; ++i) {
        static_assert(noexcept(visitor.Occurrences(i)));
        auto&& row = visitor.Occurrences(i);
        if (!AppendPackBodyWorldOccurrence(buffer, row)) return false;
    }
    const std::uint64_t trackSchedulesCount = static_cast<std::uint64_t>(value.trackScheduleCount);
    static_assert(noexcept(visitor.PrepareTrackSchedules(trackSchedulesCount)));
    if (!visitor.PrepareTrackSchedules(trackSchedulesCount)) return false;
    for (std::uint64_t i = 0; i < trackSchedulesCount; ++i) {
        static_assert(noexcept(visitor.TrackSchedules(i)));
        auto&& row = visitor.TrackSchedules(i);
        if (!AppendPackBodyWorldTrack(buffer, row)) return false;
    }
    const std::uint64_t segmentSchedulesCount = static_cast<std::uint64_t>(value.segmentScheduleCount);
    static_assert(noexcept(visitor.PrepareSegmentSchedules(segmentSchedulesCount)));
    if (!visitor.PrepareSegmentSchedules(segmentSchedulesCount)) return false;
    for (std::uint64_t i = 0; i < segmentSchedulesCount; ++i) {
        static_assert(noexcept(visitor.SegmentSchedules(i)));
        auto&& row = visitor.SegmentSchedules(i);
        if (!AppendPackBodyWorldSegment(buffer, row)) return false;
    }
    const std::uint64_t containersCount = static_cast<std::uint64_t>(value.containerCount);
    static_assert(noexcept(visitor.PrepareContainers(containersCount)));
    if (!visitor.PrepareContainers(containersCount)) return false;
    for (std::uint64_t i = 0; i < containersCount; ++i) {
        static_assert(noexcept(visitor.Containers(i)));
        auto&& row = visitor.Containers(i);
        if (!AppendPackBodyWorldContainer(buffer, row)) return false;
    }
    const std::uint64_t transitionsCount = static_cast<std::uint64_t>(value.transitionCount);
    static_assert(noexcept(visitor.PrepareTransitions(transitionsCount)));
    if (!visitor.PrepareTransitions(transitionsCount)) return false;
    for (std::uint64_t i = 0; i < transitionsCount; ++i) {
        static_assert(noexcept(visitor.Transitions(i)));
        auto&& row = visitor.Transitions(i);
        if (!AppendPackBodyWorldTransition(buffer, row)) return false;
    }
    const std::uint64_t actionsCount = static_cast<std::uint64_t>(value.actionCount);
    static_assert(noexcept(visitor.PrepareActions(actionsCount)));
    if (!visitor.PrepareActions(actionsCount)) return false;
    for (std::uint64_t i = 0; i < actionsCount; ++i) {
        static_assert(noexcept(visitor.Actions(i)));
        auto&& row = visitor.Actions(i);
        if (!AppendPackBodyWorldAction(buffer, row)) return false;
    }
    static_assert(noexcept(visitor.Finish()));
    return visitor.Finish();
}

template <class Buffer, class Visitor>
[[nodiscard]] bool ReadPackBodyWorldBody(Buffer& buffer, Visitor& visitor) noexcept {
    static_assert(noexcept(visitor.Value()));
    auto& value = visitor.Value();
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(value)>, ContainerWorldInstallViewV1>);
    {
        static_assert(noexcept(visitor.Header()));
        auto&& row = visitor.Header();
        if (!ReadPackBodyWorldBodyHeader(buffer, row)) return false;
    }
    const std::uint64_t variablesCount = static_cast<std::uint64_t>(value.variableCount);
    static_assert(noexcept(buffer.Remaining()));
    if (variablesCount > buffer.Remaining() / kPackBytes<HkPackBodyWorldVariableHeader>) return false;
    static_assert(noexcept(visitor.PrepareVariables(variablesCount)));
    if (!visitor.PrepareVariables(variablesCount)) return false;
    for (std::uint64_t i = 0; i < variablesCount; ++i) {
        static_assert(noexcept(visitor.Variables(i)));
        auto&& row = visitor.Variables(i);
        if (!ReadPackBodyWorldVariable(buffer, row)) return false;
    }
    const std::uint64_t propertiesCount = static_cast<std::uint64_t>(value.playablePropertyCount);
    static_assert(noexcept(buffer.Remaining()));
    if (propertiesCount > buffer.Remaining() / (kPackBytes<HkPackBodyWorldPropertyHeader> + kPackBytes<HkPackBodyPayloadHeader> + kPackBytes<HkPackBodyWorldStructuralHeader>)) return false;
    static_assert(noexcept(visitor.PrepareProperties(propertiesCount)));
    if (!visitor.PrepareProperties(propertiesCount)) return false;
    for (std::uint64_t i = 0; i < propertiesCount; ++i) {
        static_assert(noexcept(visitor.Properties(i)));
        auto&& row = visitor.Properties(i);
        if (!ReadPackBodyWorldProperty(buffer, row)) return false;
    }
    const std::uint64_t occurrencesCount = static_cast<std::uint64_t>(value.playableOccurrenceCount);
    static_assert(noexcept(buffer.Remaining()));
    if (occurrencesCount > buffer.Remaining() / (kPackBytes<HkPackBodyWorldOccurrenceHeader> + kPackBytes<HkPackBodyPayloadHeader>)) return false;
    static_assert(noexcept(visitor.PrepareOccurrences(occurrencesCount)));
    if (!visitor.PrepareOccurrences(occurrencesCount)) return false;
    for (std::uint64_t i = 0; i < occurrencesCount; ++i) {
        static_assert(noexcept(visitor.Occurrences(i)));
        auto&& row = visitor.Occurrences(i);
        if (!ReadPackBodyWorldOccurrence(buffer, row)) return false;
    }
    const std::uint64_t trackSchedulesCount = static_cast<std::uint64_t>(value.trackScheduleCount);
    static_assert(noexcept(buffer.Remaining()));
    if (trackSchedulesCount > buffer.Remaining() / kPackBytes<HkPackBodyWorldTrackHeader>) return false;
    static_assert(noexcept(visitor.PrepareTrackSchedules(trackSchedulesCount)));
    if (!visitor.PrepareTrackSchedules(trackSchedulesCount)) return false;
    for (std::uint64_t i = 0; i < trackSchedulesCount; ++i) {
        static_assert(noexcept(visitor.TrackSchedules(i)));
        auto&& row = visitor.TrackSchedules(i);
        if (!ReadPackBodyWorldTrack(buffer, row)) return false;
    }
    const std::uint64_t segmentSchedulesCount = static_cast<std::uint64_t>(value.segmentScheduleCount);
    static_assert(noexcept(buffer.Remaining()));
    if (segmentSchedulesCount > buffer.Remaining() / kPackBytes<HkPackBodyWorldSegmentHeader>) return false;
    static_assert(noexcept(visitor.PrepareSegmentSchedules(segmentSchedulesCount)));
    if (!visitor.PrepareSegmentSchedules(segmentSchedulesCount)) return false;
    for (std::uint64_t i = 0; i < segmentSchedulesCount; ++i) {
        static_assert(noexcept(visitor.SegmentSchedules(i)));
        auto&& row = visitor.SegmentSchedules(i);
        if (!ReadPackBodyWorldSegment(buffer, row)) return false;
    }
    const std::uint64_t containersCount = static_cast<std::uint64_t>(value.containerCount);
    static_assert(noexcept(buffer.Remaining()));
    if (containersCount > buffer.Remaining() / kPackBytes<HkPackBodyWorldContainerHeader>) return false;
    static_assert(noexcept(visitor.PrepareContainers(containersCount)));
    if (!visitor.PrepareContainers(containersCount)) return false;
    for (std::uint64_t i = 0; i < containersCount; ++i) {
        static_assert(noexcept(visitor.Containers(i)));
        auto&& row = visitor.Containers(i);
        if (!ReadPackBodyWorldContainer(buffer, row)) return false;
    }
    const std::uint64_t transitionsCount = static_cast<std::uint64_t>(value.transitionCount);
    static_assert(noexcept(buffer.Remaining()));
    if (transitionsCount > buffer.Remaining() / kPackBytes<HkPackBodyWorldTransitionHeader>) return false;
    static_assert(noexcept(visitor.PrepareTransitions(transitionsCount)));
    if (!visitor.PrepareTransitions(transitionsCount)) return false;
    for (std::uint64_t i = 0; i < transitionsCount; ++i) {
        static_assert(noexcept(visitor.Transitions(i)));
        auto&& row = visitor.Transitions(i);
        if (!ReadPackBodyWorldTransition(buffer, row)) return false;
    }
    const std::uint64_t actionsCount = static_cast<std::uint64_t>(value.actionCount);
    static_assert(noexcept(buffer.Remaining()));
    if (actionsCount > buffer.Remaining() / kPackBytes<HkPackBodyWorldActionHeader>) return false;
    static_assert(noexcept(visitor.PrepareActions(actionsCount)));
    if (!visitor.PrepareActions(actionsCount)) return false;
    for (std::uint64_t i = 0; i < actionsCount; ++i) {
        static_assert(noexcept(visitor.Actions(i)));
        auto&& row = visitor.Actions(i);
        if (!ReadPackBodyWorldAction(buffer, row)) return false;
    }
    static_assert(noexcept(visitor.Finish()));
    return visitor.Finish();
}

template <class Visitor>
[[nodiscard]] bool MeasurePackBodyWorldBody(Visitor& visitor, std::uint64_t& size) noexcept {
    static_assert(noexcept(visitor.Value()));
    auto& value = visitor.Value();
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(value)>, ContainerWorldInstallViewV1>);
    if (size > UINT64_MAX - kPackBodyWorldBodyHeaderBytes) return false;
    size += kPackBodyWorldBodyHeaderBytes;
    const std::uint64_t variablesCount = static_cast<std::uint64_t>(value.variableCount);
    if (variablesCount > (UINT64_MAX - size) / kPackBytes<HkPackBodyWorldVariableHeader>) return false;
    size += variablesCount * kPackBytes<HkPackBodyWorldVariableHeader>;
    const std::uint64_t propertiesCount = static_cast<std::uint64_t>(value.playablePropertyCount);
    static_assert(noexcept(visitor.PrepareProperties(propertiesCount)));
    if (!visitor.PrepareProperties(propertiesCount)) return false;
    for (std::uint64_t i = 0; i < propertiesCount; ++i) {
        static_assert(noexcept(visitor.Properties(i)));
        auto&& row = visitor.Properties(i);
        if (!MeasurePackBodyWorldProperty(row, size)) return false;
    }
    const std::uint64_t occurrencesCount = static_cast<std::uint64_t>(value.playableOccurrenceCount);
    static_assert(noexcept(visitor.PrepareOccurrences(occurrencesCount)));
    if (!visitor.PrepareOccurrences(occurrencesCount)) return false;
    for (std::uint64_t i = 0; i < occurrencesCount; ++i) {
        static_assert(noexcept(visitor.Occurrences(i)));
        auto&& row = visitor.Occurrences(i);
        if (!MeasurePackBodyWorldOccurrence(row, size)) return false;
    }
    const std::uint64_t trackSchedulesCount = static_cast<std::uint64_t>(value.trackScheduleCount);
    static_assert(noexcept(visitor.PrepareTrackSchedules(trackSchedulesCount)));
    if (!visitor.PrepareTrackSchedules(trackSchedulesCount)) return false;
    for (std::uint64_t i = 0; i < trackSchedulesCount; ++i) {
        static_assert(noexcept(visitor.TrackSchedules(i)));
        auto&& row = visitor.TrackSchedules(i);
        if (!MeasurePackBodyWorldTrack(row, size)) return false;
    }
    const std::uint64_t segmentSchedulesCount = static_cast<std::uint64_t>(value.segmentScheduleCount);
    static_assert(noexcept(visitor.PrepareSegmentSchedules(segmentSchedulesCount)));
    if (!visitor.PrepareSegmentSchedules(segmentSchedulesCount)) return false;
    for (std::uint64_t i = 0; i < segmentSchedulesCount; ++i) {
        static_assert(noexcept(visitor.SegmentSchedules(i)));
        auto&& row = visitor.SegmentSchedules(i);
        if (!MeasurePackBodyWorldSegment(row, size)) return false;
    }
    const std::uint64_t containersCount = static_cast<std::uint64_t>(value.containerCount);
    static_assert(noexcept(visitor.PrepareContainers(containersCount)));
    if (!visitor.PrepareContainers(containersCount)) return false;
    for (std::uint64_t i = 0; i < containersCount; ++i) {
        static_assert(noexcept(visitor.Containers(i)));
        auto&& row = visitor.Containers(i);
        if (!MeasurePackBodyWorldContainer(row, size)) return false;
    }
    const std::uint64_t transitionsCount = static_cast<std::uint64_t>(value.transitionCount);
    static_assert(noexcept(visitor.PrepareTransitions(transitionsCount)));
    if (!visitor.PrepareTransitions(transitionsCount)) return false;
    for (std::uint64_t i = 0; i < transitionsCount; ++i) {
        static_assert(noexcept(visitor.Transitions(i)));
        auto&& row = visitor.Transitions(i);
        if (!MeasurePackBodyWorldTransition(row, size)) return false;
    }
    const std::uint64_t actionsCount = static_cast<std::uint64_t>(value.actionCount);
    static_assert(noexcept(visitor.PrepareActions(actionsCount)));
    if (!visitor.PrepareActions(actionsCount)) return false;
    for (std::uint64_t i = 0; i < actionsCount; ++i) {
        static_assert(noexcept(visitor.Actions(i)));
        auto&& row = visitor.Actions(i);
        if (!MeasurePackBodyWorldAction(row, size)) return false;
    }
    return true;
}

[[nodiscard]] inline bool LayoutPackBusDefinitionSnapshot(PackBusDefinitionSnapshotHeader& value, std::uint64_t& size) noexcept {
    size = 0;
    if (size > UINT64_MAX - kPackBytes<HkPackBusDefinitionSnapshotHeader>) return false;
    size += kPackBytes<HkPackBusDefinitionSnapshotHeader>;
    if (size > std::numeric_limits<std::remove_cvref_t<decltype(value.busOffset)>>::max()) return false;
    AssignPack(value.busOffset, size);
    const std::uint64_t busesCount = static_cast<std::uint64_t>(value.busCount);
    if (busesCount > (UINT64_MAX - size) / kPackBytes<HkPackBusDefinitionBus>) return false;
    size += busesCount * kPackBytes<HkPackBusDefinitionBus>;
    if (size > std::numeric_limits<std::remove_cvref_t<decltype(value.sendOffset)>>::max()) return false;
    AssignPack(value.sendOffset, size);
    const std::uint64_t sendsCount = static_cast<std::uint64_t>(value.sendCount);
    if (sendsCount > (UINT64_MAX - size) / kPackBytes<HkPackBusDefinitionSend>) return false;
    size += sendsCount * kPackBytes<HkPackBusDefinitionSend>;
    if (size > std::numeric_limits<std::remove_cvref_t<decltype(value.effectOffset)>>::max()) return false;
    AssignPack(value.effectOffset, size);
    const std::uint64_t effectsCount = static_cast<std::uint64_t>(value.effectCount);
    if (effectsCount > (UINT64_MAX - size) / kPackBytes<HkPackBusDefinitionEffect>) return false;
    size += effectsCount * kPackBytes<HkPackBusDefinitionEffect>;
    if (size > std::numeric_limits<std::remove_cvref_t<decltype(value.effectParameterOffset)>>::max()) return false;
    AssignPack(value.effectParameterOffset, size);
    const std::uint64_t effectParametersCount = static_cast<std::uint64_t>(value.effectParameterCount);
    if (effectParametersCount > (UINT64_MAX - size) / kPackBytes<HkPackBusDefinitionEffectParameter>) return false;
    size += effectParametersCount * kPackBytes<HkPackBusDefinitionEffectParameter>;
    if (size > std::numeric_limits<std::remove_cvref_t<decltype(value.downmixOffset)>>::max()) return false;
    AssignPack(value.downmixOffset, size);
    const std::uint64_t downmixesCount = static_cast<std::uint64_t>(value.downmixCount);
    if (downmixesCount > (UINT64_MAX - size) / kPackBytes<HkPackBusDefinitionDownmix>) return false;
    size += downmixesCount * kPackBytes<HkPackBusDefinitionDownmix>;
    if (size > std::numeric_limits<std::remove_cvref_t<decltype(value.coefficientOffset)>>::max()) return false;
    AssignPack(value.coefficientOffset, size);
    const std::uint64_t coefficientsCount = static_cast<std::uint64_t>(value.coefficientCount);
    if (coefficientsCount > (UINT64_MAX - size) / kPackBytes<HkPackBusDefinitionCoefficient>) return false;
    size += coefficientsCount * kPackBytes<HkPackBusDefinitionCoefficient>;
    const auto length = size - value.busOffset;
    if (length > std::numeric_limits<std::remove_cvref_t<decltype(value.payloadBytes)>>::max()) return false;
    AssignPack(value.payloadBytes, length);
    return true;
}

[[nodiscard]] inline bool ValidatePackBusDefinitionSnapshotLayout(const PackBusDefinitionSnapshotHeader& value, std::uint64_t size) noexcept {
    auto expected = value;
    std::uint64_t expectedSize = 0;
    if (!LayoutPackBusDefinitionSnapshot(expected, expectedSize) || expectedSize != size) return false;
    return value.busOffset == expected.busOffset &&
           value.sendOffset == expected.sendOffset &&
           value.effectOffset == expected.effectOffset &&
           value.effectParameterOffset == expected.effectParameterOffset &&
           value.downmixOffset == expected.downmixOffset &&
           value.coefficientOffset == expected.coefficientOffset &&
           value.payloadBytes == expected.payloadBytes;
}

template <class Buffer, class Visitor>
[[nodiscard]] bool AppendPackBusDefinitionSnapshot(Buffer& buffer, Visitor& visitor) noexcept {
    static_assert(noexcept(visitor.Value()));
    auto& value = visitor.Value();
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(value)>, PackBusDefinitionSnapshotHeader>);
    {
        static_assert(noexcept(visitor.Header()));
        auto&& row = visitor.Header();
        if (!AppendPackRow<HkPackBusDefinitionSnapshotHeader>(buffer, row)) return false;
    }
    const std::uint64_t busesCount = static_cast<std::uint64_t>(value.busCount);
    static_assert(noexcept(visitor.PrepareBuses(busesCount)));
    if (!visitor.PrepareBuses(busesCount)) return false;
    for (std::uint64_t i = 0; i < busesCount; ++i) {
        static_assert(noexcept(visitor.Buses(i)));
        auto&& row = visitor.Buses(i);
        if (!AppendPackRow<HkPackBusDefinitionBus>(buffer, row)) return false;
    }
    const std::uint64_t sendsCount = static_cast<std::uint64_t>(value.sendCount);
    static_assert(noexcept(visitor.PrepareSends(sendsCount)));
    if (!visitor.PrepareSends(sendsCount)) return false;
    for (std::uint64_t i = 0; i < sendsCount; ++i) {
        static_assert(noexcept(visitor.Sends(i)));
        auto&& row = visitor.Sends(i);
        if (!AppendPackRow<HkPackBusDefinitionSend>(buffer, row)) return false;
    }
    const std::uint64_t effectsCount = static_cast<std::uint64_t>(value.effectCount);
    static_assert(noexcept(visitor.PrepareEffects(effectsCount)));
    if (!visitor.PrepareEffects(effectsCount)) return false;
    for (std::uint64_t i = 0; i < effectsCount; ++i) {
        static_assert(noexcept(visitor.Effects(i)));
        auto&& row = visitor.Effects(i);
        if (!AppendPackRow<HkPackBusDefinitionEffect>(buffer, row)) return false;
    }
    const std::uint64_t effectParametersCount = static_cast<std::uint64_t>(value.effectParameterCount);
    static_assert(noexcept(visitor.PrepareEffectParameters(effectParametersCount)));
    if (!visitor.PrepareEffectParameters(effectParametersCount)) return false;
    for (std::uint64_t i = 0; i < effectParametersCount; ++i) {
        static_assert(noexcept(visitor.EffectParameters(i)));
        auto&& row = visitor.EffectParameters(i);
        if (!AppendPackRow<HkPackBusDefinitionEffectParameter>(buffer, row)) return false;
    }
    const std::uint64_t downmixesCount = static_cast<std::uint64_t>(value.downmixCount);
    static_assert(noexcept(visitor.PrepareDownmixes(downmixesCount)));
    if (!visitor.PrepareDownmixes(downmixesCount)) return false;
    for (std::uint64_t i = 0; i < downmixesCount; ++i) {
        static_assert(noexcept(visitor.Downmixes(i)));
        auto&& row = visitor.Downmixes(i);
        if (!AppendPackRow<HkPackBusDefinitionDownmix>(buffer, row)) return false;
    }
    const std::uint64_t coefficientsCount = static_cast<std::uint64_t>(value.coefficientCount);
    static_assert(noexcept(visitor.PrepareCoefficients(coefficientsCount)));
    if (!visitor.PrepareCoefficients(coefficientsCount)) return false;
    for (std::uint64_t i = 0; i < coefficientsCount; ++i) {
        static_assert(noexcept(visitor.Coefficients(i)));
        auto&& row = visitor.Coefficients(i);
        if (!AppendPackRow<HkPackBusDefinitionCoefficient>(buffer, row)) return false;
    }
    static_assert(noexcept(visitor.Finish()));
    return visitor.Finish();
}

template <class Buffer, class Visitor>
[[nodiscard]] bool ReadPackBusDefinitionSnapshot(Buffer& buffer, Visitor& visitor) noexcept {
    static_assert(noexcept(visitor.Value()));
    auto& value = visitor.Value();
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(value)>, PackBusDefinitionSnapshotHeader>);
    static_assert(noexcept(buffer.Remaining()));
    const auto initialRemaining = buffer.Remaining();
    {
        static_assert(noexcept(visitor.Header()));
        auto&& row = visitor.Header();
        if (!ReadPackRow<HkPackBusDefinitionSnapshotHeader>(buffer, row)) return false;
    }
    const std::uint64_t busesCount = static_cast<std::uint64_t>(value.busCount);
    static_assert(noexcept(buffer.Remaining()));
    if (busesCount > buffer.Remaining() / kPackBytes<HkPackBusDefinitionBus>) return false;
    static_assert(noexcept(visitor.PrepareBuses(busesCount)));
    if (!visitor.PrepareBuses(busesCount)) return false;
    for (std::uint64_t i = 0; i < busesCount; ++i) {
        static_assert(noexcept(visitor.Buses(i)));
        auto&& row = visitor.Buses(i);
        if (!ReadPackRow<HkPackBusDefinitionBus>(buffer, row)) return false;
    }
    const std::uint64_t sendsCount = static_cast<std::uint64_t>(value.sendCount);
    static_assert(noexcept(buffer.Remaining()));
    if (sendsCount > buffer.Remaining() / kPackBytes<HkPackBusDefinitionSend>) return false;
    static_assert(noexcept(visitor.PrepareSends(sendsCount)));
    if (!visitor.PrepareSends(sendsCount)) return false;
    for (std::uint64_t i = 0; i < sendsCount; ++i) {
        static_assert(noexcept(visitor.Sends(i)));
        auto&& row = visitor.Sends(i);
        if (!ReadPackRow<HkPackBusDefinitionSend>(buffer, row)) return false;
    }
    const std::uint64_t effectsCount = static_cast<std::uint64_t>(value.effectCount);
    static_assert(noexcept(buffer.Remaining()));
    if (effectsCount > buffer.Remaining() / kPackBytes<HkPackBusDefinitionEffect>) return false;
    static_assert(noexcept(visitor.PrepareEffects(effectsCount)));
    if (!visitor.PrepareEffects(effectsCount)) return false;
    for (std::uint64_t i = 0; i < effectsCount; ++i) {
        static_assert(noexcept(visitor.Effects(i)));
        auto&& row = visitor.Effects(i);
        if (!ReadPackRow<HkPackBusDefinitionEffect>(buffer, row)) return false;
    }
    const std::uint64_t effectParametersCount = static_cast<std::uint64_t>(value.effectParameterCount);
    static_assert(noexcept(buffer.Remaining()));
    if (effectParametersCount > buffer.Remaining() / kPackBytes<HkPackBusDefinitionEffectParameter>) return false;
    static_assert(noexcept(visitor.PrepareEffectParameters(effectParametersCount)));
    if (!visitor.PrepareEffectParameters(effectParametersCount)) return false;
    for (std::uint64_t i = 0; i < effectParametersCount; ++i) {
        static_assert(noexcept(visitor.EffectParameters(i)));
        auto&& row = visitor.EffectParameters(i);
        if (!ReadPackRow<HkPackBusDefinitionEffectParameter>(buffer, row)) return false;
    }
    const std::uint64_t downmixesCount = static_cast<std::uint64_t>(value.downmixCount);
    static_assert(noexcept(buffer.Remaining()));
    if (downmixesCount > buffer.Remaining() / kPackBytes<HkPackBusDefinitionDownmix>) return false;
    static_assert(noexcept(visitor.PrepareDownmixes(downmixesCount)));
    if (!visitor.PrepareDownmixes(downmixesCount)) return false;
    for (std::uint64_t i = 0; i < downmixesCount; ++i) {
        static_assert(noexcept(visitor.Downmixes(i)));
        auto&& row = visitor.Downmixes(i);
        if (!ReadPackRow<HkPackBusDefinitionDownmix>(buffer, row)) return false;
    }
    const std::uint64_t coefficientsCount = static_cast<std::uint64_t>(value.coefficientCount);
    static_assert(noexcept(buffer.Remaining()));
    if (coefficientsCount > buffer.Remaining() / kPackBytes<HkPackBusDefinitionCoefficient>) return false;
    static_assert(noexcept(visitor.PrepareCoefficients(coefficientsCount)));
    if (!visitor.PrepareCoefficients(coefficientsCount)) return false;
    for (std::uint64_t i = 0; i < coefficientsCount; ++i) {
        static_assert(noexcept(visitor.Coefficients(i)));
        auto&& row = visitor.Coefficients(i);
        if (!ReadPackRow<HkPackBusDefinitionCoefficient>(buffer, row)) return false;
    }
    if (!ValidatePackBusDefinitionSnapshotLayout(value, initialRemaining - buffer.Remaining())) return false;
    static_assert(noexcept(visitor.Finish()));
    return visitor.Finish();
}

template <class Buffer, class Visitor>
[[nodiscard]] bool AppendPackEffectPreset(Buffer& buffer, Visitor& visitor) noexcept {
    static_assert(noexcept(visitor.Value()));
    auto& value = visitor.Value();
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(value)>, PackEffectPresetHeader>);
    {
        static_assert(noexcept(visitor.Header()));
        auto&& row = visitor.Header();
        if (!AppendPackRow<HkPackEffectPresetHeader>(buffer, row)) return false;
    }
    const std::uint64_t parametersCount = static_cast<std::uint64_t>(value.paramCount);
    static_assert(noexcept(visitor.PrepareParameters(parametersCount)));
    if (!visitor.PrepareParameters(parametersCount)) return false;
    for (std::uint64_t i = 0; i < parametersCount; ++i) {
        static_assert(noexcept(visitor.Parameters(i)));
        auto&& row = visitor.Parameters(i);
        if (!AppendPackRow<HkPackEffectPresetParameter>(buffer, row)) return false;
    }
    static_assert(noexcept(visitor.Finish()));
    return visitor.Finish();
}

template <class Buffer, class Visitor>
[[nodiscard]] bool ReadPackEffectPreset(Buffer& buffer, Visitor& visitor) noexcept {
    static_assert(noexcept(visitor.Value()));
    auto& value = visitor.Value();
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(value)>, PackEffectPresetHeader>);
    {
        static_assert(noexcept(visitor.Header()));
        auto&& row = visitor.Header();
        if (!ReadPackRow<HkPackEffectPresetHeader>(buffer, row)) return false;
    }
    const std::uint64_t parametersCount = static_cast<std::uint64_t>(value.paramCount);
    static_assert(noexcept(buffer.Remaining()));
    if (parametersCount > buffer.Remaining() / kPackBytes<HkPackEffectPresetParameter>) return false;
    static_assert(noexcept(visitor.PrepareParameters(parametersCount)));
    if (!visitor.PrepareParameters(parametersCount)) return false;
    for (std::uint64_t i = 0; i < parametersCount; ++i) {
        static_assert(noexcept(visitor.Parameters(i)));
        auto&& row = visitor.Parameters(i);
        if (!ReadPackRow<HkPackEffectPresetParameter>(buffer, row)) return false;
    }
    static_assert(noexcept(visitor.Finish()));
    return visitor.Finish();
}

template <class Visitor>
[[nodiscard]] bool MeasurePackEffectPreset(Visitor& visitor, std::uint64_t& size) noexcept {
    static_assert(noexcept(visitor.Value()));
    auto& value = visitor.Value();
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(value)>, PackEffectPresetHeader>);
    if (size > UINT64_MAX - kPackBytes<HkPackEffectPresetHeader>) return false;
    size += kPackBytes<HkPackEffectPresetHeader>;
    const std::uint64_t parametersCount = static_cast<std::uint64_t>(value.paramCount);
    if (parametersCount > (UINT64_MAX - size) / kPackBytes<HkPackEffectPresetParameter>) return false;
    size += parametersCount * kPackBytes<HkPackEffectPresetParameter>;
    return true;
}

template <class Buffer, class Visitor>
[[nodiscard]] bool AppendPackContainerPayload(Buffer& buffer, Visitor& visitor) noexcept {
    static_assert(noexcept(visitor.Value()));
    auto& value = visitor.Value();
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(value)>, PackContainerEntryHeader>);
    {
        static_assert(noexcept(visitor.Header()));
        auto&& row = visitor.Header();
        if (!AppendPackRow<HkPackContainerHeader>(buffer, row)) return false;
    }
    const std::uint64_t childrenCount = static_cast<std::uint64_t>(value.childCount);
    static_assert(noexcept(visitor.PrepareChildren(childrenCount)));
    if (!visitor.PrepareChildren(childrenCount)) return false;
    for (std::uint64_t i = 0; i < childrenCount; ++i) {
        static_assert(noexcept(visitor.Children(i)));
        auto&& row = visitor.Children(i);
        if (!AppendPackRow<HkPackContainerChild>(buffer, row)) return false;
    }
    static_assert(noexcept(visitor.Finish()));
    return visitor.Finish();
}

template <class Buffer, class Visitor>
[[nodiscard]] bool ReadPackContainerPayload(Buffer& buffer, Visitor& visitor) noexcept {
    static_assert(noexcept(visitor.Value()));
    auto& value = visitor.Value();
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(value)>, PackContainerEntryHeader>);
    {
        static_assert(noexcept(visitor.Header()));
        auto&& row = visitor.Header();
        if (!ReadPackRow<HkPackContainerHeader>(buffer, row)) return false;
    }
    const std::uint64_t childrenCount = static_cast<std::uint64_t>(value.childCount);
    static_assert(noexcept(buffer.Remaining()));
    if (childrenCount > buffer.Remaining() / kPackBytes<HkPackContainerChild>) return false;
    static_assert(noexcept(visitor.PrepareChildren(childrenCount)));
    if (!visitor.PrepareChildren(childrenCount)) return false;
    for (std::uint64_t i = 0; i < childrenCount; ++i) {
        static_assert(noexcept(visitor.Children(i)));
        auto&& row = visitor.Children(i);
        if (!ReadPackRow<HkPackContainerChild>(buffer, row)) return false;
    }
    static_assert(noexcept(visitor.Finish()));
    return visitor.Finish();
}

template <class Visitor>
[[nodiscard]] bool MeasurePackContainerPayload(Visitor& visitor, std::uint64_t& size) noexcept {
    static_assert(noexcept(visitor.Value()));
    auto& value = visitor.Value();
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(value)>, PackContainerEntryHeader>);
    if (size > UINT64_MAX - kPackBytes<HkPackContainerHeader>) return false;
    size += kPackBytes<HkPackContainerHeader>;
    const std::uint64_t childrenCount = static_cast<std::uint64_t>(value.childCount);
    if (childrenCount > (UINT64_MAX - size) / kPackBytes<HkPackContainerChild>) return false;
    size += childrenCount * kPackBytes<HkPackContainerChild>;
    return true;
}

template <class Buffer, class Visitor>
[[nodiscard]] bool ReadPackVariableAnchorSection(Buffer& buffer, Visitor& visitor) noexcept {
    static_assert(noexcept(visitor.Value()));
    auto& value = visitor.Value();
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(value)>, PackVariableAnchorHeader>);
    {
        static_assert(noexcept(visitor.Header()));
        auto&& row = visitor.Header();
        if (!ReadPackRow<HkPackVariableAnchorCount>(buffer, row)) return false;
    }
    const std::uint64_t anchorsCount = static_cast<std::uint64_t>(value.count);
    static_assert(noexcept(buffer.Remaining()));
    if (anchorsCount > buffer.Remaining() / kPackBytes<HkPackVariableAnchor>) return false;
    static_assert(noexcept(visitor.PrepareAnchors(anchorsCount)));
    if (!visitor.PrepareAnchors(anchorsCount)) return false;
    for (std::uint64_t i = 0; i < anchorsCount; ++i) {
        static_assert(noexcept(visitor.Anchors(i)));
        auto&& row = visitor.Anchors(i);
        if (!ReadPackRow<HkPackVariableAnchor>(buffer, row)) return false;
    }
    static_assert(noexcept(visitor.Finish()));
    return visitor.Finish();
}

template <class Buffer, class Visitor>
[[nodiscard]] bool ReadPackVariableLabelRecord(Buffer& buffer, Visitor& visitor) noexcept {
    {
        static_assert(noexcept(visitor.Wire()));
        auto&& row = visitor.Wire();
        if (!ReadPackRow<HkPackVariableLabel>(buffer, row)) return false;
    }
    static_assert(noexcept(visitor.Finish()));
    return visitor.Finish();
}

template <class Buffer, class Visitor>
[[nodiscard]] bool ReadPackVariableLabelSection(Buffer& buffer, Visitor& visitor) noexcept {
    static_assert(noexcept(visitor.Value()));
    auto& value = visitor.Value();
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(value)>, PackVariableLabelHeader>);
    {
        static_assert(noexcept(visitor.Header()));
        auto&& row = visitor.Header();
        if (!ReadPackRow<HkPackVariableLabelHeader>(buffer, row)) return false;
    }
    const std::uint64_t labelsCount = static_cast<std::uint64_t>(value.labelCount);
    static_assert(noexcept(buffer.Remaining()));
    if (labelsCount > buffer.Remaining() / kPackBytes<HkPackVariableLabel>) return false;
    static_assert(noexcept(visitor.PrepareLabels(labelsCount)));
    if (!visitor.PrepareLabels(labelsCount)) return false;
    for (std::uint64_t i = 0; i < labelsCount; ++i) {
        static_assert(noexcept(visitor.Labels(i)));
        auto&& row = visitor.Labels(i);
        if (!ReadPackVariableLabelRecord(buffer, row)) return false;
    }
    static_assert(noexcept(visitor.Finish()));
    return visitor.Finish();
}

template <class Buffer, class Visitor>
[[nodiscard]] bool ReadPackVariablePolicyRecord(Buffer& buffer, Visitor& visitor) noexcept {
    {
        static_assert(noexcept(visitor.Wire()));
        auto&& row = visitor.Wire();
        if (!ReadPackRow<HkPackVariablePolicyRow>(buffer, row)) return false;
    }
    static_assert(noexcept(visitor.Finish()));
    return visitor.Finish();
}

template <class Buffer, class Visitor>
[[nodiscard]] bool ReadPackVariablePolicyPayload(Buffer& buffer, Visitor& visitor) noexcept {
    static_assert(noexcept(visitor.Value()));
    auto& value = visitor.Value();
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(value)>, PackVariablePolicyHeader>);
    {
        static_assert(noexcept(visitor.Header()));
        auto&& row = visitor.Header();
        if (!ReadPackRow<HkPackVariablePolicyHeader>(buffer, row)) return false;
    }
    const std::uint64_t rowsCount = static_cast<std::uint64_t>(value.rowCount);
    static_assert(noexcept(buffer.Remaining()));
    if (rowsCount > buffer.Remaining() / kPackBytes<HkPackVariablePolicyRow>) return false;
    static_assert(noexcept(visitor.PrepareRows(rowsCount)));
    if (!visitor.PrepareRows(rowsCount)) return false;
    for (std::uint64_t i = 0; i < rowsCount; ++i) {
        static_assert(noexcept(visitor.Rows(i)));
        auto&& row = visitor.Rows(i);
        if (!ReadPackVariablePolicyRecord(buffer, row)) return false;
    }
    const std::uint64_t anchorGroupsCount = static_cast<std::uint64_t>(value.rowCount);
    static_assert(noexcept(buffer.Remaining()));
    if (anchorGroupsCount > buffer.Remaining() / kPackBytes<HkPackVariableAnchorCount>) return false;
    static_assert(noexcept(visitor.PrepareAnchorGroups(anchorGroupsCount)));
    if (!visitor.PrepareAnchorGroups(anchorGroupsCount)) return false;
    for (std::uint64_t i = 0; i < anchorGroupsCount; ++i) {
        static_assert(noexcept(visitor.AnchorGroups(i)));
        auto&& row = visitor.AnchorGroups(i);
        if (!ReadPackVariableAnchorSection(buffer, row)) return false;
    }
    const std::uint64_t labelGroupsCount = static_cast<std::uint64_t>(value.rowCount);
    static_assert(noexcept(buffer.Remaining()));
    if (labelGroupsCount > buffer.Remaining() / kPackBytes<HkPackVariableLabelHeader>) return false;
    static_assert(noexcept(visitor.PrepareLabelGroups(labelGroupsCount)));
    if (!visitor.PrepareLabelGroups(labelGroupsCount)) return false;
    for (std::uint64_t i = 0; i < labelGroupsCount; ++i) {
        static_assert(noexcept(visitor.LabelGroups(i)));
        auto&& row = visitor.LabelGroups(i);
        if (!ReadPackVariableLabelSection(buffer, row)) return false;
    }
    static_assert(noexcept(visitor.Finish()));
    return visitor.Finish();
}

template <class Buffer, class Visitor>
[[nodiscard]] bool ReadPackVariableRequirementRecord(Buffer& buffer, Visitor& visitor) noexcept {
    {
        static_assert(noexcept(visitor.Wire()));
        auto&& row = visitor.Wire();
        if (!ReadPackRow<HkPackVariableRequirement>(buffer, row)) return false;
    }
    static_assert(noexcept(visitor.Finish()));
    return visitor.Finish();
}

template <class Buffer, class Visitor>
[[nodiscard]] bool ReadPackVariableRequirementsPayload(Buffer& buffer, Visitor& visitor) noexcept {
    static_assert(noexcept(visitor.Value()));
    auto& value = visitor.Value();
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(value)>, PackVariableRequirementsHeader>);
    {
        static_assert(noexcept(visitor.Header()));
        auto&& row = visitor.Header();
        if (!ReadPackRow<HkPackVariableRequirementsHeader>(buffer, row)) return false;
    }
    const std::uint64_t rowsCount = static_cast<std::uint64_t>(value.rowCount);
    static_assert(noexcept(buffer.Remaining()));
    if (rowsCount > buffer.Remaining() / kPackBytes<HkPackVariableRequirement>) return false;
    static_assert(noexcept(visitor.PrepareRows(rowsCount)));
    if (!visitor.PrepareRows(rowsCount)) return false;
    for (std::uint64_t i = 0; i < rowsCount; ++i) {
        static_assert(noexcept(visitor.Rows(i)));
        auto&& row = visitor.Rows(i);
        if (!ReadPackVariableRequirementRecord(buffer, row)) return false;
    }
    static_assert(noexcept(visitor.Finish()));
    return visitor.Finish();
}

template <class Visitor>
[[nodiscard]] bool MeasurePackVariableRequirementsPayload(Visitor& visitor, std::uint64_t& size) noexcept {
    static_assert(noexcept(visitor.Value()));
    auto& value = visitor.Value();
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(value)>, PackVariableRequirementsHeader>);
    if (size > UINT64_MAX - kPackBytes<HkPackVariableRequirementsHeader>) return false;
    size += kPackBytes<HkPackVariableRequirementsHeader>;
    const std::uint64_t rowsCount = static_cast<std::uint64_t>(value.rowCount);
    if (rowsCount > (UINT64_MAX - size) / kPackBytes<HkPackVariableRequirement>) return false;
    size += rowsCount * kPackBytes<HkPackVariableRequirement>;
    return true;
}

template <class Buffer, class Visitor>
[[nodiscard]] bool AppendPackVoiceRecipeInstallRow(Buffer& buffer, Visitor& visitor) noexcept {
    {
        static_assert(noexcept(visitor.Wire()));
        auto&& row = visitor.Wire();
        if (!AppendPackRow<HkPackVoiceRecipeInstall>(buffer, row)) return false;
    }
    static_assert(noexcept(visitor.Finish()));
    return visitor.Finish();
}

template <class Buffer, class Visitor>
[[nodiscard]] bool ReadPackVoiceRecipeInstallRow(Buffer& buffer, Visitor& visitor) noexcept {
    {
        static_assert(noexcept(visitor.Wire()));
        auto&& row = visitor.Wire();
        if (!ReadPackRow<HkPackVoiceRecipeInstall>(buffer, row)) return false;
    }
    static_assert(noexcept(visitor.Finish()));
    return visitor.Finish();
}

template <class Buffer, class Visitor>
[[nodiscard]] bool AppendPackVoiceRecipePayload(Buffer& buffer, Visitor& visitor) noexcept {
    static_assert(noexcept(visitor.Value()));
    auto& value = visitor.Value();
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(value)>, PackVoiceRecipeEntryHeader>);
    {
        static_assert(noexcept(visitor.Header()));
        auto&& row = visitor.Header();
        if (!AppendPackRow<HkPackVoiceRecipeHeader>(buffer, row)) return false;
    }
    {
        static_assert(noexcept(visitor.Tone()));
        auto&& row = visitor.Tone();
        if (!AppendPackRow<HkPackVoiceRecipeTone>(buffer, row)) return false;
    }
    {
        static_assert(noexcept(visitor.Spatial()));
        auto&& row = visitor.Spatial();
        if (!AppendPackRow<HkPackVoiceRecipeSpatial>(buffer, row)) return false;
    }
    {
        static_assert(noexcept(visitor.Playback()));
        auto&& row = visitor.Playback();
        if (!AppendPackRow<HkPackVoiceRecipePlayback>(buffer, row)) return false;
    }
    const std::uint64_t installsCount = static_cast<std::uint64_t>(value.installCount);
    static_assert(noexcept(visitor.PrepareInstalls(installsCount)));
    if (!visitor.PrepareInstalls(installsCount)) return false;
    for (std::uint64_t i = 0; i < installsCount; ++i) {
        static_assert(noexcept(visitor.Installs(i)));
        auto&& row = visitor.Installs(i);
        if (!AppendPackVoiceRecipeInstallRow(buffer, row)) return false;
    }
    const std::uint64_t bindingsCount = static_cast<std::uint64_t>(value.bindingCount);
    static_assert(noexcept(visitor.PrepareBindings(bindingsCount)));
    if (!visitor.PrepareBindings(bindingsCount)) return false;
    for (std::uint64_t i = 0; i < bindingsCount; ++i) {
        static_assert(noexcept(visitor.Bindings(i)));
        auto&& row = visitor.Bindings(i);
        if (!AppendPackRow<HkPackVoiceRecipeBinding>(buffer, row)) return false;
    }
    static_assert(noexcept(visitor.Finish()));
    return visitor.Finish();
}

template <class Buffer, class Visitor>
[[nodiscard]] bool ReadPackVoiceRecipePayload(Buffer& buffer, Visitor& visitor) noexcept {
    static_assert(noexcept(visitor.Value()));
    auto& value = visitor.Value();
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(value)>, PackVoiceRecipeEntryHeader>);
    {
        static_assert(noexcept(visitor.Header()));
        auto&& row = visitor.Header();
        if (!ReadPackRow<HkPackVoiceRecipeHeader>(buffer, row)) return false;
    }
    {
        static_assert(noexcept(visitor.Tone()));
        auto&& row = visitor.Tone();
        if (!ReadPackRow<HkPackVoiceRecipeTone>(buffer, row)) return false;
    }
    {
        static_assert(noexcept(visitor.Spatial()));
        auto&& row = visitor.Spatial();
        if (!ReadPackRow<HkPackVoiceRecipeSpatial>(buffer, row)) return false;
    }
    {
        static_assert(noexcept(visitor.Playback()));
        auto&& row = visitor.Playback();
        if (!ReadPackRow<HkPackVoiceRecipePlayback>(buffer, row)) return false;
    }
    const std::uint64_t installsCount = static_cast<std::uint64_t>(value.installCount);
    static_assert(noexcept(buffer.Remaining()));
    if (installsCount > buffer.Remaining() / kPackBytes<HkPackVoiceRecipeInstall>) return false;
    static_assert(noexcept(visitor.PrepareInstalls(installsCount)));
    if (!visitor.PrepareInstalls(installsCount)) return false;
    for (std::uint64_t i = 0; i < installsCount; ++i) {
        static_assert(noexcept(visitor.Installs(i)));
        auto&& row = visitor.Installs(i);
        if (!ReadPackVoiceRecipeInstallRow(buffer, row)) return false;
    }
    const std::uint64_t bindingsCount = static_cast<std::uint64_t>(value.bindingCount);
    static_assert(noexcept(buffer.Remaining()));
    if (bindingsCount > buffer.Remaining() / kPackBytes<HkPackVoiceRecipeBinding>) return false;
    static_assert(noexcept(visitor.PrepareBindings(bindingsCount)));
    if (!visitor.PrepareBindings(bindingsCount)) return false;
    for (std::uint64_t i = 0; i < bindingsCount; ++i) {
        static_assert(noexcept(visitor.Bindings(i)));
        auto&& row = visitor.Bindings(i);
        if (!ReadPackRow<HkPackVoiceRecipeBinding>(buffer, row)) return false;
    }
    static_assert(noexcept(visitor.Finish()));
    return visitor.Finish();
}

template <class Visitor>
[[nodiscard]] bool MeasurePackVoiceRecipePayload(Visitor& visitor, std::uint64_t& size) noexcept {
    static_assert(noexcept(visitor.Value()));
    auto& value = visitor.Value();
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(value)>, PackVoiceRecipeEntryHeader>);
    if (size > UINT64_MAX - kPackBytes<HkPackVoiceRecipeHeader>) return false;
    size += kPackBytes<HkPackVoiceRecipeHeader>;
    if (size > UINT64_MAX - kPackBytes<HkPackVoiceRecipeTone>) return false;
    size += kPackBytes<HkPackVoiceRecipeTone>;
    if (size > UINT64_MAX - kPackBytes<HkPackVoiceRecipeSpatial>) return false;
    size += kPackBytes<HkPackVoiceRecipeSpatial>;
    if (size > UINT64_MAX - kPackBytes<HkPackVoiceRecipePlayback>) return false;
    size += kPackBytes<HkPackVoiceRecipePlayback>;
    const std::uint64_t installsCount = static_cast<std::uint64_t>(value.installCount);
    if (installsCount > (UINT64_MAX - size) / kPackBytes<HkPackVoiceRecipeInstall>) return false;
    size += installsCount * kPackBytes<HkPackVoiceRecipeInstall>;
    const std::uint64_t bindingsCount = static_cast<std::uint64_t>(value.bindingCount);
    if (bindingsCount > (UINT64_MAX - size) / kPackBytes<HkPackVoiceRecipeBinding>) return false;
    size += bindingsCount * kPackBytes<HkPackVoiceRecipeBinding>;
    return true;
}

}

#endif
