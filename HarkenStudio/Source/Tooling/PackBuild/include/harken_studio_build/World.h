#ifndef HARKEN_STUDIO_BUILD_CURRENT_WORLD_H
#define HARKEN_STUDIO_BUILD_CURRENT_WORLD_H

#include "harken_studio_build/PublicValues.h"
#include "harken_studio_build/Properties.h"
#include "harken_studio_build/WorldBodyVintage.h"
#include "harken_studio_build/Curve.h"

#include <array>
#include <cstdint>

namespace harkenstudiobuild {

struct VariableDefinitionV1;

using WorldGeneration = std::uint64_t;

inline constexpr WorldGeneration kNoWorldGeneration = 0;
inline constexpr std::uint64_t kWorldCapabilityVariableDefinitions = UINT64_C(1) << 0;
inline constexpr std::uint64_t kWorldCapabilityPlayableProperties = UINT64_C(1) << 1;
inline constexpr std::uint64_t kWorldCapabilityRandomDraw = UINT64_C(1) << 2;
inline constexpr std::uint64_t kWorldCapabilityRandomRotation = UINT64_C(1) << 3;
inline constexpr std::uint64_t kWorldCapabilitySequence = UINT64_C(1) << 4;
inline constexpr std::uint64_t kWorldCapabilityRandomQueue = UINT64_C(1) << 5;
inline constexpr std::uint64_t kWorldCapabilitySequenceQueue = UINT64_C(1) << 6;
inline constexpr std::uint64_t kWorldCapabilityVariableControl = UINT64_C(1) << 7;
inline constexpr std::uint64_t kWorldCapabilityTransition = UINT64_C(1) << 8;
inline constexpr std::uint64_t kWorldCapabilityActionDefinitions = UINT64_C(1) << 9;
inline constexpr std::uint64_t kWorldCapabilityKnownMask =
    (UINT64_C(1) << 10) - 1;
inline constexpr std::uint64_t kWorldCapabilityUnit2Mask =
    kWorldCapabilityVariableDefinitions |
    kWorldCapabilityPlayableProperties |
    kWorldCapabilityRandomDraw |
    kWorldCapabilityRandomRotation |
    kWorldCapabilitySequence;
inline constexpr std::uint64_t kWorldCapabilityUnit3Mask =
    kWorldCapabilityUnit2Mask |
    kWorldCapabilityRandomQueue |
    kWorldCapabilitySequenceQueue;
inline constexpr std::uint64_t kWorldCapabilityUnit4Mask =
    kWorldCapabilityUnit3Mask |
    kWorldCapabilityVariableControl;
inline constexpr std::uint64_t kWorldCapabilityUnit5V1Mask =
    kWorldCapabilityUnit4Mask |
    kWorldCapabilityTransition;
inline constexpr std::uint64_t kWorldCapabilityUnit1ActionMask =
    kWorldCapabilityUnit5V1Mask |
    kWorldCapabilityActionDefinitions;

inline constexpr std::uint32_t kContainerProgramCapacity = 64;
inline constexpr std::uint32_t kPlayablePropertyProgramCapacity = 1024;
inline constexpr std::uint32_t kPlayableOccurrencePropertyCapacity = 4096;
inline constexpr std::uint32_t kPlayablePropertyChildRowCapacity = 4096;
inline constexpr std::uint32_t kPlayablePropertyEffectParamCapacity = 16384;
inline constexpr std::uint32_t kPlayablePropertyEffectEdgeCapacity = 8192;
inline constexpr std::uint32_t kTrackScheduleProgramCapacity = 1024;
inline constexpr std::uint32_t kPieceScheduleProgramCapacity = 1024;
inline constexpr std::uint32_t kScheduleTrackRefCapacity = 4096;
inline constexpr std::uint32_t kSchedulePlacementCapacity = 4096;
inline constexpr std::uint32_t kScheduleMarkerCapacity = 4096;
inline constexpr std::uint32_t kHardVariableDefinitionCapacity = 65536;
inline constexpr std::uint32_t kHardContainerProgramCapacity = 65536;
inline constexpr std::uint32_t kHardPlayablePropertyProgramCapacity = 1048576;
inline constexpr std::uint32_t kHardPlayableOccurrencePropertyCapacity = 1048576;
inline constexpr std::uint32_t kHardPlayablePropertyChildRowCapacity = 1048576;
inline constexpr std::uint32_t kHardPlayablePropertyEffectParamCapacity = 4194304;
inline constexpr std::uint32_t kHardPlayablePropertyEffectEdgeCapacity = 2097152;
inline constexpr std::uint32_t kHardTrackScheduleProgramCapacity = 1048576;
inline constexpr std::uint32_t kHardPieceScheduleProgramCapacity = 1048576;
inline constexpr std::uint32_t kHardScheduleTrackRefCapacity = 1048576;
inline constexpr std::uint32_t kHardSchedulePlacementCapacity = 1048576;
inline constexpr std::uint32_t kHardScheduleMarkerCapacity = 1048576;
inline constexpr std::uint32_t kMaxActionSteps = 64;
inline constexpr std::uint32_t kHardActionDefinitionCapacity = 65536;
inline constexpr std::uint32_t kHardActionStepCapacity = 4194304;

enum class PlayableKindV1 : std::uint32_t {
    Sound = 0,
    Container = 1,
    Track = 2,
    Piece = 3,
};

struct PlayableRefV1 {
    PlayableKindV1 kind = PlayableKindV1::Sound;
    std::uint32_t reserved = 0;
    harkenstudio::ObjectId stableId{};
    std::uint64_t expectedProgramSemanticFingerprint = 0;
    std::uint64_t contentRevision = 0;
    harkenstudio::ObjectId routeStableId{};
    std::uint64_t compatibilityFingerprint = 0;
};

enum class TransitionSourceExitV1 : std::uint32_t {
    Immediate = 0,
    NaturalEnd = 1,
    NextBeat = 2,
    NextBar = 3,
    NextMarker = 4,
    ExitMarker = 5,
};

enum class TransitionTargetEntryV1 : std::uint32_t {
    FromEntryMarker = 0,
    SamePositionAsIncumbent = 1,
};

struct TransitionFadeV1 {
    std::uint8_t active = 0;
    std::array<std::uint8_t, 3> reserved{};
    std::uint32_t durationFrames = 0;
    std::uint32_t curveLocalId = UINT32_MAX;
};

struct TransitionRuleViewV1 {
    std::uint32_t stableRuleOrdinal = 0;
    std::uint8_t sourceAny = 0;
    std::uint8_t targetAny = 0;
    std::uint8_t bridgePresent = 0;
    std::uint8_t reserved = 0;
    PlayableRefV1 source{};
    PlayableRefV1 target{};
    TransitionSourceExitV1 sourceExit = TransitionSourceExitV1::Immediate;
    TransitionTargetEntryV1 targetEntry =
        TransitionTargetEntryV1::FromEntryMarker;
    PlayableRefV1 bridge{};
    TransitionFadeV1 sourceFade{};
    TransitionFadeV1 bridgeFadeIn{};
    TransitionFadeV1 bridgeFadeOut{};
    TransitionFadeV1 targetFade{};
    std::uint64_t requiredClockFingerprint = 0;
    harkenstudio::ObjectId requiredMarkerStableId{};
    std::uint64_t samePositionCompatibilityFingerprint = 0;
    std::uint32_t authoredStableOrder = 0;
    std::uint32_t reserved2 = 0;
};

struct PropertyScalarV1 {
    std::uint64_t bits = 0;
    harkenstudio::ObjectId objectId{};
};

struct SendV1 {
    std::uint32_t stableSendOrdinal = 0;
    std::uint32_t reserved = 0;
    harkenstudio::ObjectId targetBusId{};
    float sendLevel = 0.0f;
    std::uint32_t reserved2 = 0;
};

struct CurveV1 {
    std::uint32_t curveLocalId = 0;
    Curve curve{};
};

struct EffectParamV1 {
    std::uint32_t paramId = 0;
    std::uint32_t valueBits = 0;
};

struct EffectNodeV1 {
    std::uint32_t nodeId = 0;
    std::uint32_t effectType = 0;
    std::uint32_t paramCount = 0;
    const EffectParamV1* params = nullptr;
    harkenstudio::ObjectId effectId{};
};

struct EffectEdgeV1 {
    std::uint32_t fromNodeId = 0;
    std::uint32_t toNodeId = 0;
};

struct LayerAncestorRefV1 {
    harkenstudio::ObjectId layerStableId{};
    std::uint64_t layerProgramSemanticFingerprint = 0;
};

struct PlayablePropertyPayloadViewV1 {
    std::uint64_t presentMask = 0;
    std::array<PropertyScalarV1, 17> values{};
    std::uint32_t sendCount = 0;
    const SendV1* sends = nullptr;
    std::uint32_t effectNodeCount = 0;
    const EffectNodeV1* effectNodes = nullptr;
    std::uint32_t effectEdgeCount = 0;
    const EffectEdgeV1* effectEdges = nullptr;
};

inline constexpr std::uint64_t kResolvedSectionGroupPropertyMaskV1 = UINT64_C(0x12fe0);

bool ValidateResolvedSectionGroupPayloadV1(
    const PlayablePropertyPayloadViewV1& payload) noexcept;
struct StructuralAncestorRefV1 {
    playableproperties::ContentPropertyKindV1 contentPropertyKind =
        playableproperties::ContentPropertyKindV1::Layer;
    std::uint32_t reserved = 0;
    harkenstudio::ObjectId stableId{};
    std::uint64_t programSemanticFingerprint = 0;
    PlayablePropertyPayloadViewV1 payload{};
};

struct PlayablePropertyProgramViewV1 {
    playableproperties::ContentPropertyKindV1 contentPropertyKind =
        playableproperties::ContentPropertyKindV1::Sound;
    std::uint32_t authoredStableOrder = 0;
    harkenstudio::ObjectId stableId{};
    std::uint64_t programSemanticFingerprint = 0;
    std::uint64_t contentRevision = 0;
    harkenstudio::ObjectId routeStableId{};
    std::uint64_t compatibilityFingerprint = 0;
    std::uint32_t organizationLayerAncestorCount = 0;
    const LayerAncestorRefV1* organizationLayerAncestors = nullptr;
    std::uint32_t structuralAncestorCount = 0;
    const StructuralAncestorRefV1* structuralAncestors = nullptr;
    PlayablePropertyPayloadViewV1 payload{};
};

struct PlayableOccurrencePropertyViewV1 {
    playableproperties::ContentPropertyKindV1 parentKind =
        playableproperties::ContentPropertyKindV1::Track;
    std::uint32_t stableOccurrenceOrdinal = 0;
    harkenstudio::ObjectId parentStableId{};
    std::uint64_t parentProgramSemanticFingerprint = 0;
    PlayableRefV1 childPlayable{};
    std::uint64_t occurrenceSemanticFingerprint = 0;
    PlayablePropertyPayloadViewV1 payload{};
};

enum class ContainerModeV1 : std::uint32_t {
    Random = 0,
    RandomQueue = 1,
    Sequence = 2,
    SequenceQueue = 3,
    VariableControl = 4,
};

enum class RandomDisciplineV1 : std::uint32_t { Draw = 0, Rotation = 1 };
enum class MemoryScopeV1 : std::uint32_t { Emitter = 0, Global = 1 };
enum class RetriggerPolicyV1 : std::uint32_t {
    CreateNewInstance = 0,
    RestartExisting = 1,
};
enum class ResetPolicyV1 : std::uint32_t { ExplicitTypedResetOnly = 0 };
enum class SequenceEndPolicyV1 : std::uint32_t { Wrap = 0, StopAtEnd = 1 };
enum class QueueCycleV1 : std::uint32_t { Once = 0, Loop = 1 };
enum class QueueSpliceV1 : std::uint32_t { Butt = 0, MarkerAligned = 1 };
enum class QueueStopPolicyV1 : std::uint32_t {
    AdvanceQueue = 0,
    AbortOwningQueue = 1,
};
enum class AuthoredCompletionClassV1 : std::uint32_t {
    FiniteMediaCandidate = 0,
    BoundedTrackPieceCandidate = 1,
    SignalOnly = 2,
};
enum class AuthoredTimeDomainV1 : std::uint32_t {
    SourceFrames = 0,
    AbsoluteNanoseconds = 1,
    MusicalTicks = 2,
    NaturalMediaEnd = 3,
};

struct AuthoredTimeRationalV1 {
    AuthoredTimeDomainV1 domain = AuthoredTimeDomainV1::SourceFrames;
    std::int64_t numerator = 0;
    std::uint32_t denominator = 0;
};

enum class ActionOpcodeV1 : std::uint32_t {
    Play = 0,
    Stop = 1,
    PostAction = 2,
    SetVariable = 3,
    Reset = 4,
};

enum class ActionTargetKindV1 : std::uint32_t {
    Sound = 0,
    Container = 1,
    Track = 2,
    Piece = 3,
    Action = 4,
    Variable = 5,
    Bus = 6,
};

enum class ActionQuantizeTimingV1 : std::uint32_t {
    Immediate = 0,
    NextBeat = 1,
    NextBar = 2,
    NextMarker = 3,
    NaturalEnd = 4,
    CurrentLoopEnd = 5,
    ExitMarker = 6,
};

constexpr bool ActionQuantizeTimingSupportedV1(
    ActionOpcodeV1 opcode, ActionQuantizeTimingV1 timing) noexcept
{
    const auto value = static_cast<std::uint32_t>(timing);
    if (opcode == ActionOpcodeV1::Stop)
        return value <= static_cast<std::uint32_t>(ActionQuantizeTimingV1::ExitMarker);
    if (opcode == ActionOpcodeV1::Play)
        return value <= static_cast<std::uint32_t>(ActionQuantizeTimingV1::NextMarker);
    return timing == ActionQuantizeTimingV1::Immediate;
}

enum class ActionScopeV1 : std::uint32_t {
    Global = 0,
    Emitter = 1,
};

enum class ActionCooldownScopeV1 : std::uint32_t {
    Global = 0,
    PerEmitter = 1,
};

enum class ActionIfSilentV1 : std::uint32_t {
    Immediate = 0,
    Drop = 1,
};

struct ActionObjectRefV1 {
    ActionTargetKindV1 kind = ActionTargetKindV1::Sound;
    std::uint32_t reserved = 0;
    harkenstudio::ObjectId stableId{};
    std::uint64_t expectedProgramSemanticFingerprint = 0;
};

struct ActionStepProgramViewV1 {
    ActionOpcodeV1 opcode = ActionOpcodeV1::Play;
    std::uint32_t authoredEventIndex = 0;
    ActionObjectRefV1 target{};
    AuthoredTimeRationalV1 delay{};
    AuthoredTimeRationalV1 fade{};
    ActionObjectRefV1 quantizeReference{};
    ActionQuantizeTimingV1 quantizeTiming =
        ActionQuantizeTimingV1::Immediate;
    ActionCooldownScopeV1 cooldownScope = ActionCooldownScopeV1::Global;
    ActionIfSilentV1 ifSilent = ActionIfSilentV1::Immediate;
    ActionScopeV1 scope = ActionScopeV1::Global;
    AuthoredTimeRationalV1 cooldown{};
    std::uint32_t valueBits = 0;
    std::uint32_t reserved0 = 0;
};

struct ActionProgramViewV1 {
    harkenstudio::ObjectId actionStableId{};
    std::uint64_t programSemanticFingerprint = 0;
    const ActionStepProgramViewV1* steps = nullptr;
    std::uint32_t stepCount = 0;
    std::uint32_t directFeatureMask = 0;
    std::uint32_t transitiveFeatureMask = 0;
    std::uint32_t expandedStepOccurrenceCount = 0;
    std::uint32_t expandedFiringCount = 0;
};

struct StableAuthoredPathStepV1 {
    PlayableRefV1 playable{};
    std::uint32_t stableChildOrdinal = 0;
    std::uint32_t reserved = 0;
    harkenstudio::ObjectId trackId{};
    harkenstudio::ObjectId clipId{};
    std::uint64_t clipGuidHigh = 0;
    std::uint64_t clipGuidLow = 0;
};

struct TransitionProgramViewV1 {
    harkenstudio::ObjectId ownerContainerStableId{};
    std::uint32_t ruleCount = 0;
    std::uint32_t defaultRuleIndex = 0;
    std::uint32_t downwardPathStepCount = 0;
    std::uint32_t maxContainerDescent = 0;
    std::uint32_t maxOwnerAscent = 0;
    std::uint32_t watchFallback = 0;
    const StableAuthoredPathStepV1* downwardPathSteps = nullptr;
    const TransitionRuleViewV1* rules = nullptr;
};

enum class TransitionProgramClassV1 : std::uint32_t {
    Basic = 0,
    Advanced = 1,
    Invalid = 2,
};

struct AuthoredCompletionLeafV1 {
    std::uint32_t stableLeafOrdinal = 0;
    std::uint32_t reserved = 0;
    PlayableRefV1 content{};
    AuthoredTimeRationalV1 startOffset{};
    AuthoredTimeRationalV1 duration{};
    std::uint32_t loopCount = 0;
    std::uint8_t loopRegionPresent = 0;
    std::array<std::uint8_t, 3> reserved2{};
    AuthoredTimeRationalV1 loopStart{};
    AuthoredTimeRationalV1 loopEnd{};
    std::uint32_t pathStepCount = 0;
    std::uint32_t reserved3 = 0;
    const StableAuthoredPathStepV1* pathSteps = nullptr;
};

enum class ScheduleTimeKindV1 : std::uint32_t {
    AbsoluteNanoseconds = 0,
    MusicalTicks = 1,
};

struct ScheduleClipPlacementV1 {
    std::uint32_t stablePlacementOrdinal = 0;
    std::uint32_t stableOccurrenceOrdinal = 0;
    PlayableRefV1 content{};
    std::int64_t authoredStart = 0;
    std::int64_t authoredDuration = 0;
    std::int64_t authoredFadeIn = 0;
    std::int64_t authoredFadeOut = 0;
    harkenstudio::ObjectId clipId{};
    std::uint64_t clipGuidHigh = 0;
    std::uint64_t clipGuidLow = 0;
};

struct TrackScheduleProgramViewV1 {
    PlayableRefV1 rootDefinition{};
    std::uint64_t scheduleSemanticFingerprint = 0;
    std::uint32_t placementCount = 0;
    std::uint32_t reserved = 0;
    const ScheduleClipPlacementV1* placements = nullptr;
};

struct ScheduleTrackRefV1 {
    PlayableRefV1 trackDefinition{};
    std::uint64_t trackScheduleSemanticFingerprint = 0;
    std::uint32_t stableTrackOrdinal = 0;
    std::uint32_t reserved = 0;
};

struct ScheduleMarkerV1 {
    harkenstudio::ObjectId stableMarkerId{};
    std::uint32_t markerId = 0;
    std::uint32_t markerKind = 0;
    std::int64_t position = 0;
};

struct PieceScheduleProgramViewV1 {
    PlayableRefV1 rootDefinition{};
    std::uint64_t scheduleSemanticFingerprint = 0;
    ScheduleTimeKindV1 timeKind = ScheduleTimeKindV1::AbsoluteNanoseconds;
    std::uint32_t startPointKind = 0;
    std::uint32_t startMarkerId = 0;
    std::uint32_t reserved = 0;
    std::int64_t microsPerQuarter = 0;
    std::uint32_t meterNumerator = 0;
    std::uint32_t meterDenominator = 0;
    std::uint32_t trackRefCount = 0;
    std::uint32_t markerCount = 0;
    const ScheduleTrackRefV1* trackRefs = nullptr;
    const ScheduleMarkerV1* markers = nullptr;
};

struct TrackPieceScheduleKeyV1 {
    PlayableKindV1 kind = PlayableKindV1::Track;
    std::uint32_t reserved = 0;
    harkenstudio::ObjectId stableId{};
    std::uint64_t scheduleSemanticFingerprint = 0;
};

enum class ChildTimingHintKindV2 : std::uint32_t {
    Unknown = 0,
    InlineSound = 1,
    ScheduleReference = 2,
};

struct InlineSoundTimingHintV2 {
    AuthoredTimeRationalV1 startOffset{};
    AuthoredTimeRationalV1 duration{};
    std::uint32_t loopCount = 0;
    std::uint8_t loopRegionPresent = 0;
    std::array<std::uint8_t, 3> reserved{};
    AuthoredTimeRationalV1 loopStart{};
    AuthoredTimeRationalV1 loopEnd{};
};

union ChildTimingHintPayloadV2 {
    InlineSoundTimingHintV2 inlineSound;
    TrackPieceScheduleKeyV1 schedule;

    constexpr ChildTimingHintPayloadV2() : inlineSound{} {}
};

struct ChildTimingHintV2 {
    ChildTimingHintKindV2 kind = ChildTimingHintKindV2::Unknown;
    std::uint32_t reserved = 0;
    ChildTimingHintPayloadV2 payload{};
};

inline ChildTimingHintV2 MakeScheduleReferenceTimingHint(
    PlayableKindV1 kind, harkenstudio::ObjectId stableId,
    std::uint64_t scheduleSemanticFingerprint) noexcept {
    ChildTimingHintV2 hint{};
    hint.kind = ChildTimingHintKindV2::ScheduleReference;
    hint.payload.schedule = TrackPieceScheduleKeyV1{
        kind, 0, stableId, scheduleSemanticFingerprint};
    return hint;
}

struct ContainerChildProgramViewV2 {
    PlayableRefV1 playable{};
    std::uint32_t stableChildOrdinal = 0;
    std::uint32_t authoredStableOrder = 0;
    std::uint32_t frequencyWeight = 0;
    std::uint32_t orderWeight = 0;
    QueueStopPolicyV1 queueStopPolicy = QueueStopPolicyV1::AbortOwningQueue;
    std::uint32_t reserved = 0;
    ChildTimingHintV2 timingHint{};
};

enum class DiscreteCellKindV1 : std::uint32_t {
    Exact = 0,
    Wildcard = 1,
};

enum class ContinuousCellKindV1 : std::uint32_t {
    Unconstrained = 0,
    Interval = 1,
};

struct GridAxisV1 {
    std::uint32_t stableAxisOrdinal = 0;
    std::uint32_t authoredColumnOrder = 0;
    harkenstudio::ObjectId variableId{};
};

struct DiscreteGridCellV1 {
    DiscreteCellKindV1 kind = DiscreteCellKindV1::Wildcard;
    std::int32_t exactValue = 0;
};

struct DiscreteGridRowViewV1 {
    std::uint32_t stableRowOrdinal = 0;
    std::uint32_t authoredStableOrder = 0;
    std::uint32_t childStableOrdinal = 0;
    std::uint32_t cellCount = 0;
    const DiscreteGridCellV1* cells = nullptr;
};

struct ContinuousGridCellV1 {
    ContinuousCellKindV1 kind = ContinuousCellKindV1::Unconstrained;
    float lower = 0.0f;
    float upper = 0.0f;
    std::uint8_t lowerInclusive = 0;
    std::uint8_t upperInclusive = 0;
    std::uint16_t reserved = 0;
    std::uint32_t leftCurveLocalId = UINT32_MAX;
    std::uint32_t rightCurveLocalId = UINT32_MAX;
};

struct ContinuousGridRowViewV1 {
    std::uint32_t stableRowOrdinal = 0;
    std::uint32_t authoredStableOrder = 0;
    std::uint32_t childStableOrdinal = 0;
    std::uint32_t cellCount = 0;
    const ContinuousGridCellV1* cells = nullptr;
};

struct ContainerProgramViewV3 {
    harkenstudio::ObjectId containerStableId{};
    std::uint64_t programSemanticFingerprint = 0;
    ContainerModeV1 mode = ContainerModeV1::Random;
    RandomDisciplineV1 randomDiscipline = RandomDisciplineV1::Draw;
    MemoryScopeV1 memoryScope = MemoryScopeV1::Emitter;
    RetriggerPolicyV1 retriggerPolicy = RetriggerPolicyV1::CreateNewInstance;
    ResetPolicyV1 resetPolicy = ResetPolicyV1::ExplicitTypedResetOnly;
    SequenceEndPolicyV1 sequenceEndPolicy = SequenceEndPolicyV1::Wrap;
    QueueCycleV1 queueCycle = QueueCycleV1::Once;
    std::uint32_t queueCycleCount = 0;
    QueueSpliceV1 queueSplice = QueueSpliceV1::Butt;
    std::uint8_t resumePosition = 0;
    std::uint32_t avoidLastN = 0;
    std::uint32_t initialSequenceCursor = 0;
    std::uint32_t discreteTiming = 0;
    std::uint32_t continuousTiming = 1;
    std::uint32_t continuousNormalization = 0;
    std::uint32_t evaluationPeriodFrames = 0;
    std::uint32_t envelopeFrames = 0;
    std::uint32_t maxActiveRows = 0;
    std::uint32_t maxLiveRows = 0;
    std::uint32_t childCount = 0;
    const ContainerChildProgramViewV2* children = nullptr;
    std::uint32_t curveCount = 0;
    const CurveV1* curves = nullptr;
    std::uint32_t discreteAxisCount = 0;
    const GridAxisV1* discreteAxes = nullptr;
    std::uint32_t discreteRowCount = 0;
    const DiscreteGridRowViewV1* discreteRows = nullptr;
    std::uint32_t continuousAxisCount = 0;
    const GridAxisV1* continuousAxes = nullptr;
    std::uint32_t continuousRowCount = 0;
    const ContinuousGridRowViewV1* continuousRows = nullptr;
    std::uint32_t transitionProgramIndex = UINT32_MAX;
    std::uint32_t authoredStableOrder = 0;
};

struct WorldFormatIdentityV1 {
    std::uint32_t schema = 1;
    std::uint32_t compiler = 1;
    std::uint32_t selectionAlgorithm = 1;
    std::uint32_t gridAlgorithm = 1;
    std::uint32_t transitionAlgorithm = 1;
    std::uint32_t publicationProtocol = 1;
};

struct WorldIdentityV1 {
    std::uint64_t runtimeSemanticFingerprint = 0;
    WorldFormatIdentityV1 format{};
    std::uint64_t capabilityMask = 0;
    harkenstudio::ObjectId projectId{};
};

struct WorldCapacitySummaryV1 {
    std::uint32_t maxVariableDefinitions = 256;
    std::uint32_t maxContainerPrograms = kContainerProgramCapacity;
    std::uint32_t maxPlayablePropertyPrograms = kPlayablePropertyProgramCapacity;
    std::uint32_t maxPlayableOccurrencePropertyRows =
        kPlayableOccurrencePropertyCapacity;
    std::uint32_t maxPlayablePropertySendsTotal = kPlayablePropertyChildRowCapacity;
    std::uint32_t maxPlayablePropertyLayerAncestorRefsTotal =
        kPlayablePropertyChildRowCapacity;
    std::uint32_t maxPlayablePropertyEffectNodesTotal =
        kPlayablePropertyChildRowCapacity;
    std::uint32_t maxPlayablePropertyEffectParamsTotal =
        kPlayablePropertyEffectParamCapacity;
    std::uint32_t maxPlayablePropertyEffectEdgesTotal =
        kPlayablePropertyEffectEdgeCapacity;
    std::uint32_t maxContainerDepth = 8;
    std::uint32_t maxChildrenPerContainer = 64;
    std::uint32_t maxAvoidLastN = 8;
    std::uint32_t maxDiscreteAxes = 8;
    std::uint32_t maxDiscreteRows = 256;
    std::uint32_t maxDiscreteTrieNodes = 2048;
    std::uint32_t maxContinuousAxes = 8;
    std::uint32_t maxContinuousRows = 128;
    std::uint32_t maxContinuousCandidates = 128;
    std::uint32_t maxContinuousActiveRows = 16;
    std::uint32_t maxContinuousLiveRows = 32;
    std::uint32_t maxTransitionRules = 256;
    std::uint32_t maxCurvePoints = 8;
    std::uint32_t maxKnownScheduleLeaves = 16;
    std::uint32_t maxTrackSchedulePrograms = kTrackScheduleProgramCapacity;
    std::uint32_t maxPieceSchedulePrograms = kPieceScheduleProgramCapacity;
    std::uint32_t maxScheduleTrackRefsPerPiece = 64;
    std::uint32_t maxScheduleTrackRefsTotal = kScheduleTrackRefCapacity;
    std::uint32_t maxSchedulePlacementsPerTrack = kSchedulePlacementCapacity;
    std::uint32_t maxSchedulePlacementsTotal = kSchedulePlacementCapacity;
    std::uint32_t maxScheduleMarkersPerPiece = 256;
    std::uint32_t maxScheduleMarkersTotal = kScheduleMarkerCapacity;
    std::uint32_t maxTrackPieceScheduleDepth = 8;
    std::uint32_t maxActionDefinitions = 64;
    std::uint32_t maxActionStepsPerDefinition = kMaxActionSteps;
    std::uint32_t maxActionStepsTotal = 4096;
    std::uint32_t maxActionTriggerDepth = 8;
    std::uint32_t maxActionExpandedWork = 512;
};

template <class Values, class Visitor>
void VisitWorldCapacitySummaryV1(Values& values, Visitor& visitor) {
    visitor.U32(values.maxVariableDefinitions);
    visitor.U32(values.maxContainerPrograms);
    visitor.U32(values.maxPlayablePropertyPrograms);
    visitor.U32(values.maxPlayableOccurrencePropertyRows);
    visitor.U32(values.maxPlayablePropertySendsTotal);
    visitor.U32(values.maxPlayablePropertyLayerAncestorRefsTotal);
    visitor.U32(values.maxPlayablePropertyEffectNodesTotal);
    visitor.U32(values.maxPlayablePropertyEffectParamsTotal);
    visitor.U32(values.maxPlayablePropertyEffectEdgesTotal);
    visitor.U32(values.maxContainerDepth);
    visitor.U32(values.maxChildrenPerContainer);
    visitor.U32(values.maxAvoidLastN);
    visitor.U32(values.maxDiscreteAxes);
    visitor.U32(values.maxDiscreteRows);
    visitor.U32(values.maxDiscreteTrieNodes);
    visitor.U32(values.maxContinuousAxes);
    visitor.U32(values.maxContinuousRows);
    visitor.U32(values.maxContinuousCandidates);
    visitor.U32(values.maxContinuousActiveRows);
    visitor.U32(values.maxContinuousLiveRows);
    visitor.U32(values.maxTransitionRules);
    visitor.U32(values.maxCurvePoints);
    visitor.U32(values.maxKnownScheduleLeaves);
    visitor.U32(values.maxTrackSchedulePrograms);
    visitor.U32(values.maxPieceSchedulePrograms);
    visitor.U32(values.maxScheduleTrackRefsPerPiece);
    visitor.U32(values.maxScheduleTrackRefsTotal);
    visitor.U32(values.maxSchedulePlacementsPerTrack);
    visitor.U32(values.maxSchedulePlacementsTotal);
    visitor.U32(values.maxScheduleMarkersPerPiece);
    visitor.U32(values.maxScheduleMarkersTotal);
    visitor.U32(values.maxTrackPieceScheduleDepth);
    visitor.U32(values.maxActionDefinitions);
    visitor.U32(values.maxActionStepsPerDefinition);
    visitor.U32(values.maxActionStepsTotal);
    visitor.U32(values.maxActionTriggerDepth);
    visitor.U32(values.maxActionExpandedWork);
}

struct ContainerWorldInstallViewV1 {
    WorldIdentityV1 identity{};
    const VariableDefinitionV1* variables = nullptr;
    std::uint32_t variableCount = 0;
    const PlayablePropertyProgramViewV1* playableProperties = nullptr;
    std::uint32_t playablePropertyCount = 0;
    const PlayableOccurrencePropertyViewV1* playableOccurrences = nullptr;
    std::uint32_t playableOccurrenceCount = 0;
    const TrackScheduleProgramViewV1* trackSchedules = nullptr;
    std::uint32_t trackScheduleCount = 0;
    const PieceScheduleProgramViewV1* pieceSchedules = nullptr;
    std::uint32_t pieceScheduleCount = 0;
    const ContainerProgramViewV3* containers = nullptr;
    std::uint32_t containerCount = 0;
    const TransitionProgramViewV1* transitions = nullptr;
    std::uint32_t transitionCount = 0;
    const ActionProgramViewV1* actions = nullptr;
    std::uint32_t actionCount = 0;
    WorldCapacitySummaryV1 capacity{};
    std::uint32_t sourceBodyVintage = kCurrentWorldShippingBodyVintage;
};

bool ValidateCurrentWorldSectionGroupsV1(
    const ContainerWorldInstallViewV1& world) noexcept;

PlayablePropertyPayloadViewV1 MakeStructuralAncestorPayloadV1(
    const PlayablePropertyPayloadViewV1& payload) noexcept;

struct CurrentWorldV3PrefixValues {
    std::uint32_t schema = 0;
    std::uint32_t compiler = 0;
    std::uint32_t selectionAlgorithm = 0;
    std::uint32_t gridAlgorithm = 0;
    std::uint32_t transitionAlgorithm = 0;
    std::uint32_t publicationProtocol = 0;
    std::uint64_t runtimeSemanticFingerprint = 0;
    std::uint64_t capabilityMask = 0;
    std::uint32_t variableCount = 0;
    std::uint32_t playablePropertyProgramCount = 0;
    std::uint32_t playableOccurrencePropertyCount = 0;
    std::uint32_t playablePropertySendCount = 0;
    std::uint32_t reservedChildCount1 = 0;
    std::uint32_t reservedChildCount2 = 0;
    std::uint32_t playablePropertyLayerAncestorRefCount = 0;
    std::uint32_t playablePropertyEffectNodeCount = 0;
    std::uint32_t playablePropertyEffectParamCount = 0;
    std::uint32_t playablePropertyEffectEdgeCount = 0;
    std::uint32_t trackScheduleCount = 0;
    std::uint32_t pieceScheduleCount = 0;
    std::uint32_t scheduleTrackRefCount = 0;
    std::uint32_t schedulePlacementCount = 0;
    std::uint32_t scheduleMarkerCount = 0;
    std::uint32_t containerCount = 0;
    std::uint32_t transitionProgramCount = 0;
    std::uint32_t maxContainerDepth = 0;
    std::uint32_t maxChildrenPerContainer = 0;
    std::uint32_t maxAvoidLastN = 0;
    std::uint32_t maxDiscreteAxes = 0;
    std::uint32_t maxDiscreteRows = 0;
    std::uint32_t maxDiscreteTrieNodes = 0;
    std::uint32_t maxContinuousAxes = 0;
    std::uint32_t maxContinuousRows = 0;
    std::uint32_t maxContinuousCandidates = 0;
    std::uint32_t maxContinuousActiveRows = 0;
    std::uint32_t maxContinuousLiveRows = 0;
    std::uint32_t maxTransitionRules = 0;
    std::uint32_t maxCurvePoints = 0;
    std::uint32_t maxKnownScheduleLeaves = 0;
    std::uint32_t maxTrackSchedulePrograms = 0;
    std::uint32_t maxPieceSchedulePrograms = 0;
    std::uint32_t maxScheduleTrackRefsPerPiece = 0;
    std::uint32_t maxScheduleTrackRefsTotal = 0;
    std::uint32_t maxSchedulePlacementsPerTrack = 0;
    std::uint32_t maxSchedulePlacementsTotal = 0;
    std::uint32_t maxScheduleMarkersPerPiece = 0;
    std::uint32_t maxScheduleMarkersTotal = 0;
    std::uint32_t maxTrackPieceScheduleDepth = 0;
};

template <class Values, class Visitor>
void VisitCurrentWorldV3Prefix(Values& values, Visitor& visitor) {
    visitor.U32(values.schema);
    visitor.U32(values.compiler);
    visitor.U32(values.selectionAlgorithm);
    visitor.U32(values.gridAlgorithm);
    visitor.U32(values.transitionAlgorithm);
    visitor.U32(values.publicationProtocol);
    visitor.U64(values.runtimeSemanticFingerprint);
    visitor.U64(values.capabilityMask);
    visitor.U32(values.variableCount);
    visitor.U32(values.playablePropertyProgramCount);
    visitor.U32(values.playableOccurrencePropertyCount);
    visitor.U32(values.playablePropertySendCount);
    visitor.U32(values.reservedChildCount1);
    visitor.U32(values.reservedChildCount2);
    visitor.U32(values.playablePropertyLayerAncestorRefCount);
    visitor.U32(values.playablePropertyEffectNodeCount);
    visitor.U32(values.playablePropertyEffectParamCount);
    visitor.U32(values.playablePropertyEffectEdgeCount);
    visitor.U32(values.trackScheduleCount);
    visitor.U32(values.pieceScheduleCount);
    visitor.U32(values.scheduleTrackRefCount);
    visitor.U32(values.schedulePlacementCount);
    visitor.U32(values.scheduleMarkerCount);
    visitor.U32(values.containerCount);
    visitor.U32(values.transitionProgramCount);
    visitor.U32(values.maxContainerDepth);
    visitor.U32(values.maxChildrenPerContainer);
    visitor.U32(values.maxAvoidLastN);
    visitor.U32(values.maxDiscreteAxes);
    visitor.U32(values.maxDiscreteRows);
    visitor.U32(values.maxDiscreteTrieNodes);
    visitor.U32(values.maxContinuousAxes);
    visitor.U32(values.maxContinuousRows);
    visitor.U32(values.maxContinuousCandidates);
    visitor.U32(values.maxContinuousActiveRows);
    visitor.U32(values.maxContinuousLiveRows);
    visitor.U32(values.maxTransitionRules);
    visitor.U32(values.maxCurvePoints);
    visitor.U32(values.maxKnownScheduleLeaves);
    visitor.U32(values.maxTrackSchedulePrograms);
    visitor.U32(values.maxPieceSchedulePrograms);
    visitor.U32(values.maxScheduleTrackRefsPerPiece);
    visitor.U32(values.maxScheduleTrackRefsTotal);
    visitor.U32(values.maxSchedulePlacementsPerTrack);
    visitor.U32(values.maxSchedulePlacementsTotal);
    visitor.U32(values.maxScheduleMarkersPerPiece);
    visitor.U32(values.maxScheduleMarkersTotal);
    visitor.U32(values.maxTrackPieceScheduleDepth);
}

bool TryBuildCurrentWorldV3Prefix(
    const ContainerWorldInstallViewV1& world,
    bool zeroRuntimeSemanticFingerprint,
    CurrentWorldV3PrefixValues* out) noexcept;

enum class WorldInstallStatus : std::uint32_t {
    Ok = 0,
    InvalidFormat = 1,
    InvalidWorld = 2,
    Capacity = 3,
    CapabilityUnavailable = 4,
    GenerationExhausted = 5,
    Backpressure = 6,
};

struct WorldInstallResult {
    WorldInstallStatus status = WorldInstallStatus::InvalidWorld;
    WorldGeneration priorGeneration = 0;
    WorldGeneration resultGeneration = 0;
    bool idempotent = false;
};

std::uint64_t ComputePlayablePropertyFingerprintV1(
    const PlayablePropertyProgramViewV1& row) noexcept;
std::uint64_t ComputePlayableOccurrenceFingerprintV1(
    const PlayableOccurrencePropertyViewV1& row) noexcept;
std::uint64_t ComputeTrackScheduleFingerprintV1(
    const TrackScheduleProgramViewV1& row) noexcept;
std::uint64_t ComputePieceScheduleFingerprintV1(
    const PieceScheduleProgramViewV1& row) noexcept;
std::uint64_t ComputeActionProgramFingerprintV1(
    const ActionProgramViewV1& program) noexcept;
std::uint64_t ComputeCurrentWorldFingerprintV4(
    const ContainerWorldInstallViewV1& world) noexcept;
std::uint64_t ComputeCurrentWorldFingerprintV5(
    const ContainerWorldInstallViewV1& world) noexcept;
TransitionProgramClassV1 ClassifyTransitionProgramV1(
    const TransitionProgramViewV1& program) noexcept;
WorldInstallStatus ValidateCurrentWorldV1(
    const ContainerWorldInstallViewV1& world) noexcept;

} // namespace harkenstudiobuild

#endif // HARKEN_STUDIO_BUILD_CURRENT_WORLD_H
