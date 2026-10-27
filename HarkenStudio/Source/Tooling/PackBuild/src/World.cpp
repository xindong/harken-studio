#include "harken_studio_build/World.h"
#include "harken_studio_build/MediaContentRevision.h"
#include "harken_studio_build/WorldValidation.h"

#include "harken_studio_build/VariableDefinition.h"
#include "harken_studio_build/BindingTargets.h"
#include "harken_studio_build/EffectEncoding.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <numeric>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace harkenstudiobuild {
namespace {

enum class ValidationScope : std::uint8_t {
    ExactWorld = 0,
    PartitionLocal = 1,
};

bool IsPartitionLocal(ValidationScope scope) noexcept {
    return scope == ValidationScope::PartitionLocal;
}

class Fnv64 {
  public:
    explicit Fnv64(const char *domain) noexcept : hash_(domain, std::strlen(domain) + 1) {
    }

    void Byte(std::uint8_t value) noexcept {
        hash_.AddBytes(&value, sizeof(value));
    }
    void U32(std::uint32_t value) noexcept {
        hash_.AddU32(value);
    }
    void U16(std::uint16_t value) noexcept {
        Byte(static_cast<std::uint8_t>(value));
        Byte(static_cast<std::uint8_t>(value >> 8));
    }
    void U64(std::uint64_t value) noexcept {
        hash_.AddU64(value);
    }
    void ObjectId(harkenstudio::ObjectId value) noexcept {
        const auto bytes = harkenstudio::ObjectIdBytes(value);
        hash_.AddBytes(bytes.data(), bytes.size());
    }
    void I64(std::int64_t value) noexcept {
        U64(std::bit_cast<std::uint64_t>(value));
    }
    std::uint64_t Finish() const noexcept {
        return hash_.Finish();
    }

  private:
    MediaContentRevisionHasherV1 hash_;
};

bool ExactFormat(const WorldFormatIdentityV1 &value) noexcept {
    return value.schema == 1 && value.compiler == 1 && value.selectionAlgorithm == 1 &&
           value.gridAlgorithm == 1 && value.transitionAlgorithm == 1 &&
           value.publicationProtocol == 1;
}

bool ValidKind(playableproperties::ContentPropertyKindV1 kind) noexcept {
    return static_cast<std::uint32_t>(kind) <=
           static_cast<std::uint32_t>(playableproperties::ContentPropertyKindV1::Piece);
}

bool ValidStructuralAncestorKind(playableproperties::ContentPropertyKindV1 kind) noexcept {
    return kind == playableproperties::ContentPropertyKindV1::Layer ||
           kind == playableproperties::ContentPropertyKindV1::Container ||
           kind == playableproperties::ContentPropertyKindV1::Track ||
           kind == playableproperties::ContentPropertyKindV1::Piece;
}

bool ValidPlayableKind(PlayableKindV1 kind) noexcept {
    return static_cast<std::uint32_t>(kind) <= static_cast<std::uint32_t>(PlayableKindV1::Piece);
}

playableproperties::ContentPropertyKindV1 PropertyKind(PlayableKindV1 kind) {
    return static_cast<playableproperties::ContentPropertyKindV1>(static_cast<std::uint32_t>(kind) +
                                                                  1u);
}

bool IsPositiveZeroBits(std::uint64_t bits) noexcept {
    return bits == 0;
}

bool ValidFiniteF32(std::uint64_t bits) noexcept {
    if ((bits >> 32) != 0) {
        return false;
    }
    const float value = std::bit_cast<float>(static_cast<std::uint32_t>(bits));
    return std::isfinite(value) && (value != 0.0f || IsPositiveZeroBits(bits));
}

bool ValidCanonicalF32(float value) noexcept {
    return std::isfinite(value) && (value != 0.0f || !std::signbit(value));
}

bool PointerMatches(std::uint32_t count, const void *pointer) noexcept {
    return count == 0 || pointer != nullptr;
}

bool ValidVariableDefinition(const VariableDefinitionV1 &row) noexcept {
    if (row.variableId == harkenstudio::ObjectId{} ||
        (row.kind != VariableKindV1::Step && row.kind != VariableKindV1::Smooth) ||
        !ValidCanonicalF32(row.rangeMin) || !ValidCanonicalF32(row.rangeMax) ||
        !ValidCanonicalF32(row.defaultValue) || row.rangeMin > row.rangeMax ||
        row.defaultValue < row.rangeMin || row.defaultValue > row.rangeMax) {
        return false;
    }
    return row.kind != VariableKindV1::Step ||
           (row.defaultValue >= -16777216.0f && row.defaultValue <= 16777216.0f &&
            row.defaultValue == std::floor(row.defaultValue));
}

void HashPlayableRef(Fnv64 &hash, const PlayableRefV1 &ref) noexcept {
    hash.U32(static_cast<std::uint32_t>(ref.kind));
    hash.U32(ref.reserved);
    hash.ObjectId(ref.stableId);
    hash.U64(ref.expectedProgramSemanticFingerprint);
    hash.U64(ref.contentRevision);
    hash.ObjectId(ref.routeStableId);
    hash.U64(ref.compatibilityFingerprint);
}

void HashTransitionFade(Fnv64 &hash, const TransitionFadeV1 &fade) noexcept {
    hash.Byte(fade.active);
    for (const std::uint8_t value : fade.reserved)
        hash.Byte(value);
    hash.U32(fade.durationFrames);
    hash.U32(fade.curveLocalId);
}

void HashTransition(Fnv64 &hash, const TransitionProgramViewV1 &program) noexcept {
    hash.ObjectId(program.ownerContainerStableId);
    hash.U32(program.ruleCount);
    hash.U32(program.defaultRuleIndex);
    hash.U32(program.downwardPathStepCount);
    hash.U32(program.maxContainerDescent);
    hash.U32(program.maxOwnerAscent);
    hash.U32(program.watchFallback);
    for (std::uint32_t i = 0; i < program.downwardPathStepCount; ++i) {
        const auto &step = program.downwardPathSteps[i];
        HashPlayableRef(hash, step.playable);
        hash.U32(step.stableChildOrdinal);
        hash.U32(step.reserved);
        hash.ObjectId(step.trackId);
        hash.ObjectId(step.clipId);
        hash.U64(step.clipGuidHigh);
        hash.U64(step.clipGuidLow);
    }
    for (std::uint32_t i = 0; i < program.ruleCount; ++i) {
        const auto &rule = program.rules[i];
        hash.U32(rule.stableRuleOrdinal);
        hash.Byte(rule.sourceAny);
        hash.Byte(rule.targetAny);
        hash.Byte(rule.bridgePresent);
        hash.Byte(rule.reserved);
        HashPlayableRef(hash, rule.source);
        HashPlayableRef(hash, rule.target);
        hash.U32(static_cast<std::uint32_t>(rule.sourceExit));
        hash.U32(static_cast<std::uint32_t>(rule.targetEntry));
        HashPlayableRef(hash, rule.bridge);
        HashTransitionFade(hash, rule.sourceFade);
        HashTransitionFade(hash, rule.bridgeFadeIn);
        HashTransitionFade(hash, rule.bridgeFadeOut);
        HashTransitionFade(hash, rule.targetFade);
        hash.U64(rule.requiredClockFingerprint);
        hash.ObjectId(rule.requiredMarkerStableId);
        hash.U64(rule.samePositionCompatibilityFingerprint);
        hash.U32(rule.authoredStableOrder);
        hash.U32(rule.reserved2);
    }
}

void HashActionRef(Fnv64 &hash, const ActionObjectRefV1 &ref) noexcept {
    hash.U32(static_cast<std::uint32_t>(ref.kind));
    hash.U32(ref.reserved);
    hash.ObjectId(ref.stableId);
    hash.U64(ref.expectedProgramSemanticFingerprint);
}

void HashActionTime(Fnv64 &hash, const AuthoredTimeRationalV1 &time) noexcept {
    hash.U32(static_cast<std::uint32_t>(time.domain));
    hash.I64(time.numerator);
    hash.U32(time.denominator);
}

void HashActionProgram(Fnv64 &hash, const ActionProgramViewV1 &program,
                       bool zeroSelfFingerprint) noexcept {
    hash.ObjectId(program.actionStableId);
    hash.U64(zeroSelfFingerprint ? 0 : program.programSemanticFingerprint);
    hash.U32(program.stepCount);
    hash.U32(program.directFeatureMask);
    hash.U32(program.transitiveFeatureMask);
    hash.U32(program.expandedStepOccurrenceCount);
    hash.U32(program.expandedFiringCount);
    for (std::uint32_t i = 0; i < program.stepCount; ++i) {
        const auto &step = program.steps[i];
        hash.U32(static_cast<std::uint32_t>(step.opcode));
        hash.U32(step.authoredEventIndex);
        HashActionRef(hash, step.target);
        HashActionTime(hash, step.delay);
        HashActionTime(hash, step.fade);
        HashActionRef(hash, step.quantizeReference);
        hash.U32(static_cast<std::uint32_t>(step.quantizeTiming));
        hash.U32(static_cast<std::uint32_t>(step.cooldownScope));
        hash.U32(static_cast<std::uint32_t>(step.ifSilent));
        hash.U32(static_cast<std::uint32_t>(step.scope));
        HashActionTime(hash, step.cooldown);
        hash.U32(step.valueBits);
        hash.U32(step.reserved0);
    }
}

void HashCurve(Fnv64 &hash, const CurveV1 &row) noexcept {
    hash.U32(row.curveLocalId);
    hash.U32(row.curve.pointCount);
    for (std::uint32_t i = 0; i < row.curve.pointCount; ++i) {
        hash.U32(std::bit_cast<std::uint32_t>(row.curve.points[i].x));
        hash.U32(std::bit_cast<std::uint32_t>(row.curve.points[i].y));
    }
    for (std::uint32_t i = 0; i + 1 < row.curve.pointCount; ++i) {
        const CurveHandlePair &handles = row.curve.handles[i];
        hash.U32(std::bit_cast<std::uint32_t>(handles.h1x));
        hash.U32(std::bit_cast<std::uint32_t>(handles.h1y));
        hash.U32(std::bit_cast<std::uint32_t>(handles.h2x));
        hash.U32(std::bit_cast<std::uint32_t>(handles.h2y));
    }
}

void HashPayloadHeader(Fnv64 &hash, const PlayablePropertyPayloadViewV1 &payload) noexcept {
    hash.U64(payload.presentMask);
    hash.U32(static_cast<std::uint32_t>(payload.values[0].bits));
    hash.U32(static_cast<std::uint32_t>(payload.values[1].bits));
    hash.U32(static_cast<std::uint32_t>(payload.values[2].bits));
    hash.U32(static_cast<std::uint32_t>(payload.values[5].bits));
    hash.U32(static_cast<std::uint32_t>(payload.values[4].bits));
    hash.U32(static_cast<std::uint32_t>(payload.values[3].bits));
    hash.Byte(static_cast<std::uint8_t>(payload.values[10].bits));
    hash.U16(0);
    hash.U32(static_cast<std::uint32_t>(payload.values[8].bits));
    hash.U32(static_cast<std::uint32_t>(payload.values[9].bits));
    hash.U32(static_cast<std::uint32_t>(payload.values[11].bits));
    hash.U32(static_cast<std::uint32_t>(payload.values[12].bits));
    hash.ObjectId(payload.values[6].objectId);
    hash.ObjectId(payload.values[13].objectId);
    hash.U64(payload.values[14].bits);
    hash.U32(static_cast<std::uint32_t>(payload.values[16].bits));
    hash.U32(payload.sendCount);
    hash.U32(0);
    hash.U32(0);
    hash.U32(payload.effectNodeCount);
    hash.U32(payload.effectEdgeCount);
}

void HashPayloadBody(Fnv64 &hash, const PlayablePropertyPayloadViewV1 &payload) noexcept {
    for (std::uint32_t i = 0; i < payload.sendCount; ++i) {
        const SendV1 &row = payload.sends[i];
        hash.U32(row.stableSendOrdinal);
        hash.U32(row.reserved);
        hash.ObjectId(row.targetBusId);
        hash.U32(std::bit_cast<std::uint32_t>(row.sendLevel));
        hash.U32(row.reserved2);
    }
    for (std::uint32_t i = 0; i < payload.effectNodeCount; ++i) {
        const EffectNodeV1 &node = payload.effectNodes[i];
        hash.U32(node.nodeId);
        hash.ObjectId(node.effectId);
        hash.U32(node.effectType);
        hash.U32(node.paramCount);
        hash.U32(0);
        for (std::uint32_t p = 0; p < node.paramCount; ++p) {
            hash.U32(node.params[p].paramId);
            hash.U32(node.params[p].valueBits);
        }
    }
    for (std::uint32_t i = 0; i < payload.effectEdgeCount; ++i) {
        hash.U32(payload.effectEdges[i].fromNodeId);
        hash.U32(payload.effectEdges[i].toNodeId);
    }
}

bool ValidCurve(const CurveV1 &curve) noexcept {
    if (curve.curve.pointCount == 0 || curve.curve.pointCount > kMaxCurvePoints) {
        return false;
    }
    for (std::uint32_t i = 0; i < curve.curve.pointCount; ++i) {
        if (!ValidCanonicalF32(curve.curve.points[i].x) ||
            !ValidCanonicalF32(curve.curve.points[i].y) ||
            (i != 0 && curve.curve.points[i - 1].x >= curve.curve.points[i].x)) {
            return false;
        }
        if (i + 1 < curve.curve.pointCount) {
            const auto &handles = curve.curve.handles[i];
            const float x0 = curve.curve.points[i].x;
            const float x1 = curve.curve.points[i + 1].x;
            if (!ValidCanonicalF32(handles.h1x) || !ValidCanonicalF32(handles.h1y) ||
                !ValidCanonicalF32(handles.h2x) || !ValidCanonicalF32(handles.h2y) ||
                handles.h1x < x0 || handles.h1x > x1 || handles.h2x < x0 || handles.h2x > x1) {
                return false;
            }
        }
    }
    return true;
}

bool ValidContinuousWeightCurve(const CurveV1 &curve) noexcept {
    if (curve.curve.pointCount < 2 || curve.curve.pointCount > kMaxCurvePoints) {
        return false;
    }
    for (std::uint32_t i = 0; i < curve.curve.pointCount; ++i) {
        if (!(curve.curve.points[i].y >= 0.0f))
            return false;
        if (i + 1 < curve.curve.pointCount &&
            (!(curve.curve.handles[i].h1y >= 0.0f) || !(curve.curve.handles[i].h2y >= 0.0f))) {
            return false;
        }
    }
    return true;
}

bool ValidPayload(const PlayablePropertyPayloadViewV1 &payload) noexcept {
    using playableproperties::PlayablePropertyDefaultV1;
    using playableproperties::PlayablePropertyWireV1;
    if ((payload.presentMask & ~playableproperties::kPlayablePropertyKnownMaskV1) != 0 ||
        !PointerMatches(payload.sendCount, payload.sends) || payload.values[12].bits != 0 ||
        payload.values[14].bits != 0 ||
        !PointerMatches(payload.effectNodeCount, payload.effectNodes) ||
        !PointerMatches(payload.effectEdgeCount, payload.effectEdges)) {
        return false;
    }

    for (std::size_t i = 0; i < std::size(playableproperties::kRuntimeAndVtrcPropertyTableV1);
         ++i) {
        const auto &meta = playableproperties::kRuntimeAndVtrcPropertyTableV1[i];
        const bool present = (payload.presentMask & meta.presentBit) != 0;
        const auto &value = payload.values[static_cast<std::uint8_t>(meta.id) - 1];
        const std::uint64_t bits = value.bits;
        if (meta.wire == PlayablePropertyWireV1::ObjectId) {
            if (bits != 0 || (!present && value.objectId != harkenstudio::ObjectId{})) {
                return false;
            }
            continue;
        }
        if (value.objectId != harkenstudio::ObjectId{}) {
            return false;
        }
        if (!present) {
            const std::uint64_t expected =
                meta.canonicalDefault == PlayablePropertyDefaultV1::One
                    ? std::bit_cast<std::uint32_t>(1.0f)
                : meta.canonicalDefault == PlayablePropertyDefaultV1::IntegerOne ? 1
                                                                                 : 0;
            if (bits != expected) {
                return false;
            }
            if ((meta.wire == PlayablePropertyWireV1::RowSpan && payload.sendCount != 0) ||
                (meta.wire == PlayablePropertyWireV1::EffectProgram &&
                 (payload.effectNodeCount != 0 || payload.effectEdgeCount != 0))) {
                return false;
            }
            continue;
        }
        if (meta.wire == PlayablePropertyWireV1::F32 && !ValidFiniteF32(bits)) {
            return false;
        }
        if (meta.wire == PlayablePropertyWireV1::U8 && bits > 1) {
            return false;
        }
        if ((meta.wire == PlayablePropertyWireV1::U32 && bits > UINT32_MAX) ||
            ((meta.wire == PlayablePropertyWireV1::RowSpan ||
              meta.wire == PlayablePropertyWireV1::EffectProgram) &&
             bits != 0)) {
            return false;
        }
    }
    if ((payload.presentMask & (UINT64_C(1) << 4)) != 0 && payload.values[4].bits > 1) {
        return false;
    }
    if ((payload.presentMask & (UINT64_C(1) << 16)) != 0) {
        const std::uint64_t bits = payload.values[16].bits;
        const float value = std::bit_cast<float>(static_cast<std::uint32_t>(bits));
        if (!ValidFiniteF32(bits) || value < 0.0f || value > 1.0f) {
            return false;
        }
    }

    for (std::uint32_t i = 0; i < payload.sendCount; ++i) {
        const SendV1 &row = payload.sends[i];
        if (row.targetBusId == harkenstudio::ObjectId{} || row.reserved != 0 || row.reserved2 != 0 ||
            !std::isfinite(row.sendLevel) ||
            (row.sendLevel == 0.0f && std::signbit(row.sendLevel)) ||
            (i != 0 && payload.sends[i - 1].stableSendOrdinal >= row.stableSendOrdinal)) {
            return false;
        }
    }
    for (std::uint32_t i = 0; i < payload.effectNodeCount; ++i) {
        const EffectNodeV1 &node = payload.effectNodes[i];
        const PackEffectSchemaRow *effect = FindPackEffectSchemaByType(node.effectType);
        if (node.nodeId == 0 || effect == nullptr ||
            !PointerMatches(node.paramCount, node.params) || node.paramCount > effect->paramCount ||
            (i != 0 && payload.effectNodes[i - 1].nodeId >= node.nodeId)) {
            return false;
        }
        for (std::uint32_t p = 0; p < node.paramCount; ++p) {
            const float value = std::bit_cast<float>(node.params[p].valueBits);
            if (node.params[p].paramId != p || !std::isfinite(value) ||
                (value == 0.0f && std::signbit(value))) {
                return false;
            }
        }
    }
    for (std::uint32_t i = 0; i < payload.effectEdgeCount; ++i) {
        const EffectEdgeV1 &edge = payload.effectEdges[i];
        const auto nodeExists = [&](std::uint32_t id) {
            return std::find_if(payload.effectNodes, payload.effectNodes + payload.effectNodeCount,
                                [&](const EffectNodeV1 &node) { return node.nodeId == id; }) !=
                   payload.effectNodes + payload.effectNodeCount;
        };
        if (payload.effectNodeCount == 0 || !nodeExists(edge.fromNodeId) ||
            !nodeExists(edge.toNodeId) || edge.fromNodeId == edge.toNodeId ||
            (i != 0 && std::pair(payload.effectEdges[i - 1].fromNodeId,
                                 payload.effectEdges[i - 1].toNodeId) >=
                           std::pair(edge.fromNodeId, edge.toNodeId))) {
            return false;
        }
    }
    std::vector<std::uint32_t> indegree(payload.effectNodeCount, 0);
    for (std::uint32_t i = 0; i < payload.effectEdgeCount; ++i) {
        const auto *destination = std::lower_bound(
            payload.effectNodes, payload.effectNodes + payload.effectNodeCount,
            payload.effectEdges[i].toNodeId,
            [](const EffectNodeV1 &node, std::uint32_t id) { return node.nodeId < id; });
        ++indegree[static_cast<std::size_t>(destination - payload.effectNodes)];
    }
    std::vector<std::uint32_t> ready;
    ready.reserve(payload.effectNodeCount);
    for (std::uint32_t i = 0; i < payload.effectNodeCount; ++i) {
        if (indegree[i] == 0)
            ready.push_back(i);
    }
    for (std::size_t cursor = 0; cursor < ready.size(); ++cursor) {
        const std::uint32_t n = ready[cursor];
        if (payload.effectEdgeCount == 0)
            continue;
        const auto *edge = std::lower_bound(
            payload.effectEdges, payload.effectEdges + payload.effectEdgeCount,
            std::pair(payload.effectNodes[n].nodeId, 0u),
            [](const EffectEdgeV1 &candidate, const std::pair<std::uint32_t, std::uint32_t> &key) {
                return std::pair(candidate.fromNodeId, candidate.toNodeId) < key;
            });
        while (edge != payload.effectEdges + payload.effectEdgeCount &&
               edge->fromNodeId == payload.effectNodes[n].nodeId) {
            const auto *destination = std::lower_bound(
                payload.effectNodes, payload.effectNodes + payload.effectNodeCount, edge->toNodeId,
                [](const EffectNodeV1 &node, std::uint32_t id) { return node.nodeId < id; });
            const std::uint32_t destinationIndex =
                static_cast<std::uint32_t>(destination - payload.effectNodes);
            if (--indegree[destinationIndex] == 0) {
                ready.push_back(destinationIndex);
            }
            ++edge;
        }
    }
    if (ready.size() != payload.effectNodeCount)
        return false;
    return true;
}

bool ValidPlayableRef(const PlayableRefV1 &ref) noexcept {
    return ValidPlayableKind(ref.kind) && ref.reserved == 0 && ref.stableId != harkenstudio::ObjectId{} &&
           ref.expectedProgramSemanticFingerprint != 0 && ref.contentRevision != 0;
}

bool ValidTime(const AuthoredTimeRationalV1 &value, bool allowInactive) noexcept {
    const auto domain = static_cast<std::uint32_t>(value.domain);
    if (value.denominator == 0) {
        return allowInactive && domain == 0 && value.numerator == 0;
    }
    if (domain > static_cast<std::uint32_t>(AuthoredTimeDomainV1::NaturalMediaEnd)) {
        return false;
    }
    const std::uint64_t magnitude = value.numerator < 0
                                        ? static_cast<std::uint64_t>(-(value.numerator + 1)) + 1
                                        : static_cast<std::uint64_t>(value.numerator);
    return std::gcd(magnitude, static_cast<std::uint64_t>(value.denominator)) == 1;
}

void HashTime(Fnv64 &hash, const AuthoredTimeRationalV1 &value) noexcept {
    hash.U32(static_cast<std::uint32_t>(value.domain));
    hash.I64(value.numerator);
    hash.U32(value.denominator);
}

bool IsInactiveInlineSoundTimingHint(const InlineSoundTimingHintV2 &hint) noexcept {
    return hint.startOffset.domain == AuthoredTimeDomainV1::SourceFrames &&
           hint.startOffset.numerator == 0 && hint.startOffset.denominator == 0 &&
           hint.duration.domain == AuthoredTimeDomainV1::SourceFrames &&
           hint.duration.numerator == 0 && hint.duration.denominator == 0 && hint.loopCount == 0 &&
           hint.loopRegionPresent == 0 && hint.reserved == std::array<std::uint8_t, 3>{} &&
           hint.loopStart.domain == AuthoredTimeDomainV1::SourceFrames &&
           hint.loopStart.numerator == 0 && hint.loopStart.denominator == 0 &&
           hint.loopEnd.domain == AuthoredTimeDomainV1::SourceFrames &&
           hint.loopEnd.numerator == 0 && hint.loopEnd.denominator == 0;
}

bool ValidInlineSoundTimingHint(const ContainerChildProgramViewV2 &child) noexcept {
    const auto &hint = child.timingHint.payload.inlineSound;
    if (child.playable.kind != PlayableKindV1::Sound || child.timingHint.reserved != 0 ||
        hint.reserved != std::array<std::uint8_t, 3>{} || !ValidTime(hint.startOffset, false) ||
        !ValidTime(hint.duration, false) || hint.loopRegionPresent > 1) {
        return false;
    }
    if (hint.loopRegionPresent == 0) {
        return ValidTime(hint.loopStart, true) && hint.loopStart.denominator == 0 &&
               ValidTime(hint.loopEnd, true) && hint.loopEnd.denominator == 0;
    }
    return ValidTime(hint.loopStart, false) && ValidTime(hint.loopEnd, false);
}

bool ValidChildTimingHintShape(const ContainerChildProgramViewV2 &child,
                               ContainerModeV1 mode) noexcept {
    const auto &hint = child.timingHint;
    if (hint.reserved != 0 ||
        static_cast<std::uint32_t>(hint.kind) >
            static_cast<std::uint32_t>(ChildTimingHintKindV2::ScheduleReference)) {
        return false;
    }
    const bool queue =
        mode == ContainerModeV1::RandomQueue || mode == ContainerModeV1::SequenceQueue;
    if (!queue || child.playable.kind == PlayableKindV1::Container) {
        return hint.kind == ChildTimingHintKindV2::Unknown &&
               IsInactiveInlineSoundTimingHint(hint.payload.inlineSound);
    }
    if (child.playable.kind == PlayableKindV1::Track ||
        child.playable.kind == PlayableKindV1::Piece) {
        const auto &schedule = hint.payload.schedule;
        return hint.kind == ChildTimingHintKindV2::ScheduleReference && schedule.reserved == 0 &&
               schedule.kind == child.playable.kind &&
               schedule.stableId == child.playable.stableId &&
               schedule.scheduleSemanticFingerprint != 0;
    }
    return child.playable.kind == PlayableKindV1::Sound &&
           (hint.kind == ChildTimingHintKindV2::Unknown
                ? IsInactiveInlineSoundTimingHint(hint.payload.inlineSound)
                : hint.kind == ChildTimingHintKindV2::InlineSound &&
                      ValidInlineSoundTimingHint(child));
}

bool CanonicalInactiveCurrentContainer(const ContainerProgramViewV3 &row) noexcept {
    return row.queueCycle == QueueCycleV1::Once && row.queueCycleCount == 0 &&
           row.queueSplice == QueueSpliceV1::Butt && row.resumePosition == 0 &&
           row.discreteTiming == 0 && row.continuousTiming == 1 && row.continuousNormalization == 0 &&
           row.evaluationPeriodFrames == 0 && row.envelopeFrames == 0 && row.maxActiveRows == 0 &&
           row.maxLiveRows == 0 && row.curveCount == 0 && row.discreteAxisCount == 0 &&
           row.discreteRowCount == 0 && row.continuousAxisCount == 0 &&
           row.continuousRowCount == 0 && row.transitionProgramIndex == UINT32_MAX;
}

bool ValidCurrentContainer(const ContainerProgramViewV3 &row) noexcept {
    if (row.containerStableId == harkenstudio::ObjectId{} || row.programSemanticFingerprint == 0 ||
        !PointerMatches(row.childCount, row.children) ||
        !PointerMatches(row.curveCount, row.curves) ||
        !PointerMatches(row.discreteAxisCount, row.discreteAxes) ||
        !PointerMatches(row.discreteRowCount, row.discreteRows) ||
        !PointerMatches(row.continuousAxisCount, row.continuousAxes) ||
        !PointerMatches(row.continuousRowCount, row.continuousRows) ||
        row.retriggerPolicy != RetriggerPolicyV1::CreateNewInstance ||
        row.resetPolicy != ResetPolicyV1::ExplicitTypedResetOnly) {
        return false;
    }
    const std::uint32_t mode = static_cast<std::uint32_t>(row.mode);
    if (mode > static_cast<std::uint32_t>(ContainerModeV1::VariableControl) ||
        static_cast<std::uint32_t>(row.randomDiscipline) > 1 ||
        static_cast<std::uint32_t>(row.memoryScope) > 1 ||
        static_cast<std::uint32_t>(row.retriggerPolicy) > 1 ||
        static_cast<std::uint32_t>(row.sequenceEndPolicy) > 1 ||
        static_cast<std::uint32_t>(row.queueCycle) > 1 ||
        static_cast<std::uint32_t>(row.queueSplice) > 1) {
        return false;
    }
    if (row.childCount == 0) {
        return row.mode == ContainerModeV1::Random &&
               row.randomDiscipline == RandomDisciplineV1::Draw &&
               CanonicalInactiveCurrentContainer(row) && row.avoidLastN == 0 &&
               row.sequenceEndPolicy == SequenceEndPolicyV1::Wrap && row.initialSequenceCursor == 0;
    }
    for (std::uint32_t i = 0; i < row.childCount; ++i) {
        const ContainerChildProgramViewV2 &child = row.children[i];
        if (!ValidPlayableRef(child.playable) || child.reserved != 0 ||
            static_cast<std::uint32_t>(child.queueStopPolicy) > 1 ||
            !ValidChildTimingHintShape(child, row.mode) ||
            ((row.mode == ContainerModeV1::RandomQueue ||
              row.mode == ContainerModeV1::SequenceQueue) &&
             row.queueSplice == QueueSpliceV1::MarkerAligned &&
             child.timingHint.kind == ChildTimingHintKindV2::Unknown) ||
            (i != 0 && row.children[i - 1].stableChildOrdinal >= child.stableChildOrdinal)) {
            return false;
        }
    }
    const bool queueMode =
        row.mode == ContainerModeV1::RandomQueue || row.mode == ContainerModeV1::SequenceQueue;
    if ((!queueMode && row.queueCycleCount != 0) ||
        (queueMode && row.queueCycle == QueueCycleV1::Once && row.queueCycleCount != 0) ||
        (queueMode && row.queueCycle == QueueCycleV1::Loop && row.queueCycleCount == 1)) {
        return false;
    }
    if (row.mode == ContainerModeV1::Random) {
        if (!CanonicalInactiveCurrentContainer(row) ||
            row.sequenceEndPolicy != SequenceEndPolicyV1::Wrap || row.initialSequenceCursor != 0) {
            return false;
        }
        std::uint32_t positiveWeightChildCount = 0;
        for (std::uint32_t i = 0; i < row.childCount; ++i) {
            if (row.children[i].queueStopPolicy != QueueStopPolicyV1::AbortOwningQueue ||
                row.children[i].timingHint.kind != ChildTimingHintKindV2::Unknown) {
                return false;
            }
            if (row.randomDiscipline == RandomDisciplineV1::Draw) {
                if (row.children[i].orderWeight != 0) {
                    return false;
                }
                positiveWeightChildCount += row.children[i].frequencyWeight != 0 ? 1u : 0u;
            } else if (row.randomDiscipline == RandomDisciplineV1::Rotation) {
                if (row.children[i].frequencyWeight != 0 || row.children[i].orderWeight == 0) {
                    return false;
                }
            } else {
                return false;
            }
        }
        return row.randomDiscipline == RandomDisciplineV1::Draw
                   ? positiveWeightChildCount != 0 && row.avoidLastN < positiveWeightChildCount
                   : row.avoidLastN < row.childCount;
    }
    if (row.mode == ContainerModeV1::Sequence) {
        if (!CanonicalInactiveCurrentContainer(row) ||
            row.randomDiscipline != RandomDisciplineV1::Draw || row.avoidLastN != 0 ||
            row.initialSequenceCursor >= row.childCount) {
            return false;
        }
        for (std::uint32_t i = 0; i < row.childCount; ++i) {
            if (row.children[i].queueStopPolicy != QueueStopPolicyV1::AbortOwningQueue ||
                row.children[i].timingHint.kind != ChildTimingHintKindV2::Unknown) {
                return false;
            }
            if (row.children[i].frequencyWeight != 0 || row.children[i].orderWeight != 0) {
                return false;
            }
        }
        return true;
    }
    if (row.mode == ContainerModeV1::RandomQueue) {
        if (row.sequenceEndPolicy != SequenceEndPolicyV1::Wrap || row.initialSequenceCursor != 0 ||
            row.memoryScope != MemoryScopeV1::Emitter || row.discreteAxisCount != 0 ||
            row.discreteRowCount != 0 || row.continuousAxisCount != 0 ||
            row.continuousRowCount != 0) {
            return false;
        }
        for (std::uint32_t i = 0; i < row.childCount; ++i) {
            if (row.children[i].frequencyWeight != 0 || row.children[i].orderWeight == 0) {
                return false;
            }
        }
        return true;
    }
    if (row.mode == ContainerModeV1::SequenceQueue) {
        if (row.randomDiscipline != RandomDisciplineV1::Draw || row.avoidLastN != 0 ||
            (row.memoryScope != MemoryScopeV1::Emitter &&
             row.memoryScope != MemoryScopeV1::Global) ||
            row.discreteAxisCount != 0 || row.discreteRowCount != 0 ||
            row.continuousAxisCount != 0 || row.continuousRowCount != 0) {
            return false;
        }
        for (std::uint32_t i = 0; i < row.childCount; ++i) {
            if (row.children[i].frequencyWeight != 0 || row.children[i].orderWeight != 0) {
                return false;
            }
        }
        return true;
    }
    if (row.randomDiscipline != RandomDisciplineV1::Draw ||
        row.memoryScope != MemoryScopeV1::Emitter || row.avoidLastN != 0 ||
        row.sequenceEndPolicy != SequenceEndPolicyV1::Wrap || row.initialSequenceCursor != 0 ||
        row.queueCycle != QueueCycleV1::Once || row.queueSplice != QueueSpliceV1::Butt ||
        row.resumePosition != 0 || (row.discreteRowCount == 0 && row.continuousRowCount == 0) ||
        ((row.discreteAxisCount == 0) != (row.discreteRowCount == 0)) ||
        (row.continuousAxisCount != 0 && row.continuousRowCount == 0)) {
        return false;
    }
    const bool hasDiscrete = row.discreteAxisCount != 0;
    const bool hasContinuous = row.continuousRowCount != 0;
    std::uint32_t continuousMemberCount = 0;
    for (std::uint32_t index = 0; index < row.continuousRowCount; ++index) {
        bool firstOccurrence = true;
        for (std::uint32_t prior = 0; prior < index; ++prior)
            firstOccurrence = firstOccurrence && row.continuousRows[prior].childStableOrdinal !=
                row.continuousRows[index].childStableOrdinal;
        continuousMemberCount += firstOccurrence;
    }
    if (row.discreteTiming > 1 || row.continuousTiming > 1 || row.continuousNormalization > 1 ||
        (!hasDiscrete && row.discreteTiming != 0) ||
        (!hasContinuous && (row.continuousNormalization != 0 || row.envelopeFrames != 0 ||
                            row.maxActiveRows != 0 || row.maxLiveRows != 0)) ||
        (hasContinuous && (row.envelopeFrames == 1 || row.maxActiveRows == 0 ||
                           row.maxLiveRows < row.maxActiveRows || continuousMemberCount > row.maxActiveRows)) ||
        (((hasContinuous && row.continuousAxisCount != 0 && row.continuousTiming == 1) || row.discreteTiming == 1) != (row.evaluationPeriodFrames != 0)) ||
        (row.transitionProgramIndex != UINT32_MAX && (!hasDiscrete || row.discreteTiming != 1))) {
        return false;
    }
    for (std::uint32_t i = 0; i < row.childCount; ++i) {
        if (row.children[i].frequencyWeight != 0 || row.children[i].orderWeight != 0 ||
            row.children[i].queueStopPolicy != QueueStopPolicyV1::AbortOwningQueue ||
            row.children[i].timingHint.kind != ChildTimingHintKindV2::Unknown) {
            return false;
        }
    }
    return true;
}

template <class WorldView>
bool IsKnownNonPlayableActionTarget(const WorldView& world, harkenstudio::ObjectId stableId) noexcept {
    for (std::uint32_t i = 0; i < world.actionCount; ++i)
        if (world.actions[i].actionStableId == stableId) return true;
    for (std::uint32_t i = 0; i < world.variableCount; ++i)
        if (world.variables[i].variableId == stableId) return true;
    return false;
}

template <class WorldView>
const PlayablePropertyProgramViewV1* FindActionTargetProperty(
    const WorldView& world, harkenstudio::ObjectId stableId) noexcept {
    for (std::uint32_t i = 0; i < world.playablePropertyCount; ++i)
        if (world.playableProperties[i].stableId == stableId)
            return &world.playableProperties[i];
    return nullptr;
}

template <class WorldView>
const PlayablePropertyProgramViewV1 *FindProperty(const WorldView &world,
                                                  playableproperties::ContentPropertyKindV1 kind,
                                                  harkenstudio::ObjectId stableId) noexcept {
    if (world.playablePropertyCount == 0)
        return nullptr;
    const auto key = std::pair(static_cast<std::uint32_t>(kind), stableId);
    const auto *found = std::lower_bound(
        world.playableProperties, world.playableProperties + world.playablePropertyCount, key,
        [](const PlayablePropertyProgramViewV1 &row,
           const std::pair<std::uint32_t, harkenstudio::ObjectId> &candidate) {
            return std::pair(static_cast<std::uint32_t>(row.contentPropertyKind), row.stableId) <
                   candidate;
        });
    return found != world.playableProperties + world.playablePropertyCount &&
                   found->contentPropertyKind == kind && found->stableId == stableId
               ? found
               : nullptr;
}

bool SameRef(const PlayableRefV1 &a, const PlayableRefV1 &b) noexcept {
    return a.kind == b.kind && a.stableId == b.stableId &&
           a.expectedProgramSemanticFingerprint == b.expectedProgramSemanticFingerprint &&
           a.contentRevision == b.contentRevision && a.routeStableId == b.routeStableId &&
           a.compatibilityFingerprint == b.compatibilityFingerprint;
}

template <class WorldView>
bool AccumulateBoundedTransitionEndpointLeaves(const WorldView &world,
                                               const PlayableRefV1 &endpoint, std::uint32_t depth,
                                               std::uint32_t *leafCount) noexcept {
    if (leafCount == nullptr || world.capacity.maxKnownScheduleLeaves == 0 ||
        *leafCount > world.capacity.maxKnownScheduleLeaves || depth == 0 ||
        depth > world.capacity.maxContainerDepth) {
        return false;
    }
    if (endpoint.kind == PlayableKindV1::Sound) {
        if (*leafCount == world.capacity.maxKnownScheduleLeaves) {
            return false;
        }
        ++*leafCount;
        return true;
    }
    if (endpoint.kind == PlayableKindV1::Container)
        return false;
    if (endpoint.kind == PlayableKindV1::Track) {
        const auto *schedule =
            std::lower_bound(world.trackSchedules, world.trackSchedules + world.trackScheduleCount,
                             endpoint.stableId,
                             [](const TrackScheduleProgramViewV1 &candidate, harkenstudio::ObjectId stableId) {
                                 return candidate.rootDefinition.stableId < stableId;
                             });
        if (schedule == world.trackSchedules + world.trackScheduleCount ||
            !SameRef(schedule->rootDefinition, endpoint)) {
            return false;
        }
        for (std::uint32_t index = 0; index < schedule->placementCount; ++index) {
            if (!AccumulateBoundedTransitionEndpointLeaves(
                    world, schedule->placements[index].content, depth + 1u, leafCount)) {
                return false;
            }
        }
        return *leafCount != 0 && *leafCount <= world.capacity.maxKnownScheduleLeaves;
    }
    const auto *schedule = std::lower_bound(
        world.pieceSchedules, world.pieceSchedules + world.pieceScheduleCount, endpoint.stableId,
        [](const PieceScheduleProgramViewV1 &candidate, harkenstudio::ObjectId stableId) {
            return candidate.rootDefinition.stableId < stableId;
        });
    if (schedule == world.pieceSchedules + world.pieceScheduleCount ||
        !SameRef(schedule->rootDefinition, endpoint)) {
        return false;
    }
    for (std::uint32_t index = 0; index < schedule->trackRefCount; ++index) {
        if (!AccumulateBoundedTransitionEndpointLeaves(
                world, schedule->trackRefs[index].trackDefinition, depth + 1u, leafCount)) {
            return false;
        }
    }
    return *leafCount != 0 && *leafCount <= world.capacity.maxKnownScheduleLeaves;
}

template <class WorldView>
bool TransitionEndpointHasBoundedResolvedVoices(const WorldView &world,
                                                const PlayableRefV1 &endpoint) noexcept {
    std::uint32_t leafCount = 0;
    return AccumulateBoundedTransitionEndpointLeaves(world, endpoint, 1, &leafCount) &&
           leafCount != 0 && leafCount <= world.capacity.maxKnownScheduleLeaves;
}

template <class WorldView, class ContainerView, class ChildView>
bool VariableControlTransitionEndpointSupported(const WorldView &world,
                                                const ContainerView &container,
                                                const ChildView &child) noexcept {
    if (!TransitionEndpointHasBoundedResolvedVoices(world, child.playable)) {
        return false;
    }
    for (std::uint32_t index = 0; index < container.continuousRowCount; ++index) {
        if (container.continuousRows[index].childStableOrdinal == child.stableChildOrdinal &&
            child.playable.kind != PlayableKindV1::Sound) {
            return false;
        }
    }
    return true;
}

template <class ContainerView>
bool TransitionEndpointIsDirectChild(const ContainerView &container,
                                     const PlayableRefV1 &endpoint) noexcept {
    for (std::uint32_t index = 0; index < container.childCount; ++index) {
        if (SameRef(container.children[index].playable, endpoint)) {
            return true;
        }
    }
    return false;
}

bool IsZeroPlayableRef(const PlayableRefV1 &ref) noexcept {
    return ref.kind == PlayableKindV1::Sound && ref.reserved == 0 && ref.stableId == harkenstudio::ObjectId{} &&
           ref.expectedProgramSemanticFingerprint == 0 && ref.contentRevision == 0 &&
           ref.routeStableId == harkenstudio::ObjectId{} && ref.compatibilityFingerprint == 0;
}

bool ValidTransitionFade(const TransitionFadeV1 &fade) noexcept {
    if (fade.active > 1 || fade.reserved != std::array<std::uint8_t, 3>{}) {
        return false;
    }
    return fade.active != 0 || (fade.durationFrames == 0 && fade.curveLocalId == UINT32_MAX);
}

TransitionProgramClassV1 ClassifyTransitionProgramShape(
    const TransitionProgramViewV1 &program) noexcept {
    if (program.ownerContainerStableId == harkenstudio::ObjectId{} || program.ruleCount == 0 ||
        !PointerMatches(program.ruleCount, program.rules) ||
        !PointerMatches(program.downwardPathStepCount, program.downwardPathSteps) ||
        program.defaultRuleIndex >= program.ruleCount || program.watchFallback != 0) {
        return TransitionProgramClassV1::Invalid;
    }
    for (std::uint32_t i = 0; i < program.downwardPathStepCount; ++i) {
        const auto &step = program.downwardPathSteps[i];
        if (!ValidPlayableRef(step.playable) || step.reserved != 0) {
            return TransitionProgramClassV1::Invalid;
        }
    }
    bool advanced = program.downwardPathStepCount != 0 || program.maxContainerDescent != 0 ||
                    program.maxOwnerAscent != 0;
    std::uint32_t defaultCount = 0;
    for (std::uint32_t i = 0; i < program.ruleCount; ++i) {
        const auto &rule = program.rules[i];
        if (rule.sourceAny > 1 || rule.targetAny > 1 || rule.bridgePresent > 1 ||
            rule.reserved != 0 || rule.reserved2 != 0 ||
            static_cast<std::uint32_t>(rule.sourceExit) >
                static_cast<std::uint32_t>(TransitionSourceExitV1::ExitMarker) ||
            static_cast<std::uint32_t>(rule.targetEntry) >
                static_cast<std::uint32_t>(TransitionTargetEntryV1::SamePositionAsIncumbent) ||
            !ValidTransitionFade(rule.sourceFade) || !ValidTransitionFade(rule.bridgeFadeIn) ||
            !ValidTransitionFade(rule.bridgeFadeOut) || !ValidTransitionFade(rule.targetFade) ||
            (rule.sourceAny ? !IsZeroPlayableRef(rule.source) : !ValidPlayableRef(rule.source)) ||
            (rule.targetAny ? !IsZeroPlayableRef(rule.target) : !ValidPlayableRef(rule.target)) ||
            (rule.bridgePresent ? !ValidPlayableRef(rule.bridge)
                                : !IsZeroPlayableRef(rule.bridge)) ||
            (rule.bridgePresent &&
             rule.targetEntry == TransitionTargetEntryV1::SamePositionAsIncumbent) ||
            (!rule.bridgePresent &&
             (rule.bridgeFadeIn.active != 0 || rule.bridgeFadeOut.active != 0)) ||
            ((rule.sourceExit == TransitionSourceExitV1::NextBeat ||
              rule.sourceExit == TransitionSourceExitV1::NextBar) !=
             (rule.requiredClockFingerprint != 0)) ||
            ((rule.sourceExit == TransitionSourceExitV1::NextMarker ||
              rule.sourceExit == TransitionSourceExitV1::ExitMarker) !=
             (rule.requiredMarkerStableId != harkenstudio::ObjectId{})) ||
            ((rule.targetEntry == TransitionTargetEntryV1::SamePositionAsIncumbent) !=
             (rule.samePositionCompatibilityFingerprint != 0))) {
            return TransitionProgramClassV1::Invalid;
        }
        const bool isDefault = rule.sourceAny != 0 && rule.targetAny != 0;
        defaultCount += isDefault ? 1u : 0u;
        if ((i == program.defaultRuleIndex) != isDefault) {
            return TransitionProgramClassV1::Invalid;
        }
        if (i != 0 && program.rules[i - 1].stableRuleOrdinal >= rule.stableRuleOrdinal) {
            return TransitionProgramClassV1::Invalid;
        }
        for (std::uint32_t prior = 0; prior < i; ++prior) {
            const auto &other = program.rules[prior];
            if (other.authoredStableOrder == rule.authoredStableOrder ||
                (other.sourceAny == rule.sourceAny && other.targetAny == rule.targetAny &&
                 (rule.sourceAny || SameRef(other.source, rule.source)) &&
                 (rule.targetAny || SameRef(other.target, rule.target)))) {
                return TransitionProgramClassV1::Invalid;
            }
        }
        advanced = advanced || rule.bridgePresent != 0 ||
                   rule.sourceExit > TransitionSourceExitV1::NaturalEnd ||
                   rule.targetEntry != TransitionTargetEntryV1::FromEntryMarker;
    }
    if (defaultCount != 1)
        return TransitionProgramClassV1::Invalid;
    return advanced ? TransitionProgramClassV1::Advanced : TransitionProgramClassV1::Basic;
}

template <class WorldView>
const VariableDefinitionV1 *FindVariable(const WorldView &world, harkenstudio::ObjectId variableId) {
    if (world.variableCount == 0)
        return nullptr;
    const auto *found = std::lower_bound(
        world.variables, world.variables + world.variableCount, variableId,
        [](const VariableDefinitionV1 &row, harkenstudio::ObjectId id) { return row.variableId < id; });
    return found != world.variables + world.variableCount && found->variableId == variableId
               ? found
               : nullptr;
}

bool DiscreteRangeCanRoundToInt32(const VariableDefinitionV1 &definition) noexcept {
    const double roundedMin = std::floor(static_cast<double>(definition.rangeMin) + 0.5);
    const double roundedMax = std::floor(static_cast<double>(definition.rangeMax) + 0.5);
    return roundedMin >= static_cast<double>((std::numeric_limits<std::int32_t>::min)()) &&
           roundedMax <= static_cast<double>((std::numeric_limits<std::int32_t>::max)());
}

template <class ContainerView>
auto FindContainerChild(const ContainerView &container, std::uint32_t stableChildOrdinal) {
    const auto *found = std::lower_bound(
        container.children, container.children + container.childCount, stableChildOrdinal,
        [](const auto &row, std::uint32_t ordinal) { return row.stableChildOrdinal < ordinal; });
    return found != container.children + container.childCount &&
                   found->stableChildOrdinal == stableChildOrdinal
               ? found
               : nullptr;
}

template <class WorldView>
bool ValidGridAxes(const WorldView &world, const GridAxisV1 *axes, std::uint32_t axisCount,
                   ValidationScope scope) {
    std::vector<std::uint32_t> authoredOrders;
    authoredOrders.reserve(axisCount);
    for (std::uint32_t i = 0; i < axisCount; ++i) {
        const auto &axis = axes[i];
        if (axis.variableId == harkenstudio::ObjectId{} ||
            (!IsPartitionLocal(scope) && FindVariable(world, axis.variableId) == nullptr) ||
            (i != 0 && axes[i - 1].stableAxisOrdinal >= axis.stableAxisOrdinal)) {
            return false;
        }
        authoredOrders.push_back(axis.authoredColumnOrder);
    }
    std::sort(authoredOrders.begin(), authoredOrders.end());
    for (std::uint32_t i = 0; i < axisCount; ++i) {
        if (authoredOrders[i] != i)
            return false;
    }
    return true;
}

template <class WorldView, class ContainerView>
bool ValidDiscreteGrid(const WorldView &world, const ContainerView &container,
                       ValidationScope scope) {
    if (!ValidGridAxes(world, container.discreteAxes, container.discreteAxisCount, scope)) {
        return false;
    }
    for (std::uint32_t i = 0; i < container.discreteAxisCount; ++i) {
        const auto *definition = FindVariable(world, container.discreteAxes[i].variableId);
        if (definition == nullptr) {
            if (!IsPartitionLocal(scope))
                return false;
        } else if (!DiscreteRangeCanRoundToInt32(*definition)) {
            return false;
        }
    }
    std::vector<std::uint32_t> authoredOrders;
    authoredOrders.reserve(container.discreteRowCount);
    for (std::uint32_t i = 0; i < container.discreteRowCount; ++i) {
        const auto &row = container.discreteRows[i];
        if (row.cellCount != container.discreteAxisCount ||
            !PointerMatches(row.cellCount, row.cells) ||
            FindContainerChild(container, row.childStableOrdinal) == nullptr ||
            (i != 0 && container.discreteRows[i - 1].stableRowOrdinal >= row.stableRowOrdinal)) {
            return false;
        }
        authoredOrders.push_back(row.authoredStableOrder);
        for (std::uint32_t cellIndex = 0; cellIndex < row.cellCount; ++cellIndex) {
            const auto &cell = row.cells[cellIndex];
            if (static_cast<std::uint32_t>(cell.kind) >
                    static_cast<std::uint32_t>(DiscreteCellKindV1::Wildcard) ||
                (cell.kind == DiscreteCellKindV1::Wildcard && cell.exactValue != 0)) {
                return false;
            }
        }
    }
    std::sort(authoredOrders.begin(), authoredOrders.end());
    return std::adjacent_find(authoredOrders.begin(), authoredOrders.end()) == authoredOrders.end();
}

template <class WorldView, class ContainerView>
bool ValidContinuousGrid(const WorldView &world, const ContainerView &container,
                         ValidationScope scope) {
    if (!ValidGridAxes(world, container.continuousAxes, container.continuousAxisCount, scope)) {
        return false;
    }
    std::vector<std::uint32_t> authoredOrders;
    authoredOrders.reserve(container.continuousRowCount);
    for (std::uint32_t i = 0; i < container.continuousRowCount; ++i) {
        const auto &row = container.continuousRows[i];
        if (row.cellCount != container.continuousAxisCount ||
            !PointerMatches(row.cellCount, row.cells) ||
            FindContainerChild(container, row.childStableOrdinal) == nullptr ||
            (i != 0 && container.continuousRows[i - 1].stableRowOrdinal >= row.stableRowOrdinal)) {
            return false;
        }
        authoredOrders.push_back(row.authoredStableOrder);
        for (std::uint32_t cellIndex = 0; cellIndex < row.cellCount; ++cellIndex) {
            const auto &cell = row.cells[cellIndex];
            if (static_cast<std::uint32_t>(cell.kind) >
                    static_cast<std::uint32_t>(ContinuousCellKindV1::Interval) ||
                cell.reserved != 0 || cell.lowerInclusive > 1 || cell.upperInclusive > 1) {
                return false;
            }
            if (cell.kind == ContinuousCellKindV1::Unconstrained) {
                if (cell.lower != 0.0f || std::signbit(cell.lower) || cell.upper != 0.0f ||
                    std::signbit(cell.upper) || cell.lowerInclusive != 0 ||
                    cell.upperInclusive != 0 || cell.leftCurveLocalId != UINT32_MAX ||
                    cell.rightCurveLocalId != UINT32_MAX) {
                    return false;
                }
            } else if (!ValidCanonicalF32(cell.lower) || !ValidCanonicalF32(cell.upper) ||
                       cell.lower > cell.upper) {
                return false;
            }
        }
    }
    std::sort(authoredOrders.begin(), authoredOrders.end());
    if (std::adjacent_find(authoredOrders.begin(), authoredOrders.end()) != authoredOrders.end()) {
        return false;
    }
    return true;
}

template <class WorldView, class ContainerView>
bool ValidContainerCurveReferences(const WorldView &world, const ContainerView &container,
                                   bool includeTypedTransitions) {
    std::vector<std::uint32_t> referencedCurves;
    referencedCurves.reserve(container.curveCount);
    const auto referenceCurve = [&](std::uint32_t curveLocalId, bool continuousWeight) {
        if (curveLocalId == UINT32_MAX)
            return true;
        const auto *curve = std::lower_bound(
            container.curves, container.curves + container.curveCount, curveLocalId,
            [](const CurveV1 &candidate, std::uint32_t id) { return candidate.curveLocalId < id; });
        if (curve == container.curves + container.curveCount ||
            curve->curveLocalId != curveLocalId || !ValidCurve(*curve) ||
            curve->curve.pointCount < 2 ||
            (continuousWeight && !ValidContinuousWeightCurve(*curve)) ||
            std::find(referencedCurves.begin(), referencedCurves.end(), curveLocalId) !=
                referencedCurves.end()) {
            return false;
        }
        referencedCurves.push_back(curveLocalId);
        return true;
    };
    for (std::uint32_t row = 0; row < container.continuousRowCount; ++row) {
        for (std::uint32_t cell = 0; cell < container.continuousRows[row].cellCount; ++cell) {
            const auto &value = container.continuousRows[row].cells[cell];
            if (!referenceCurve(value.leftCurveLocalId, true) ||
                !referenceCurve(value.rightCurveLocalId, true)) {
                return false;
            }
        }
    }
    if (container.transitionProgramIndex != UINT32_MAX) {
        if (container.transitionProgramIndex >= world.transitionCount) {
            return false;
        }
        const auto &transition = world.transitions[container.transitionProgramIndex];
        for (std::uint32_t rule = 0; rule < transition.ruleCount; ++rule) {
            for (const TransitionFadeV1 *fade :
                 {&transition.rules[rule].sourceFade, &transition.rules[rule].bridgeFadeIn,
                  &transition.rules[rule].bridgeFadeOut, &transition.rules[rule].targetFade}) {
                if (fade->active != 0 && (fade->curveLocalId == UINT32_MAX ||
                                          !referenceCurve(fade->curveLocalId, false))) {
                    return false;
                }
            }
        }
    }
    return referencedCurves.size() == container.curveCount;
}

template <class WorldView, class ContainerView>
bool ValidVariableControlGrids(const WorldView &world, const ContainerView &container,
                               ValidationScope scope) {
    return ValidDiscreteGrid(world, container, scope) &&
           ValidContinuousGrid(world, container, scope);
}

} // namespace

bool ValidateResolvedSectionGroupPayloadV1(const PlayablePropertyPayloadViewV1 &payload) noexcept {
    constexpr auto spreadBit = UINT64_C(1) << 9;
    constexpr auto requiredMask = kResolvedSectionGroupPropertyMaskV1 & ~spreadBit;
    if ((payload.presentMask & requiredMask) != requiredMask ||
        ((payload.presentMask & spreadBit) == 0 && payload.values[9].bits != 0) ||
        payload.values[6].objectId == harkenstudio::ObjectId{} || payload.values[7].bits != 0 ||
        payload.values[10].bits > 1 || !PointerMatches(payload.sendCount, payload.sends) ||
        payload.sendCount > kHardPlayablePropertyChildRowCapacity) {
        return false;
    }
    const auto inRange = [&](std::size_t slot, float low, float high) {
        const auto bits = payload.values[slot].bits;
        const float value = std::bit_cast<float>(static_cast<std::uint32_t>(bits));
        return ValidFiniteF32(bits) && value >= low && value <= high;
    };
    if (!inRange(5, -96.0f, 24.0f) || !inRange(8, -1.0f, 1.0f) || !inRange(9, 0.0f, 100.0f) ||
        !inRange(11, 0.01f, 100.0f) || !inRange(16, 0.0f, 1.0f)) {
        return false;
    }
    for (std::uint32_t i = 0; i < payload.sendCount; ++i) {
        const auto &send = payload.sends[i];
        if (send.targetBusId == harkenstudio::ObjectId{} || send.targetBusId == payload.values[6].objectId ||
            send.reserved != 0 || send.reserved2 != 0 || !std::isfinite(send.sendLevel) ||
            send.sendLevel < -96.0f || send.sendLevel > 24.0f ||
            (send.sendLevel == 0.0f && std::signbit(send.sendLevel)) ||
            (i != 0 && payload.sends[i - 1].stableSendOrdinal >= send.stableSendOrdinal)) {
            return false;
        }
        if (payload.sendCount <= 16)
            for (std::uint32_t previous = 0; previous < i; ++previous)
                if (payload.sends[previous].targetBusId == send.targetBusId)
                    return false;
    }
    if (payload.sendCount > 16) {
        try {
            std::vector<harkenstudio::ObjectId> targets;
            targets.reserve(payload.sendCount);
            for (std::uint32_t i = 0; i < payload.sendCount; ++i)
                targets.push_back(payload.sends[i].targetBusId);
            std::sort(targets.begin(), targets.end());
            if (std::adjacent_find(targets.begin(), targets.end()) != targets.end())
                return false;
        } catch (...) {
            return false;
        }
    }
    return true;
}

PlayablePropertyPayloadViewV1 MakeStructuralAncestorPayloadV1(
    const PlayablePropertyPayloadViewV1 &source) noexcept {
    auto payload = source;
    constexpr auto excludedMask =
        kResolvedSectionGroupPropertyMaskV1 | (UINT64_C(1) << 4) | (UINT64_C(1) << 15);
    payload.presentMask &= ~excludedMask;
    for (std::uint32_t index = 0; index < payload.values.size(); ++index) {
        if ((excludedMask & (UINT64_C(1) << index)) != 0) {
            payload.values[index] = {};
            payload.values[index].bits =
                index == 11 || index == 16 ? UINT64_C(0x3f800000) : 0;
        }
    }
    payload.sendCount = 0;
    payload.sends = nullptr;
    payload.effectNodeCount = 0;
    payload.effectNodes = nullptr;
    payload.effectEdgeCount = 0;
    payload.effectEdges = nullptr;
    return payload;
}

bool ValidateCurrentWorldSectionGroupsV1(const ContainerWorldInstallViewV1 &world) noexcept {
    if (world.sourceBodyVintage != kCurrentWorldShippingBodyVintage ||
        !PointerMatches(world.playablePropertyCount, world.playableProperties) ||
        !PointerMatches(world.playableOccurrenceCount, world.playableOccurrences) ||
        world.playablePropertyCount > kHardPlayablePropertyProgramCapacity ||
        world.playableOccurrenceCount > kHardPlayableOccurrencePropertyCapacity) {
        return false;
    }
    constexpr std::uint64_t ancestorLocalMask =
        kResolvedSectionGroupPropertyMaskV1 | (UINT64_C(1) << 4) | (UINT64_C(1) << 15);
    for (std::uint32_t i = 0; i < world.playablePropertyCount; ++i) {
        const auto &row = world.playableProperties[i];
        if (!ValidKind(row.contentPropertyKind) ||
            !ValidateResolvedSectionGroupPayloadV1(row.payload) ||
            !PointerMatches(row.structuralAncestorCount, row.structuralAncestors) ||
            row.structuralAncestorCount > kHardPlayablePropertyChildRowCapacity) {
            return false;
        }
        for (std::uint32_t a = 0; a < row.structuralAncestorCount; ++a) {
            const auto &payload = row.structuralAncestors[a].payload;
            if ((payload.presentMask & ancestorLocalMask) != 0 || !ValidPayload(payload)) {
                return false;
            }
        }
    }
    for (std::uint32_t i = 0; i < world.playableOccurrenceCount; ++i)
        if ((world.playableOccurrences[i].payload.presentMask &
             kResolvedSectionGroupPropertyMaskV1) != 0)
            return false;
    return true;
}

TransitionProgramClassV1 ClassifyTransitionProgramV1(
    const TransitionProgramViewV1 &program) noexcept {
    return ClassifyTransitionProgramShape(program);
}

namespace {

std::uint64_t ComputePlayablePropertyFingerprintImpl(const PlayablePropertyProgramViewV1 &row,
                                                     bool includeAncestorPayloadHeader) noexcept {
    Fnv64 hash("HARKEN-PROPERTY-DEFINITION-V1");
    hash.U32(static_cast<std::uint32_t>(row.contentPropertyKind));
    hash.U32(row.authoredStableOrder);
    hash.ObjectId(row.stableId);
    hash.U64(0);
    hash.U64(row.contentRevision);
    hash.ObjectId(row.routeStableId);
    hash.U64(row.compatibilityFingerprint);
    hash.U32(row.organizationLayerAncestorCount);
    hash.U32(0);
    HashPayloadHeader(hash, row.payload);
    for (std::uint32_t i = 0; i < row.organizationLayerAncestorCount; ++i) {
        hash.ObjectId(row.organizationLayerAncestors[i].layerStableId);
        hash.U64(row.organizationLayerAncestors[i].layerProgramSemanticFingerprint);
    }
    if (row.structuralAncestorCount != 0) {
        hash.U32(row.structuralAncestorCount);
        hash.U32(0);
        for (std::uint32_t i = 0; i < row.structuralAncestorCount; ++i) {
            hash.U32(static_cast<std::uint32_t>(row.structuralAncestors[i].contentPropertyKind));
            hash.U32(row.structuralAncestors[i].reserved);
            hash.ObjectId(row.structuralAncestors[i].stableId);
            hash.U64(row.structuralAncestors[i].programSemanticFingerprint);
            if (includeAncestorPayloadHeader) {
                HashPayloadHeader(hash, row.structuralAncestors[i].payload);
            }
            HashPayloadBody(hash, row.structuralAncestors[i].payload);
        }
    }
    HashPayloadBody(hash, row.payload);
    return hash.Finish();
}

} // namespace

std::uint64_t ComputePlayablePropertyFingerprintV1(
    const PlayablePropertyProgramViewV1 &row) noexcept {
    return ComputePlayablePropertyFingerprintImpl(row, true);
}

std::uint64_t ComputePlayableOccurrenceFingerprintV1(
    const PlayableOccurrencePropertyViewV1 &row) noexcept {
    Fnv64 hash("HARKEN-PROPERTY-OCCURRENCE-V1");
    hash.U32(static_cast<std::uint32_t>(row.parentKind));
    hash.U32(row.stableOccurrenceOrdinal);
    hash.ObjectId(row.parentStableId);
    hash.U64(row.parentProgramSemanticFingerprint);
    HashPlayableRef(hash, row.childPlayable);
    hash.U64(0);
    HashPayloadHeader(hash, row.payload);
    HashPayloadBody(hash, row.payload);
    return hash.Finish();
}

void HashSchedulePlacement(Fnv64 &hash, const ScheduleClipPlacementV1 &row) noexcept {
    hash.U32(row.stablePlacementOrdinal);
    hash.U32(row.stableOccurrenceOrdinal);
    HashPlayableRef(hash, row.content);
    hash.I64(row.authoredStart);
    hash.I64(row.authoredDuration);
    hash.I64(row.authoredFadeIn);
    hash.I64(row.authoredFadeOut);
    hash.ObjectId(row.clipId);
    hash.U64(row.clipGuidHigh);
    hash.U64(row.clipGuidLow);
}

void HashTrackSchedule(Fnv64 &hash, const TrackScheduleProgramViewV1 &row,
                       bool zeroFingerprint) noexcept {
    HashPlayableRef(hash, row.rootDefinition);
    hash.U64(zeroFingerprint ? 0 : row.scheduleSemanticFingerprint);
    hash.U32(row.placementCount);
    hash.U32(row.reserved);
    for (std::uint32_t i = 0; i < row.placementCount; ++i) {
        HashSchedulePlacement(hash, row.placements[i]);
    }
}

void HashPieceSchedule(Fnv64 &hash, const PieceScheduleProgramViewV1 &row,
                       bool zeroFingerprint) noexcept {
    HashPlayableRef(hash, row.rootDefinition);
    hash.U64(zeroFingerprint ? 0 : row.scheduleSemanticFingerprint);
    hash.U32(static_cast<std::uint32_t>(row.timeKind));
    hash.U32(row.startPointKind);
    hash.U32(row.startMarkerId);
    hash.U32(row.reserved);
    hash.I64(row.microsPerQuarter);
    hash.U32(row.meterNumerator);
    hash.U32(row.meterDenominator);
    hash.U32(row.trackRefCount);
    hash.U32(row.markerCount);
    for (std::uint32_t i = 0; i < row.trackRefCount; ++i) {
        const auto &ref = row.trackRefs[i];
        HashPlayableRef(hash, ref.trackDefinition);
        hash.U64(ref.trackScheduleSemanticFingerprint);
        hash.U32(ref.stableTrackOrdinal);
        hash.U32(ref.reserved);
    }
    for (std::uint32_t i = 0; i < row.markerCount; ++i) {
        const auto &marker = row.markers[i];
        hash.ObjectId(marker.stableMarkerId);
        hash.U32(marker.markerId);
        hash.U32(marker.markerKind);
        hash.I64(marker.position);
    }
}

std::uint64_t ComputeTrackScheduleFingerprintV1(const TrackScheduleProgramViewV1 &row) noexcept {
    Fnv64 hash("HARKEN-TRACK-SCHEDULE-V1");
    HashTrackSchedule(hash, row, true);
    return hash.Finish();
}

std::uint64_t ComputePieceScheduleFingerprintV1(const PieceScheduleProgramViewV1 &row) noexcept {
    Fnv64 hash("HARKEN-PIECE-SCHEDULE-V1");
    HashPieceSchedule(hash, row, true);
    return hash.Finish();
}

void HashInlineSoundTimingHint(Fnv64 &hash, const InlineSoundTimingHintV2 &hint) noexcept {
    HashTime(hash, hint.startOffset);
    HashTime(hash, hint.duration);
    hash.U32(hint.loopCount);
    hash.Byte(hint.loopRegionPresent);
    hash.Byte(hint.reserved[0]);
    hash.Byte(hint.reserved[1]);
    hash.Byte(hint.reserved[2]);
    HashTime(hash, hint.loopStart);
    HashTime(hash, hint.loopEnd);
}

void HashContainerV9(Fnv64 &hash, const ContainerProgramViewV3 &row) noexcept {
    hash.ObjectId(row.containerStableId);
    hash.U64(row.programSemanticFingerprint);
    hash.U32(static_cast<std::uint32_t>(row.mode));
    hash.U32(static_cast<std::uint32_t>(row.randomDiscipline));
    hash.U32(static_cast<std::uint32_t>(row.memoryScope));
    hash.U32(static_cast<std::uint32_t>(row.retriggerPolicy));
    hash.U32(static_cast<std::uint32_t>(row.resetPolicy));
    hash.U32(static_cast<std::uint32_t>(row.sequenceEndPolicy));
    hash.U32(static_cast<std::uint32_t>(row.queueCycle));
    hash.U32(static_cast<std::uint32_t>(row.queueSplice));
    hash.U32(row.queueCycleCount);
    hash.Byte(row.resumePosition);
    hash.Byte(0);
    hash.Byte(0);
    hash.Byte(0);
    hash.U32(row.avoidLastN);
    hash.U32(row.initialSequenceCursor);
    hash.U32(row.discreteTiming);
    hash.U32(row.continuousTiming);
    hash.U32(row.continuousNormalization);
    hash.U32(row.evaluationPeriodFrames);
    hash.U32(row.envelopeFrames);
    hash.U32(row.maxActiveRows);
    hash.U32(row.maxLiveRows);
    hash.U32(row.childCount);
    hash.U32(row.curveCount);
    hash.U32(row.discreteAxisCount);
    hash.U32(row.discreteRowCount);
    hash.U32(row.continuousAxisCount);
    hash.U32(row.continuousRowCount);
    hash.U32(row.transitionProgramIndex);
    hash.U32(row.authoredStableOrder);
    for (std::uint32_t i = 0; i < row.curveCount; ++i) {
        HashCurve(hash, row.curves[i]);
    }
    for (std::uint32_t i = 0; i < row.childCount; ++i) {
        const auto &child = row.children[i];
        HashPlayableRef(hash, child.playable);
        hash.U32(child.stableChildOrdinal);
        hash.U32(child.authoredStableOrder);
        hash.U32(child.frequencyWeight);
        hash.U32(child.orderWeight);
        hash.U32(static_cast<std::uint32_t>(child.queueStopPolicy));
        hash.U32(child.reserved);
        hash.U32(static_cast<std::uint32_t>(child.timingHint.kind));
        hash.U32(child.timingHint.reserved);
        if (child.timingHint.kind == ChildTimingHintKindV2::InlineSound) {
            HashInlineSoundTimingHint(hash, child.timingHint.payload.inlineSound);
        } else if (child.timingHint.kind == ChildTimingHintKindV2::ScheduleReference) {
            const auto &schedule = child.timingHint.payload.schedule;
            hash.U32(static_cast<std::uint32_t>(schedule.kind));
            hash.U32(schedule.reserved);
            hash.ObjectId(schedule.stableId);
            hash.U64(schedule.scheduleSemanticFingerprint);
        }
    }
    for (std::uint32_t i = 0; i < row.discreteAxisCount; ++i) {
        const auto &axis = row.discreteAxes[i];
        hash.U32(axis.stableAxisOrdinal);
        hash.U32(axis.authoredColumnOrder);
        hash.ObjectId(axis.variableId);
    }
    for (std::uint32_t i = 0; i < row.discreteRowCount; ++i) {
        const auto &gridRow = row.discreteRows[i];
        hash.U32(gridRow.stableRowOrdinal);
        hash.U32(gridRow.authoredStableOrder);
        hash.U32(gridRow.childStableOrdinal);
        hash.U32(gridRow.cellCount);
        for (std::uint32_t cellIndex = 0; cellIndex < gridRow.cellCount; ++cellIndex) {
            const auto &cell = gridRow.cells[cellIndex];
            hash.U32(static_cast<std::uint32_t>(cell.kind));
            hash.U32(std::bit_cast<std::uint32_t>(cell.exactValue));
        }
    }
    for (std::uint32_t i = 0; i < row.continuousAxisCount; ++i) {
        const auto &axis = row.continuousAxes[i];
        hash.U32(axis.stableAxisOrdinal);
        hash.U32(axis.authoredColumnOrder);
        hash.ObjectId(axis.variableId);
    }
    for (std::uint32_t i = 0; i < row.continuousRowCount; ++i) {
        const auto &gridRow = row.continuousRows[i];
        hash.U32(gridRow.stableRowOrdinal);
        hash.U32(gridRow.authoredStableOrder);
        hash.U32(gridRow.childStableOrdinal);
        hash.U32(gridRow.cellCount);
        for (std::uint32_t cellIndex = 0; cellIndex < gridRow.cellCount; ++cellIndex) {
            const auto &cell = gridRow.cells[cellIndex];
            hash.U32(static_cast<std::uint32_t>(cell.kind));
            hash.U32(std::bit_cast<std::uint32_t>(cell.lower));
            hash.U32(std::bit_cast<std::uint32_t>(cell.upper));
            hash.Byte(cell.lowerInclusive);
            hash.Byte(cell.upperInclusive);
            hash.U16(cell.reserved);
            hash.U32(cell.leftCurveLocalId);
            hash.U32(cell.rightCurveLocalId);
        }
    }
}

template <class WorldView>
bool TryBuildCurrentWorldV3PrefixImpl(const WorldView &world, bool zeroRuntimeSemanticFingerprint,
                                      CurrentWorldV3PrefixValues *out) noexcept {
    if (out == nullptr)
        return false;
    const auto countPointer = [](std::uint32_t count, const void *pointer,
                                 std::uint32_t hardLimit) {
        return count <= hardLimit &&
               ((count == 0 && pointer == nullptr) || (count != 0 && pointer != nullptr));
    };
    if (!countPointer(world.variableCount, world.variables, kHardVariableDefinitionCapacity) ||
        !countPointer(world.playablePropertyCount, world.playableProperties,
                      kHardPlayablePropertyProgramCapacity) ||
        !countPointer(world.playableOccurrenceCount, world.playableOccurrences,
                      kHardPlayableOccurrencePropertyCapacity) ||
        !countPointer(world.trackScheduleCount, world.trackSchedules,
                      kHardTrackScheduleProgramCapacity) ||
        !countPointer(world.pieceScheduleCount, world.pieceSchedules,
                      kHardPieceScheduleProgramCapacity) ||
        !countPointer(world.containerCount, world.containers, kHardContainerProgramCapacity) ||
        !countPointer(world.transitionCount, world.transitions, 65536)) {
        return false;
    }
    CurrentWorldV3PrefixValues values{};
    values.schema = world.identity.format.schema;
    values.compiler = world.identity.format.compiler;
    values.selectionAlgorithm = world.identity.format.selectionAlgorithm;
    values.gridAlgorithm = world.identity.format.gridAlgorithm;
    values.transitionAlgorithm = world.identity.format.transitionAlgorithm;
    values.publicationProtocol = world.identity.format.publicationProtocol;
    values.runtimeSemanticFingerprint =
        zeroRuntimeSemanticFingerprint ? 0 : world.identity.runtimeSemanticFingerprint;
    values.capabilityMask = world.identity.capabilityMask;
    values.variableCount = world.variableCount;
    values.playablePropertyProgramCount = world.playablePropertyCount;
    values.playableOccurrencePropertyCount = world.playableOccurrenceCount;
    values.trackScheduleCount = world.trackScheduleCount;
    values.pieceScheduleCount = world.pieceScheduleCount;
    values.containerCount = world.containerCount;
    values.transitionProgramCount = world.transitionCount;
    values.maxContainerDepth = world.capacity.maxContainerDepth;
    values.maxChildrenPerContainer = world.capacity.maxChildrenPerContainer;
    values.maxAvoidLastN = world.capacity.maxAvoidLastN;
    values.maxDiscreteAxes = world.capacity.maxDiscreteAxes;
    values.maxDiscreteRows = world.capacity.maxDiscreteRows;
    values.maxDiscreteTrieNodes = world.capacity.maxDiscreteTrieNodes;
    values.maxContinuousAxes = world.capacity.maxContinuousAxes;
    values.maxContinuousRows = world.capacity.maxContinuousRows;
    values.maxContinuousCandidates = world.capacity.maxContinuousCandidates;
    values.maxContinuousActiveRows = world.capacity.maxContinuousActiveRows;
    values.maxContinuousLiveRows = world.capacity.maxContinuousLiveRows;
    values.maxTransitionRules = world.capacity.maxTransitionRules;
    values.maxCurvePoints = world.capacity.maxCurvePoints;
    values.maxKnownScheduleLeaves = world.capacity.maxKnownScheduleLeaves;
    values.maxTrackSchedulePrograms = world.capacity.maxTrackSchedulePrograms;
    values.maxPieceSchedulePrograms = world.capacity.maxPieceSchedulePrograms;
    values.maxScheduleTrackRefsPerPiece = world.capacity.maxScheduleTrackRefsPerPiece;
    values.maxScheduleTrackRefsTotal = world.capacity.maxScheduleTrackRefsTotal;
    values.maxSchedulePlacementsPerTrack = world.capacity.maxSchedulePlacementsPerTrack;
    values.maxSchedulePlacementsTotal = world.capacity.maxSchedulePlacementsTotal;
    values.maxScheduleMarkersPerPiece = world.capacity.maxScheduleMarkersPerPiece;
    values.maxScheduleMarkersTotal = world.capacity.maxScheduleMarkersTotal;
    values.maxTrackPieceScheduleDepth = world.capacity.maxTrackPieceScheduleDepth;
    const auto add = [](std::uint32_t amount, std::uint32_t *total) {
        if (amount > UINT32_MAX - *total)
            return false;
        *total += amount;
        return true;
    };
    const auto addBounded = [&](std::uint32_t amount, std::uint32_t limit, std::uint32_t *total) {
        return add(amount, total) && *total <= limit;
    };
    const auto countPayload = [&](const PlayablePropertyPayloadViewV1 &payload) {
        if (!countPointer(payload.sendCount, payload.sends,
                          kHardPlayablePropertyChildRowCapacity) ||
            !countPointer(payload.effectNodeCount, payload.effectNodes,
                          kHardPlayablePropertyChildRowCapacity) ||
            !countPointer(payload.effectEdgeCount, payload.effectEdges,
                          kHardPlayablePropertyEffectEdgeCapacity)) {
            return false;
        }
        if (!addBounded(payload.sendCount, kHardPlayablePropertyChildRowCapacity,
                        &values.playablePropertySendCount) ||
            !addBounded(payload.effectNodeCount, kHardPlayablePropertyChildRowCapacity,
                        &values.playablePropertyEffectNodeCount) ||
            !addBounded(payload.effectEdgeCount, kHardPlayablePropertyEffectEdgeCapacity,
                        &values.playablePropertyEffectEdgeCount)) {
            return false;
        }
        for (std::uint32_t i = 0; i < payload.effectNodeCount; ++i) {
            if (!countPointer(payload.effectNodes[i].paramCount, payload.effectNodes[i].params,
                              kHardPlayablePropertyEffectParamCapacity) ||
                !addBounded(payload.effectNodes[i].paramCount,
                            kHardPlayablePropertyEffectParamCapacity,
                            &values.playablePropertyEffectParamCount)) {
                return false;
            }
        }
        return true;
    };
    for (std::uint32_t i = 0; i < world.playablePropertyCount; ++i) {
        if (world.playableProperties == nullptr ||
            !countPointer(world.playableProperties[i].organizationLayerAncestorCount,
                          world.playableProperties[i].organizationLayerAncestors,
                          kHardPlayablePropertyChildRowCapacity) ||
            !countPayload(world.playableProperties[i].payload) ||
            !addBounded(world.playableProperties[i].organizationLayerAncestorCount,
                        kHardPlayablePropertyChildRowCapacity,
                        &values.playablePropertyLayerAncestorRefCount)) {
            return false;
        }
    }
    for (std::uint32_t i = 0; i < world.playableOccurrenceCount; ++i) {
        if (world.playableOccurrences == nullptr ||
            !countPayload(world.playableOccurrences[i].payload)) {
            return false;
        }
    }
    for (std::uint32_t i = 0; i < world.trackScheduleCount; ++i) {
        if (world.trackSchedules == nullptr ||
            !countPointer(world.trackSchedules[i].placementCount,
                          world.trackSchedules[i].placements, kHardSchedulePlacementCapacity) ||
            !addBounded(world.trackSchedules[i].placementCount, kHardSchedulePlacementCapacity,
                        &values.schedulePlacementCount)) {
            return false;
        }
    }
    for (std::uint32_t i = 0; i < world.pieceScheduleCount; ++i) {
        if (world.pieceSchedules == nullptr ||
            !countPointer(world.pieceSchedules[i].trackRefCount, world.pieceSchedules[i].trackRefs,
                          kHardScheduleTrackRefCapacity) ||
            !countPointer(world.pieceSchedules[i].markerCount, world.pieceSchedules[i].markers,
                          kHardScheduleMarkerCapacity) ||
            !addBounded(world.pieceSchedules[i].trackRefCount, kHardScheduleTrackRefCapacity,
                        &values.scheduleTrackRefCount) ||
            !addBounded(world.pieceSchedules[i].markerCount, kHardScheduleMarkerCapacity,
                        &values.scheduleMarkerCount)) {
            return false;
        }
    }
    for (std::uint32_t i = 0; i < world.containerCount; ++i) {
        const auto &container = world.containers[i];
        if (!countPointer(container.childCount, container.children,
                          kHardPlayablePropertyChildRowCapacity) ||
            !countPointer(container.curveCount, container.curves,
                          kHardPlayablePropertyChildRowCapacity) ||
            !countPointer(container.discreteAxisCount, container.discreteAxes, 32) ||
            !countPointer(container.discreteRowCount, container.discreteRows, 65536) ||
            !countPointer(container.continuousAxisCount, container.continuousAxes, 32) ||
            !countPointer(container.continuousRowCount, container.continuousRows, 65536)) {
            return false;
        }
        for (std::uint32_t row = 0; row < container.discreteRowCount; ++row) {
            if (!countPointer(container.discreteRows[row].cellCount,
                              container.discreteRows[row].cells, 32)) {
                return false;
            }
        }
        for (std::uint32_t row = 0; row < container.continuousRowCount; ++row) {
            if (!countPointer(container.continuousRows[row].cellCount,
                              container.continuousRows[row].cells, 32)) {
                return false;
            }
        }
    }
    *out = values;
    return true;
}

bool TryBuildCurrentWorldV3Prefix(const ContainerWorldInstallViewV1 &world,
                                  bool zeroRuntimeSemanticFingerprint,
                                  CurrentWorldV3PrefixValues *out) noexcept {
    return TryBuildCurrentWorldV3PrefixImpl(world, zeroRuntimeSemanticFingerprint, out);
}

bool GrowAuthoredWorldCapacityV1(const ContainerWorldInstallViewV1& world,
                               WorldCapacitySummaryV1* capacity) noexcept {
    CurrentWorldV3PrefixValues counts{};
    if (capacity == nullptr || !TryBuildCurrentWorldV3Prefix(world, true, &counts)
        || world.actionCount > kHardActionDefinitionCapacity
        || (world.actionCount != 0 && world.actions == nullptr))
        return false;
    auto required = *capacity;
    required.maxVariableDefinitions = std::max(required.maxVariableDefinitions, world.variableCount);
    required.maxContainerPrograms = std::max(required.maxContainerPrograms, world.containerCount);
    required.maxPlayablePropertyPrograms = std::max(
        required.maxPlayablePropertyPrograms, world.playablePropertyCount);
    required.maxPlayableOccurrencePropertyRows = std::max(
        required.maxPlayableOccurrencePropertyRows, world.playableOccurrenceCount);
    required.maxPlayablePropertySendsTotal = std::max(
        required.maxPlayablePropertySendsTotal, counts.playablePropertySendCount);
    required.maxPlayablePropertyLayerAncestorRefsTotal = std::max(
        required.maxPlayablePropertyLayerAncestorRefsTotal,
        counts.playablePropertyLayerAncestorRefCount);
    required.maxPlayablePropertyEffectNodesTotal = std::max(
        required.maxPlayablePropertyEffectNodesTotal, counts.playablePropertyEffectNodeCount);
    required.maxPlayablePropertyEffectParamsTotal = std::max(
        required.maxPlayablePropertyEffectParamsTotal, counts.playablePropertyEffectParamCount);
    required.maxPlayablePropertyEffectEdgesTotal = std::max(
        required.maxPlayablePropertyEffectEdgesTotal, counts.playablePropertyEffectEdgeCount);
    required.maxTrackSchedulePrograms = std::max(
        required.maxTrackSchedulePrograms, world.trackScheduleCount);
    required.maxPieceSchedulePrograms = std::max(
        required.maxPieceSchedulePrograms, world.pieceScheduleCount);
    required.maxScheduleTrackRefsTotal = std::max(
        required.maxScheduleTrackRefsTotal, counts.scheduleTrackRefCount);
    required.maxSchedulePlacementsTotal = std::max(
        required.maxSchedulePlacementsTotal, counts.schedulePlacementCount);
    required.maxScheduleMarkersTotal = std::max(
        required.maxScheduleMarkersTotal, counts.scheduleMarkerCount);
    required.maxActionDefinitions = std::max(required.maxActionDefinitions, world.actionCount);
    std::uint64_t steps = 0;
    for (std::uint32_t i = 0; i < world.actionCount; ++i) {
        steps += world.actions[i].stepCount;
        if (steps > kHardActionStepCapacity)
            return false;
    }
    required.maxActionStepsTotal = std::max(
        required.maxActionStepsTotal, static_cast<std::uint32_t>(steps));
    for (std::uint32_t i = 0; i < world.containerCount; ++i) {
        const auto& container = world.containers[i];
        if (container.childCount > kHardContainerChildCapacity
            || container.avoidLastN > kHardSelectionAvoidCapacity)
            return false;
        required.maxChildrenPerContainer = std::max(
            required.maxChildrenPerContainer, container.childCount);
        required.maxAvoidLastN = std::max(required.maxAvoidLastN, container.avoidLastN);
    }
    *capacity = required;
    return true;
}

std::uint64_t ComputeActionProgramFingerprintV1(const ActionProgramViewV1 &program) noexcept {
    Fnv64 hash("HARKEN-ACTION-PROGRAM-V1");
    if (!PointerMatches(program.stepCount, program.steps))
        return 0;
    HashActionProgram(hash, program, true);
    return hash.Finish();
}

std::uint64_t ComputeCurrentWorldFingerprintV4(const ContainerWorldInstallViewV1 &world) noexcept {
    return ComputeCurrentWorldFingerprintV5(world);
}

std::uint64_t ComputeCurrentWorldFingerprintV5(const ContainerWorldInstallViewV1 &world) noexcept {
    Fnv64 hash("HARKEN-WORLD-V5");
    hash.ObjectId(world.identity.projectId);
    CurrentWorldV3PrefixValues prefix{};
    if (!TryBuildCurrentWorldV3Prefix(world, true, &prefix) ||
        !PointerMatches(world.actionCount, world.actions)) {
        return 0;
    }
    VisitCurrentWorldV3Prefix(prefix, hash);
    hash.U32(world.actionCount);
    WorldCapacitySummaryV1 capacity = world.capacity;
    VisitWorldCapacitySummaryV1(capacity, hash);
    for (std::uint32_t i = 0; i < world.variableCount; ++i) {
        const auto &row = world.variables[i];
        hash.ObjectId(row.variableId);
        hash.U32(static_cast<std::uint32_t>(row.kind));
        hash.U32(1u);
        hash.U32(std::bit_cast<std::uint32_t>(row.rangeMin));
        hash.U32(std::bit_cast<std::uint32_t>(row.rangeMax));
        hash.U32(std::bit_cast<std::uint32_t>(row.defaultValue));
        hash.U32(0);
    }
    for (std::uint32_t i = 0; i < world.playablePropertyCount; ++i) {
        const auto &row = world.playableProperties[i];
        hash.U32(static_cast<std::uint32_t>(row.contentPropertyKind));
        hash.U32(row.authoredStableOrder);
        hash.ObjectId(row.stableId);
        hash.U64(row.programSemanticFingerprint);
        hash.U64(row.contentRevision);
        hash.ObjectId(row.routeStableId);
        hash.U64(row.compatibilityFingerprint);
        hash.U32(row.organizationLayerAncestorCount);
        hash.U32(0);
        HashPayloadHeader(hash, row.payload);
        for (std::uint32_t a = 0; a < row.organizationLayerAncestorCount; ++a) {
            hash.ObjectId(row.organizationLayerAncestors[a].layerStableId);
            hash.U64(row.organizationLayerAncestors[a].layerProgramSemanticFingerprint);
        }
        HashPayloadBody(hash, row.payload);
    }
    for (std::uint32_t i = 0; i < world.playableOccurrenceCount; ++i) {
        const auto &row = world.playableOccurrences[i];
        hash.U32(static_cast<std::uint32_t>(row.parentKind));
        hash.U32(row.stableOccurrenceOrdinal);
        hash.ObjectId(row.parentStableId);
        hash.U64(row.parentProgramSemanticFingerprint);
        HashPlayableRef(hash, row.childPlayable);
        hash.U64(row.occurrenceSemanticFingerprint);
        HashPayloadHeader(hash, row.payload);
        HashPayloadBody(hash, row.payload);
    }
    for (std::uint32_t i = 0; i < world.trackScheduleCount; ++i) {
        HashTrackSchedule(hash, world.trackSchedules[i], false);
    }
    for (std::uint32_t i = 0; i < world.pieceScheduleCount; ++i) {
        HashPieceSchedule(hash, world.pieceSchedules[i], false);
    }
    for (std::uint32_t i = 0; i < world.containerCount; ++i) {
        HashContainerV9(hash, world.containers[i]);
    }
    for (std::uint32_t i = 0; i < world.transitionCount; ++i) {
        HashTransition(hash, world.transitions[i]);
    }
    for (std::uint32_t i = 0; i < world.actionCount; ++i) {
        HashActionProgram(hash, world.actions[i], false);
    }
    return hash.Finish();
}

WorldInstallStatus ValidateCurrentWorldImpl(const ContainerWorldInstallViewV1 &world,
                                            ValidationScope scope) {
    if (!ExactFormat(world.identity.format) ||
        (world.identity.capabilityMask & ~kWorldCapabilityKnownMask) != 0) {
        return WorldInstallStatus::InvalidFormat;
    }
    if (!PointerMatches(world.variableCount, world.variables) ||
        !PointerMatches(world.playablePropertyCount, world.playableProperties) ||
        !PointerMatches(world.playableOccurrenceCount, world.playableOccurrences) ||
        !PointerMatches(world.trackScheduleCount, world.trackSchedules) ||
        !PointerMatches(world.pieceScheduleCount, world.pieceSchedules) ||
        !PointerMatches(world.containerCount, world.containers) ||
        !PointerMatches(world.transitionCount, world.transitions) ||
        !PointerMatches(world.actionCount, world.actions)) {
        return WorldInstallStatus::InvalidWorld;
    }
    if (world.capacity.maxTrackSchedulePrograms > kHardTrackScheduleProgramCapacity ||
        world.capacity.maxPieceSchedulePrograms > kHardPieceScheduleProgramCapacity ||
        world.capacity.maxScheduleTrackRefsPerPiece > 1024 ||
        world.capacity.maxScheduleTrackRefsTotal > kHardScheduleTrackRefCapacity ||
        world.capacity.maxSchedulePlacementsPerTrack > 65536 ||
        world.capacity.maxSchedulePlacementsTotal > kHardSchedulePlacementCapacity ||
        world.capacity.maxScheduleMarkersPerPiece > 65536 ||
        world.capacity.maxScheduleMarkersTotal > kHardScheduleMarkerCapacity ||
        world.capacity.maxTrackPieceScheduleDepth > 64) {
        return WorldInstallStatus::InvalidWorld;
    }
    if (world.variableCount > world.capacity.maxVariableDefinitions ||
        world.playablePropertyCount > world.capacity.maxPlayablePropertyPrograms ||
        world.playableOccurrenceCount > world.capacity.maxPlayableOccurrencePropertyRows ||
        world.trackScheduleCount > world.capacity.maxTrackSchedulePrograms ||
        world.pieceScheduleCount > world.capacity.maxPieceSchedulePrograms ||
        world.containerCount > world.capacity.maxContainerPrograms ||
        world.actionCount > world.capacity.maxActionDefinitions ||
        world.capacity.maxVariableDefinitions > kHardVariableDefinitionCapacity ||
        world.capacity.maxContainerPrograms > kHardContainerProgramCapacity ||
        world.capacity.maxPlayablePropertyPrograms > kHardPlayablePropertyProgramCapacity ||
        world.capacity.maxPlayableOccurrencePropertyRows >
            kHardPlayableOccurrencePropertyCapacity ||
        world.capacity.maxPlayablePropertySendsTotal > kHardPlayablePropertyChildRowCapacity ||
        world.capacity.maxPlayablePropertyLayerAncestorRefsTotal >
            kHardPlayablePropertyChildRowCapacity ||
        world.capacity.maxPlayablePropertyEffectNodesTotal >
            kHardPlayablePropertyChildRowCapacity ||
        world.capacity.maxPlayablePropertyEffectParamsTotal >
            kHardPlayablePropertyEffectParamCapacity ||
        world.capacity.maxPlayablePropertyEffectEdgesTotal >
            kHardPlayablePropertyEffectEdgeCapacity ||
        world.capacity.maxTrackSchedulePrograms > kHardTrackScheduleProgramCapacity ||
        world.capacity.maxPieceSchedulePrograms > kHardPieceScheduleProgramCapacity ||
        world.capacity.maxScheduleTrackRefsPerPiece > 1024 ||
        world.capacity.maxScheduleTrackRefsTotal > kHardScheduleTrackRefCapacity ||
        world.capacity.maxSchedulePlacementsPerTrack > 65536 ||
        world.capacity.maxSchedulePlacementsTotal > kHardSchedulePlacementCapacity ||
        world.capacity.maxScheduleMarkersPerPiece > 65536 ||
        world.capacity.maxScheduleMarkersTotal > kHardScheduleMarkerCapacity ||
        world.capacity.maxTrackPieceScheduleDepth > 64 || world.capacity.maxContainerDepth > 64 ||
        world.capacity.maxChildrenPerContainer > kHardContainerChildCapacity ||
        world.capacity.maxAvoidLastN > kHardSelectionAvoidCapacity ||
        world.capacity.maxDiscreteAxes > 32 || world.capacity.maxDiscreteRows > 65536 ||
        world.capacity.maxDiscreteTrieNodes > 1048576 || world.capacity.maxContinuousAxes > 32 ||
        world.capacity.maxContinuousRows > 65536 ||
        world.capacity.maxContinuousCandidates > 65536 ||
        world.capacity.maxContinuousActiveRows > 256 ||
        world.capacity.maxContinuousLiveRows > 512 || world.capacity.maxTransitionRules > 65536 ||
        world.capacity.maxCurvePoints > 8 || world.capacity.maxKnownScheduleLeaves > 64 ||
        world.capacity.maxActionDefinitions == 0 ||
        world.capacity.maxActionDefinitions > kHardActionDefinitionCapacity ||
        world.capacity.maxActionStepsPerDefinition == 0 ||
        world.capacity.maxActionStepsPerDefinition > kMaxActionSteps ||
        world.capacity.maxActionStepsTotal == 0 ||
        world.capacity.maxActionStepsTotal > kHardActionStepCapacity ||
        static_cast<std::uint64_t>(world.capacity.maxActionStepsTotal) >
            static_cast<std::uint64_t>(world.capacity.maxActionDefinitions) *
                world.capacity.maxActionStepsPerDefinition ||
        world.capacity.maxActionTriggerDepth == 0 || world.capacity.maxActionTriggerDepth > 64 ||
        world.capacity.maxActionExpandedWork == 0 || world.capacity.maxActionExpandedWork > 4096 ||
        world.capacity.maxActionExpandedWork >
            kMaxActionSteps * world.capacity.maxActionTriggerDepth) {
        return WorldInstallStatus::Capacity;
    }

    for (std::uint32_t i = 0; i < world.variableCount; ++i) {
        if (!ValidVariableDefinition(world.variables[i]) ||
            (i != 0 && world.variables[i - 1].variableId >= world.variables[i].variableId)) {
            return WorldInstallStatus::InvalidWorld;
        }
    }

    const std::uint64_t requiredMask = detail::ComputeCurrentWorldLocalCapabilityMaskV1(world);
    std::uint64_t sendTotal = 0;
    std::uint64_t layerTotal = 0;
    std::uint64_t nodeTotal = 0;
    std::uint64_t paramTotal = 0;
    std::uint64_t edgeTotal = 0;
    std::vector<std::uint32_t> propertyAuthoredOrders;
    propertyAuthoredOrders.reserve(world.playablePropertyCount);
    for (std::uint32_t i = 0; i < world.playablePropertyCount; ++i) {
        const auto &row = world.playableProperties[i];
        if (!ValidKind(row.contentPropertyKind) || row.stableId == harkenstudio::ObjectId{} ||
            !PointerMatches(row.organizationLayerAncestorCount, row.organizationLayerAncestors) ||
            !PointerMatches(row.structuralAncestorCount, row.structuralAncestors) ||
            !ValidPayload(row.payload) ||
            row.programSemanticFingerprint != ComputePlayablePropertyFingerprintImpl(row, true) ||
            (i != 0 &&
             std::pair(
                 static_cast<std::uint32_t>(world.playableProperties[i - 1].contentPropertyKind),
                 world.playableProperties[i - 1].stableId) >=
                 std::pair(static_cast<std::uint32_t>(row.contentPropertyKind), row.stableId))) {
            return WorldInstallStatus::InvalidWorld;
        }
        if (row.contentPropertyKind == playableproperties::ContentPropertyKindV1::Layer &&
            (row.contentRevision != 0 || row.routeStableId != harkenstudio::ObjectId{} ||
             row.compatibilityFingerprint != 0)) {
            return WorldInstallStatus::InvalidWorld;
        }
        if (row.contentPropertyKind != playableproperties::ContentPropertyKindV1::Layer &&
            row.contentRevision == 0) {
            return WorldInstallStatus::InvalidWorld;
        }
        propertyAuthoredOrders.push_back(row.authoredStableOrder);
        for (std::uint32_t a = 0; a < row.organizationLayerAncestorCount; ++a) {
            const auto &ancestor = row.organizationLayerAncestors[a];
            if (ancestor.layerStableId == harkenstudio::ObjectId{} ||
                ancestor.layerProgramSemanticFingerprint == 0 ||
                (a != 0 &&
                 row.organizationLayerAncestors[a - 1].layerStableId == ancestor.layerStableId)) {
                return WorldInstallStatus::InvalidWorld;
            }
        }
        for (std::uint32_t a = 0; a < row.structuralAncestorCount; ++a) {
            const auto &ancestor = row.structuralAncestors[a];
            if (!ValidStructuralAncestorKind(ancestor.contentPropertyKind) ||
                ancestor.reserved != 0 || ancestor.stableId == harkenstudio::ObjectId{} ||
                ancestor.programSemanticFingerprint == 0 || !ValidPayload(ancestor.payload) ||
                (a != 0 && row.structuralAncestors[a - 1].stableId == ancestor.stableId &&
                 row.structuralAncestors[a - 1].contentPropertyKind ==
                     ancestor.contentPropertyKind)) {
                return WorldInstallStatus::InvalidWorld;
            }
            sendTotal += ancestor.payload.sendCount;
            nodeTotal += ancestor.payload.effectNodeCount;
            edgeTotal += ancestor.payload.effectEdgeCount;
            for (std::uint32_t n = 0; n < ancestor.payload.effectNodeCount; ++n) {
                paramTotal += ancestor.payload.effectNodes[n].paramCount;
            }
        }
        sendTotal += row.payload.sendCount;
        layerTotal += row.organizationLayerAncestorCount;
        nodeTotal += row.payload.effectNodeCount;
        edgeTotal += row.payload.effectEdgeCount;
        for (std::uint32_t n = 0; n < row.payload.effectNodeCount; ++n) {
            paramTotal += row.payload.effectNodes[n].paramCount;
        }
    }
    std::sort(propertyAuthoredOrders.begin(), propertyAuthoredOrders.end());
    if (std::adjacent_find(propertyAuthoredOrders.begin(), propertyAuthoredOrders.end()) !=
        propertyAuthoredOrders.end()) {
        return WorldInstallStatus::InvalidWorld;
    }
    for (std::uint32_t i = 0; i < world.playablePropertyCount; ++i) {
        const auto &row = world.playableProperties[i];
        for (std::uint32_t a = 0; a < row.organizationLayerAncestorCount; ++a) {
            const auto &ancestor = row.organizationLayerAncestors[a];
            const auto *layer = FindProperty(
                world, playableproperties::ContentPropertyKindV1::Layer, ancestor.layerStableId);
            if (layer == nullptr) {
                if (IsPartitionLocal(scope))
                    continue;
                return WorldInstallStatus::InvalidWorld;
            }
            if (layer->programSemanticFingerprint != ancestor.layerProgramSemanticFingerprint) {
                return WorldInstallStatus::InvalidWorld;
            }
            if (layer->organizationLayerAncestorCount != a) {
                return WorldInstallStatus::InvalidWorld;
            }
            for (std::uint32_t prefix = 0; prefix < a; ++prefix) {
                if (layer->organizationLayerAncestors[prefix].layerStableId !=
                        row.organizationLayerAncestors[prefix].layerStableId ||
                    layer->organizationLayerAncestors[prefix].layerProgramSemanticFingerprint !=
                        row.organizationLayerAncestors[prefix].layerProgramSemanticFingerprint) {
                    return WorldInstallStatus::InvalidWorld;
                }
            }
        }
    }
    for (std::uint32_t i = 0; i < world.playableOccurrenceCount; ++i) {
        const auto &row = world.playableOccurrences[i];
        if ((row.parentKind != playableproperties::ContentPropertyKindV1::Container &&
             row.parentKind != playableproperties::ContentPropertyKindV1::Track) ||
            row.parentStableId == harkenstudio::ObjectId{} || row.parentProgramSemanticFingerprint == 0 ||
            !ValidPlayableRef(row.childPlayable) || !ValidPayload(row.payload) ||
            row.occurrenceSemanticFingerprint != ComputePlayableOccurrenceFingerprintV1(row) ||
            (i != 0 &&
             std::tuple(static_cast<std::uint32_t>(world.playableOccurrences[i - 1].parentKind),
                        world.playableOccurrences[i - 1].parentStableId,
                        world.playableOccurrences[i - 1].stableOccurrenceOrdinal) >=
                 std::tuple(static_cast<std::uint32_t>(row.parentKind), row.parentStableId,
                            row.stableOccurrenceOrdinal))) {
            return WorldInstallStatus::InvalidWorld;
        }
        const auto *parent = FindProperty(world, row.parentKind, row.parentStableId);
        const auto *child =
            FindProperty(world, PropertyKind(row.childPlayable.kind), row.childPlayable.stableId);
        if (parent == nullptr ||
            parent->programSemanticFingerprint != row.parentProgramSemanticFingerprint) {
            return WorldInstallStatus::InvalidWorld;
        }
        if (child == nullptr) {
            if (!IsPartitionLocal(scope)) {
                return WorldInstallStatus::InvalidWorld;
            }
        } else if (child->programSemanticFingerprint !=
                       row.childPlayable.expectedProgramSemanticFingerprint ||
                   child->contentRevision != row.childPlayable.contentRevision ||
                   child->routeStableId != row.childPlayable.routeStableId ||
                   child->compatibilityFingerprint != row.childPlayable.compatibilityFingerprint) {
            return WorldInstallStatus::InvalidWorld;
        }
        sendTotal += row.payload.sendCount;
        nodeTotal += row.payload.effectNodeCount;
        edgeTotal += row.payload.effectEdgeCount;
        for (std::uint32_t n = 0; n < row.payload.effectNodeCount; ++n) {
            paramTotal += row.payload.effectNodes[n].paramCount;
        }
    }
    if (sendTotal > world.capacity.maxPlayablePropertySendsTotal ||
        layerTotal > world.capacity.maxPlayablePropertyLayerAncestorRefsTotal ||
        nodeTotal > world.capacity.maxPlayablePropertyEffectNodesTotal ||
        paramTotal > world.capacity.maxPlayablePropertyEffectParamsTotal ||
        edgeTotal > world.capacity.maxPlayablePropertyEffectEdgesTotal) {
        return WorldInstallStatus::Capacity;
    }

    std::uint64_t schedulePlacementTotal = 0;
    std::uint64_t scheduleTrackRefTotal = 0;
    std::uint64_t scheduleMarkerTotal = 0;
    std::vector<harkenstudio::ObjectId> pieceOwnedTrackIds;
    std::vector<harkenstudio::ObjectId> scheduleClipIds;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> scheduleClipGuids;
    std::vector<harkenstudio::ObjectId> scheduleMarkerStableIds;
    pieceOwnedTrackIds.reserve(world.pieceScheduleCount);
    for (std::uint32_t i = 0; i < world.trackScheduleCount; ++i) {
        const auto &row = world.trackSchedules[i];
        if (row.reserved != 0 || row.rootDefinition.kind != PlayableKindV1::Track ||
            !ValidPlayableRef(row.rootDefinition) ||
            !PointerMatches(row.placementCount, row.placements) ||
            row.scheduleSemanticFingerprint == 0 ||
            row.scheduleSemanticFingerprint != ComputeTrackScheduleFingerprintV1(row) ||
            (i != 0 &&
             world.trackSchedules[i - 1].rootDefinition.stableId >= row.rootDefinition.stableId)) {
            return WorldInstallStatus::InvalidWorld;
        }
        if (row.placementCount > world.capacity.maxSchedulePlacementsPerTrack) {
            return WorldInstallStatus::Capacity;
        }
        const auto *definition = FindProperty(
            world, playableproperties::ContentPropertyKindV1::Track, row.rootDefinition.stableId);
        if (definition == nullptr ||
            definition->programSemanticFingerprint !=
                row.rootDefinition.expectedProgramSemanticFingerprint ||
            definition->contentRevision != row.rootDefinition.contentRevision ||
            definition->routeStableId != row.rootDefinition.routeStableId ||
            definition->compatibilityFingerprint != row.rootDefinition.compatibilityFingerprint) {
            return WorldInstallStatus::InvalidWorld;
        }
        std::vector<std::uint32_t> placementOccurrenceOrdinals;
        placementOccurrenceOrdinals.reserve(row.placementCount);
        for (std::uint32_t p = 0; p < row.placementCount; ++p) {
            const auto &placement = row.placements[p];
            if (placement.stablePlacementOrdinal != p || !ValidPlayableRef(placement.content) ||
                placement.content.kind == PlayableKindV1::Track ||
                placement.authoredDuration < 0 ||
                (placement.authoredDuration > 0 &&
                 placement.authoredStart > INT64_MAX -
                     placement.authoredDuration) ||
                placement.authoredFadeIn < 0 ||
                placement.authoredFadeOut < 0 || placement.clipId == harkenstudio::ObjectId{} ||
                (placement.clipGuidHigh == 0 && placement.clipGuidLow == 0) ||
                (p != 0 && placement.authoredStart < row.placements[p - 1].authoredStart)) {
                return WorldInstallStatus::InvalidWorld;
            }
            const auto parentKey = std::tuple(
                static_cast<std::uint32_t>(playableproperties::ContentPropertyKindV1::Track),
                row.rootDefinition.stableId, placement.stableOccurrenceOrdinal);
            const auto *occurrence = std::lower_bound(
                world.playableOccurrences,
                world.playableOccurrences + world.playableOccurrenceCount, parentKey,
                [](const PlayableOccurrencePropertyViewV1 &candidate, const auto &key) {
                    return std::tuple(static_cast<std::uint32_t>(candidate.parentKind),
                                      candidate.parentStableId,
                                      candidate.stableOccurrenceOrdinal) < key;
                });
            if (occurrence == world.playableOccurrences + world.playableOccurrenceCount ||
                std::tuple(static_cast<std::uint32_t>(occurrence->parentKind),
                           occurrence->parentStableId,
                           occurrence->stableOccurrenceOrdinal) != parentKey ||
                occurrence->parentProgramSemanticFingerprint !=
                    row.rootDefinition.expectedProgramSemanticFingerprint ||
                !SameRef(occurrence->childPlayable, placement.content)) {
                return WorldInstallStatus::InvalidWorld;
            }
            placementOccurrenceOrdinals.push_back(placement.stableOccurrenceOrdinal);
            scheduleClipIds.push_back(placement.clipId);
            scheduleClipGuids.emplace_back(placement.clipGuidHigh, placement.clipGuidLow);
        }
        std::sort(placementOccurrenceOrdinals.begin(), placementOccurrenceOrdinals.end());
        if (std::adjacent_find(placementOccurrenceOrdinals.begin(),
                               placementOccurrenceOrdinals.end()) !=
            placementOccurrenceOrdinals.end()) {
            return WorldInstallStatus::InvalidWorld;
        }
        if (world.playableOccurrenceCount == 0) {
            if (!placementOccurrenceOrdinals.empty()) {
                return WorldInstallStatus::InvalidWorld;
            }
        } else {
            const auto parentKey = std::pair(
                static_cast<std::uint32_t>(playableproperties::ContentPropertyKindV1::Track),
                row.rootDefinition.stableId);
            const auto occurrenceBegin = std::lower_bound(
                world.playableOccurrences,
                world.playableOccurrences + world.playableOccurrenceCount, parentKey,
                [](const PlayableOccurrencePropertyViewV1 &candidate, const auto &key) {
                    return std::pair(static_cast<std::uint32_t>(candidate.parentKind),
                                     candidate.parentStableId) < key;
                });
            const auto occurrenceEnd = std::upper_bound(
                occurrenceBegin, world.playableOccurrences + world.playableOccurrenceCount,
                parentKey, [](const auto &key, const PlayableOccurrencePropertyViewV1 &candidate) {
                    return key < std::pair(static_cast<std::uint32_t>(candidate.parentKind),
                                           candidate.parentStableId);
                });
            if (static_cast<std::size_t>(occurrenceEnd - occurrenceBegin) !=
                placementOccurrenceOrdinals.size()) {
                return WorldInstallStatus::InvalidWorld;
            }
            for (std::size_t ordinal = 0; ordinal < placementOccurrenceOrdinals.size(); ++ordinal) {
                if (occurrenceBegin[ordinal].stableOccurrenceOrdinal !=
                    placementOccurrenceOrdinals[ordinal]) {
                    return WorldInstallStatus::InvalidWorld;
                }
            }
        }
        schedulePlacementTotal += row.placementCount;
        if (schedulePlacementTotal > world.capacity.maxSchedulePlacementsTotal) {
            return WorldInstallStatus::Capacity;
        }
    }
    for (std::uint32_t i = 0; i < world.pieceScheduleCount; ++i) {
        const auto &row = world.pieceSchedules[i];
        const auto timeKind = static_cast<std::uint32_t>(row.timeKind);
        if (row.rootDefinition.kind != PlayableKindV1::Piece ||
            !ValidPlayableRef(row.rootDefinition) || row.reserved != 0 ||
            row.scheduleSemanticFingerprint == 0 ||
            row.scheduleSemanticFingerprint != ComputePieceScheduleFingerprintV1(row) ||
            timeKind > static_cast<std::uint32_t>(ScheduleTimeKindV1::MusicalTicks) ||
            row.startPointKind > 1 || !PointerMatches(row.trackRefCount, row.trackRefs) ||
            !PointerMatches(row.markerCount, row.markers) ||
            (i != 0 &&
             world.pieceSchedules[i - 1].rootDefinition.stableId >= row.rootDefinition.stableId)) {
            return WorldInstallStatus::InvalidWorld;
        }
        if ((row.startPointKind == 0 && row.startMarkerId != 0) ||
            (row.startPointKind == 1 && row.startMarkerId == 0) ||
            (row.timeKind == ScheduleTimeKindV1::AbsoluteNanoseconds &&
             (row.microsPerQuarter != 0 || row.meterNumerator != 0 || row.meterDenominator != 0)) ||
            (row.timeKind == ScheduleTimeKindV1::MusicalTicks &&
             (row.microsPerQuarter <= 0 || row.meterNumerator == 0 || row.meterDenominator == 0))) {
            return WorldInstallStatus::InvalidWorld;
        }
        if (row.trackRefCount > world.capacity.maxScheduleTrackRefsPerPiece ||
            row.markerCount > world.capacity.maxScheduleMarkersPerPiece) {
            return WorldInstallStatus::Capacity;
        }
        const auto *definition = FindProperty(
            world, playableproperties::ContentPropertyKindV1::Piece, row.rootDefinition.stableId);
        if (definition == nullptr ||
            definition->programSemanticFingerprint !=
                row.rootDefinition.expectedProgramSemanticFingerprint ||
            definition->contentRevision != row.rootDefinition.contentRevision ||
            definition->routeStableId != row.rootDefinition.routeStableId ||
            definition->compatibilityFingerprint != row.rootDefinition.compatibilityFingerprint) {
            return WorldInstallStatus::InvalidWorld;
        }
        for (std::uint32_t t = 0; t < row.trackRefCount; ++t) {
            const auto &ref = row.trackRefs[t];
            if (ref.reserved != 0 || ref.stableTrackOrdinal != t ||
                ref.trackDefinition.kind != PlayableKindV1::Track ||
                !ValidPlayableRef(ref.trackDefinition) ||
                (t != 0 &&
                 row.trackRefs[t - 1].trackDefinition.stableId >= ref.trackDefinition.stableId)) {
                return WorldInstallStatus::InvalidWorld;
            }
            const auto *track = std::lower_bound(
                world.trackSchedules, world.trackSchedules + world.trackScheduleCount,
                ref.trackDefinition.stableId,
                [](const TrackScheduleProgramViewV1 &candidate, harkenstudio::ObjectId stableId) {
                    return candidate.rootDefinition.stableId < stableId;
                });
            if (track == world.trackSchedules + world.trackScheduleCount ||
                track->rootDefinition.stableId != ref.trackDefinition.stableId) {
                if (IsPartitionLocal(scope)) {
                    pieceOwnedTrackIds.push_back(ref.trackDefinition.stableId);
                    continue;
                }
                return WorldInstallStatus::InvalidWorld;
            }
            if (!SameRef(track->rootDefinition, ref.trackDefinition) ||
                track->scheduleSemanticFingerprint != ref.trackScheduleSemanticFingerprint) {
                return WorldInstallStatus::InvalidWorld;
            }
            pieceOwnedTrackIds.push_back(ref.trackDefinition.stableId);
        }
        for (std::uint32_t c = 0; c < row.markerCount; ++c) {
            const auto &marker = row.markers[c];
            if (marker.stableMarkerId == harkenstudio::ObjectId{} || marker.markerId == 0 || marker.markerKind > 1 ||
                (c != 0 && row.markers[c - 1].markerId == marker.markerId) ||
                (c != 0 && std::pair(row.markers[c - 1].markerId, row.markers[c - 1].stableMarkerId) >=
                               std::pair(marker.markerId, marker.stableMarkerId))) {
                return WorldInstallStatus::InvalidWorld;
            }
            scheduleMarkerStableIds.push_back(marker.stableMarkerId);
        }
        if (row.startPointKind == 1) {
            const auto *marker =
                std::lower_bound(row.markers, row.markers + row.markerCount, row.startMarkerId,
                                 [](const ScheduleMarkerV1 &candidate, std::uint32_t markerId) {
                                     return candidate.markerId < markerId;
                                 });
            if (marker == row.markers + row.markerCount || marker->markerId != row.startMarkerId) {
                return WorldInstallStatus::InvalidWorld;
            }
        }
        scheduleTrackRefTotal += row.trackRefCount;
        scheduleMarkerTotal += row.markerCount;
        if (scheduleTrackRefTotal > world.capacity.maxScheduleTrackRefsTotal ||
            scheduleMarkerTotal > world.capacity.maxScheduleMarkersTotal) {
            return WorldInstallStatus::Capacity;
        }
    }
    std::sort(pieceOwnedTrackIds.begin(), pieceOwnedTrackIds.end());
    if (std::adjacent_find(pieceOwnedTrackIds.begin(), pieceOwnedTrackIds.end()) !=
        pieceOwnedTrackIds.end()) {
        return WorldInstallStatus::InvalidWorld;
    }
    std::sort(scheduleClipIds.begin(), scheduleClipIds.end());
    std::sort(scheduleClipGuids.begin(), scheduleClipGuids.end());
    std::sort(scheduleMarkerStableIds.begin(), scheduleMarkerStableIds.end());
    if (std::adjacent_find(scheduleClipIds.begin(), scheduleClipIds.end()) !=
            scheduleClipIds.end() ||
        std::adjacent_find(scheduleClipGuids.begin(), scheduleClipGuids.end()) !=
            scheduleClipGuids.end() ||
        std::adjacent_find(scheduleMarkerStableIds.begin(), scheduleMarkerStableIds.end()) !=
            scheduleMarkerStableIds.end()) {
        return WorldInstallStatus::InvalidWorld;
    }
    const std::size_t trackNodeCount = world.trackScheduleCount;
    const std::size_t scheduleNodeCount = trackNodeCount + world.pieceScheduleCount;
    std::vector<std::uint8_t> scheduleVisit(scheduleNodeCount, 0);
    std::vector<std::uint32_t> scheduleHeight(scheduleNodeCount, 0);
    bool scheduleCycle = false;
    bool scheduleDepthExceeded = false;
    std::function<bool(std::size_t, std::uint32_t)> visitSchedule = [&](std::size_t node,
                                                                        std::uint32_t pathDepth) {
        if (pathDepth > world.capacity.maxTrackPieceScheduleDepth) {
            scheduleDepthExceeded = true;
            return false;
        }
        if (scheduleVisit[node] == 1) {
            scheduleCycle = true;
            return false;
        }
        if (scheduleVisit[node] == 2) {
            if (scheduleHeight[node] >
                world.capacity.maxTrackPieceScheduleDepth - (pathDepth - 1)) {
                scheduleDepthExceeded = true;
                return false;
            }
            return true;
        }
        scheduleVisit[node] = 1;
        std::uint32_t height = 1;
        const auto visitChild = [&](std::size_t child) {
            if (!visitSchedule(child, pathDepth + 1))
                return false;
            height = std::max(height, 1u + scheduleHeight[child]);
            if (height > world.capacity.maxTrackPieceScheduleDepth) {
                scheduleDepthExceeded = true;
                return false;
            }
            return true;
        };
        if (node < trackNodeCount) {
            const auto &track = world.trackSchedules[node];
            for (std::uint32_t p = 0; p < track.placementCount; ++p) {
                const auto &target = track.placements[p].content;
                if (target.kind != PlayableKindV1::Piece)
                    continue;
                const auto *piece = std::lower_bound(
                    world.pieceSchedules, world.pieceSchedules + world.pieceScheduleCount,
                    target.stableId,
                    [](const PieceScheduleProgramViewV1 &candidate, harkenstudio::ObjectId stableId) {
                        return candidate.rootDefinition.stableId < stableId;
                    });
                if (piece == world.pieceSchedules + world.pieceScheduleCount ||
                    piece->rootDefinition.stableId != target.stableId) {
                    if (IsPartitionLocal(scope))
                        continue;
                    return false;
                }
                if (!visitChild(trackNodeCount +
                                static_cast<std::size_t>(piece - world.pieceSchedules))) {
                    return false;
                }
            }
        } else {
            const auto &piece = world.pieceSchedules[node - trackNodeCount];
            for (std::uint32_t t = 0; t < piece.trackRefCount; ++t) {
                const auto stableId = piece.trackRefs[t].trackDefinition.stableId;
                const auto *track = std::lower_bound(
                    world.trackSchedules, world.trackSchedules + world.trackScheduleCount, stableId,
                    [](const TrackScheduleProgramViewV1 &candidate, harkenstudio::ObjectId id) {
                        return candidate.rootDefinition.stableId < id;
                    });
                if (track == world.trackSchedules + world.trackScheduleCount ||
                    track->rootDefinition.stableId != stableId) {
                    if (IsPartitionLocal(scope))
                        continue;
                    return false;
                }
                if (!visitChild(static_cast<std::size_t>(track - world.trackSchedules))) {
                    return false;
                }
            }
        }
        scheduleVisit[node] = 2;
        scheduleHeight[node] = height;
        return true;
    };
    for (std::size_t node = 0; node < scheduleNodeCount; ++node) {
        if (!visitSchedule(node, 1)) {
            return scheduleDepthExceeded && !scheduleCycle ? WorldInstallStatus::Capacity
                                                           : WorldInstallStatus::InvalidWorld;
        }
    }
    const auto trackDefinitionCount = static_cast<std::uint32_t>(std::count_if(
        world.playableProperties, world.playableProperties + world.playablePropertyCount,
        [](const PlayablePropertyProgramViewV1 &row) {
            return row.contentPropertyKind == playableproperties::ContentPropertyKindV1::Track;
        }));
    const auto pieceDefinitionCount = static_cast<std::uint32_t>(std::count_if(
        world.playableProperties, world.playableProperties + world.playablePropertyCount,
        [](const PlayablePropertyProgramViewV1 &row) {
            return row.contentPropertyKind == playableproperties::ContentPropertyKindV1::Piece;
        }));
    if ((trackDefinitionCount != world.trackScheduleCount ||
         pieceDefinitionCount != world.pieceScheduleCount)) {
        return WorldInstallStatus::InvalidWorld;
    }

    bool advancedTransitionRequested = false;
    bool transitionConsumerUnavailable = false;
    std::uint64_t transitionRuleCount = 0;
    for (std::uint32_t i = 0; i < world.transitionCount; ++i) {
        const auto &transition = world.transitions[i];
        const TransitionProgramClassV1 classification = ClassifyTransitionProgramShape(transition);
        if (classification == TransitionProgramClassV1::Invalid ||
            transition.maxContainerDescent > world.capacity.maxContainerDepth ||
            transition.maxOwnerAscent > world.capacity.maxContainerDepth ||
            transition.downwardPathStepCount > world.capacity.maxContainerDepth ||
            (i != 0 && world.transitions[i - 1].ownerContainerStableId >=
                           transition.ownerContainerStableId)) {
            return WorldInstallStatus::InvalidWorld;
        }
        advancedTransitionRequested =
            advancedTransitionRequested || classification == TransitionProgramClassV1::Advanced;
        transitionRuleCount += transition.ruleCount;
        if (transitionRuleCount > world.capacity.maxTransitionRules) {
            return WorldInstallStatus::Capacity;
        }
    }

    std::vector<std::uint32_t> containerAuthoredOrders;
    containerAuthoredOrders.reserve(world.containerCount);
    for (std::uint32_t i = 0; i < world.containerCount; ++i) {
        const auto &row = world.containers[i];
        bool containerValid = false;
        containerValid = ValidCurrentContainer(row);
        if (!containerValid ||
            (i != 0 && world.containers[i - 1].containerStableId >= row.containerStableId)) {
            return WorldInstallStatus::InvalidWorld;
        }
        if (row.mode == ContainerModeV1::VariableControl &&
            !ValidVariableControlGrids(world, row, scope)) {
            return WorldInstallStatus::InvalidWorld;
        }
        if (row.transitionProgramIndex != UINT32_MAX &&
            row.transitionProgramIndex >= world.transitionCount) {
            return WorldInstallStatus::InvalidWorld;
        }
        if (row.transitionProgramIndex != UINT32_MAX &&
            world.transitions[row.transitionProgramIndex].ownerContainerStableId !=
                row.containerStableId) {
            return WorldInstallStatus::InvalidWorld;
        }
        if (row.transitionProgramIndex != UINT32_MAX) {
            const auto &transition = world.transitions[row.transitionProgramIndex];
            for (std::uint32_t ruleIndex = 0; ruleIndex < transition.ruleCount; ++ruleIndex) {
                const auto &rule = transition.rules[ruleIndex];
                if ((rule.sourceAny == 0 && !TransitionEndpointIsDirectChild(row, rule.source)) ||
                    (rule.targetAny == 0 && !TransitionEndpointIsDirectChild(row, rule.target))) {
                    return WorldInstallStatus::InvalidWorld;
                }
            }
            if (!IsPartitionLocal(scope)) {
                for (std::uint32_t c = 0; c < row.childCount; ++c) {
                    const auto &child = row.children[c];
                    transitionConsumerUnavailable =
                        transitionConsumerUnavailable ||
                        (row.mode == ContainerModeV1::VariableControl
                             ? !VariableControlTransitionEndpointSupported(world, row, child)
                             : !TransitionEndpointHasBoundedResolvedVoices(world, child.playable));
                }
            }
        }
        if (row.childCount > world.capacity.maxChildrenPerContainer ||
            row.avoidLastN > world.capacity.maxAvoidLastN ||
            row.discreteAxisCount > world.capacity.maxDiscreteAxes ||
            row.discreteRowCount > world.capacity.maxDiscreteRows ||
            row.continuousAxisCount > world.capacity.maxContinuousAxes ||
            row.continuousRowCount > world.capacity.maxContinuousRows ||
            row.continuousRowCount > world.capacity.maxContinuousCandidates ||
            row.maxActiveRows > world.capacity.maxContinuousActiveRows ||
            row.maxLiveRows > world.capacity.maxContinuousLiveRows) {
            return WorldInstallStatus::Capacity;
        }

        for (std::uint32_t c = 0; c < row.curveCount; ++c) {
            if (!ValidCurve(row.curves[c]) ||
                row.curves[c].curve.pointCount > world.capacity.maxCurvePoints ||
                (c != 0 && row.curves[c - 1].curveLocalId >= row.curves[c].curveLocalId)) {
                return row.curves[c].curve.pointCount > world.capacity.maxCurvePoints
                           ? WorldInstallStatus::Capacity
                           : WorldInstallStatus::InvalidWorld;
            }
        }
        if (!ValidContainerCurveReferences(world, row, true)) {
            return WorldInstallStatus::InvalidWorld;
        }
        const auto *definition = FindProperty(
            world, playableproperties::ContentPropertyKindV1::Container, row.containerStableId);
        if (definition == nullptr) {
            return WorldInstallStatus::InvalidWorld;
        }
        if (row.programSemanticFingerprint != definition->programSemanticFingerprint) {
            return WorldInstallStatus::InvalidWorld;
        }
        for (std::uint32_t c = 0; c < row.childCount; ++c) {
            const auto &child = row.children[c];
            bool isSchedule = false;
            TrackPieceScheduleKeyV1 key{};
            isSchedule = child.timingHint.kind == ChildTimingHintKindV2::ScheduleReference;
            if (isSchedule) {
                key = child.timingHint.payload.schedule;
            }
            if (!isSchedule) {
                continue;
            }
            if (key.kind == PlayableKindV1::Track) {
                const auto *schedule = std::lower_bound(
                    world.trackSchedules, world.trackSchedules + world.trackScheduleCount,
                    key.stableId,
                    [](const TrackScheduleProgramViewV1 &candidate, harkenstudio::ObjectId stableId) {
                        return candidate.rootDefinition.stableId < stableId;
                    });
                if (schedule == world.trackSchedules + world.trackScheduleCount ||
                    schedule->rootDefinition.stableId != key.stableId) {
                    if (IsPartitionLocal(scope))
                        continue;
                    return WorldInstallStatus::InvalidWorld;
                }
                if (schedule->scheduleSemanticFingerprint != key.scheduleSemanticFingerprint) {
                    return WorldInstallStatus::InvalidWorld;
                }
            } else {
                const auto *schedule = std::lower_bound(
                    world.pieceSchedules, world.pieceSchedules + world.pieceScheduleCount,
                    key.stableId,
                    [](const PieceScheduleProgramViewV1 &candidate, harkenstudio::ObjectId stableId) {
                        return candidate.rootDefinition.stableId < stableId;
                    });
                if (schedule == world.pieceSchedules + world.pieceScheduleCount ||
                    schedule->rootDefinition.stableId != key.stableId) {
                    if (IsPartitionLocal(scope))
                        continue;
                    return WorldInstallStatus::InvalidWorld;
                }
                if (schedule->scheduleSemanticFingerprint != key.scheduleSemanticFingerprint) {
                    return WorldInstallStatus::InvalidWorld;
                }
            }
        }
        containerAuthoredOrders.push_back(row.authoredStableOrder);
        std::vector<std::uint32_t> childAuthoredOrders;
        childAuthoredOrders.reserve(row.childCount);
        for (std::uint32_t c = 0; c < row.childCount; ++c) {
            childAuthoredOrders.push_back(row.children[c].authoredStableOrder);
        }
        std::sort(childAuthoredOrders.begin(), childAuthoredOrders.end());
        if (std::adjacent_find(childAuthoredOrders.begin(), childAuthoredOrders.end()) !=
            childAuthoredOrders.end()) {
            return WorldInstallStatus::InvalidWorld;
        }
        if (world.playableOccurrenceCount == 0) {
            if (row.childCount != 0) {
                return WorldInstallStatus::InvalidWorld;
            }
            continue;
        }
        const auto parentKey = std::pair(
            static_cast<std::uint32_t>(playableproperties::ContentPropertyKindV1::Container),
            row.containerStableId);
        const auto *occurrenceBegin =
            std::lower_bound(world.playableOccurrences,
                             world.playableOccurrences + world.playableOccurrenceCount, parentKey,
                             [](const PlayableOccurrencePropertyViewV1 &candidate,
                                const std::pair<std::uint32_t, harkenstudio::ObjectId> &key) {
                                 return std::pair(static_cast<std::uint32_t>(candidate.parentKind),
                                                  candidate.parentStableId) < key;
                             });
        const auto *occurrenceEnd = std::upper_bound(
            occurrenceBegin, world.playableOccurrences + world.playableOccurrenceCount, parentKey,
            [](const std::pair<std::uint32_t, harkenstudio::ObjectId> &key,
               const PlayableOccurrencePropertyViewV1 &candidate) {
                return key < std::pair(static_cast<std::uint32_t>(candidate.parentKind),
                                       candidate.parentStableId);
            });
        for (std::uint32_t c = 0; c < row.childCount; ++c) {
            const auto *occurrence = std::lower_bound(
                occurrenceBegin, occurrenceEnd, row.children[c].stableChildOrdinal,
                [](const PlayableOccurrencePropertyViewV1 &candidate, std::uint32_t ordinal) {
                    return candidate.stableOccurrenceOrdinal < ordinal;
                });
            if (occurrence == occurrenceEnd ||
                occurrence->stableOccurrenceOrdinal != row.children[c].stableChildOrdinal ||
                !SameRef(occurrence->childPlayable, row.children[c].playable)) {
                return WorldInstallStatus::InvalidWorld;
            }
        }
        const auto occurrenceCount = static_cast<std::uint32_t>(occurrenceEnd - occurrenceBegin);
        if (occurrenceCount != row.childCount) {
            return WorldInstallStatus::InvalidWorld;
        }
    }
    if (world.transitionCount != 0 && world.containerCount == 0) {
        return WorldInstallStatus::InvalidWorld;
    }
    for (std::uint32_t transitionIndex = 0; transitionIndex < world.transitionCount;
         ++transitionIndex) {
        const auto *owner =
            std::lower_bound(world.containers, world.containers + world.containerCount,
                             world.transitions[transitionIndex].ownerContainerStableId,
                             [](const auto &container, harkenstudio::ObjectId stableId) {
                                 return container.containerStableId < stableId;
                             });
        if (owner == world.containers + world.containerCount ||
            owner->containerStableId != world.transitions[transitionIndex].ownerContainerStableId ||
            owner->transitionProgramIndex != transitionIndex) {
            return WorldInstallStatus::InvalidWorld;
        }
    }
    std::sort(containerAuthoredOrders.begin(), containerAuthoredOrders.end());
    if (std::adjacent_find(containerAuthoredOrders.begin(), containerAuthoredOrders.end()) !=
        containerAuthoredOrders.end()) {
        return WorldInstallStatus::InvalidWorld;
    }
    std::vector<std::uint8_t> containerState(world.containerCount, 0);
    bool containerDepthExceeded = false;
    std::function<bool(std::uint32_t, std::uint32_t)> visitContainer = [&](std::uint32_t index,
                                                                           std::uint32_t depth) {
        if (depth > world.capacity.maxContainerDepth) {
            containerDepthExceeded = true;
            return false;
        }
        if (containerState[index] == 1)
            return false;
        if (containerState[index] == 2)
            return true;
        containerState[index] = 1;
        const auto &container = world.containers[index];
        for (std::uint32_t child = 0; child < container.childCount; ++child) {
            if (container.children[child].playable.kind != PlayableKindV1::Container) {
                continue;
            }
            const harkenstudio::ObjectId childId = container.children[child].playable.stableId;
            const auto *found = std::lower_bound(
                world.containers, world.containers + world.containerCount, childId,
                [](const auto &row, harkenstudio::ObjectId id) { return row.containerStableId < id; });
            if (found == world.containers + world.containerCount ||
                found->containerStableId != childId) {
                if (IsPartitionLocal(scope))
                    continue;
                return false;
            }
            if (!visitContainer(static_cast<std::uint32_t>(found - world.containers), depth + 1)) {
                return false;
            }
        }
        containerState[index] = 2;
        return true;
    };
    for (std::uint32_t i = 0; i < world.containerCount; ++i) {
        if (!visitContainer(i, 1)) {
            return containerDepthExceeded ? WorldInstallStatus::Capacity
                                          : WorldInstallStatus::InvalidWorld;
        }
    }
    std::uint64_t actionStepTotal = 0;
    for (std::uint32_t i = 0; i < world.actionCount; ++i) {
        const auto &action = world.actions[i];
        if (action.actionStableId == harkenstudio::ObjectId{} || action.programSemanticFingerprint == 0 ||
            action.stepCount == 0 ||
            action.stepCount > world.capacity.maxActionStepsPerDefinition ||
            !PointerMatches(action.stepCount, action.steps) ||
            (i != 0 && world.actions[i - 1].actionStableId >= action.actionStableId)) {
            return WorldInstallStatus::InvalidWorld;
        }
        actionStepTotal += action.stepCount;
        if (actionStepTotal > world.capacity.maxActionStepsTotal ||
            (action.directFeatureMask & ~15u) != 0 || (action.transitiveFeatureMask & ~15u) != 0 ||
            action.expandedStepOccurrenceCount == 0 || action.expandedFiringCount == 0) {
            return WorldInstallStatus::InvalidWorld;
        }
        std::uint32_t directFeatureMask = 0;
        for (std::uint32_t stepIndex = 0; stepIndex < action.stepCount; ++stepIndex) {
            const auto &step = action.steps[stepIndex];
            const auto validActionTime = [](const AuthoredTimeRationalV1 &time) {
                return time.domain == AuthoredTimeDomainV1::AbsoluteNanoseconds &&
                       time.numerator >= 0 && time.denominator == 1;
            };
            const auto zeroActionTime = [&](const AuthoredTimeRationalV1 &time) {
                return validActionTime(time) && time.numerator == 0;
            };
            const bool zeroTime = zeroActionTime(step.delay) && zeroActionTime(step.fade) &&
                                  zeroActionTime(step.cooldown);
            if (step.authoredEventIndex != stepIndex || step.target.reserved != 0 ||
                step.target.stableId == harkenstudio::ObjectId{} || step.quantizeReference.reserved != 0 ||
                step.reserved0 != 0) {
                return WorldInstallStatus::InvalidWorld;
            }
            if (step.opcode == ActionOpcodeV1::Play || step.opcode == ActionOpcodeV1::Stop) {
                const bool stop = step.opcode == ActionOpcodeV1::Stop;
                if (step.target.expectedProgramSemanticFingerprint == 0
                    || static_cast<std::uint32_t>(step.target.kind) >
                        static_cast<std::uint32_t>(ActionTargetKindV1::Piece) ||
                    step.scope != ActionScopeV1::Global || step.valueBits != 0 ||
                    !validActionTime(step.delay) || !validActionTime(step.fade) ||
                    !validActionTime(step.cooldown) || (!stop && !zeroActionTime(step.fade)) ||
                    !ActionQuantizeTimingSupportedV1(
                        step.opcode, step.quantizeTiming) ||
                    (step.cooldown.numerator == 0 &&
                     step.cooldownScope != ActionCooldownScopeV1::Global) ||
                    (step.cooldownScope != ActionCooldownScopeV1::Global &&
                     step.cooldownScope != ActionCooldownScopeV1::PerEmitter) ||
                    (step.ifSilent != ActionIfSilentV1::Immediate &&
                     step.ifSilent != ActionIfSilentV1::Drop)) {
                    return WorldInstallStatus::InvalidWorld;
                }
                const auto propertyKind = static_cast<playableproperties::ContentPropertyKindV1>(
                    static_cast<std::uint32_t>(step.target.kind) + 1u);
                const auto *property = FindActionTargetProperty(world, step.target.stableId);
                if (property == nullptr) {
                    if (IsKnownNonPlayableActionTarget(world, step.target.stableId)
                        || (!stop && !IsPartitionLocal(scope))) {
                        return WorldInstallStatus::InvalidWorld;
                    }
                } else if (property->contentPropertyKind != propertyKind
                           || property->programSemanticFingerprint !=
                           step.target.expectedProgramSemanticFingerprint) {
                    return WorldInstallStatus::InvalidWorld;
                }
                const bool immediate = step.quantizeTiming == ActionQuantizeTimingV1::Immediate;
                if (immediate) {
                    if (step.quantizeReference.kind != ActionTargetKindV1::Sound ||
                        step.quantizeReference.stableId != harkenstudio::ObjectId{} ||
                        step.quantizeReference.expectedProgramSemanticFingerprint != 0 ||
                        step.ifSilent != ActionIfSilentV1::Immediate) {
                        return WorldInstallStatus::InvalidWorld;
                    }
                } else if (step.quantizeReference.stableId == harkenstudio::ObjectId{}) {
                    if (!stop || step.quantizeReference.kind != ActionTargetKindV1::Sound ||
                        step.quantizeReference.expectedProgramSemanticFingerprint != 0) {
                        return WorldInstallStatus::InvalidWorld;
                    }
                } else {
                    if (step.quantizeReference.expectedProgramSemanticFingerprint == 0
                        || static_cast<std::uint32_t>(step.quantizeReference.kind) >
                        static_cast<std::uint32_t>(ActionTargetKindV1::Piece)) {
                        return WorldInstallStatus::InvalidWorld;
                    }
                    const auto referenceKind =
                        static_cast<playableproperties::ContentPropertyKindV1>(
                            static_cast<std::uint32_t>(step.quantizeReference.kind) + 1u);
                    const auto *reference =
                        FindActionTargetProperty(world, step.quantizeReference.stableId);
                    if (reference == nullptr) {
                        if (IsKnownNonPlayableActionTarget(world, step.quantizeReference.stableId)
                            || (!stop && !IsPartitionLocal(scope))) {
                            return WorldInstallStatus::InvalidWorld;
                        }
                    } else if (reference->contentPropertyKind != referenceKind
                               || reference->programSemanticFingerprint !=
                               step.quantizeReference.expectedProgramSemanticFingerprint) {
                        return WorldInstallStatus::InvalidWorld;
                    }
                }
                if (step.delay.numerator != 0)
                    directFeatureMask |= 1u;
                if (!immediate || step.quantizeReference.stableId != harkenstudio::ObjectId{}) {
                    directFeatureMask |= 2u;
                }
                if (step.cooldown.numerator != 0)
                    directFeatureMask |= 4u;
                if (step.cooldownScope != ActionCooldownScopeV1::Global ||
                    step.ifSilent != ActionIfSilentV1::Immediate) {
                    directFeatureMask |= 8u;
                }
                continue;
            }
            if (step.opcode == ActionOpcodeV1::PostAction) {
                if (step.target.kind != ActionTargetKindV1::Action ||
                    step.target.expectedProgramSemanticFingerprint == 0 ||
                    !validActionTime(step.delay) || !zeroActionTime(step.fade) ||
                    !validActionTime(step.cooldown) ||
                    !ActionQuantizeTimingSupportedV1(
                        step.opcode, step.quantizeTiming) ||
                    (step.cooldown.numerator == 0 &&
                     step.cooldownScope != ActionCooldownScopeV1::Global) ||
                    (step.cooldownScope != ActionCooldownScopeV1::Global &&
                     step.cooldownScope != ActionCooldownScopeV1::PerEmitter) ||
                    (step.ifSilent != ActionIfSilentV1::Immediate &&
                     step.ifSilent != ActionIfSilentV1::Drop) ||
                    step.scope != ActionScopeV1::Global || step.valueBits != 0) {
                    return WorldInstallStatus::InvalidWorld;
                }
                const bool immediate = step.quantizeTiming == ActionQuantizeTimingV1::Immediate;
                if (immediate) {
                    if (step.quantizeReference.kind != ActionTargetKindV1::Sound ||
                        step.quantizeReference.stableId != harkenstudio::ObjectId{} ||
                        step.quantizeReference.expectedProgramSemanticFingerprint != 0 ||
                        step.ifSilent != ActionIfSilentV1::Immediate) {
                        return WorldInstallStatus::InvalidWorld;
                    }
                } else {
                    if (step.quantizeReference.stableId == harkenstudio::ObjectId{} ||
                        static_cast<std::uint32_t>(step.quantizeReference.kind) >
                            static_cast<std::uint32_t>(ActionTargetKindV1::Piece)) {
                        return WorldInstallStatus::InvalidWorld;
                    }
                    const auto referenceKind =
                        static_cast<playableproperties::ContentPropertyKindV1>(
                            static_cast<std::uint32_t>(step.quantizeReference.kind) + 1u);
                    const auto *reference =
                        FindProperty(world, referenceKind, step.quantizeReference.stableId);
                    if (reference == nullptr) {
                        if (!IsPartitionLocal(scope)) {
                            return WorldInstallStatus::InvalidWorld;
                        }
                    } else if (reference->programSemanticFingerprint !=
                               step.quantizeReference.expectedProgramSemanticFingerprint) {
                        return WorldInstallStatus::InvalidWorld;
                    }
                }
                if (step.delay.numerator != 0)
                    directFeatureMask |= 1u;
                if (!immediate || step.quantizeReference.stableId != harkenstudio::ObjectId{}) {
                    directFeatureMask |= 2u;
                }
                if (step.cooldown.numerator != 0)
                    directFeatureMask |= 4u;
                if (step.cooldownScope != ActionCooldownScopeV1::Global ||
                    step.ifSilent != ActionIfSilentV1::Immediate) {
                    directFeatureMask |= 8u;
                }
                continue;
            }
            if (step.opcode == ActionOpcodeV1::Reset) {
                if (step.target.kind != ActionTargetKindV1::Container ||
                    !validActionTime(step.delay) || !zeroActionTime(step.fade) ||
                    !zeroActionTime(step.cooldown) ||
                    step.quantizeReference.kind != ActionTargetKindV1::Sound ||
                    step.quantizeReference.stableId != harkenstudio::ObjectId{} ||
                    step.quantizeReference.expectedProgramSemanticFingerprint != 0 ||
                    step.quantizeTiming != ActionQuantizeTimingV1::Immediate ||
                    step.cooldownScope != ActionCooldownScopeV1::Global ||
                    step.ifSilent != ActionIfSilentV1::Immediate ||
                    (step.scope != ActionScopeV1::Global && step.scope != ActionScopeV1::Emitter) ||
                    step.valueBits != 0) {
                    return WorldInstallStatus::InvalidWorld;
                }
                const auto *property =
                    FindProperty(world, playableproperties::ContentPropertyKindV1::Container,
                                 step.target.stableId);
                if (property == nullptr) {
                    if (!IsPartitionLocal(scope)) {
                        return WorldInstallStatus::InvalidWorld;
                    }
                } else if (property->programSemanticFingerprint !=
                           step.target.expectedProgramSemanticFingerprint) {
                    return WorldInstallStatus::InvalidWorld;
                }
                if (step.delay.numerator != 0)
                    directFeatureMask |= 1u;
                continue;
            }
            if (step.opcode != ActionOpcodeV1::SetVariable ||
                step.target.kind != ActionTargetKindV1::Variable ||
                step.target.expectedProgramSemanticFingerprint != 0 ||
                !validActionTime(step.delay) || !zeroActionTime(step.fade) ||
                !zeroActionTime(step.cooldown) ||
                step.quantizeReference.kind != ActionTargetKindV1::Sound ||
                step.quantizeReference.stableId != harkenstudio::ObjectId{} ||
                step.quantizeReference.expectedProgramSemanticFingerprint != 0 ||
                step.quantizeTiming != ActionQuantizeTimingV1::Immediate ||
                step.cooldownScope != ActionCooldownScopeV1::Global ||
                step.ifSilent != ActionIfSilentV1::Immediate) {
                return WorldInstallStatus::InvalidWorld;
            }
            const auto *variable = FindVariable(world, step.target.stableId);
            const float value = std::bit_cast<float>(step.valueBits);
            if (!ValidCanonicalF32(value) ||
                (step.scope != ActionScopeV1::Global && step.scope != ActionScopeV1::Emitter)) {
                return WorldInstallStatus::InvalidWorld;
            }
            if (variable == nullptr) {
                if (!IsPartitionLocal(scope)) {
                    return WorldInstallStatus::InvalidWorld;
                }
            } else if (value < variable->rangeMin || value > variable->rangeMax ||
                       (variable->kind == VariableKindV1::Step &&
                        (value < -16777216.0f || value > 16777216.0f ||
                         value != std::floor(value)))) {
                return WorldInstallStatus::InvalidWorld;
            }
            if (step.delay.numerator != 0)
                directFeatureMask |= 1u;
        }
        if (directFeatureMask != action.directFeatureMask) {
            return WorldInstallStatus::InvalidWorld;
        }
        if (ComputeActionProgramFingerprintV1(action) != action.programSemanticFingerprint) {
            return WorldInstallStatus::InvalidWorld;
        }
    }
    std::vector<std::uint8_t> actionState(world.actionCount, 0);
    std::vector<std::uint8_t> actionComplete(world.actionCount, 0);
    std::vector<std::uint32_t> actionDepth(world.actionCount, 0);
    std::function<bool(std::uint32_t, std::uint32_t)> visitAction =
        [&](std::uint32_t index, std::uint32_t traversalDepth) {
            if (actionState[index] == 1)
                return false;
            if (actionState[index] == 2)
                return true;
            actionState[index] = 1;
            const auto &action = world.actions[index];
            std::uint64_t expandedSteps = action.stepCount;
            std::uint64_t expandedFirings = 1;
            std::uint32_t depth = 1;
            std::uint32_t transitiveMask = action.directFeatureMask;
            bool complete = true;
            for (std::uint32_t stepIndex = 0; stepIndex < action.stepCount; ++stepIndex) {
                const auto &step = action.steps[stepIndex];
                if (step.opcode != ActionOpcodeV1::PostAction)
                    continue;
                const auto *child = std::lower_bound(
                    world.actions, world.actions + world.actionCount, step.target.stableId,
                    [](const ActionProgramViewV1 &candidate, harkenstudio::ObjectId stableId) {
                        return candidate.actionStableId < stableId;
                    });
                if (child == world.actions + world.actionCount ||
                    child->actionStableId != step.target.stableId) {
                    if (IsPartitionLocal(scope)) {
                        complete = false;
                        continue;
                    }
                    return false;
                }
                if (child->programSemanticFingerprint !=
                    step.target.expectedProgramSemanticFingerprint) {
                    return false;
                }
                const std::uint32_t childIndex = static_cast<std::uint32_t>(child - world.actions);
                if (traversalDepth >= world.capacity.maxActionTriggerDepth ||
                    !visitAction(childIndex, traversalDepth + 1u)) {
                    return false;
                }
                if (actionComplete[childIndex] == 0) {
                    complete = false;
                    continue;
                }
                expandedSteps += child->expandedStepOccurrenceCount;
                expandedFirings += child->expandedFiringCount;
                transitiveMask |= child->transitiveFeatureMask;
                depth = std::max(depth, actionDepth[childIndex] + 1u);
                if (expandedSteps > UINT32_MAX || expandedFirings > UINT32_MAX) {
                    return false;
                }
            }
            const bool aggregateMatches =
                !complete || (expandedSteps == action.expandedStepOccurrenceCount &&
                              expandedFirings == action.expandedFiringCount &&
                              transitiveMask == action.transitiveFeatureMask);
            if (!aggregateMatches ||
                action.expandedStepOccurrenceCount > world.capacity.maxActionExpandedWork ||
                depth > world.capacity.maxActionTriggerDepth) {
                return false;
            }
            actionDepth[index] = depth;
            actionComplete[index] = complete ? 1 : 0;
            actionState[index] = 2;
            return true;
        };
    for (std::uint32_t i = 0; i < world.actionCount; ++i) {
        if (!visitAction(i, 1))
            return WorldInstallStatus::InvalidWorld;
    }
    if (world.identity.capabilityMask != requiredMask) {
        return WorldInstallStatus::InvalidFormat;
    }
    const auto expectedFingerprint = ComputeCurrentWorldFingerprintV5(world);
    if (world.identity.runtimeSemanticFingerprint == 0 ||
        world.identity.runtimeSemanticFingerprint != expectedFingerprint) {
        return WorldInstallStatus::InvalidWorld;
    }
    if (advancedTransitionRequested || transitionConsumerUnavailable) {
        return WorldInstallStatus::CapabilityUnavailable;
    }
    constexpr auto supportedMask = kWorldCapabilityUnit1ActionMask;
    if ((requiredMask & ~supportedMask) != 0) {
        return WorldInstallStatus::CapabilityUnavailable;
    }
    return WorldInstallStatus::Ok;
}

template <class WorldView>
std::uint64_t ComputeCurrentWorldLocalCapabilityMaskImpl(const WorldView &world) noexcept {
    std::uint64_t mask = world.variableCount != 0 ? kWorldCapabilityVariableDefinitions : 0;
    if (world.playablePropertyCount != 0 || world.playableOccurrenceCount != 0) {
        mask |= kWorldCapabilityPlayableProperties;
    }
    if (world.containers != nullptr) {
        for (std::uint32_t i = 0; i < world.containerCount; ++i) {
            const auto &row = world.containers[i];
            switch (row.mode) {
            case ContainerModeV1::Random:
                mask |= row.randomDiscipline == RandomDisciplineV1::Draw
                            ? kWorldCapabilityRandomDraw
                            : kWorldCapabilityRandomRotation;
                break;
            case ContainerModeV1::Sequence:
                mask |= kWorldCapabilitySequence;
                break;
            case ContainerModeV1::RandomQueue:
                mask |= kWorldCapabilityRandomQueue;
                break;
            case ContainerModeV1::SequenceQueue:
                mask |= kWorldCapabilitySequenceQueue;
                break;
            case ContainerModeV1::VariableControl:
                mask |= kWorldCapabilityVariableControl;
                break;
            }
        }
    }
    if (world.transitionCount != 0) {
        mask |= kWorldCapabilityTransition;
    }
    if (world.actionCount != 0) {
        mask |= kWorldCapabilityActionDefinitions;
    }
    return mask;
}

std::uint64_t detail::ComputeCurrentWorldLocalCapabilityMaskV1(
    const ContainerWorldInstallViewV1 &world) noexcept {
    return ComputeCurrentWorldLocalCapabilityMaskImpl(world);
}

WorldInstallStatus ValidateCurrentWorldV1(const ContainerWorldInstallViewV1 &world) noexcept {
    try {
        if (world.sourceBodyVintage != kCurrentWorldShippingBodyVintage)
            return WorldInstallStatus::InvalidFormat;
        if (!ValidateCurrentWorldSectionGroupsV1(world))
            return WorldInstallStatus::InvalidWorld;
        return ValidateCurrentWorldImpl(world, ValidationScope::ExactWorld);
    } catch (...) {
        return WorldInstallStatus::Capacity;
    }
}

WorldInstallStatus detail::ValidateCurrentWorldPartitionLocalV1(
    const ContainerWorldInstallViewV1 &world) noexcept {
    try {
        if (world.sourceBodyVintage != kCurrentWorldShippingBodyVintage)
            return WorldInstallStatus::InvalidFormat;
        if (!ValidateCurrentWorldSectionGroupsV1(world))
            return WorldInstallStatus::InvalidWorld;
        return ValidateCurrentWorldImpl(world, ValidationScope::PartitionLocal);
    } catch (...) {
        return WorldInstallStatus::Capacity;
    }
}

} // namespace harkenstudiobuild
