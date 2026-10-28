#include "harken/core/CurrentWorldCodec.h"
#include "harken/core/LogicAllocCheckpoint.h"
#include "harken/param/VariableService.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstring>
#include <limits>
#include <new>
#include <utility>
#include <type_traits>

namespace harken {
namespace {
void AppendU32(std::vector<std::uint8_t>& out,
               std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value >> 16));
    out.push_back(static_cast<std::uint8_t>(value >> 24));
}

void AppendU8(std::vector<std::uint8_t>& out,
              std::uint8_t value) {
    out.push_back(value);
}

void AppendU16(std::vector<std::uint8_t>& out,
               std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
}

void AppendU64(std::vector<std::uint8_t>& out,
               std::uint64_t value) {
    AppendU32(out, static_cast<std::uint32_t>(value));
    AppendU32(out, static_cast<std::uint32_t>(value >> 32));
}

void AppendF32(std::vector<std::uint8_t>& out, float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof bits);
    AppendU32(out, bits);
}

void AppendObjectId(std::vector<std::uint8_t>& out, VnObjectId value) {
    const auto bytes = ObjectIdBytes(value);
    out.insert(out.end(), bytes.begin(), bytes.end());
}

struct Reader {
    const std::uint8_t* bytes = nullptr;
    std::size_t size = 0;
    std::size_t offset = 0;
    bool failed = false;

    std::uint8_t U8() noexcept {
        if (failed || size - offset < 1) {
            failed = true;
            return 0;
        }
        return bytes[offset++];
    }

    std::uint16_t U16() noexcept {
        if (failed || size - offset < 2) {
            failed = true;
            return 0;
        }
        const std::uint16_t value =
            static_cast<std::uint16_t>(bytes[offset]) |
            static_cast<std::uint16_t>(bytes[offset + 1] << 8);
        offset += 2;
        return value;
    }

    std::uint32_t U32() noexcept {
        if (failed || size - offset < 4) {
            failed = true;
            return 0;
        }
        const std::uint32_t value =
            static_cast<std::uint32_t>(bytes[offset]) |
            (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
            (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
            (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
        offset += 4;
        return value;
    }

    std::uint64_t U64() noexcept {
        const std::uint64_t lo = U32();
        const std::uint64_t hi = U32();
        return lo | (hi << 32);
    }

    VnObjectId ObjectId() noexcept {
        if (failed || size - offset < 16) {
            failed = true;
            return {};
        }
        const auto value = ObjectIdFromBytes(std::span<const std::uint8_t, 16>(bytes + offset, 16));
        offset += 16;
        return value;
    }

    float F32() noexcept {
        const std::uint32_t bits = U32();
        float value = 0.0f;
        std::memcpy(&value, &bits, sizeof value);
        return value;
    }
};

struct PrefixEncodeVisitor {
    std::vector<std::uint8_t>& out;
    void U32(std::uint32_t value) { AppendU32(out, value); }
    void U64(std::uint64_t value) { AppendU64(out, value); }
    void ObjectId(VnObjectId value) { AppendObjectId(out, value); }
};

struct PrefixDecodeVisitor {
    Reader& reader;
    void U32(std::uint32_t& value) noexcept { value = reader.U32(); }
    void U64(std::uint64_t& value) noexcept { value = reader.U64(); }
    void ObjectId(VnObjectId& value) noexcept { value = reader.ObjectId(); }
};
} // namespace

namespace {

constexpr std::uint32_t kTraceWorldChildCap =
    kHardPlayablePropertyChildRowCapacity;

bool CountPointer(std::uint32_t count, const void* pointer,
                  std::uint32_t cap) noexcept {
    return count <= cap && ((count == 0) == (pointer == nullptr));
}

bool AddCount(std::uint32_t value, std::uint32_t cap,
              std::uint32_t& total) noexcept {
    if (value > cap || total > cap - value) {
        return false;
    }
    total += value;
    return true;
}

bool ValidStructuralAncestorKind(
    playableproperties::ContentPropertyKindV1 kind) noexcept {
    return kind == playableproperties::ContentPropertyKindV1::Layer ||
           kind == playableproperties::ContentPropertyKindV1::Container ||
           kind == playableproperties::ContentPropertyKindV1::Track ||
           kind == playableproperties::ContentPropertyKindV1::Segment;
}

void EncodePlayableRef(const PlayableRefV1& row,
                       std::vector<std::uint8_t>& out) {
    AppendU32(out, static_cast<std::uint32_t>(row.kind));
    AppendU32(out, row.reserved);
    AppendObjectId(out, row.stableId);
    AppendU64(out, row.expectedProgramSemanticFingerprint);
    AppendU64(out, row.contentRevision);
    AppendObjectId(out, row.routeStableId);
    AppendU64(out, row.compatibilityFingerprint);
}

void EncodeActionRef(const ActionObjectRefV1& row,
                     std::vector<std::uint8_t>& out) {
    AppendU32(out, static_cast<std::uint32_t>(row.kind));
    AppendU32(out, row.reserved);
    AppendObjectId(out, row.stableId);
    AppendU64(out, row.expectedProgramSemanticFingerprint);
}

void EncodeActionTime(const AuthoredTimeRationalV1& row,
                      std::vector<std::uint8_t>& out) {
    AppendU32(out, static_cast<std::uint32_t>(row.domain));
    AppendU64(out, std::bit_cast<std::uint64_t>(row.numerator));
    AppendU32(out, row.denominator);
}

void EncodeTime(const AuthoredTimeRationalV1& row,
                std::vector<std::uint8_t>& out) {
    AppendU32(out, static_cast<std::uint32_t>(row.domain));
    AppendU64(out, static_cast<std::uint64_t>(row.numerator));
    AppendU32(out, row.denominator);
}

bool EncodeCurve(const CurveV1& row, std::vector<std::uint8_t>& out) {
    if (row.curve.pointCount > kMaxCurvePoints) {
        return false;
    }
    AppendU32(out, row.curveLocalId);
    AppendU32(out, row.curve.pointCount);
    for (std::uint32_t i = 0; i < row.curve.pointCount; ++i) {
        AppendF32(out, row.curve.points[i].x);
        AppendF32(out, row.curve.points[i].y);
    }
    for (std::uint32_t i = 0; i + 1 < row.curve.pointCount; ++i) {
        const CurveHandlePair& handles = row.curve.handles[i];
        AppendF32(out, handles.h1x);
        AppendF32(out, handles.h1y);
        AppendF32(out, handles.h2x);
        AppendF32(out, handles.h2y);
    }
    return true;
}

void EncodePayloadHeader(const PlayablePropertyPayloadViewV1& payload,
                         std::vector<std::uint8_t>& out,
                         bool includeSpatializationMix) {
    AppendU64(out, payload.presentMask);
    AppendU32(out, static_cast<std::uint32_t>(payload.values[0].bits));
    AppendU32(out, static_cast<std::uint32_t>(payload.values[1].bits));
    AppendU32(out, static_cast<std::uint32_t>(payload.values[2].bits));
    AppendU32(out, static_cast<std::uint32_t>(payload.values[5].bits));
    AppendU32(out, static_cast<std::uint32_t>(payload.values[4].bits));
    AppendU32(out, static_cast<std::uint32_t>(payload.values[3].bits));
    AppendU8(out, static_cast<std::uint8_t>(payload.values[10].bits));
    AppendU16(out, 0);
    AppendU32(out, static_cast<std::uint32_t>(payload.values[8].bits));
    AppendU32(out, static_cast<std::uint32_t>(payload.values[9].bits));
    AppendU32(out, static_cast<std::uint32_t>(payload.values[11].bits));
    AppendU32(out, static_cast<std::uint32_t>(payload.values[12].bits));
    AppendObjectId(out, payload.values[6].objectId);
    AppendObjectId(out, payload.values[13].objectId);
    AppendU64(out, payload.values[14].bits);
    if (includeSpatializationMix) {
        AppendU32(out, static_cast<std::uint32_t>(payload.values[16].bits));
    }
    AppendU32(out, payload.sendCount);
    AppendU32(out, 0);
    AppendU32(out, 0);
    AppendU32(out, payload.effectNodeCount);
    AppendU32(out, payload.effectEdgeCount);
}

bool EncodePayloadBody(const PlayablePropertyPayloadViewV1& payload,
                       std::vector<std::uint8_t>& out) {
    if (!CountPointer(payload.sendCount, payload.sends,
                      kHardPlayablePropertyChildRowCapacity) ||
        !CountPointer(payload.effectNodeCount, payload.effectNodes,
                      kHardPlayablePropertyChildRowCapacity) ||
        !CountPointer(payload.effectEdgeCount, payload.effectEdges,
                      kHardPlayablePropertyEffectEdgeCapacity)) {
        return false;
    }
    for (std::uint32_t i = 0; i < payload.sendCount; ++i) {
        const SendV1& row = payload.sends[i];
        AppendU32(out, row.stableSendOrdinal);
        AppendU32(out, row.reserved);
        AppendObjectId(out, row.targetBusId);
        AppendF32(out, row.sendLevel);
        AppendU32(out, row.reserved2);
    }
    for (std::uint32_t i = 0; i < payload.effectNodeCount; ++i) {
        const EffectNodeV1& node = payload.effectNodes[i];
        if (!CountPointer(node.paramCount, node.params,
                          kHardPlayablePropertyEffectParamCapacity)) {
            return false;
        }
        AppendU32(out, node.nodeId);
        AppendObjectId(out, node.effectId);
        AppendU32(out, node.effectType);
        AppendU32(out, node.paramCount);
        AppendU32(out, 0);
        for (std::uint32_t p = 0; p < node.paramCount; ++p) {
            AppendU32(out, node.params[p].paramId);
            AppendU32(out, node.params[p].valueBits);
        }
    }
    for (std::uint32_t i = 0; i < payload.effectEdgeCount; ++i) {
        AppendU32(out, payload.effectEdges[i].fromNodeId);
        AppendU32(out, payload.effectEdges[i].toNodeId);
    }
    return true;
}

bool CountPayload(const PlayablePropertyPayloadViewV1& payload,
                  std::array<std::uint32_t, 6>& totals) noexcept {
    if (!CountPointer(payload.sendCount, payload.sends,
                      kHardPlayablePropertyChildRowCapacity) ||
        !CountPointer(payload.effectNodeCount, payload.effectNodes,
                      kHardPlayablePropertyChildRowCapacity) ||
        !CountPointer(payload.effectEdgeCount, payload.effectEdges,
                      kHardPlayablePropertyEffectEdgeCapacity) ||
        !AddCount(payload.sendCount, kHardPlayablePropertyChildRowCapacity,
                  totals[0]) ||
        !AddCount(payload.effectNodeCount,
                  kHardPlayablePropertyChildRowCapacity, totals[3]) ||
        !AddCount(payload.effectEdgeCount,
                  kHardPlayablePropertyEffectEdgeCapacity, totals[5])) {
        return false;
    }
    for (std::uint32_t i = 0; i < payload.effectNodeCount; ++i) {
        const EffectNodeV1& node = payload.effectNodes[i];
        if (!CountPointer(node.paramCount, node.params,
                          kHardPlayablePropertyEffectParamCapacity) ||
            !AddCount(node.paramCount,
                      kHardPlayablePropertyEffectParamCapacity, totals[4])) {
            return false;
        }
    }
    return true;
}

bool EncodeCompletion(const AuthoredCompletionTemplateV1& completion,
                      std::vector<std::uint8_t>& out) {
    if (!CountPointer(completion.leafCount, completion.leaves,
                      kTraceWorldChildCap)) {
        return false;
    }
    AppendU32(out, static_cast<std::uint32_t>(completion.completionClass));
    AppendU32(out, completion.leafCount);
    for (std::uint32_t i = 0; i < completion.leafCount; ++i) {
        const AuthoredCompletionLeafV1& leaf = completion.leaves[i];
        if (!CountPointer(leaf.pathStepCount, leaf.pathSteps,
                          kTraceWorldChildCap)) {
            return false;
        }
        AppendU32(out, leaf.stableLeafOrdinal);
        AppendU32(out, leaf.reserved);
        EncodePlayableRef(leaf.content, out);
        EncodeTime(leaf.startOffset, out);
        EncodeTime(leaf.duration, out);
        AppendU32(out, leaf.loopCount);
        AppendU8(out, leaf.loopRegionPresent);
        AppendU8(out, leaf.reserved2[0]);
        AppendU8(out, leaf.reserved2[1]);
        AppendU8(out, leaf.reserved2[2]);
        EncodeTime(leaf.loopStart, out);
        EncodeTime(leaf.loopEnd, out);
        AppendU32(out, leaf.pathStepCount);
        AppendU32(out, leaf.reserved3);
        for (std::uint32_t p = 0; p < leaf.pathStepCount; ++p) {
            const StableAuthoredPathStepV1& step = leaf.pathSteps[p];
            EncodePlayableRef(step.playable, out);
            AppendU32(out, step.stableChildOrdinal);
            AppendU32(out, step.reserved);
            AppendObjectId(out, step.trackId);
            AppendObjectId(out, step.clipId);
            AppendU64(out, step.clipGuidHigh);
            AppendU64(out, step.clipGuidLow);
        }
    }
    return true;
}

void EncodeInlineSoundTimingHint(const InlineSoundTimingHintV2& hint,
                                 std::vector<std::uint8_t>& out) {
    EncodeTime(hint.startOffset, out);
    EncodeTime(hint.duration, out);
    AppendU32(out, hint.loopCount);
    AppendU8(out, hint.loopRegionPresent);
    AppendU8(out, hint.reserved[0]);
    AppendU8(out, hint.reserved[1]);
    AppendU8(out, hint.reserved[2]);
    EncodeTime(hint.loopStart, out);
    EncodeTime(hint.loopEnd, out);
}

bool V9CapacityPrefixMatches(const WorldCapacitySummaryV1& prefix,
                             const WorldCapacitySummaryV1& complete) noexcept {
    return prefix.maxContainerDepth == complete.maxContainerDepth &&
           prefix.maxChildrenPerContainer == complete.maxChildrenPerContainer &&
           prefix.maxAvoidLastN == complete.maxAvoidLastN &&
           prefix.maxDiscreteAxes == complete.maxDiscreteAxes &&
           prefix.maxDiscreteRows == complete.maxDiscreteRows &&
           prefix.maxDiscreteTrieNodes == complete.maxDiscreteTrieNodes &&
           prefix.maxContinuousAxes == complete.maxContinuousAxes &&
           prefix.maxContinuousRows == complete.maxContinuousRows &&
           prefix.maxContinuousCandidates == complete.maxContinuousCandidates &&
           prefix.maxContinuousActiveRows ==
               complete.maxContinuousActiveRows &&
           prefix.maxContinuousLiveRows == complete.maxContinuousLiveRows &&
           prefix.maxTransitionRules == complete.maxTransitionRules &&
           prefix.maxCurvePoints == complete.maxCurvePoints &&
           prefix.maxKnownScheduleLeaves == complete.maxKnownScheduleLeaves &&
           prefix.maxTrackSchedulePrograms ==
               complete.maxTrackSchedulePrograms &&
           prefix.maxSegmentSchedulePrograms ==
               complete.maxSegmentSchedulePrograms &&
           prefix.maxScheduleTrackRefsPerSegment ==
               complete.maxScheduleTrackRefsPerSegment &&
           prefix.maxScheduleTrackRefsTotal ==
               complete.maxScheduleTrackRefsTotal &&
           prefix.maxSchedulePlacementsPerTrack ==
               complete.maxSchedulePlacementsPerTrack &&
           prefix.maxSchedulePlacementsTotal ==
               complete.maxSchedulePlacementsTotal &&
           prefix.maxScheduleMarkersPerSegment ==
               complete.maxScheduleMarkersPerSegment &&
           prefix.maxScheduleMarkersTotal == complete.maxScheduleMarkersTotal &&
           prefix.maxTrackSegmentScheduleDepth ==
               complete.maxTrackSegmentScheduleDepth &&
           prefix.maxActionDefinitions == complete.maxActionDefinitions &&
           prefix.maxActionStepsPerDefinition ==
               complete.maxActionStepsPerDefinition &&
           prefix.maxActionStepsTotal == complete.maxActionStepsTotal &&
           prefix.maxActionTriggerDepth == complete.maxActionTriggerDepth &&
           prefix.maxActionExpandedWork == complete.maxActionExpandedWork;
}

void EncodeSchedulePlacement(const ScheduleClipPlacementV1& row,
                             std::vector<std::uint8_t>& out) {
    AppendU32(out, row.stablePlacementOrdinal);
    AppendU32(out, row.stableOccurrenceOrdinal);
    EncodePlayableRef(row.content, out);
    AppendU64(out, std::bit_cast<std::uint64_t>(row.authoredStart));
    AppendU64(out, std::bit_cast<std::uint64_t>(row.authoredDuration));
    AppendU64(out, std::bit_cast<std::uint64_t>(row.authoredFadeIn));
    AppendU64(out, std::bit_cast<std::uint64_t>(row.authoredFadeOut));
    AppendObjectId(out, row.clipId);
    AppendU64(out, row.clipGuidHigh);
    AppendU64(out, row.clipGuidLow);
}

PlayableRefV1 DecodePlayableRef(Reader& reader) noexcept {
    PlayableRefV1 row{};
    row.kind = static_cast<PlayableKindV1>(reader.U32());
    row.reserved = reader.U32();
    row.stableId = reader.ObjectId();
    row.expectedProgramSemanticFingerprint = reader.U64();
    row.contentRevision = reader.U64();
    row.routeStableId = reader.ObjectId();
    row.compatibilityFingerprint = reader.U64();
    return row;
}

ActionObjectRefV1 DecodeActionRef(Reader& reader) noexcept {
    ActionObjectRefV1 row{};
    row.kind = static_cast<ActionTargetKindV1>(reader.U32());
    row.reserved = reader.U32();
    row.stableId = reader.ObjectId();
    row.expectedProgramSemanticFingerprint = reader.U64();
    return row;
}

AuthoredTimeRationalV1 DecodeActionTime(Reader& reader) noexcept {
    AuthoredTimeRationalV1 row{};
    row.domain = static_cast<AuthoredTimeDomainV1>(reader.U32());
    row.numerator = std::bit_cast<std::int64_t>(reader.U64());
    row.denominator = reader.U32();
    return row;
}

AuthoredTimeRationalV1 DecodeTime(Reader& reader) noexcept {
    AuthoredTimeRationalV1 row{};
    row.domain = static_cast<AuthoredTimeDomainV1>(reader.U32());
    row.numerator = static_cast<std::int64_t>(reader.U64());
    row.denominator = reader.U32();
    return row;
}

bool DecodeCurve(Reader& reader, CurveV1& row,
                 bool allowRawInvalid) noexcept {
    row = {};
    row.curveLocalId = reader.U32();
    const std::uint32_t pointCount = reader.U32();
    if (reader.failed || pointCount > kMaxCurvePoints) {
        return false;
    }
    std::array<CurvePoint, kMaxCurvePoints> points{};
    std::array<CurveHandlePair, kMaxCurvePoints - 1> handles{};
    for (std::uint32_t i = 0; i < pointCount; ++i) {
        points[i].x = reader.F32();
        points[i].y = reader.F32();
    }
    for (std::uint32_t i = 0; i + 1 < pointCount; ++i) {
        handles[i].h1x = reader.F32();
        handles[i].h1y = reader.F32();
        handles[i].h2x = reader.F32();
        handles[i].h2y = reader.F32();
    }
    if (reader.failed) {
        return false;
    }
    if (pointCount != 0 &&
        row.curve.SetPoints(points.data(), handles.data(), pointCount)) {
        return true;
    }
    if (!allowRawInvalid) {
        return false;
    }
    row.curve = {};
    row.curve.pointCount = pointCount;
    for (std::uint32_t i = 0; i < pointCount; ++i) {
        row.curve.points[i] = points[i];
    }
    for (std::uint32_t i = 0; i + 1 < pointCount; ++i) {
        row.curve.handles[i] = handles[i];
    }
    return true;
}

void DecodePayloadHeader(Reader& reader,
                         PlayablePropertyPayloadViewV1& payload,
                         std::uint32_t worldTraceVintage) noexcept {
    payload.presentMask = reader.U64();
    payload.values[0].bits = reader.U32();
    payload.values[1].bits = reader.U32();
    payload.values[2].bits = reader.U32();
    payload.values[5].bits = reader.U32();
    payload.values[4].bits = reader.U32();
    payload.values[3].bits = worldTraceVintage >= 5
        ? reader.U32()
        : reader.U8();
    payload.values[10].bits = reader.U8();
    const std::uint16_t reserved = reader.U16();
    payload.values[8].bits = reader.U32();
    payload.values[9].bits = reader.U32();
    payload.values[11].bits = reader.U32();
    payload.values[12].bits = reader.U32();
    payload.values[6].objectId = reader.ObjectId();
    payload.values[13].objectId = reader.ObjectId();
    payload.values[14].bits = reader.U64();
    if (worldTraceVintage >= kCurrentWorldBodyVintageV13) {
        payload.values[16].bits = reader.U32();
    } else {
        payload.values[16].bits = std::bit_cast<std::uint32_t>(1.0f);
    }
    payload.sendCount = reader.U32();
    const std::uint32_t reservedCount1 = reader.U32();
    const std::uint32_t reservedCount2 = reader.U32();
    payload.effectNodeCount = reader.U32();
    payload.effectEdgeCount = reader.U32();
    if (reserved != 0 || payload.values[14].bits != 0 ||
        reservedCount1 != 0 || reservedCount2 != 0) {
        reader.failed = true;
    }
}

} // namespace

struct CurrentWorldBodyImage::Impl {
    struct OwnedPayload {
        PlayablePropertyPayloadViewV1 view{};
        std::vector<SendV1> sends;
        std::vector<EffectNodeV1> nodes;
        std::vector<std::vector<EffectParamV1>> params;
        std::vector<EffectEdgeV1> edges;

        void Refresh() noexcept {
            view.sendCount = static_cast<std::uint32_t>(sends.size());
            view.sends = sends.empty() ? nullptr : sends.data();
            view.effectNodeCount = static_cast<std::uint32_t>(nodes.size());
            view.effectNodes = nodes.empty() ? nullptr : nodes.data();
            for (std::size_t i = 0; i < nodes.size(); ++i) {
                nodes[i].paramCount =
                    static_cast<std::uint32_t>(params[i].size());
                nodes[i].params = params[i].empty() ? nullptr : params[i].data();
            }
            view.effectEdgeCount = static_cast<std::uint32_t>(edges.size());
            view.effectEdges = edges.empty() ? nullptr : edges.data();
        }
    };

    struct OwnedProperty {
        PlayablePropertyProgramViewV1 view{};
        std::vector<LayerAncestorRefV1> ancestors;
        std::vector<StructuralAncestorRefV1> structuralAncestors;
        std::vector<OwnedPayload> structuralPayloads;
        OwnedPayload payload;
        void Refresh() noexcept {
            payload.Refresh();
            view.organizationLayerAncestorCount =
                static_cast<std::uint32_t>(ancestors.size());
            view.organizationLayerAncestors =
                ancestors.empty() ? nullptr : ancestors.data();
            view.structuralAncestorCount =
                static_cast<std::uint32_t>(structuralAncestors.size());
            view.structuralAncestors = structuralAncestors.empty()
                ? nullptr : structuralAncestors.data();
            for (std::size_t i = 0; i < structuralAncestors.size(); ++i) {
                structuralPayloads[i].Refresh();
                structuralAncestors[i].payload = structuralPayloads[i].view;
            }
            view.payload = payload.view;
        }
    };

    struct OwnedOccurrence {
        PlayableOccurrencePropertyViewV1 view{};
        OwnedPayload payload;
        void Refresh() noexcept {
            payload.Refresh();
            view.payload = payload.view;
        }
    };

    struct OwnedCurrentContainer {
        ContainerProgramViewV3 view{};
        std::vector<CurveV1> curves;
        std::vector<ContainerChildProgramViewV2> children;
        std::vector<GridAxisV1> discreteAxes;
        std::vector<DiscreteGridRowViewV1> discreteRows;
        std::vector<std::vector<DiscreteGridCellV1>> discreteCells;
        std::vector<GridAxisV1> continuousAxes;
        std::vector<ContinuousGridRowViewV1> continuousRows;
        std::vector<std::vector<ContinuousGridCellV1>> continuousCells;
        void Refresh() noexcept {
            view.curveCount = static_cast<std::uint32_t>(curves.size());
            view.curves = curves.empty() ? nullptr : curves.data();
            view.childCount = static_cast<std::uint32_t>(children.size());
            view.children = children.empty() ? nullptr : children.data();
            view.discreteAxisCount =
                static_cast<std::uint32_t>(discreteAxes.size());
            view.discreteAxes =
                discreteAxes.empty() ? nullptr : discreteAxes.data();
            view.discreteRowCount =
                static_cast<std::uint32_t>(discreteRows.size());
            view.discreteRows =
                discreteRows.empty() ? nullptr : discreteRows.data();
            for (std::size_t i = 0; i < discreteRows.size(); ++i) {
                discreteRows[i].cellCount =
                    static_cast<std::uint32_t>(discreteCells[i].size());
                discreteRows[i].cells = discreteCells[i].empty()
                    ? nullptr : discreteCells[i].data();
            }
            view.continuousAxisCount =
                static_cast<std::uint32_t>(continuousAxes.size());
            view.continuousAxes =
                continuousAxes.empty() ? nullptr : continuousAxes.data();
            view.continuousRowCount =
                static_cast<std::uint32_t>(continuousRows.size());
            view.continuousRows =
                continuousRows.empty() ? nullptr : continuousRows.data();
            for (std::size_t i = 0; i < continuousRows.size(); ++i) {
                continuousRows[i].cellCount =
                    static_cast<std::uint32_t>(continuousCells[i].size());
                continuousRows[i].cells = continuousCells[i].empty()
                    ? nullptr : continuousCells[i].data();
            }
        }
    };

    struct OwnedContainer {
        ContainerProgramViewV2 view{};
        std::vector<CurveV1> curves;
        std::vector<ContainerChildProgramViewV1> children;
        std::vector<std::vector<AuthoredCompletionLeafV1>> leaves;
        std::vector<std::vector<std::vector<StableAuthoredPathStepV1>>> paths;
        std::vector<GridAxisV1> discreteAxes;
        std::vector<DiscreteGridRowViewV1> discreteRows;
        std::vector<std::vector<DiscreteGridCellV1>> discreteCells;
        std::vector<GridAxisV1> continuousAxes;
        std::vector<ContinuousGridRowViewV1> continuousRows;
        std::vector<std::vector<ContinuousGridCellV1>> continuousCells;
        void Refresh() noexcept {
            view.curveCount = static_cast<std::uint32_t>(curves.size());
            view.curves = curves.empty() ? nullptr : curves.data();
            view.childCount = static_cast<std::uint32_t>(children.size());
            view.children = children.empty() ? nullptr : children.data();
            for (std::size_t i = 0; i < children.size(); ++i) {
                auto& completion = children[i].completion;
                if (completion.source != CompletionSourceV1::InlineTemplate) {
                    continue;
                }
                completion.payload.inlineTemplate.leafCount =
                    static_cast<std::uint32_t>(leaves[i].size());
                completion.payload.inlineTemplate.leaves =
                    leaves[i].empty() ? nullptr : leaves[i].data();
                for (std::size_t p = 0; p < leaves[i].size(); ++p) {
                    leaves[i][p].pathStepCount =
                        static_cast<std::uint32_t>(paths[i][p].size());
                    leaves[i][p].pathSteps = paths[i][p].empty()
                        ? nullptr : paths[i][p].data();
                }
            }
            view.discreteAxisCount =
                static_cast<std::uint32_t>(discreteAxes.size());
            view.discreteAxes =
                discreteAxes.empty() ? nullptr : discreteAxes.data();
            view.discreteRowCount =
                static_cast<std::uint32_t>(discreteRows.size());
            view.discreteRows =
                discreteRows.empty() ? nullptr : discreteRows.data();
            for (std::size_t i = 0; i < discreteRows.size(); ++i) {
                discreteRows[i].cellCount =
                    static_cast<std::uint32_t>(discreteCells[i].size());
                discreteRows[i].cells = discreteCells[i].empty()
                    ? nullptr : discreteCells[i].data();
            }
            view.continuousAxisCount =
                static_cast<std::uint32_t>(continuousAxes.size());
            view.continuousAxes =
                continuousAxes.empty() ? nullptr : continuousAxes.data();
            view.continuousRowCount =
                static_cast<std::uint32_t>(continuousRows.size());
            view.continuousRows =
                continuousRows.empty() ? nullptr : continuousRows.data();
            for (std::size_t i = 0; i < continuousRows.size(); ++i) {
                continuousRows[i].cellCount =
                    static_cast<std::uint32_t>(continuousCells[i].size());
                continuousRows[i].cells = continuousCells[i].empty()
                    ? nullptr : continuousCells[i].data();
            }
        }
    };

    struct OwnedTrackSchedule {
        TrackScheduleProgramViewV1 view{};
        std::vector<ScheduleClipPlacementV1> placements;
        void Refresh() noexcept {
            view.placementCount =
                static_cast<std::uint32_t>(placements.size());
            view.placements = placements.empty() ? nullptr : placements.data();
        }
    };

    struct OwnedSegmentSchedule {
        SegmentScheduleProgramViewV1 view{};
        std::vector<ScheduleTrackRefV1> trackRefs;
        std::vector<ScheduleMarkerV1> markers;
        void Refresh() noexcept {
            view.trackRefCount = static_cast<std::uint32_t>(trackRefs.size());
            view.trackRefs = trackRefs.empty() ? nullptr : trackRefs.data();
            view.markerCount = static_cast<std::uint32_t>(markers.size());
            view.markers = markers.empty() ? nullptr : markers.data();
        }
    };

    struct OwnedTransition {
        TransitionProgramViewV1 view{};
        std::vector<StableAuthoredPathStepV1> downwardPathSteps;
        std::vector<TransitionRuleViewV1> rules;
        void Refresh() noexcept {
            view.downwardPathStepCount =
                static_cast<std::uint32_t>(downwardPathSteps.size());
            view.downwardPathSteps = downwardPathSteps.empty()
                ? nullptr : downwardPathSteps.data();
            view.ruleCount = static_cast<std::uint32_t>(rules.size());
            view.rules = rules.empty() ? nullptr : rules.data();
        }
    };

    struct OwnedAction {
        ActionProgramViewV1 view{};
        std::vector<ActionStepProgramViewV1> steps;
        void Refresh() noexcept {
            view.stepCount = static_cast<std::uint32_t>(steps.size());
            view.steps = steps.empty() ? nullptr : steps.data();
        }
    };

    ContainerWorldInstallViewV1 view{};
    std::vector<VariableDefinitionV1> variables;
    std::vector<OwnedProperty> properties;
    std::vector<PlayablePropertyProgramViewV1> propertyViews;
    std::vector<OwnedOccurrence> occurrences;
    std::vector<PlayableOccurrencePropertyViewV1> occurrenceViews;
    std::vector<OwnedTrackSchedule> trackSchedules;
    std::vector<TrackScheduleProgramViewV1> trackScheduleViews;
    std::vector<OwnedSegmentSchedule> segmentSchedules;
    std::vector<SegmentScheduleProgramViewV1> segmentScheduleViews;
    std::vector<OwnedCurrentContainer> currentContainers;
    std::vector<ContainerProgramViewV3> currentContainerViews;
    std::vector<OwnedContainer> containers;
    std::vector<ContainerProgramViewV2> containerViews;
    HistoricalContainerWorldViewV1 historicalView{};
    bool hasReplayView = false;
    std::vector<OwnedTransition> transitions;
    std::vector<TransitionProgramViewV1> transitionViews;
    std::vector<TransitionProgramViewV1> transitionSentinels;
    bool useTransitionSentinel = false;
    std::vector<OwnedAction> actions;
    std::vector<ActionProgramViewV1> actionViews;

    void Refresh() noexcept {
        propertyViews.clear();
        propertyViews.reserve(properties.size());
        for (auto& row : properties) {
            row.Refresh();
            propertyViews.push_back(row.view);
        }
        occurrenceViews.clear();
        occurrenceViews.reserve(occurrences.size());
        for (auto& row : occurrences) {
            row.Refresh();
            occurrenceViews.push_back(row.view);
        }
        trackScheduleViews.clear();
        trackScheduleViews.reserve(trackSchedules.size());
        for (auto& row : trackSchedules) {
            row.Refresh();
            trackScheduleViews.push_back(row.view);
        }
        segmentScheduleViews.clear();
        segmentScheduleViews.reserve(segmentSchedules.size());
        for (auto& row : segmentSchedules) {
            row.Refresh();
            segmentScheduleViews.push_back(row.view);
        }
        currentContainerViews.clear();
        currentContainerViews.reserve(currentContainers.size());
        for (auto& row : currentContainers) {
            row.Refresh();
            currentContainerViews.push_back(row.view);
        }
        containerViews.clear();
        containerViews.reserve(containers.size());
        for (auto& row : containers) {
            row.Refresh();
            containerViews.push_back(row.view);
        }
        transitionViews.clear();
        transitionViews.reserve(transitions.size());
        for (auto& row : transitions) {
            row.Refresh();
            transitionViews.push_back(row.view);
        }
        view.variables = variables.empty() ? nullptr : variables.data();
        view.variableCount = static_cast<std::uint32_t>(variables.size());
        view.playableProperties =
            propertyViews.empty() ? nullptr : propertyViews.data();
        view.playablePropertyCount =
            static_cast<std::uint32_t>(propertyViews.size());
        view.playableOccurrences =
            occurrenceViews.empty() ? nullptr : occurrenceViews.data();
        view.playableOccurrenceCount =
            static_cast<std::uint32_t>(occurrenceViews.size());
        view.trackSchedules = trackScheduleViews.empty()
            ? nullptr : trackScheduleViews.data();
        view.trackScheduleCount =
            static_cast<std::uint32_t>(trackScheduleViews.size());
        view.segmentSchedules = segmentScheduleViews.empty()
            ? nullptr : segmentScheduleViews.data();
        view.segmentScheduleCount =
            static_cast<std::uint32_t>(segmentScheduleViews.size());
        view.containers = currentContainerViews.empty()
            ? nullptr : currentContainerViews.data();
        view.containerCount =
            static_cast<std::uint32_t>(currentContainerViews.size());
        if (useTransitionSentinel) {
            view.transitions = transitionSentinels.empty()
                ? nullptr : transitionSentinels.data();
            view.transitionCount =
                static_cast<std::uint32_t>(transitionSentinels.size());
        } else {
            view.transitions = transitionViews.empty()
                ? nullptr : transitionViews.data();
            view.transitionCount =
                static_cast<std::uint32_t>(transitionViews.size());
        }
        actionViews.clear();
        actionViews.reserve(actions.size());
        for (auto& row : actions) {
            row.Refresh();
            actionViews.push_back(row.view);
        }
        view.actions = actionViews.empty() ? nullptr : actionViews.data();
        view.actionCount = static_cast<std::uint32_t>(actionViews.size());
        historicalView.identity = view.identity;
        historicalView.variables = view.variables;
        historicalView.variableCount = view.variableCount;
        historicalView.playableProperties = view.playableProperties;
        historicalView.playablePropertyCount = view.playablePropertyCount;
        historicalView.playableOccurrences = view.playableOccurrences;
        historicalView.playableOccurrenceCount = view.playableOccurrenceCount;
        historicalView.trackSchedules = view.trackSchedules;
        historicalView.trackScheduleCount = view.trackScheduleCount;
        historicalView.segmentSchedules = view.segmentSchedules;
        historicalView.segmentScheduleCount = view.segmentScheduleCount;
        historicalView.containers = containerViews.empty()
            ? nullptr : containerViews.data();
        historicalView.containerCount =
            static_cast<std::uint32_t>(containerViews.size());
        historicalView.transitions = view.transitions;
        historicalView.transitionCount = view.transitionCount;
        historicalView.actions = view.actions;
        historicalView.actionCount = view.actionCount;
        historicalView.capacity = view.capacity;
    }
};

namespace {

template <typename OwnedPayload>
bool DecodePayloadBody(Reader& reader, OwnedPayload& payload,
                       bool) {
    const auto header = payload.view;
    if (header.sendCount > kHardPlayablePropertyChildRowCapacity ||
        header.effectNodeCount > kHardPlayablePropertyChildRowCapacity ||
        header.effectEdgeCount > kHardPlayablePropertyEffectEdgeCapacity) {
        return false;
    }
    payload.sends.resize(header.sendCount);
    for (SendV1& row : payload.sends) {
        row.stableSendOrdinal = reader.U32();
        row.reserved = reader.U32();
        row.targetBusId = reader.ObjectId();
        row.sendLevel = reader.F32();
        row.reserved2 = reader.U32();
    }
    payload.nodes.resize(header.effectNodeCount);
    payload.params.resize(header.effectNodeCount);
    std::uint32_t paramTotal = 0;
    for (std::size_t i = 0; i < payload.nodes.size(); ++i) {
        EffectNodeV1& row = payload.nodes[i];
        row.nodeId = reader.U32();
        row.effectId = reader.ObjectId();
        row.effectType = reader.U32();
        const std::uint32_t paramCount = reader.U32();
        const std::uint32_t reserved = reader.U32();
        if (reserved != 0 ||
            !AddCount(paramCount, kHardPlayablePropertyEffectParamCapacity,
                      paramTotal)) {
            return false;
        }
        payload.params[i].resize(paramCount);
        for (EffectParamV1& param : payload.params[i]) {
            param.paramId = reader.U32();
            param.valueBits = reader.U32();
        }
    }
    payload.edges.resize(header.effectEdgeCount);
    for (EffectEdgeV1& row : payload.edges) {
        row.fromNodeId = reader.U32();
        row.toNodeId = reader.U32();
    }
    return !reader.failed;
}

bool DecodeCompletion(
    Reader& reader, ContainerChildProgramViewV1& child,
    std::vector<AuthoredCompletionLeafV1>& leaves,
    std::vector<std::vector<StableAuthoredPathStepV1>>& paths) {
    child.completion.source = CompletionSourceV1::InlineTemplate;
    child.completion.reserved = 0;
    child.completion.payload.inlineTemplate.completionClass =
        static_cast<AuthoredCompletionClassV1>(reader.U32());
    const std::uint32_t leafCount = reader.U32();
    if (leafCount > kTraceWorldChildCap) {
        return false;
    }
    leaves.resize(leafCount);
    paths.resize(leafCount);
    for (std::size_t i = 0; i < leaves.size(); ++i) {
        AuthoredCompletionLeafV1& leaf = leaves[i];
        leaf.stableLeafOrdinal = reader.U32();
        leaf.reserved = reader.U32();
        leaf.content = DecodePlayableRef(reader);
        leaf.startOffset = DecodeTime(reader);
        leaf.duration = DecodeTime(reader);
        leaf.loopCount = reader.U32();
        leaf.loopRegionPresent = reader.U8();
        leaf.reserved2[0] = reader.U8();
        leaf.reserved2[1] = reader.U8();
        leaf.reserved2[2] = reader.U8();
        leaf.loopStart = DecodeTime(reader);
        leaf.loopEnd = DecodeTime(reader);
        const std::uint32_t pathCount = reader.U32();
        leaf.reserved3 = reader.U32();
        if (pathCount > kTraceWorldChildCap) {
            return false;
        }
        paths[i].resize(pathCount);
        for (StableAuthoredPathStepV1& step : paths[i]) {
            step.playable = DecodePlayableRef(reader);
            step.stableChildOrdinal = reader.U32();
            step.reserved = reader.U32();
            step.trackId = reader.ObjectId();
            step.clipId = reader.ObjectId();
            step.clipGuidHigh = reader.U64();
            step.clipGuidLow = reader.U64();
        }
    }
    return !reader.failed;
}

} // namespace

template <typename World>
bool EncodeCurrentWorldBodyImpl(
    const World& world,
    std::vector<std::uint8_t>& out,
    bool includeActions, bool includeStructuralAncestors,
    bool useV9ContainerLayout, bool includeSpatializationMix,
    bool includeEntryTimeline = false) noexcept {
    try {
        out.clear();
        if (!CountPointer(world.variableCount, world.variables,
                          kHardVariableDefinitionCapacity) ||
            !CountPointer(world.playablePropertyCount,
                          world.playableProperties,
                          kHardPlayablePropertyProgramCapacity) ||
            !CountPointer(world.playableOccurrenceCount,
                          world.playableOccurrences,
                          kHardPlayableOccurrencePropertyCapacity) ||
            !CountPointer(world.trackScheduleCount, world.trackSchedules,
                          kHardTrackScheduleProgramCapacity) ||
            !CountPointer(world.segmentScheduleCount, world.segmentSchedules,
                          kHardSegmentScheduleProgramCapacity) ||
            !CountPointer(world.containerCount, world.containers,
                           kHardContainerProgramCapacity) ||
            !CountPointer(world.transitionCount, world.transitions,
                           kTraceWorldChildCap) ||
            (includeActions &&
             !CountPointer(world.actionCount, world.actions,
                           kHardActionDefinitionCapacity)) ||
            (!includeActions && world.actionCount != 0)) {
            return false;
        }
        std::array<std::uint32_t, 6> totals{};
        std::uint32_t ancestorTotal = 0;
        std::uint32_t structuralAncestorTotal = 0;
        std::uint32_t trackRefTotal = 0;
        std::uint32_t placementTotal = 0;
        std::uint32_t markerTotal = 0;
        for (std::uint32_t i = 0; i < world.playablePropertyCount; ++i) {
            const auto& row = world.playableProperties[i];
            if (!CountPointer(row.organizationLayerAncestorCount,
                              row.organizationLayerAncestors,
                              kHardPlayablePropertyChildRowCapacity) ||
                !AddCount(row.organizationLayerAncestorCount,
                          kHardPlayablePropertyChildRowCapacity,
                          ancestorTotal) ||
                !CountPointer(row.structuralAncestorCount,
                              row.structuralAncestors,
                              kHardPlayablePropertyChildRowCapacity) ||
                (!includeStructuralAncestors &&
                 row.structuralAncestorCount != 0) ||
                (includeStructuralAncestors &&
                 !AddCount(row.structuralAncestorCount,
                           kHardPlayablePropertyChildRowCapacity,
                           structuralAncestorTotal)) ||
                !CountPayload(row.payload, totals)) {
                return false;
            }
            if (includeStructuralAncestors) {
                for (std::uint32_t a = 0; a < row.structuralAncestorCount; ++a) {
                    std::array<std::uint32_t, 6> ignoredTotals{};
                    if (!CountPayload(row.structuralAncestors[a].payload,
                                      ignoredTotals))
                        return false;
                }
            }
        }
        for (std::uint32_t i = 0; i < world.playableOccurrenceCount; ++i) {
            if (!CountPayload(world.playableOccurrences[i].payload, totals)) {
                return false;
            }
        }
        for (std::uint32_t i = 0; i < world.trackScheduleCount; ++i) {
            const auto& row = world.trackSchedules[i];
            if (!CountPointer(row.placementCount, row.placements,
                              kHardSchedulePlacementCapacity) ||
                !AddCount(row.placementCount,
                          kHardSchedulePlacementCapacity,
                          placementTotal)) {
                return false;
            }
        }
        for (std::uint32_t i = 0; i < world.segmentScheduleCount; ++i) {
            const auto& row = world.segmentSchedules[i];
            if (!includeEntryTimeline &&
                (row.entryMarker.stableMarkerId != VnObjectId{} ||
                 row.entryMarker.position != 0 || row.bounds.contentStart != 0 ||
                 row.bounds.contentEnd != 0 || row.bounds.nominalStart != 0 ||
                 row.bounds.nominalEnd != 0)) return false;
            if (!CountPointer(row.trackRefCount, row.trackRefs,
                              kHardScheduleTrackRefCapacity) ||
                !CountPointer(row.markerCount, row.markers,
                              kHardScheduleMarkerCapacity) ||
                !AddCount(row.trackRefCount, kHardScheduleTrackRefCapacity,
                          trackRefTotal) ||
                !AddCount(row.markerCount, kHardScheduleMarkerCapacity, markerTotal)) {
                return false;
            }
        }
        std::uint32_t transitionRuleTotal = 0;
        std::uint32_t transitionPathTotal = 0;
        for (std::uint32_t i = 0; i < world.transitionCount; ++i) {
            const auto& row = world.transitions[i];
            if (!CountPointer(row.ruleCount, row.rules,
                              kTraceWorldChildCap) ||
                !CountPointer(row.downwardPathStepCount,
                              row.downwardPathSteps,
                              kTraceWorldChildCap) ||
                !AddCount(row.ruleCount, kTraceWorldChildCap,
                          transitionRuleTotal) ||
                !AddCount(row.downwardPathStepCount, kTraceWorldChildCap,
                          transitionPathTotal)) {
                return false;
            }
        }
        CurrentWorldV3PrefixValues prefix{};
        if (!TryBuildCurrentWorldV3Prefix(world, false, &prefix) ||
            prefix.playablePropertySendCount != totals[0] ||
            prefix.reservedChildCount1 != totals[1] ||
            prefix.reservedChildCount2 != totals[2] ||
            prefix.playablePropertyLayerAncestorRefCount != ancestorTotal ||
            prefix.playablePropertyEffectNodeCount != totals[3] ||
            prefix.playablePropertyEffectParamCount != totals[4] ||
            prefix.playablePropertyEffectEdgeCount != totals[5] ||
            prefix.scheduleTrackRefCount != trackRefTotal ||
            prefix.schedulePlacementCount != placementTotal ||
            prefix.scheduleMarkerCount != markerTotal) {
            return false;
        }
        PrefixEncodeVisitor prefixVisitor{out};
        VisitCurrentWorldV3Prefix(prefix, prefixVisitor);
        if (includeActions) {
            AppendU32(out, world.actionCount);
            AppendU32(out, world.capacity.maxActionDefinitions);
            AppendU32(out, world.capacity.maxActionStepsPerDefinition);
            AppendU32(out, world.capacity.maxActionStepsTotal);
            AppendU32(out, world.capacity.maxActionTriggerDepth);
            AppendU32(out, world.capacity.maxActionExpandedWork);
        }
        if (includeStructuralAncestors) {
            AppendU32(out, structuralAncestorTotal);
        }
        if (useV9ContainerLayout) {
            WorldCapacitySummaryV1 capacity = world.capacity;
            PrefixEncodeVisitor capacityVisitor{out};
            const std::size_t capacityOffset = out.size();
            VisitWorldCapacitySummaryV1(capacity, capacityVisitor);
            if (out.size() - capacityOffset !=
                kCurrentWorldV9CapacityWireBytes) {
                out.clear();
                return false;
            }
            AppendObjectId(out, world.identity.projectId);
        }
        for (std::uint32_t i = 0; i < world.variableCount; ++i) {
            const VariableDefinitionV1& row = world.variables[i];
            AppendObjectId(out, row.variableId);
            AppendU32(out, static_cast<std::uint32_t>(row.kind));
            AppendU32(out, includeActions ? 1u : 0u);
            AppendF32(out, row.rangeMin);
            AppendF32(out, row.rangeMax);
            AppendF32(out, row.defaultValue);
            AppendU32(out, 0);
        }
        for (std::uint32_t i = 0; i < world.playablePropertyCount; ++i) {
            const PlayablePropertyProgramViewV1& row =
                world.playableProperties[i];
            AppendU32(out, static_cast<std::uint32_t>(row.contentPropertyKind));
            AppendU32(out, row.authoredStableOrder);
            AppendObjectId(out, row.stableId);
            AppendU64(out, row.programSemanticFingerprint);
            AppendU64(out, row.contentRevision);
            AppendObjectId(out, row.routeStableId);
            AppendU64(out, row.compatibilityFingerprint);
            AppendU32(out, row.organizationLayerAncestorCount);
            AppendU32(out, 0);
            EncodePayloadHeader(row.payload, out, includeSpatializationMix);
            for (std::uint32_t a = 0;
                 a < row.organizationLayerAncestorCount; ++a) {
                AppendObjectId(out, row.organizationLayerAncestors[a].layerStableId);
                AppendU64(out, row.organizationLayerAncestors[a]
                                   .layerProgramSemanticFingerprint);
            }
            if (includeStructuralAncestors) {
                AppendU32(out, row.structuralAncestorCount);
                AppendU32(out, 0);
                for (std::uint32_t a = 0;
                     a < row.structuralAncestorCount; ++a) {
                    const auto& ancestor = row.structuralAncestors[a];
                    AppendU32(out, static_cast<std::uint32_t>(
                                         ancestor.contentPropertyKind));
                    AppendU32(out, ancestor.reserved);
                    AppendObjectId(out, ancestor.stableId);
                    AppendU64(out, ancestor.programSemanticFingerprint);
                    EncodePayloadHeader(ancestor.payload, out,
                                        includeSpatializationMix);
                    if (!EncodePayloadBody(ancestor.payload, out)) return false;
                }
            }
            if (!EncodePayloadBody(row.payload, out)) return false;
        }
        for (std::uint32_t i = 0; i < world.playableOccurrenceCount; ++i) {
            const PlayableOccurrencePropertyViewV1& row =
                world.playableOccurrences[i];
            AppendU32(out, static_cast<std::uint32_t>(row.parentKind));
            AppendU32(out, row.stableOccurrenceOrdinal);
            AppendObjectId(out, row.parentStableId);
            AppendU64(out, row.parentProgramSemanticFingerprint);
            EncodePlayableRef(row.childPlayable, out);
            AppendU64(out, row.occurrenceSemanticFingerprint);
            EncodePayloadHeader(row.payload, out, includeSpatializationMix);
            if (!EncodePayloadBody(row.payload, out)) return false;
        }
        for (std::uint32_t i = 0; i < world.trackScheduleCount; ++i) {
            const auto& row = world.trackSchedules[i];
            EncodePlayableRef(row.rootDefinition, out);
            AppendU64(out, row.scheduleSemanticFingerprint);
            AppendU32(out, row.placementCount);
            AppendU32(out, row.reserved);
            for (std::uint32_t p = 0; p < row.placementCount; ++p) {
                EncodeSchedulePlacement(row.placements[p], out);
            }
        }
        for (std::uint32_t i = 0; i < world.segmentScheduleCount; ++i) {
            const auto& row = world.segmentSchedules[i];
            EncodePlayableRef(row.rootDefinition, out);
            AppendU64(out, row.scheduleSemanticFingerprint);
            AppendU32(out, static_cast<std::uint32_t>(row.timeKind));
            AppendU32(out, row.startPointKind);
            AppendU32(out, row.startMarkerId);
            AppendU32(out, row.reserved);
            AppendU64(out, std::bit_cast<std::uint64_t>(
                               row.microsPerQuarter));
            AppendU32(out, row.meterNumerator);
            AppendU32(out, row.meterDenominator);
            AppendU32(out, row.trackRefCount);
            AppendU32(out, row.markerCount);
            if (includeEntryTimeline) {
                AppendObjectId(out, row.entryMarker.stableMarkerId);
                AppendU64(out, std::bit_cast<std::uint64_t>(row.entryMarker.position));
                AppendU64(out, std::bit_cast<std::uint64_t>(row.bounds.contentStart));
                AppendU64(out, std::bit_cast<std::uint64_t>(row.bounds.contentEnd));
                AppendU64(out, std::bit_cast<std::uint64_t>(row.bounds.nominalStart));
                AppendU64(out, std::bit_cast<std::uint64_t>(row.bounds.nominalEnd));
            }
            for (std::uint32_t t = 0; t < row.trackRefCount; ++t) {
                const auto& ref = row.trackRefs[t];
                EncodePlayableRef(ref.trackDefinition, out);
                AppendU64(out, ref.trackScheduleSemanticFingerprint);
                AppendU32(out, ref.stableTrackOrdinal);
                AppendU32(out, ref.reserved);
            }
            for (std::uint32_t c = 0; c < row.markerCount; ++c) {
                const auto& marker = row.markers[c];
                AppendObjectId(out, marker.stableMarkerId);
                AppendU32(out, marker.markerId);
                AppendU32(out, marker.markerKind);
                AppendU64(out, std::bit_cast<std::uint64_t>(marker.position));
            }
        }
        for (std::uint32_t i = 0; i < world.containerCount; ++i) {
            const auto& row = world.containers[i];
            if (!CountPointer(row.curveCount, row.curves,
                              kTraceWorldChildCap) ||
                !CountPointer(row.childCount, row.children,
                              kTraceWorldChildCap) ||
                !CountPointer(row.discreteAxisCount, row.discreteAxes,
                              kTraceWorldChildCap) ||
                !CountPointer(row.discreteRowCount, row.discreteRows,
                              kTraceWorldChildCap) ||
                !CountPointer(row.continuousAxisCount, row.continuousAxes,
                              kTraceWorldChildCap) ||
                !CountPointer(row.continuousRowCount, row.continuousRows,
                              kTraceWorldChildCap)) {
                return false;
            }
            AppendObjectId(out, row.containerStableId);
            AppendU64(out, row.programSemanticFingerprint);
            AppendU32(out, static_cast<std::uint32_t>(row.mode));
            AppendU32(out, static_cast<std::uint32_t>(row.randomDiscipline));
            AppendU32(out, static_cast<std::uint32_t>(row.memoryScope));
            AppendU32(out, static_cast<std::uint32_t>(row.retriggerPolicy));
            AppendU32(out, static_cast<std::uint32_t>(row.resetPolicy));
            AppendU32(out, static_cast<std::uint32_t>(row.sequenceEndPolicy));
            AppendU32(out, static_cast<std::uint32_t>(row.queueCycle));
            AppendU32(out, static_cast<std::uint32_t>(row.queueSplice));
            if constexpr (std::is_same_v<World, ContainerWorldInstallViewV1>) {
                AppendU32(out, row.queueCycleCount);
                AppendU8(out, row.resumePosition);
                AppendU8(out, 0u);
                AppendU8(out, 0u);
                AppendU8(out, 0u);
            } else {
                AppendU32(out, static_cast<std::uint32_t>(row.signalOnlyPolicy));
                AppendU8(out, row.resumePosition);
                AppendU8(out, row.reservedBool[0]);
                AppendU8(out, row.reservedBool[1]);
                AppendU8(out, row.reservedBool[2]);
            }
            AppendU32(out, row.avoidLastN);
            AppendU32(out, row.initialSequenceCursor);
            AppendU32(out, row.discreteTiming);
            if constexpr (std::is_same_v<World, ContainerWorldInstallViewV1>)
                AppendU32(out, row.continuousTiming);
            AppendU32(out, row.continuousNormalization);
            AppendU32(out, row.evaluationPeriodFrames);
            AppendU32(out, row.envelopeFrames);
            AppendU32(out, row.maxActiveRows);
            AppendU32(out, row.maxLiveRows);
            AppendU32(out, row.childCount);
            AppendU32(out, row.curveCount);
            AppendU32(out, row.discreteAxisCount);
            AppendU32(out, row.discreteRowCount);
            AppendU32(out, row.continuousAxisCount);
            AppendU32(out, row.continuousRowCount);
            AppendU32(out, row.transitionProgramIndex);
            AppendU32(out, row.authoredStableOrder);
            for (std::uint32_t c = 0; c < row.curveCount; ++c) {
                if (!EncodeCurve(row.curves[c], out)) return false;
            }
            for (std::uint32_t c = 0; c < row.childCount; ++c) {
                const auto& child = row.children[c];
                EncodePlayableRef(child.playable, out);
                AppendU32(out, child.stableChildOrdinal);
                AppendU32(out, child.authoredStableOrder);
                AppendU32(out, child.frequencyWeight);
                AppendU32(out, child.orderWeight);
                AppendU32(out, static_cast<std::uint32_t>(child.queueStopPolicy));
                AppendU32(out, child.reserved);
                if constexpr (std::is_same_v<World, ContainerWorldInstallViewV1>) {
                    AppendU32(out, static_cast<std::uint32_t>(
                                       child.timingHint.kind));
                    AppendU32(out, child.timingHint.reserved);
                    if (child.timingHint.kind ==
                        ChildTimingHintKindV2::InlineSound) {
                        EncodeInlineSoundTimingHint(
                            child.timingHint.payload.inlineSound, out);
                    } else if (child.timingHint.kind ==
                               ChildTimingHintKindV2::ScheduleReference) {
                        const auto& schedule =
                            child.timingHint.payload.schedule;
                        AppendU32(out,
                                  static_cast<std::uint32_t>(schedule.kind));
                        AppendU32(out, schedule.reserved);
                        AppendObjectId(out, schedule.stableId);
                        AppendU64(out,
                                  schedule.scheduleSemanticFingerprint);
                    } else if (child.timingHint.kind !=
                               ChildTimingHintKindV2::Unknown) {
                        out.clear();
                        return false;
                    }
                } else {
                    AppendU32(out, static_cast<std::uint32_t>(
                                       child.completion.source));
                    AppendU32(out, child.completion.reserved);
                    if (child.completion.source ==
                        CompletionSourceV1::InlineTemplate) {
                        if (!EncodeCompletion(
                                child.completion.payload.inlineTemplate, out)) {
                            return false;
                        }
                    } else if (child.completion.source ==
                               CompletionSourceV1::ScheduleReference) {
                        const auto& schedule =
                            child.completion.payload.schedule;
                        AppendU32(out,
                                  static_cast<std::uint32_t>(schedule.kind));
                        AppendU32(out, schedule.reserved);
                        AppendObjectId(out, schedule.stableId);
                        AppendU64(out,
                                  schedule.scheduleSemanticFingerprint);
                    } else {
                        out.clear();
                        return false;
                    }
                }
            }
            for (std::uint32_t a = 0; a < row.discreteAxisCount; ++a) {
                AppendU32(out, row.discreteAxes[a].stableAxisOrdinal);
                AppendU32(out, row.discreteAxes[a].authoredColumnOrder);
                AppendObjectId(out, row.discreteAxes[a].variableId);
            }
            for (std::uint32_t r = 0; r < row.discreteRowCount; ++r) {
                const auto& gridRow = row.discreteRows[r];
                if (!CountPointer(gridRow.cellCount, gridRow.cells,
                                  kTraceWorldChildCap)) {
                    out.clear();
                    return false;
                }
                AppendU32(out, gridRow.stableRowOrdinal);
                AppendU32(out, gridRow.authoredStableOrder);
                AppendU32(out, gridRow.childStableOrdinal);
                AppendU32(out, gridRow.cellCount);
                for (std::uint32_t c = 0; c < gridRow.cellCount; ++c) {
                    AppendU32(out, static_cast<std::uint32_t>(
                                       gridRow.cells[c].kind));
                    AppendU32(out, std::bit_cast<std::uint32_t>(
                                       gridRow.cells[c].exactValue));
                }
            }
            for (std::uint32_t a = 0; a < row.continuousAxisCount; ++a) {
                AppendU32(out, row.continuousAxes[a].stableAxisOrdinal);
                AppendU32(out, row.continuousAxes[a].authoredColumnOrder);
                AppendObjectId(out, row.continuousAxes[a].variableId);
            }
            for (std::uint32_t r = 0; r < row.continuousRowCount; ++r) {
                const auto& gridRow = row.continuousRows[r];
                if (!CountPointer(gridRow.cellCount, gridRow.cells,
                                  kTraceWorldChildCap)) {
                    out.clear();
                    return false;
                }
                AppendU32(out, gridRow.stableRowOrdinal);
                AppendU32(out, gridRow.authoredStableOrder);
                AppendU32(out, gridRow.childStableOrdinal);
                AppendU32(out, gridRow.cellCount);
                for (std::uint32_t c = 0; c < gridRow.cellCount; ++c) {
                    const auto& cell = gridRow.cells[c];
                    AppendU32(out, static_cast<std::uint32_t>(cell.kind));
                    AppendU32(out, std::bit_cast<std::uint32_t>(cell.lower));
                    AppendU32(out, std::bit_cast<std::uint32_t>(cell.upper));
                    AppendU8(out, cell.lowerInclusive);
                    AppendU8(out, cell.upperInclusive);
                    AppendU16(out, cell.reserved);
                    AppendU32(out, cell.leftCurveLocalId);
                    AppendU32(out, cell.rightCurveLocalId);
                }
            }
        }
        for (std::uint32_t i = 0; i < world.transitionCount; ++i) {
            const auto& row = world.transitions[i];
            AppendObjectId(out, row.ownerContainerStableId);
            AppendU32(out, row.ruleCount);
            AppendU32(out, row.defaultRuleIndex);
            AppendU32(out, row.downwardPathStepCount);
            AppendU32(out, row.maxContainerDescent);
            AppendU32(out, row.maxOwnerAscent);
            AppendU32(out, row.watchFallback);
            for (std::uint32_t p = 0; p < row.downwardPathStepCount; ++p) {
                const auto& step = row.downwardPathSteps[p];
                EncodePlayableRef(step.playable, out);
                AppendU32(out, step.stableChildOrdinal);
                AppendU32(out, step.reserved);
                AppendObjectId(out, step.trackId);
                AppendObjectId(out, step.clipId);
                AppendU64(out, step.clipGuidHigh);
                AppendU64(out, step.clipGuidLow);
            }
            for (std::uint32_t r = 0; r < row.ruleCount; ++r) {
                const auto& rule = row.rules[r];
                AppendU32(out, rule.stableRuleOrdinal);
                AppendU8(out, rule.sourceAny);
                AppendU8(out, rule.targetAny);
                AppendU8(out, rule.bridgePresent);
                AppendU8(out, rule.reserved);
                EncodePlayableRef(rule.source, out);
                EncodePlayableRef(rule.target, out);
                AppendU32(out, static_cast<std::uint32_t>(rule.sourceExit));
                AppendU32(out, static_cast<std::uint32_t>(rule.targetEntry));
                EncodePlayableRef(rule.bridge, out);
                const TransitionFadeV1 fades[] = {
                    rule.sourceFade, rule.bridgeFadeIn,
                    rule.bridgeFadeOut, rule.targetFade};
                for (const auto& fade : fades) {
                    AppendU8(out, fade.active);
                    AppendU8(out, fade.reserved[0]);
                    AppendU8(out, fade.reserved[1]);
                    AppendU8(out, fade.reserved[2]);
                    AppendU32(out, fade.durationFrames);
                    AppendU32(out, fade.curveLocalId);
                }
                AppendU64(out, rule.requiredClockFingerprint);
                AppendObjectId(out, rule.requiredMarkerStableId);
                AppendU64(out,
                          rule.samePositionCompatibilityFingerprint);
                AppendU32(out, rule.authoredStableOrder);
                AppendU32(out, rule.reserved2);
            }
        }
        if (includeActions) {
            std::uint64_t stepTotal = 0;
            for (std::uint32_t i = 0; i < world.actionCount; ++i) {
                const auto& action = world.actions[i];
                if (!CountPointer(action.stepCount, action.steps,
                                  kHardActionStepCapacity) ||
                    action.stepCount == 0) {
                    out.clear();
                    return false;
                }
                stepTotal += action.stepCount;
                if (stepTotal > kHardActionStepCapacity) {
                    out.clear();
                    return false;
                }
                AppendObjectId(out, action.actionStableId);
                AppendU64(out, action.programSemanticFingerprint);
                AppendU32(out, action.stepCount);
                AppendU32(out, action.directFeatureMask);
                AppendU32(out, action.transitiveFeatureMask);
                AppendU32(out, action.expandedStepOccurrenceCount);
                AppendU32(out, action.expandedFiringCount);
                for (std::uint32_t s = 0; s < action.stepCount; ++s) {
                    const auto& step = action.steps[s];
                    if (!includeEntryTimeline &&
                        (step.quantizeTiming == ActionQuantizeTimingV1::EntryMarker ||
                         step.quantizeTiming == ActionQuantizeTimingV1::ReferenceExitMarker ||
                         step.quantizeTiming == ActionQuantizeTimingV1::TargetNextMarker)) {
                        out.clear();
                        return false;
                    }
                    AppendU32(out, static_cast<std::uint32_t>(step.opcode));
                    AppendU32(out, step.authoredEventIndex);
                    EncodeActionRef(step.target, out);
                    EncodeActionTime(step.delay, out);
                    EncodeActionTime(step.fade, out);
                    EncodeActionRef(step.quantizeReference, out);
                    AppendU32(out, static_cast<std::uint32_t>(
                                       step.quantizeTiming));
                    AppendU32(out, static_cast<std::uint32_t>(
                                       step.cooldownScope));
                    AppendU32(out, static_cast<std::uint32_t>(step.ifSilent));
                    AppendU32(out, static_cast<std::uint32_t>(step.scope));
                    EncodeActionTime(step.cooldown, out);
                    AppendU32(out, step.valueBits);
                    AppendU32(out, step.reserved0);
                }
            }
        }
        return true;
    } catch (...) {
        out.clear();
        return false;
    }
}
bool EncodeCurrentWorldBodyV5(
    const HistoricalContainerWorldViewV1& world,
    std::vector<std::uint8_t>& out) noexcept {
    return EncodeCurrentWorldBodyImpl(world, out, false, false, false, false);
}

bool EncodeCurrentWorldBodyV6(
    const HistoricalContainerWorldViewV1& world,
    std::vector<std::uint8_t>& out) noexcept {
    return EncodeCurrentWorldBodyImpl(world, out, true, false, false, false);
}

bool EncodeCurrentWorldBodyV7(
    const HistoricalContainerWorldViewV1& world,
    std::vector<std::uint8_t>& out) noexcept {
    return EncodeCurrentWorldBodyImpl(world, out, true, true, false, false);
}

bool EncodeCurrentWorldBodyV8(
    const HistoricalContainerWorldViewV1& world,
    std::vector<std::uint8_t>& out) noexcept {
    return EncodeCurrentWorldBodyImpl(world, out, true, true, false, false);
}

bool EncodeCurrentWorldBodyV9(
    const ContainerWorldInstallViewV1& world,
    std::vector<std::uint8_t>& out) noexcept {
    return EncodeCurrentWorldBodyImpl(world, out, true, true, true, false);
}

bool EncodeCurrentWorldShippingBody(
    const ContainerWorldInstallViewV1& world,
    std::vector<std::uint8_t>& out) noexcept {
    if (world.sourceBodyVintage != kCurrentWorldShippingBodyVintage ||
        !ValidateCurrentWorldSectionGroupsV1(world)) {
        out.clear();
        return false;
    }
    return EncodeCurrentWorldBodyImpl(world, out, true, true, true, true);
}

bool EncodeCurrentWorldShippingBodyRawAttempt(
    const ContainerWorldInstallViewV1& world,
    std::vector<std::uint8_t>& out) noexcept {
    if (world.sourceBodyVintage != kCurrentWorldShippingBodyVintage) {
        out.clear();
        return false;
    }
    return EncodeCurrentWorldBodyImpl(world, out, true, true, true, true);
}

bool EncodeCurrentWorldBodyV19(
    const ContainerWorldInstallViewV1& world,
    std::vector<std::uint8_t>& out) noexcept {
    if (world.sourceBodyVintage != kCurrentWorldBodyVintageV19 ||
        !ValidateCurrentWorldSectionGroupsV1(world)) {
        out.clear();
        return false;
    }
    return EncodeCurrentWorldBodyImpl(world, out, true, true, true, true, true);
}

CurrentWorldBodyImage::CurrentWorldBodyImage() noexcept = default;

CurrentWorldBodyImage::~CurrentWorldBodyImage() = default;

CurrentWorldBodyDecodeResult CurrentWorldBodyImage::DecodeWithResult(
    const std::uint8_t* bytes, std::size_t byteCount,
    std::uint32_t worldTraceVintage, CurrentWorldDecodeMode mode) noexcept {
    const bool strictArtifact =
        mode == CurrentWorldDecodeMode::StrictArtifact;
    const bool replayRawAttempt =
        mode == CurrentWorldDecodeMode::ReplayRawAttempt;
    const bool replayCurrentAttempt =
        mode == CurrentWorldDecodeMode::ReplayCurrentAttempt;
    if (!strictArtifact && !replayRawAttempt && !replayCurrentAttempt) {
        return CurrentWorldBodyDecodeResult::Invalid;
    }
    if (!IsReadableCurrentWorldBodyVintage(worldTraceVintage) || replayRawAttempt ||
        (replayCurrentAttempt && worldTraceVintage != kCurrentWorldShippingBodyVintage)) {
        return CurrentWorldBodyDecodeResult::Invalid;
    }
    const bool allowRawInvalid = false;
    if (bytes == nullptr && byteCount != 0) {
        return CurrentWorldBodyDecodeResult::Invalid;
    }
    const bool currentShippingVintage =
        IsReadableCurrentWorldBodyVintage(worldTraceVintage);
    try {
        detail::LogicAllocCheckpoint(
            detail::LogicAllocSite::CurrentWorldDecodedImage);
        auto decoded = std::make_unique<Impl>();
        Reader reader{bytes, byteCount};
        auto& world = decoded->view;
        world.sourceBodyVintage = worldTraceVintage;
        std::uint32_t variableCount = 0;
        std::uint32_t propertyCount = 0;
        std::uint32_t occurrenceCount = 0;
        std::array<std::uint32_t, 7> expectedTotals{};
        std::uint32_t trackScheduleCount = 0;
        std::uint32_t segmentScheduleCount = 0;
        std::uint32_t expectedTrackRefTotal = 0;
        std::uint32_t expectedPlacementTotal = 0;
        std::uint32_t expectedMarkerTotal = 0;
        std::uint32_t expectedStructuralAncestorTotal = 0;
        std::uint32_t containerCount = 0;
        std::uint32_t actionCount = 0;
        if (worldTraceVintage >= 3) {
            CurrentWorldV3PrefixValues prefix{};
            PrefixDecodeVisitor prefixVisitor{reader};
            VisitCurrentWorldV3Prefix(prefix, prefixVisitor);
            world.identity.format.schema = prefix.schema;
            world.identity.format.compiler = prefix.compiler;
            world.identity.format.selectionAlgorithm =
                prefix.selectionAlgorithm;
            world.identity.format.gridAlgorithm = prefix.gridAlgorithm;
            world.identity.format.transitionAlgorithm =
                prefix.transitionAlgorithm;
            world.identity.format.publicationProtocol =
                prefix.publicationProtocol;
            world.identity.runtimeSemanticFingerprint =
                prefix.runtimeSemanticFingerprint;
            world.identity.capabilityMask = prefix.capabilityMask;
            variableCount = prefix.variableCount;
            propertyCount = prefix.playablePropertyProgramCount;
            occurrenceCount = prefix.playableOccurrencePropertyCount;
            expectedTotals = {
                prefix.playablePropertySendCount,
                prefix.reservedChildCount1,
                prefix.reservedChildCount2,
                prefix.playablePropertyLayerAncestorRefCount,
                prefix.playablePropertyEffectNodeCount,
                prefix.playablePropertyEffectParamCount,
                prefix.playablePropertyEffectEdgeCount};
            trackScheduleCount = prefix.trackScheduleCount;
            segmentScheduleCount = prefix.segmentScheduleCount;
            expectedTrackRefTotal = prefix.scheduleTrackRefCount;
            expectedPlacementTotal = prefix.schedulePlacementCount;
            expectedMarkerTotal = prefix.scheduleMarkerCount;
            containerCount = prefix.containerCount;
            world.transitionCount = prefix.transitionProgramCount;
            world.capacity.maxContainerDepth = prefix.maxContainerDepth;
            world.capacity.maxChildrenPerContainer =
                prefix.maxChildrenPerContainer;
            world.capacity.maxAvoidLastN = prefix.maxAvoidLastN;
            world.capacity.maxDiscreteAxes = prefix.maxDiscreteAxes;
            world.capacity.maxDiscreteRows = prefix.maxDiscreteRows;
            world.capacity.maxDiscreteTrieNodes =
                prefix.maxDiscreteTrieNodes;
            world.capacity.maxContinuousAxes = prefix.maxContinuousAxes;
            world.capacity.maxContinuousRows = prefix.maxContinuousRows;
            world.capacity.maxContinuousCandidates =
                prefix.maxContinuousCandidates;
            world.capacity.maxContinuousActiveRows =
                prefix.maxContinuousActiveRows;
            world.capacity.maxContinuousLiveRows =
                prefix.maxContinuousLiveRows;
            world.capacity.maxTransitionRules = prefix.maxTransitionRules;
            world.capacity.maxCurvePoints = prefix.maxCurvePoints;
            world.capacity.maxKnownScheduleLeaves =
                prefix.maxKnownScheduleLeaves;
            world.capacity.maxTrackSchedulePrograms =
                prefix.maxTrackSchedulePrograms;
            world.capacity.maxSegmentSchedulePrograms =
                prefix.maxSegmentSchedulePrograms;
            world.capacity.maxScheduleTrackRefsPerSegment =
                prefix.maxScheduleTrackRefsPerSegment;
            world.capacity.maxScheduleTrackRefsTotal =
                prefix.maxScheduleTrackRefsTotal;
            world.capacity.maxSchedulePlacementsPerTrack =
                prefix.maxSchedulePlacementsPerTrack;
            world.capacity.maxSchedulePlacementsTotal =
                prefix.maxSchedulePlacementsTotal;
            world.capacity.maxScheduleMarkersPerSegment =
                prefix.maxScheduleMarkersPerSegment;
            world.capacity.maxScheduleMarkersTotal =
                prefix.maxScheduleMarkersTotal;
            world.capacity.maxTrackSegmentScheduleDepth =
                prefix.maxTrackSegmentScheduleDepth;
            if (worldTraceVintage >= 6) {
                actionCount = reader.U32();
                world.capacity.maxActionDefinitions = reader.U32();
                world.capacity.maxActionStepsPerDefinition = reader.U32();
                world.capacity.maxActionStepsTotal = reader.U32();
                world.capacity.maxActionTriggerDepth = reader.U32();
                world.capacity.maxActionExpandedWork = reader.U32();
            }
            if (worldTraceVintage >= 7) {
                expectedStructuralAncestorTotal = reader.U32();
            }
            if (currentShippingVintage) {
                WorldCapacitySummaryV1 completeCapacity{};
                PrefixDecodeVisitor capacityVisitor{reader};
                VisitWorldCapacitySummaryV1(completeCapacity,
                                            capacityVisitor);
                if (reader.failed ||
                    !V9CapacityPrefixMatches(world.capacity,
                                             completeCapacity)) {
                    return CurrentWorldBodyDecodeResult::Invalid;
                }
                world.capacity = completeCapacity;
                world.identity.projectId = reader.ObjectId();
            }
        } else {
            world.identity.format.schema = reader.U32();
            world.identity.format.compiler = reader.U32();
            world.identity.format.selectionAlgorithm = reader.U32();
            world.identity.format.gridAlgorithm = reader.U32();
            world.identity.format.transitionAlgorithm = reader.U32();
            world.identity.format.publicationProtocol = reader.U32();
            world.identity.runtimeSemanticFingerprint = reader.U64();
            world.identity.capabilityMask = reader.U64();
            variableCount = reader.U32();
            propertyCount = reader.U32();
            occurrenceCount = reader.U32();
            expectedTotals = {
                reader.U32(), reader.U32(), reader.U32(), reader.U32(),
                reader.U32(), reader.U32(), reader.U32()};
            containerCount = reader.U32();
            world.transitionCount = reader.U32();
            world.capacity.maxContainerDepth = reader.U32();
            world.capacity.maxChildrenPerContainer = reader.U32();
            world.capacity.maxAvoidLastN = reader.U32();
            world.capacity.maxDiscreteAxes = reader.U32();
            world.capacity.maxDiscreteRows = reader.U32();
            world.capacity.maxDiscreteTrieNodes = reader.U32();
            world.capacity.maxContinuousAxes = reader.U32();
            world.capacity.maxContinuousRows = reader.U32();
            world.capacity.maxContinuousCandidates = reader.U32();
            world.capacity.maxContinuousActiveRows = reader.U32();
            world.capacity.maxContinuousLiveRows = reader.U32();
            world.capacity.maxTransitionRules = reader.U32();
            world.capacity.maxCurvePoints = reader.U32();
            world.capacity.maxKnownScheduleLeaves = reader.U32();
        }
        if (reader.failed || variableCount > kHardVariableDefinitionCapacity ||
            propertyCount > kHardPlayablePropertyProgramCapacity ||
            occurrenceCount > kHardPlayableOccurrencePropertyCapacity ||
            trackScheduleCount > kHardTrackScheduleProgramCapacity ||
            segmentScheduleCount > kHardSegmentScheduleProgramCapacity ||
            containerCount > kHardContainerProgramCapacity ||
            world.transitionCount > kTraceWorldChildCap ||
            actionCount > kHardActionDefinitionCapacity ||
            static_cast<std::uint64_t>(actionCount) *
                    kCurrentWorldActionHeaderWireBytes >
                reader.size - reader.offset ||
            expectedTotals[0] > kHardPlayablePropertyChildRowCapacity ||
            expectedTotals[1] > kHardPlayablePropertyChildRowCapacity ||
            expectedTotals[2] > kHardPlayablePropertyChildRowCapacity ||
            expectedTotals[3] > kHardPlayablePropertyChildRowCapacity ||
            expectedTotals[4] > kHardPlayablePropertyChildRowCapacity ||
            expectedTotals[5] > kHardPlayablePropertyEffectParamCapacity ||
            expectedTotals[6] > kHardPlayablePropertyEffectEdgeCapacity ||
            expectedStructuralAncestorTotal >
                kHardPlayablePropertyChildRowCapacity ||
            expectedTrackRefTotal > kHardScheduleTrackRefCapacity ||
            expectedPlacementTotal > kHardSchedulePlacementCapacity ||
            expectedMarkerTotal > kHardScheduleMarkerCapacity ||
            (worldTraceVintage >= 3 &&
             (world.capacity.maxTrackSchedulePrograms >
                  kHardTrackScheduleProgramCapacity ||
              world.capacity.maxSegmentSchedulePrograms >
                  kHardSegmentScheduleProgramCapacity ||
              world.capacity.maxScheduleTrackRefsPerSegment > 1024 ||
              world.capacity.maxScheduleTrackRefsTotal >
                  kHardScheduleTrackRefCapacity ||
              world.capacity.maxSchedulePlacementsPerTrack > 65536 ||
              world.capacity.maxSchedulePlacementsTotal >
                  kHardSchedulePlacementCapacity ||
              world.capacity.maxScheduleMarkersPerSegment > 65536 ||
              world.capacity.maxScheduleMarkersTotal >
                  kHardScheduleMarkerCapacity ||
              world.capacity.maxTrackSegmentScheduleDepth > 64))) {
            return CurrentWorldBodyDecodeResult::Invalid;
        }
        decoded->variables.resize(variableCount);
        for (VariableDefinitionV1& row : decoded->variables) {
            row.variableId = reader.ObjectId();
            row.kind = static_cast<VariableKindV1>(reader.U32());
            const std::uint32_t encodedScope = reader.U32();
            row.rangeMin = reader.F32();
            row.rangeMax = reader.F32();
            row.defaultValue = reader.F32();
            const std::uint32_t reserved2 = reader.U32();
            if (worldTraceVintage >= 6) {
                if (encodedScope != 1) reader.failed = true;
            } else if (encodedScope != 0) {
                reader.failed = true;
            }
            if (reserved2 != 0) reader.failed = true;
        }
        std::array<std::uint32_t, 6> actualPayloadTotals{};
        std::uint32_t actualAncestors = 0;
        std::uint32_t actualStructuralAncestors = 0;
        decoded->properties.resize(propertyCount);
        for (auto& owned : decoded->properties) {
            auto& row = owned.view;
            row.contentPropertyKind =
                static_cast<playableproperties::ContentPropertyKindV1>(
                    reader.U32());
            row.authoredStableOrder = reader.U32();
            row.stableId = reader.ObjectId();
            row.programSemanticFingerprint = reader.U64();
            row.contentRevision = reader.U64();
            row.routeStableId = reader.ObjectId();
            row.compatibilityFingerprint = reader.U64();
            const std::uint32_t ancestorCount = reader.U32();
            if (reader.U32() != 0 ||
                !AddCount(ancestorCount,
                          kHardPlayablePropertyChildRowCapacity,
                          actualAncestors)) {
                return CurrentWorldBodyDecodeResult::Invalid;
            }
            DecodePayloadHeader(reader, owned.payload.view,
                                worldTraceVintage);
            owned.ancestors.resize(ancestorCount);
            for (LayerAncestorRefV1& ancestor : owned.ancestors) {
                ancestor.layerStableId = reader.ObjectId();
                ancestor.layerProgramSemanticFingerprint = reader.U64();
            }
            if (worldTraceVintage >= 7) {
                const std::uint32_t structuralCount = reader.U32();
                if (reader.U32() != 0 ||
                    !AddCount(structuralCount,
                              kHardPlayablePropertyChildRowCapacity,
                              actualStructuralAncestors)) {
                    return CurrentWorldBodyDecodeResult::Invalid;
                }
                owned.structuralAncestors.resize(structuralCount);
                owned.structuralPayloads.resize(structuralCount);
                for (std::uint32_t a = 0; a < structuralCount; ++a) {
                    auto& ancestor = owned.structuralAncestors[a];
                    ancestor.contentPropertyKind =
                        static_cast<playableproperties::ContentPropertyKindV1>(
                            reader.U32());
                    ancestor.reserved = reader.U32();
                    ancestor.stableId = reader.ObjectId();
                    ancestor.programSemanticFingerprint = reader.U64();
                    DecodePayloadHeader(reader, owned.structuralPayloads[a].view,
                                        worldTraceVintage);
                    if (!DecodePayloadBody(reader, owned.structuralPayloads[a],
                                           allowRawInvalid)) {
                        return CurrentWorldBodyDecodeResult::Invalid;
                    }
                    owned.structuralPayloads[a].Refresh();
                    ancestor.payload = owned.structuralPayloads[a].view;
                }
                for (std::uint32_t a = 0; a < structuralCount; ++a) {
                    const auto& ancestor = owned.structuralAncestors[a];
                    if (!ValidStructuralAncestorKind(
                            ancestor.contentPropertyKind) ||
                        ancestor.reserved != 0 || ancestor.stableId == VnObjectId{} ||
                        ancestor.programSemanticFingerprint == 0 ||
                        (a != 0 &&
                         owned.structuralAncestors[a - 1].contentPropertyKind ==
                             ancestor.contentPropertyKind &&
                         owned.structuralAncestors[a - 1].stableId ==
                             ancestor.stableId)) {
                        return CurrentWorldBodyDecodeResult::Invalid;
                    }
                }
            }
            if (!DecodePayloadBody(reader, owned.payload, allowRawInvalid)) {
                return CurrentWorldBodyDecodeResult::Invalid;
            }
            owned.payload.Refresh();
            if (!CountPayload(owned.payload.view, actualPayloadTotals)) {
                return CurrentWorldBodyDecodeResult::Invalid;
            }
        }
        if (worldTraceVintage >= 7 &&
            actualStructuralAncestors != expectedStructuralAncestorTotal) {
            return CurrentWorldBodyDecodeResult::Invalid;
        }
        decoded->occurrences.resize(occurrenceCount);
        for (auto& owned : decoded->occurrences) {
            auto& row = owned.view;
            row.parentKind =
                static_cast<playableproperties::ContentPropertyKindV1>(
                    reader.U32());
            row.stableOccurrenceOrdinal = reader.U32();
            row.parentStableId = reader.ObjectId();
            row.parentProgramSemanticFingerprint = reader.U64();
            row.childPlayable = DecodePlayableRef(reader);
            row.occurrenceSemanticFingerprint = reader.U64();
            DecodePayloadHeader(reader, owned.payload.view,
                                worldTraceVintage);
            if (!DecodePayloadBody(reader, owned.payload, allowRawInvalid)) {
                return CurrentWorldBodyDecodeResult::Invalid;
            }
            owned.payload.Refresh();
            if (!CountPayload(owned.payload.view, actualPayloadTotals)) {
                return CurrentWorldBodyDecodeResult::Invalid;
            }
        }
        std::uint32_t actualPlacementTotal = 0;
        decoded->trackSchedules.resize(trackScheduleCount);
        for (auto& owned : decoded->trackSchedules) {
            auto& row = owned.view;
            row.rootDefinition = DecodePlayableRef(reader);
            row.scheduleSemanticFingerprint = reader.U64();
            const std::uint32_t placementCount = reader.U32();
            row.reserved = reader.U32();
            if (!AddCount(placementCount, kHardSchedulePlacementCapacity,
                          actualPlacementTotal)) {
                return CurrentWorldBodyDecodeResult::Invalid;
            }
            owned.placements.resize(placementCount);
            for (auto& placement : owned.placements) {
                placement.stablePlacementOrdinal = reader.U32();
                placement.stableOccurrenceOrdinal = reader.U32();
                placement.content = DecodePlayableRef(reader);
                placement.authoredStart =
                    std::bit_cast<std::int64_t>(reader.U64());
                placement.authoredDuration =
                    std::bit_cast<std::int64_t>(reader.U64());
                placement.authoredFadeIn =
                    std::bit_cast<std::int64_t>(reader.U64());
                placement.authoredFadeOut =
                    std::bit_cast<std::int64_t>(reader.U64());
                placement.clipId = reader.ObjectId();
                placement.clipGuidHigh = reader.U64();
                placement.clipGuidLow = reader.U64();
            }
        }
        std::uint32_t actualTrackRefTotal = 0;
        std::uint32_t actualMarkerTotal = 0;
        decoded->segmentSchedules.resize(segmentScheduleCount);
        for (auto& owned : decoded->segmentSchedules) {
            auto& row = owned.view;
            row.rootDefinition = DecodePlayableRef(reader);
            row.scheduleSemanticFingerprint = reader.U64();
            row.timeKind = static_cast<ScheduleTimeKindV1>(reader.U32());
            row.startPointKind = reader.U32();
            row.startMarkerId = reader.U32();
            row.reserved = reader.U32();
            row.microsPerQuarter =
                std::bit_cast<std::int64_t>(reader.U64());
            row.meterNumerator = reader.U32();
            row.meterDenominator = reader.U32();
            const std::uint32_t trackRefCount = reader.U32();
            const std::uint32_t markerCount = reader.U32();
            if (worldTraceVintage == kCurrentWorldBodyVintageV19) {
                row.entryMarker.stableMarkerId = reader.ObjectId();
                row.entryMarker.position = std::bit_cast<std::int64_t>(reader.U64());
                row.bounds.contentStart = std::bit_cast<std::int64_t>(reader.U64());
                row.bounds.contentEnd = std::bit_cast<std::int64_t>(reader.U64());
                row.bounds.nominalStart = std::bit_cast<std::int64_t>(reader.U64());
                row.bounds.nominalEnd = std::bit_cast<std::int64_t>(reader.U64());
            }
            if (!AddCount(trackRefCount, kHardScheduleTrackRefCapacity,
                          actualTrackRefTotal) ||
                !AddCount(markerCount, kHardScheduleMarkerCapacity,
                          actualMarkerTotal)) {
                return CurrentWorldBodyDecodeResult::Invalid;
            }
            owned.trackRefs.resize(trackRefCount);
            for (auto& ref : owned.trackRefs) {
                ref.trackDefinition = DecodePlayableRef(reader);
                ref.trackScheduleSemanticFingerprint = reader.U64();
                ref.stableTrackOrdinal = reader.U32();
                ref.reserved = reader.U32();
            }
            owned.markers.resize(markerCount);
            for (auto& marker : owned.markers) {
                marker.stableMarkerId = reader.ObjectId();
                marker.markerId = reader.U32();
                marker.markerKind = reader.U32();
                marker.position = std::bit_cast<std::int64_t>(reader.U64());
            }
        }
        if (currentShippingVintage) {
        decoded->currentContainers.resize(containerCount);
        for (auto& owned : decoded->currentContainers) {
            auto& row = owned.view;
            row.containerStableId = reader.ObjectId();
            row.programSemanticFingerprint = reader.U64();
            row.mode = static_cast<ContainerModeV1>(reader.U32());
            row.randomDiscipline =
                static_cast<RandomDisciplineV1>(reader.U32());
            row.memoryScope = static_cast<MemoryScopeV1>(reader.U32());
            row.retriggerPolicy =
                static_cast<RetriggerPolicyV1>(reader.U32());
            row.resetPolicy = static_cast<ResetPolicyV1>(reader.U32());
            row.sequenceEndPolicy =
                static_cast<SequenceEndPolicyV1>(reader.U32());
            row.queueCycle = static_cast<QueueCycleV1>(reader.U32());
            row.queueSplice = static_cast<QueueSpliceV1>(reader.U32());
            row.queueCycleCount = reader.U32();
            row.resumePosition = reader.U8();
            const std::uint8_t pad0 = reader.U8();
            const std::uint8_t pad1 = reader.U8();
            const std::uint8_t pad2 = reader.U8();
            if (pad0 != 0 || pad1 != 0 || pad2 != 0) {
                reader.failed = true;
            }
            row.avoidLastN = reader.U32();
            row.initialSequenceCursor = reader.U32();
            row.discreteTiming = reader.U32();
            row.continuousTiming = reader.U32();
            row.continuousNormalization = reader.U32();
            row.evaluationPeriodFrames = reader.U32();
            row.envelopeFrames = reader.U32();
            row.maxActiveRows = reader.U32();
            row.maxLiveRows = reader.U32();
            const std::uint32_t childCount = reader.U32();
            const std::uint32_t curveCount = reader.U32();
            const std::uint32_t discreteAxisCount = reader.U32();
            const std::uint32_t discreteRowCount = reader.U32();
            const std::uint32_t continuousAxisCount = reader.U32();
            const std::uint32_t continuousRowCount = reader.U32();
            row.transitionProgramIndex = reader.U32();
            row.authoredStableOrder = reader.U32();
            if (childCount > kTraceWorldChildCap ||
                curveCount > kTraceWorldChildCap ||
                discreteAxisCount > kTraceWorldChildCap ||
                discreteRowCount > kTraceWorldChildCap ||
                continuousAxisCount > kTraceWorldChildCap ||
                continuousRowCount > kTraceWorldChildCap) {
                return CurrentWorldBodyDecodeResult::Invalid;
            }
            owned.curves.resize(curveCount);
            for (CurveV1& curve : owned.curves) {
                if (!DecodeCurve(reader, curve, allowRawInvalid)) {
                    return CurrentWorldBodyDecodeResult::Invalid;
                }
            }
            owned.children.resize(childCount);
            for (std::size_t i = 0; i < owned.children.size(); ++i) {
                auto& child = owned.children[i];
                child.playable = DecodePlayableRef(reader);
                child.stableChildOrdinal = reader.U32();
                child.authoredStableOrder = reader.U32();
                child.frequencyWeight = reader.U32();
                child.orderWeight = reader.U32();
                child.queueStopPolicy =
                    static_cast<QueueStopPolicyV1>(reader.U32());
                child.reserved = reader.U32();
                child.timingHint.kind =
                    static_cast<ChildTimingHintKindV2>(reader.U32());
                child.timingHint.reserved = reader.U32();
                if (child.timingHint.kind ==
                    ChildTimingHintKindV2::InlineSound) {
                    auto& hint =
                        child.timingHint.payload.inlineSound;
                    hint.startOffset = DecodeTime(reader);
                    hint.duration = DecodeTime(reader);
                    hint.loopCount = reader.U32();
                    hint.loopRegionPresent = reader.U8();
                    hint.reserved[0] = reader.U8();
                    hint.reserved[1] = reader.U8();
                    hint.reserved[2] = reader.U8();
                    hint.loopStart = DecodeTime(reader);
                    hint.loopEnd = DecodeTime(reader);
                } else if (child.timingHint.kind ==
                           ChildTimingHintKindV2::ScheduleReference) {
                    auto& schedule =
                        child.timingHint.payload.schedule;
                    schedule.kind =
                        static_cast<PlayableKindV1>(reader.U32());
                    schedule.reserved = reader.U32();
                    schedule.stableId = reader.ObjectId();
                    schedule.scheduleSemanticFingerprint = reader.U64();
                } else if (child.timingHint.kind !=
                           ChildTimingHintKindV2::Unknown) {
                    return CurrentWorldBodyDecodeResult::Invalid;
                }
            }
            if (worldTraceVintage >= 3) {
                owned.discreteAxes.resize(discreteAxisCount);
                for (auto& axis : owned.discreteAxes) {
                    axis.stableAxisOrdinal = reader.U32();
                    axis.authoredColumnOrder = reader.U32();
                    axis.variableId = reader.ObjectId();
                }
                owned.discreteRows.resize(discreteRowCount);
                owned.discreteCells.resize(discreteRowCount);
                for (std::size_t i = 0; i < owned.discreteRows.size(); ++i) {
                    auto& gridRow = owned.discreteRows[i];
                    gridRow.stableRowOrdinal = reader.U32();
                    gridRow.authoredStableOrder = reader.U32();
                    gridRow.childStableOrdinal = reader.U32();
                    const std::uint32_t cellCount = reader.U32();
                    if (cellCount > kTraceWorldChildCap) {
                        return CurrentWorldBodyDecodeResult::Invalid;
                    }
                    owned.discreteCells[i].resize(cellCount);
                    for (auto& cell : owned.discreteCells[i]) {
                        cell.kind = static_cast<DiscreteCellKindV1>(
                            reader.U32());
                        cell.exactValue = std::bit_cast<std::int32_t>(
                            reader.U32());
                    }
                }
                owned.continuousAxes.resize(continuousAxisCount);
                for (auto& axis : owned.continuousAxes) {
                    axis.stableAxisOrdinal = reader.U32();
                    axis.authoredColumnOrder = reader.U32();
                    axis.variableId = reader.ObjectId();
                }
                owned.continuousRows.resize(continuousRowCount);
                owned.continuousCells.resize(continuousRowCount);
                for (std::size_t i = 0; i < owned.continuousRows.size(); ++i) {
                    auto& gridRow = owned.continuousRows[i];
                    gridRow.stableRowOrdinal = reader.U32();
                    gridRow.authoredStableOrder = reader.U32();
                    gridRow.childStableOrdinal = reader.U32();
                    const std::uint32_t cellCount = reader.U32();
                    if (cellCount > kTraceWorldChildCap) {
                        return CurrentWorldBodyDecodeResult::Invalid;
                    }
                    owned.continuousCells[i].resize(cellCount);
                    for (auto& cell : owned.continuousCells[i]) {
                        cell.kind = static_cast<ContinuousCellKindV1>(
                            reader.U32());
                        cell.lower = std::bit_cast<float>(reader.U32());
                        cell.upper = std::bit_cast<float>(reader.U32());
                        cell.lowerInclusive = reader.U8();
                        cell.upperInclusive = reader.U8();
                        cell.reserved = reader.U16();
                        cell.leftCurveLocalId = reader.U32();
                        cell.rightCurveLocalId = reader.U32();
                    }
                }
            } else if (discreteAxisCount != 0 || discreteRowCount != 0 ||
                       continuousAxisCount != 0 ||
                       continuousRowCount != 0) {
                return CurrentWorldBodyDecodeResult::Invalid;
            }
        }
        } else {
        decoded->containers.resize(containerCount);
        for (auto& owned : decoded->containers) {
            auto& row = owned.view;
            row.containerStableId = reader.ObjectId();
            row.programSemanticFingerprint = reader.U64();
            row.mode = static_cast<ContainerModeV1>(reader.U32());
            row.randomDiscipline =
                static_cast<RandomDisciplineV1>(reader.U32());
            row.memoryScope = static_cast<MemoryScopeV1>(reader.U32());
            row.retriggerPolicy =
                static_cast<RetriggerPolicyV1>(reader.U32());
            row.resetPolicy = static_cast<ResetPolicyV1>(reader.U32());
            row.sequenceEndPolicy =
                static_cast<SequenceEndPolicyV1>(reader.U32());
            row.queueCycle = static_cast<QueueCycleV1>(reader.U32());
            row.queueSplice = static_cast<QueueSpliceV1>(reader.U32());
            row.signalOnlyPolicy =
                static_cast<SignalOnlyPolicyV1>(reader.U32());
            row.resumePosition = reader.U8();
            row.reservedBool[0] = reader.U8();
            row.reservedBool[1] = reader.U8();
            row.reservedBool[2] = reader.U8();
            row.avoidLastN = reader.U32();
            row.initialSequenceCursor = reader.U32();
            row.discreteTiming = reader.U32();
            row.continuousNormalization = reader.U32();
            row.evaluationPeriodFrames = reader.U32();
            row.envelopeFrames = reader.U32();
            row.maxActiveRows = reader.U32();
            row.maxLiveRows = reader.U32();
            const std::uint32_t childCount = reader.U32();
            const std::uint32_t curveCount = reader.U32();
            const std::uint32_t discreteAxisCount = reader.U32();
            const std::uint32_t discreteRowCount = reader.U32();
            const std::uint32_t continuousAxisCount = reader.U32();
            const std::uint32_t continuousRowCount = reader.U32();
            row.transitionProgramIndex = reader.U32();
            row.authoredStableOrder = reader.U32();
            if (childCount > kTraceWorldChildCap ||
                curveCount > kTraceWorldChildCap ||
                discreteAxisCount > kTraceWorldChildCap ||
                discreteRowCount > kTraceWorldChildCap ||
                continuousAxisCount > kTraceWorldChildCap ||
                continuousRowCount > kTraceWorldChildCap) {
                return CurrentWorldBodyDecodeResult::Invalid;
            }
            owned.curves.resize(curveCount);
            for (CurveV1& curve : owned.curves) {
                if (!DecodeCurve(reader, curve, allowRawInvalid)) {
                    return CurrentWorldBodyDecodeResult::Invalid;
                }
            }
            owned.children.resize(childCount);
            owned.leaves.resize(childCount);
            owned.paths.resize(childCount);
            for (std::size_t i = 0; i < owned.children.size(); ++i) {
                auto& child = owned.children[i];
                child.playable = DecodePlayableRef(reader);
                child.stableChildOrdinal = reader.U32();
                child.authoredStableOrder = reader.U32();
                child.frequencyWeight = reader.U32();
                child.orderWeight = reader.U32();
                child.queueStopPolicy =
                    static_cast<QueueStopPolicyV1>(reader.U32());
                child.reserved = reader.U32();
                if (worldTraceVintage >= 3) {
                    child.completion.source =
                        static_cast<CompletionSourceV1>(reader.U32());
                    child.completion.reserved = reader.U32();
                    if (child.completion.source ==
                        CompletionSourceV1::InlineTemplate) {
                        if (!DecodeCompletion(reader, child, owned.leaves[i],
                                              owned.paths[i])) {
                            return CurrentWorldBodyDecodeResult::Invalid;
                        }
                    } else if (child.completion.source ==
                               CompletionSourceV1::ScheduleReference) {
                        auto& schedule = child.completion.payload.schedule;
                        schedule.kind =
                            static_cast<PlayableKindV1>(reader.U32());
                        schedule.reserved = reader.U32();
                        schedule.stableId = reader.ObjectId();
                        schedule.scheduleSemanticFingerprint = reader.U64();
                    } else {
                        return CurrentWorldBodyDecodeResult::Invalid;
                    }
                } else if (!DecodeCompletion(reader, child, owned.leaves[i],
                                             owned.paths[i])) {
                    return CurrentWorldBodyDecodeResult::Invalid;
                }
            }
            if (worldTraceVintage >= 3) {
                owned.discreteAxes.resize(discreteAxisCount);
                for (auto& axis : owned.discreteAxes) {
                    axis.stableAxisOrdinal = reader.U32();
                    axis.authoredColumnOrder = reader.U32();
                    axis.variableId = reader.ObjectId();
                }
                owned.discreteRows.resize(discreteRowCount);
                owned.discreteCells.resize(discreteRowCount);
                for (std::size_t i = 0; i < owned.discreteRows.size(); ++i) {
                    auto& gridRow = owned.discreteRows[i];
                    gridRow.stableRowOrdinal = reader.U32();
                    gridRow.authoredStableOrder = reader.U32();
                    gridRow.childStableOrdinal = reader.U32();
                    const std::uint32_t cellCount = reader.U32();
                    if (cellCount > kTraceWorldChildCap) {
                        return CurrentWorldBodyDecodeResult::Invalid;
                    }
                    owned.discreteCells[i].resize(cellCount);
                    for (auto& cell : owned.discreteCells[i]) {
                        cell.kind = static_cast<DiscreteCellKindV1>(
                            reader.U32());
                        cell.exactValue = std::bit_cast<std::int32_t>(
                            reader.U32());
                    }
                }
                owned.continuousAxes.resize(continuousAxisCount);
                for (auto& axis : owned.continuousAxes) {
                    axis.stableAxisOrdinal = reader.U32();
                    axis.authoredColumnOrder = reader.U32();
                    axis.variableId = reader.ObjectId();
                }
                owned.continuousRows.resize(continuousRowCount);
                owned.continuousCells.resize(continuousRowCount);
                for (std::size_t i = 0; i < owned.continuousRows.size(); ++i) {
                    auto& gridRow = owned.continuousRows[i];
                    gridRow.stableRowOrdinal = reader.U32();
                    gridRow.authoredStableOrder = reader.U32();
                    gridRow.childStableOrdinal = reader.U32();
                    const std::uint32_t cellCount = reader.U32();
                    if (cellCount > kTraceWorldChildCap) {
                        return CurrentWorldBodyDecodeResult::Invalid;
                    }
                    owned.continuousCells[i].resize(cellCount);
                    for (auto& cell : owned.continuousCells[i]) {
                        cell.kind = static_cast<ContinuousCellKindV1>(
                            reader.U32());
                        cell.lower = std::bit_cast<float>(reader.U32());
                        cell.upper = std::bit_cast<float>(reader.U32());
                        cell.lowerInclusive = reader.U8();
                        cell.upperInclusive = reader.U8();
                        cell.reserved = reader.U16();
                        cell.leftCurveLocalId = reader.U32();
                        cell.rightCurveLocalId = reader.U32();
                    }
                }
            } else if (discreteAxisCount != 0 || discreteRowCount != 0 ||
                       continuousAxisCount != 0 ||
                       continuousRowCount != 0) {
                return CurrentWorldBodyDecodeResult::Invalid;
            }
        }
        }
        if (worldTraceVintage >= 4) {
            std::uint32_t transitionRuleTotal = 0;
            std::uint32_t transitionPathTotal = 0;
            decoded->transitions.resize(world.transitionCount);
            for (auto& owned : decoded->transitions) {
                auto& row = owned.view;
                row.ownerContainerStableId = reader.ObjectId();
                const std::uint32_t ruleCount = reader.U32();
                row.defaultRuleIndex = reader.U32();
                const std::uint32_t pathCount = reader.U32();
                row.maxContainerDescent = reader.U32();
                row.maxOwnerAscent = reader.U32();
                row.watchFallback = reader.U32();
                if (ruleCount > kTraceWorldChildCap ||
                    pathCount > kTraceWorldChildCap ||
                    !AddCount(ruleCount, kTraceWorldChildCap,
                              transitionRuleTotal) ||
                    !AddCount(pathCount, kTraceWorldChildCap,
                              transitionPathTotal)) {
                    return CurrentWorldBodyDecodeResult::Invalid;
                }
                owned.downwardPathSteps.resize(pathCount);
                for (auto& step : owned.downwardPathSteps) {
                    step.playable = DecodePlayableRef(reader);
                    step.stableChildOrdinal = reader.U32();
                    step.reserved = reader.U32();
                    step.trackId = reader.ObjectId();
                    step.clipId = reader.ObjectId();
                    step.clipGuidHigh = reader.U64();
                    step.clipGuidLow = reader.U64();
                }
                owned.rules.resize(ruleCount);
                for (auto& rule : owned.rules) {
                    rule.stableRuleOrdinal = reader.U32();
                    rule.sourceAny = reader.U8();
                    rule.targetAny = reader.U8();
                    rule.bridgePresent = reader.U8();
                    rule.reserved = reader.U8();
                    rule.source = DecodePlayableRef(reader);
                    rule.target = DecodePlayableRef(reader);
                    rule.sourceExit = static_cast<TransitionSourceExitV1>(
                        reader.U32());
                    rule.targetEntry = static_cast<TransitionTargetEntryV1>(
                        reader.U32());
                    rule.bridge = DecodePlayableRef(reader);
                    TransitionFadeV1* fades[] = {
                        &rule.sourceFade, &rule.bridgeFadeIn,
                        &rule.bridgeFadeOut, &rule.targetFade};
                    for (auto* fade : fades) {
                        fade->active = reader.U8();
                        fade->reserved[0] = reader.U8();
                        fade->reserved[1] = reader.U8();
                        fade->reserved[2] = reader.U8();
                        fade->durationFrames = reader.U32();
                        fade->curveLocalId = reader.U32();
                    }
                    rule.requiredClockFingerprint = reader.U64();
                    rule.requiredMarkerStableId = reader.ObjectId();
                    rule.samePositionCompatibilityFingerprint = reader.U64();
                    rule.authoredStableOrder = reader.U32();
                    rule.reserved2 = reader.U32();
                }
            }
        } else {
            decoded->transitionSentinels.resize(world.transitionCount);
            decoded->useTransitionSentinel = world.transitionCount != 0;
        }
        if (worldTraceVintage >= 6) {
            decoded->actions.resize(actionCount);
            std::uint64_t actionStepTotal = 0;
            for (auto& owned : decoded->actions) {
                auto& action = owned.view;
                if (reader.failed ||
                    reader.size - reader.offset <
                        kCurrentWorldActionHeaderWireBytes) {
                    return CurrentWorldBodyDecodeResult::Invalid;
                }
                action.actionStableId = reader.ObjectId();
                action.programSemanticFingerprint = reader.U64();
                const std::uint32_t stepCount = reader.U32();
                action.directFeatureMask = reader.U32();
                action.transitiveFeatureMask = reader.U32();
                action.expandedStepOccurrenceCount = reader.U32();
                action.expandedFiringCount = reader.U32();
                actionStepTotal += stepCount;
                if (stepCount == 0 || stepCount > kHardActionStepCapacity ||
                    actionStepTotal > kHardActionStepCapacity ||
                    static_cast<std::uint64_t>(stepCount) *
                            kCurrentWorldActionStepWireBytes >
                        reader.size - reader.offset) {
                    return CurrentWorldBodyDecodeResult::Invalid;
                }
                owned.steps.resize(stepCount);
                for (auto& step : owned.steps) {
                    step.opcode = static_cast<ActionOpcodeV1>(reader.U32());
                    step.authoredEventIndex = reader.U32();
                    step.target = DecodeActionRef(reader);
                    step.delay = DecodeActionTime(reader);
                    step.fade = DecodeActionTime(reader);
                    step.quantizeReference = DecodeActionRef(reader);
                    step.quantizeTiming = static_cast<ActionQuantizeTimingV1>(
                        reader.U32());
                    step.cooldownScope = static_cast<ActionCooldownScopeV1>(
                        reader.U32());
                    step.ifSilent = static_cast<ActionIfSilentV1>(reader.U32());
                    step.scope = static_cast<ActionScopeV1>(reader.U32());
                    step.cooldown = DecodeActionTime(reader);
                    step.valueBits = reader.U32();
                    step.reserved0 = reader.U32();
                }
            }
        }
        const std::array<std::uint32_t, 7> actualTotals = {
            actualPayloadTotals[0], actualPayloadTotals[1],
            actualPayloadTotals[2], actualAncestors, actualPayloadTotals[3],
            actualPayloadTotals[4], actualPayloadTotals[5]};
        if (reader.failed || reader.offset != reader.size ||
            actualTrackRefTotal != expectedTrackRefTotal ||
            actualPlacementTotal != expectedPlacementTotal ||
            actualMarkerTotal != expectedMarkerTotal ||
            actualTotals != expectedTotals) {
            return CurrentWorldBodyDecodeResult::Invalid;
        }
        decoded->Refresh();
        if (strictArtifact &&
            !ValidateCurrentWorldSectionGroupsV1(decoded->view))
            return CurrentWorldBodyDecodeResult::Invalid;
        decoded->hasReplayView = !currentShippingVintage;
        impl_ = std::move(decoded);
        return CurrentWorldBodyDecodeResult::Ok;
    } catch (const std::bad_alloc&) {
        return CurrentWorldBodyDecodeResult::Capacity;
    } catch (...) {
        return CurrentWorldBodyDecodeResult::Invalid;
    }
}

const ContainerWorldInstallViewV1* CurrentWorldBodyImage::ProductionView() const noexcept {
    return impl_ != nullptr && !impl_->hasReplayView ? &impl_->view : nullptr;
}

const HistoricalContainerWorldViewV1* CurrentWorldBodyImage::ReplayView() const noexcept {
    return impl_ != nullptr && impl_->hasReplayView ? &impl_->historicalView : nullptr;
}

} // namespace harken
