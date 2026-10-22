#include "PreviewPlay.h"

#include "Engine.h"
#include "harken/core/CurrentWorld.h"
#include "harken/param/VariableService.h"
#include "harken_preview/PreviewAbi.h"

#include <harken/Harken.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace harkenhost {
namespace {

constexpr std::uint32_t kSampleRate = 48000;
constexpr std::uint32_t kBlockFrames = 256;
constexpr VnSoundId kSoundA{0x76656e7573686f73, 9801};
constexpr VnSoundId kSoundB{0x76656e7573686f73, 9802};
constexpr VnObjectId kContainer{0x76656e7573686f73, 9803};
constexpr VnObjectId kTrack{0x76656e7573686f73, 9804};
constexpr VnObjectId kPiece{0x76656e7573686f73, 9805};
constexpr VnVariableId kDiscreteVariable{0x76656e7573686f73, 9811};
constexpr VnVariableId kContinuousVariable{0x76656e7573686f73, 9812};
constexpr float kSilenceFloor = 1.0e-4f;

enum class PreviewMode {
    RandomDraw,
    RandomRotation,
    Sequence,
    RandomQueue,
    SequenceQueue,
    VariableDiscrete,
    VariableContinuous,
    VariableDual,
    VariableTransition,
    Track,
    Piece,
};

struct Options {
    PreviewMode mode = PreviewMode::RandomDraw;
    double seconds = 2.0;
    std::uint32_t repeats = 1;
    bool describe = false;
    bool nullMode = false;
};

bool IsVariableMode(PreviewMode mode) {
    return mode == PreviewMode::VariableDiscrete ||
           mode == PreviewMode::VariableContinuous ||
           mode == PreviewMode::VariableDual ||
           mode == PreviewMode::VariableTransition;
}

bool HasDiscreteGrid(PreviewMode mode) {
    return mode == PreviewMode::VariableDiscrete ||
           mode == PreviewMode::VariableDual ||
           mode == PreviewMode::VariableTransition;
}

bool HasContinuousGrid(PreviewMode mode) {
    return mode == PreviewMode::VariableContinuous ||
           mode == PreviewMode::VariableDual;
}

bool ParseOptions(int argc, char** argv, Options* out) {
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--describe") {
            out->describe = true;
        } else if (arg == "--null") {
            out->nullMode = true;
        } else if (arg == "--mode" && i + 1 < argc) {
            const std::string mode = argv[++i];
            if (mode == "random") {
                out->mode = PreviewMode::RandomDraw;
            } else if (mode == "rotation") {
                out->mode = PreviewMode::RandomRotation;
            } else if (mode == "sequence") {
                out->mode = PreviewMode::Sequence;
            } else if (mode == "random-queue") {
                out->mode = PreviewMode::RandomQueue;
            } else if (mode == "sequence-queue") {
                out->mode = PreviewMode::SequenceQueue;
            } else if (mode == "variable-discrete") {
                out->mode = PreviewMode::VariableDiscrete;
            } else if (mode == "variable-continuous") {
                out->mode = PreviewMode::VariableContinuous;
            } else if (mode == "variable-dual") {
                out->mode = PreviewMode::VariableDual;
            } else if (mode == "variable-transition") {
                out->mode = PreviewMode::VariableTransition;
            } else if (mode == "track") {
                out->mode = PreviewMode::Track;
            } else if (mode == "piece") {
                out->mode = PreviewMode::Piece;
            } else {
                return false;
            }
        } else if (arg == "--seconds" && i + 1 < argc) {
            char* end = nullptr;
            const double seconds = std::strtod(argv[++i], &end);
            if (end == argv[i] || *end != '\0' || !(seconds > 0.0) ||
                !std::isfinite(seconds) || seconds > 60.0) {
                return false;
            }
            out->seconds = seconds;
        } else if (arg == "--repeats" && i + 1 < argc) {
            char* end = nullptr;
            const unsigned long repeats = std::strtoul(argv[++i], &end, 10);
            if (end == argv[i] || *end != '\0' || repeats == 0 ||
                repeats > 32) {
                return false;
            }
            out->repeats = static_cast<std::uint32_t>(repeats);
        } else {
            return false;
        }
    }
    return true;
}

std::vector<float> MakeTone(float frequency, double seconds) {
    const double duration = std::max(0.75, seconds + 0.25);
    std::vector<float> samples(static_cast<std::size_t>(
        std::ceil(duration * static_cast<double>(kSampleRate))));
    for (std::uint32_t i = 0; i < samples.size(); ++i) {
        const double phase = 2.0 * 3.14159265358979323846 * frequency *
                             static_cast<double>(i) / kSampleRate;
        samples[i] = 0.25f * static_cast<float>(std::sin(phase));
    }
    return samples;
}

harken::PlayablePropertyPayloadViewV1 CanonicalPayload() {
    harken::PlayablePropertyPayloadViewV1 payload{};
    payload.values[3].bits = 1;
    payload.values[11].bits = std::bit_cast<std::uint32_t>(1.0f);
    payload.values[16].bits = std::bit_cast<std::uint32_t>(1.0f);
    return payload;
}

harken::PlayablePropertyPayloadViewV1 ResolvedPayload() {
    auto payload = CanonicalPayload();
    payload.presentMask = harken::kResolvedSectionGroupPropertyMaskV1;
    payload.values[6].objectId = VN_BUS_MASTER;
    return payload;
}

struct PreviewWorld {
    std::array<harken::VariableDefinitionV1, 2> variables{};
    std::array<harken::PlayablePropertyProgramViewV1, 5> properties{};
    std::array<harken::PlayableOccurrencePropertyViewV1, 4> occurrences{};
    std::array<harken::ContainerChildProgramViewV2, 4> children{};
    harken::GridAxisV1 discreteAxis{};
    harken::GridAxisV1 continuousAxis{};
    std::array<harken::DiscreteGridCellV1, 2> discreteCells{};
    std::array<harken::DiscreteGridRowViewV1, 2> discreteRows{};
    std::array<harken::ContinuousGridCellV1, 2> continuousCells{};
    std::array<harken::ContinuousGridRowViewV1, 2> continuousRows{};
    harken::ScheduleClipPlacementV1 placement{};
    harken::TrackScheduleProgramViewV1 track{};
    harken::ScheduleTrackRefV1 trackRef{};
    harken::PieceScheduleProgramViewV1 piece{};
    std::array<harken::CurveV1, 2> transitionCurves{};
    harken::TransitionRuleViewV1 transitionRule{};
    harken::TransitionProgramViewV1 transition{};
    harken::ContainerProgramViewV3 container{};
    harken::ContainerWorldInstallViewV1 install{};

    PreviewWorld(std::uint64_t revisionA, std::uint64_t revisionB,
                 PreviewMode mode) {
        const bool variableMode = IsVariableMode(mode);
        const bool hasDiscrete = HasDiscreteGrid(mode);
        const bool hasContinuous = HasContinuousGrid(mode);
        const std::array<VnObjectId, 2> soundIds{kSoundA, kSoundB};
        const std::array<std::uint64_t, 2> revisions{revisionA, revisionB};
        for (std::uint32_t i = 0; i < 2; ++i) {
            auto& property = properties[i];
            property.contentPropertyKind =
                harken::playableproperties::ContentPropertyKindV1::Sound;
            property.authoredStableOrder = i;
            property.stableId = soundIds[i];
            property.contentRevision = revisions[i];
            property.routeStableId = VN_BUS_MASTER;
            property.compatibilityFingerprint = i + 1u;
            property.payload = ResolvedPayload();
            property.programSemanticFingerprint =
                harken::ComputePlayablePropertyFingerprintV1(property);
        }
        auto& root = properties[2];
        root.contentPropertyKind =
            harken::playableproperties::ContentPropertyKindV1::Container;
        root.authoredStableOrder = 2;
        root.stableId = kContainer;
        root.contentRevision = 1;
        root.routeStableId = VN_BUS_MASTER;
        root.compatibilityFingerprint = 3;
        root.payload = ResolvedPayload();
        root.programSemanticFingerprint =
            harken::ComputePlayablePropertyFingerprintV1(root);

        auto& trackProperty = properties[3];
        trackProperty.contentPropertyKind =
            harken::playableproperties::ContentPropertyKindV1::Track;
        trackProperty.authoredStableOrder = 3;
        trackProperty.stableId = kTrack;
        trackProperty.contentRevision = 1;
        trackProperty.routeStableId = VN_BUS_MASTER;
        trackProperty.compatibilityFingerprint = 4;
        trackProperty.payload = ResolvedPayload();
        trackProperty.programSemanticFingerprint =
            harken::ComputePlayablePropertyFingerprintV1(trackProperty);

        auto& pieceProperty = properties[4];
        pieceProperty.contentPropertyKind =
            harken::playableproperties::ContentPropertyKindV1::Piece;
        pieceProperty.authoredStableOrder = 4;
        pieceProperty.stableId = kPiece;
        pieceProperty.contentRevision = 1;
        pieceProperty.routeStableId = VN_BUS_MASTER;
        pieceProperty.compatibilityFingerprint = 5;
        pieceProperty.payload = ResolvedPayload();
        pieceProperty.programSemanticFingerprint =
            harken::ComputePlayablePropertyFingerprintV1(pieceProperty);

        const bool randomQueue = mode == PreviewMode::RandomQueue;
        const bool sequenceQueue = mode == PreviewMode::SequenceQueue;
        const bool queueMode = randomQueue || sequenceQueue;
        const bool scheduleMode =
            mode == PreviewMode::Track || mode == PreviewMode::Piece;
        const std::uint32_t childCount =
            mode == PreviewMode::VariableDual ? 4u : 2u;

        for (std::uint32_t i = 0; i < childCount; ++i) {
            auto& child = children[i];
            child.playable.kind = harken::PlayableKindV1::Sound;
            const std::uint32_t soundIndex = i & 1u;
            child.playable.stableId = properties[soundIndex].stableId;
            child.playable.expectedProgramSemanticFingerprint =
                properties[soundIndex].programSemanticFingerprint;
            child.playable.contentRevision =
                properties[soundIndex].contentRevision;
            child.playable.routeStableId = VN_BUS_MASTER;
            child.playable.compatibilityFingerprint =
                properties[soundIndex].compatibilityFingerprint;
            child.stableChildOrdinal = i + 1u;
            child.authoredStableOrder = i;
            child.frequencyWeight =
                mode == PreviewMode::RandomDraw ? 1u : 0u;
            child.orderWeight =
                mode == PreviewMode::RandomRotation || randomQueue ? 1u : 0u;
            if (queueMode) {
                auto& hint = child.timingHint.payload.inlineSound;
                hint.startOffset = {
                    harken::AuthoredTimeDomainV1::SourceFrames, 0, 1};
                hint.duration = {
                    harken::AuthoredTimeDomainV1::NaturalMediaEnd, 0, 1};
                child.queueStopPolicy =
                    harken::QueueStopPolicyV1::AdvanceQueue;
                child.timingHint.kind =
                    harken::ChildTimingHintKindV2::InlineSound;
            }

            auto& occurrence = occurrences[i];
            occurrence.parentKind = harken::playableproperties::
                ContentPropertyKindV1::Container;
            occurrence.parentStableId = kContainer;
            occurrence.parentProgramSemanticFingerprint =
                root.programSemanticFingerprint;
            occurrence.stableOccurrenceOrdinal = child.stableChildOrdinal;
            occurrence.childPlayable = child.playable;
            occurrence.payload = CanonicalPayload();
            occurrence.occurrenceSemanticFingerprint =
                harken::ComputePlayableOccurrenceFingerprintV1(occurrence);
        }

        if (hasDiscrete) {
            variables[0] = {kDiscreteVariable, harken::VariableKindV1::Step,
                            0.0f, 8.0f, 3.0f};
            discreteAxis = {1, 0, kDiscreteVariable};
            discreteCells[0] = {harken::DiscreteCellKindV1::Exact, 3};
            discreteCells[1] = {harken::DiscreteCellKindV1::Exact, 7};
            for (std::uint32_t i = 0; i < discreteRows.size(); ++i) {
                discreteRows[i].stableRowOrdinal = i + 1u;
                discreteRows[i].authoredStableOrder = i;
                discreteRows[i].childStableOrdinal = i + 1u;
                discreteRows[i].cellCount = 1;
                discreteRows[i].cells = &discreteCells[i];
            }
        }
        if (hasContinuous) {
            variables[hasDiscrete ? 1u : 0u] = {
                kContinuousVariable, harken::VariableKindV1::Smooth,
                0.0f, 8.0f, 2.0f};
            continuousAxis = {1, 0, kContinuousVariable};
            continuousCells[0].kind =
                harken::ContinuousCellKindV1::Interval;
            continuousCells[0].lower = 0.0f;
            continuousCells[0].upper = 4.0f;
            continuousCells[0].lowerInclusive = 1;
            continuousCells[0].upperInclusive = 1;
            continuousCells[1].kind =
                harken::ContinuousCellKindV1::Interval;
            continuousCells[1].lower = 5.0f;
            continuousCells[1].upper = 8.0f;
            continuousCells[1].lowerInclusive = 1;
            continuousCells[1].upperInclusive = 1;
            for (std::uint32_t i = 0; i < continuousRows.size(); ++i) {
                continuousRows[i].stableRowOrdinal = i + 1u;
                continuousRows[i].authoredStableOrder = i;
                continuousRows[i].childStableOrdinal = 1u + i;
                continuousRows[i].cellCount = 1;
                continuousRows[i].cells = &continuousCells[i];
            }
        }

        placement.stablePlacementOrdinal = 0;
        placement.stableOccurrenceOrdinal = 1;
        placement.content = children[0].playable;
        placement.clipId = {0x76656e7573686f73, 1};
        placement.clipGuidLow = 1;
        track.rootDefinition.kind = harken::PlayableKindV1::Track;
        track.rootDefinition.stableId = kTrack;
        track.rootDefinition.expectedProgramSemanticFingerprint =
            trackProperty.programSemanticFingerprint;
        track.rootDefinition.contentRevision = trackProperty.contentRevision;
        track.rootDefinition.routeStableId = trackProperty.routeStableId;
        track.rootDefinition.compatibilityFingerprint =
            trackProperty.compatibilityFingerprint;
        track.placementCount = 1;
        track.placements = &placement;
        track.scheduleSemanticFingerprint =
            harken::ComputeTrackScheduleFingerprintV1(track);

        if (!variableMode) {
            auto& trackOccurrence = occurrences[2];
            trackOccurrence.parentKind =
                harken::playableproperties::ContentPropertyKindV1::Track;
            trackOccurrence.parentStableId = kTrack;
            trackOccurrence.parentProgramSemanticFingerprint =
                trackProperty.programSemanticFingerprint;
            trackOccurrence.stableOccurrenceOrdinal =
                placement.stableOccurrenceOrdinal;
            trackOccurrence.childPlayable = placement.content;
            trackOccurrence.payload = CanonicalPayload();
            trackOccurrence.occurrenceSemanticFingerprint =
                harken::ComputePlayableOccurrenceFingerprintV1(
                    trackOccurrence);
        }

        trackRef.trackDefinition = track.rootDefinition;
        trackRef.trackScheduleSemanticFingerprint =
            track.scheduleSemanticFingerprint;
        piece.rootDefinition.kind = harken::PlayableKindV1::Piece;
        piece.rootDefinition.stableId = kPiece;
        piece.rootDefinition.expectedProgramSemanticFingerprint =
            pieceProperty.programSemanticFingerprint;
        piece.rootDefinition.contentRevision = pieceProperty.contentRevision;
        piece.rootDefinition.routeStableId = pieceProperty.routeStableId;
        piece.rootDefinition.compatibilityFingerprint =
            pieceProperty.compatibilityFingerprint;
        piece.timeKind = harken::ScheduleTimeKindV1::AbsoluteNanoseconds;
        piece.trackRefCount = 1;
        piece.trackRefs = &trackRef;
        piece.scheduleSemanticFingerprint =
            harken::ComputePieceScheduleFingerprintV1(piece);

        container.containerStableId = kContainer;
        container.programSemanticFingerprint =
            root.programSemanticFingerprint;
        container.mode = variableMode
            ? harken::ContainerModeV1::VariableControl
            : sequenceQueue
            ? harken::ContainerModeV1::SequenceQueue
            : randomQueue ? harken::ContainerModeV1::RandomQueue
            : mode == PreviewMode::Sequence
                ? harken::ContainerModeV1::Sequence
                : harken::ContainerModeV1::Random;
        container.randomDiscipline =
            mode == PreviewMode::RandomRotation || randomQueue
                ? harken::RandomDisciplineV1::Rotation
                : harken::RandomDisciplineV1::Draw;
        container.memoryScope = variableMode || queueMode
            ? harken::MemoryScopeV1::Emitter
            : harken::MemoryScopeV1::Global;
        container.avoidLastN =
            mode == PreviewMode::RandomDraw || randomQueue ? 1u : 0u;
        if (randomQueue) {
            container.queueCycle = harken::QueueCycleV1::Loop;
        }
        container.childCount = childCount;
        container.children = children.data();
        if (hasDiscrete) {
            container.discreteTiming = 1;
            container.discreteAxisCount = 1;
            container.discreteAxes = &discreteAxis;
            container.discreteRowCount = discreteRows.size();
            container.discreteRows = discreteRows.data();
            container.evaluationPeriodFrames = kBlockFrames;
        }
        if (hasContinuous) {
            container.continuousAxisCount = 1;
            container.continuousAxes = &continuousAxis;
            container.continuousRowCount = continuousRows.size();
            container.continuousRows = continuousRows.data();
            container.evaluationPeriodFrames = kBlockFrames;
            container.envelopeFrames = 0;
            container.maxActiveRows = 1;
            container.maxLiveRows = 2;
        }

        install.variables = variableMode ? variables.data() : nullptr;
        install.variableCount = hasDiscrete && hasContinuous
            ? 2u : variableMode ? 1u : 0u;
        if (scheduleMode) {
            properties[1] = properties[3];
            properties[2] = properties[4];
            install.playableProperties = properties.data();
            install.playablePropertyCount = 3;
            install.playableOccurrences = &occurrences[2];
            install.playableOccurrenceCount = 1;
            install.trackSchedules = &track;
            install.trackScheduleCount = 1;
            install.pieceSchedules = &piece;
            install.pieceScheduleCount = 1;
        } else {
            install.playableProperties = properties.data();
            install.playablePropertyCount = variableMode
                ? 3u : static_cast<std::uint32_t>(properties.size());
            install.playableOccurrences = occurrences.data();
            install.playableOccurrenceCount = variableMode ? childCount : 3u;
            install.trackSchedules = variableMode ? nullptr : &track;
            install.trackScheduleCount = variableMode ? 0u : 1u;
            install.pieceSchedules = variableMode ? nullptr : &piece;
            install.pieceScheduleCount = variableMode ? 0u : 1u;
            install.containers = &container;
            install.containerCount = 1;
        }
        if (mode == PreviewMode::VariableTransition) {
            for (std::uint32_t i = 0; i < transitionCurves.size(); ++i) {
                transitionCurves[i].curveLocalId = i + 1u;
                transitionCurves[i].curve.pointCount = 2;
                transitionCurves[i].curve.points[0] = {0.0f, 0.0f};
                transitionCurves[i].curve.points[1] = {1.0f, 1.0f};
            }
            transitionRule.stableRuleOrdinal = 1;
            transitionRule.sourceAny = 1;
            transitionRule.targetAny = 1;
            transitionRule.sourceFade.active = 1;
            transitionRule.sourceFade.durationFrames = kBlockFrames;
            transitionRule.sourceFade.curveLocalId =
                transitionCurves[0].curveLocalId;
            transitionRule.targetFade.active = 1;
            transitionRule.targetFade.durationFrames = kBlockFrames;
            transitionRule.targetFade.curveLocalId =
                transitionCurves[1].curveLocalId;
            transition.ownerContainerStableId = kContainer;
            transition.ruleCount = 1;
            transition.defaultRuleIndex = 0;
            transition.rules = &transitionRule;
            container.curveCount = transitionCurves.size();
            container.curves = transitionCurves.data();
            container.transitionProgramIndex = 0;
            install.transitions = &transition;
            install.transitionCount = 1;
        }
        install.identity.capabilityMask =
            harken::kWorldCapabilityPlayableProperties |
            (scheduleMode
                 ? 0u
                 : variableMode
                 ? harken::kWorldCapabilityVariableDefinitions |
                       harken::kWorldCapabilityVariableControl
                 : sequenceQueue
                 ? harken::kWorldCapabilitySequenceQueue
                 : randomQueue
                       ? harken::kWorldCapabilityRandomQueue
                       : mode == PreviewMode::Sequence
                 ? harken::kWorldCapabilitySequence
                       : mode == PreviewMode::RandomRotation
                       ? harken::kWorldCapabilityRandomRotation
                       : harken::kWorldCapabilityRandomDraw);
        if (mode == PreviewMode::VariableTransition) {
            install.identity.capabilityMask |=
                harken::kWorldCapabilityTransition;
        }
        install.identity.runtimeSemanticFingerprint =
            harken::ComputeCurrentWorldFingerprintV5(install);
    }
};

const char* ToneName(VnObjectId soundId) {
    if (soundId == kSoundA) {
        return "220";
    }
    if (soundId == kSoundB) {
        return "880";
    }
    return "?";
}

const char* PreviewTone(VnObjectId soundId) {
    if (soundId == kSoundA) {
        return "low 220 Hz";
    }
    if (soundId == kSoundB) {
        return "high 880 Hz";
    }
    return "unknown";
}

const char* ModeName(PreviewMode mode) {
    switch (mode) {
        case PreviewMode::RandomDraw:
            return "Random Draw";
        case PreviewMode::RandomRotation:
            return "Random Rotation";
        case PreviewMode::Sequence:
            return "Sequence";
        case PreviewMode::RandomQueue:
            return "Random Queue";
        case PreviewMode::SequenceQueue:
            return "Sequence Queue";
        case PreviewMode::VariableDiscrete:
            return "Variable Discrete";
        case PreviewMode::VariableContinuous:
            return "Variable Continuous";
        case PreviewMode::VariableDual:
            return "Variable Dual";
        case PreviewMode::VariableTransition:
            return "Variable Transition";
        case PreviewMode::Track:
            return "Track";
        case PreviewMode::Piece:
            return "Piece";
    }
    return "Unknown";
}

}

void PrintPreviewPlayUsage() {
    std::fprintf(stderr,
                 "\npreview-play plays a typed current-world Container through "
                 "the normal Runtime path.\n"
                 "  --mode random|rotation|sequence|random-queue|"
                 "sequence-queue|variable-discrete|variable-continuous|"
                 "variable-dual|variable-transition|track|piece\n"
                 "  --null\n"
                 "  --seconds N\n"
                 "  --repeats N\n"
                 "  --describe\n");
}

int RunPreviewPlay(int argc, char** argv) {
    Options options;
    if (!ParseOptions(argc, argv, &options)) {
        PrintPreviewPlayUsage();
        return 1;
    }
    if (options.describe) {
        const bool transition =
            options.mode == PreviewMode::VariableTransition;
        std::printf(
            "scenario=%s typed-preview=1 basic-transition=%u; "
            "current-world playback uses the "
            "normal Decision/page/arm path; success requires eventCount>0 "
            "and non-silence on the selected Null or device probe.\n",
            transition
                ? "variable-transition"
                : options.mode == PreviewMode::VariableDiscrete
                ? "variable-discrete"
                : options.mode == PreviewMode::VariableContinuous
                      ? "variable-continuous"
                      : options.mode == PreviewMode::VariableDual
                            ? "variable-dual"
                            : "typed-playable",
            transition ? 1u : 0u);
        return 0;
    }

    VnConfig config{};
    config.structSize = sizeof(config);
    config.sampleRate = kSampleRate;
    config.blockFrames = kBlockFrames;
    config.deviceMode = options.nullMode ? VN_DEVICE_NULL : VN_DEVICE_REAL;
    config.maxVoices = 16;
    config.maxBuses = 2;
    if (Vn_Init(&config) != VN_OK) {
        std::fprintf(stderr, "error: Vn_Init failed\n");
        return 3;
    }
    harken::Engine* engine = harken::detail::ActiveEngine();
    if (engine == nullptr) {
        Vn_Term();
        return 3;
    }
    const auto advanceOneBlock = [&]() {
        if (options.nullMode) {
            engine->nullBackend.Pump(1);
        }
        return Vn_Update();
    };
    if (Vn_Update() != VN_OK) {
        std::fprintf(stderr, "error: startup Vn_Update failed\n");
        Vn_Term();
        return 3;
    }
    const bool queuePreview =
        options.mode == PreviewMode::SequenceQueue ||
        options.mode == PreviewMode::RandomQueue;
    const std::vector<float> toneA = MakeTone(220.0f, options.seconds);
    const std::vector<float> toneB = MakeTone(880.0f, options.seconds);
    VnPCMDesc pcmA{};
    pcmA.structSize = sizeof(pcmA);
    pcmA.sampleRate = kSampleRate;
    pcmA.channels = 1;
    pcmA.layout = VN_PCM_LAYOUT_INTERLEAVED;
    pcmA.policy = VN_PCM_POLICY_COPY;
    pcmA.frames = toneA.size();
    pcmA.samplesInterleaved = toneA.data();
    VnPCMDesc pcmB = pcmA;
    pcmB.frames = toneB.size();
    pcmB.samplesInterleaved = toneB.data();
    if (Vn_RegisterPCM(kSoundA, &pcmA) != VN_OK ||
        Vn_RegisterPCM(kSoundB, &pcmB) != VN_OK) {
        std::fprintf(stderr, "error: PCM registration failed\n");
        Vn_Term();
        return 3;
    }
    PreviewWorld world(engine->registry.Lookup(kSoundA).contentRevision,
                       engine->registry.Lookup(kSoundB).contentRevision,
                       options.mode);
    const harken::WorldInstallResult install =
        harken::InstallContainerWorldV1(*engine, world.install);
    if (install.status != harken::WorldInstallStatus::Ok) {
        std::fprintf(stderr,
                     "error: current-world install failed (status %u)\n",
                     static_cast<unsigned>(install.status));
        Vn_Term();
        return 3;
    }
    if (Vn_Update() != VN_OK) {
        std::fprintf(stderr, "error: initial Vn_Update failed\n");
        Vn_Term();
        return 3;
    }
    VnPreviewPlayPlayableDescV2 desc{};
    desc.structSize = sizeof(desc);
    desc.playableKind = options.mode == PreviewMode::Track
        ? VN_PREVIEW_PLAYABLE_KIND_TRACK
        : options.mode == PreviewMode::Piece
              ? VN_PREVIEW_PLAYABLE_KIND_PIECE
              : VN_PREVIEW_PLAYABLE_KIND_CONTAINER;
    desc.stableId = options.mode == PreviewMode::Track
        ? kTrack
        : options.mode == PreviewMode::Piece ? kPiece : kContainer;
    VnEmitterHandle emitter = 0;
    if ((options.mode == PreviewMode::RandomQueue ||
         options.mode == PreviewMode::SequenceQueue) &&
        Vn_CreateEmitter(&emitter) != VN_OK) {
        std::fprintf(stderr, "error: Preview emitter creation failed\n");
        Vn_Term();
        return 3;
    }
    desc.emitterHandle = emitter;
    for (std::uint32_t trigger = 0; trigger < options.repeats; ++trigger) {
        if (IsVariableMode(options.mode) && trigger != 0) {
            if ((HasDiscreteGrid(options.mode) &&
                 Vn_SetValue(kDiscreteVariable, 3.0f) != VN_OK) ||
                (HasContinuousGrid(options.mode) &&
                 Vn_SetValue(kContinuousVariable, 2.0f) != VN_OK)) {
                std::fprintf(stderr,
                             "error: Variable reset before Preview failed\n");
                Vn_Term();
                return 3;
            }
            for (std::uint32_t block = 0; block < 8; ++block) {
                if (advanceOneBlock() != VN_OK) {
                    std::fprintf(stderr,
                                 "error: Variable reset service failed\n");
                    Vn_Term();
                    return 3;
                }
            }
        }
        VnPreviewPlayableHandle handle = 0;
        const VnPreviewResult play = VnPreview_PlayPlayable(&desc, &handle);
        if (play != VN_PREVIEW_OK) {
            std::fprintf(stderr, "error: Preview play refused (%d)\n", play);
            Vn_Term();
            return 3;
        }
        const std::uint64_t internalHandle =
            handle & ~(std::uint64_t{1} << 63);
        std::printf("%s trigger %u/%u: root %llu\n",
                    ModeName(options.mode), trigger + 1u, options.repeats,
                    static_cast<unsigned long long>(handle));
        std::uint32_t eventCount = 0;
        std::array<VnObjectId, 8> heardTones{};
        std::uint32_t heardCount = 0;
        const auto recordTone = [&](VnObjectId soundId) {
            if (harken::IsNull(soundId)) {
                return;
            }
            if (heardCount != 0 && heardTones[heardCount - 1u] == soundId) {
                return;
            }
            if (heardCount < heardTones.size()) {
                heardTones[heardCount++] = soundId;
            }
        };
        for (const harken::EnginePlayableServiceRow& row :
             engine->playableServices) {
            if (row.phase != harken::EnginePlayableStartPhase::Free &&
                row.rootInstance == internalHandle) {
                eventCount +=
                    static_cast<std::uint32_t>(row.schedule.eventCount);
                recordTone(row.leaf.stableId);
                std::printf("%s selected: %s (Sound %s)\n",
                            ModeName(options.mode),
                            PreviewTone(row.leaf.stableId),
                            harken::ObjectIdString(row.leaf.stableId).c_str());
                if (queuePreview) {
                    std::printf(
                        "%s first=%s continuation=%u successor=%s\n",
                        ModeName(options.mode),
                        ToneName(row.leaf.stableId),
                        row.queueContinuationPrepared ? 1u : 0u,
                        row.queueContinuationPrepared
                            ? ToneName(row.queueSuccessorLeaf.stableId)
                            : "-");
                }
                break;
            }
        }
        std::fflush(stdout);
        const auto probeStart = std::chrono::steady_clock::now();
        const auto deadline = probeStart +
            std::chrono::milliseconds(static_cast<int>(
                options.seconds * 1000.0));
        const double retargetSeconds =
            options.mode == PreviewMode::VariableTransition
                ? std::min(options.seconds * 0.5,
                           8.0 * kBlockFrames / kSampleRate)
                : std::min(0.15, options.seconds * 0.25);
        const auto retargetDeadline = probeStart +
            std::chrono::milliseconds(static_cast<int>(
                retargetSeconds * 1000.0));
        const std::uint32_t nullBlockCount = std::max(
            1u, static_cast<std::uint32_t>(std::ceil(
                    options.seconds * kSampleRate / kBlockFrames)));
        const std::uint32_t nullRetargetBlock =
            options.mode == PreviewMode::VariableTransition
                ? std::min(nullBlockCount / 2u, 8u)
                : std::min(nullBlockCount / 4u, 8u);
        std::uint32_t nullBlock = 0;
        bool retargeted = false;
        VnPreviewPlayableStateV2 state{};
        state.structSize = sizeof(state);
        bool stale = false;
        bool sawActive = false;
        bool sawTransitionPrepared = false;
        bool sawTransitionToken = false;
        bool sawTransitionApplied = false;
        bool sawTransitionLocked = false;
        std::uint64_t lastTransitionFrame = 0;
        std::uint32_t lastDiscreteOrdinal = 0;
        float maxMasterPeak = 0.0f;
        const auto observeTransition = [&]() {
            if (options.mode != PreviewMode::VariableTransition ||
                sawTransitionLocked) {
                return;
            }
            for (const harken::EnginePlayableServiceRow& row :
                 engine->playableServices) {
                if (row.phase == harken::EnginePlayableStartPhase::Free ||
                    row.rootInstance != internalHandle) {
                    continue;
                }
                sawTransitionPrepared = sawTransitionPrepared ||
                    row.variableControlDiscreteRetargetPrepared;
                sawTransitionApplied = sawTransitionApplied ||
                    row.variableControlDiscreteRetargetApplied;
                sawTransitionLocked = sawTransitionLocked ||
                    sawTransitionApplied;
                if (row.variableControlDiscreteMemberCount != 0) {
                    std::size_t memberSlot = 0;
                    if (engine->ResolvePlayableMemberSlot(row.scheduleStorageBase, &memberSlot)) {
                        lastDiscreteOrdinal =
                            engine->ReadPlayableMember(memberSlot).voice.Facts().target.occurrenceOrdinal;
                    }
                }
                if (row.variableControlDiscreteRetargetFrame != 0) {
                    lastTransitionFrame =
                        row.variableControlDiscreteRetargetFrame;
                }
                if (!row.variableControlDiscreteRetargetTransition.Valid()) {
                    continue;
                }
                sawTransitionToken = true;
                harken::TransitionInstanceSnapshot transition{};
                sawTransitionLocked = sawTransitionLocked ||
                    (engine->transitionInstances.Capture(
                         row.variableControlDiscreteRetargetTransition,
                         &transition) &&
                     transition.state ==
                         harken::TransitionAudibleGateState::Locked);
                if (sawTransitionLocked) break;
            }
        };
        while (options.nullMode ? nullBlock < nullBlockCount
                                : std::chrono::steady_clock::now() <
                                      deadline) {
            if (!retargeted && IsVariableMode(options.mode) &&
                (options.nullMode
                     ? nullBlock >= nullRetargetBlock
                     : std::chrono::steady_clock::now() >=
                           retargetDeadline)) {
                if ((HasDiscreteGrid(options.mode) &&
                     Vn_SetValue(kDiscreteVariable, 7.0f) != VN_OK) ||
                    (HasContinuousGrid(options.mode) &&
                     Vn_SetValue(kContinuousVariable, 7.0f) != VN_OK)) {
                    std::fprintf(stderr,
                                 "error: Variable retarget failed\n");
                    Vn_Term();
                    return 3;
                }
                retargeted = true;
            }
            if (options.nullMode) {
                engine->nullBackend.Pump(1);
                observeTransition();
            }
            if (Vn_Update() != VN_OK) {
                std::fprintf(stderr, "error: Vn_Update failed\n");
                Vn_Term();
                return 3;
            }
            observeTransition();
            if (queuePreview) {
                for (const harken::EnginePlayableServiceRow& row :
                     engine->playableServices) {
                    if (row.phase == harken::EnginePlayableStartPhase::Free ||
                        row.rootInstance != internalHandle) {
                        continue;
                    }
                    recordTone(row.leaf.stableId);
                    break;
                }
            }
            ++nullBlock;
            const VnPreviewResult query =
                VnPreview_QueryPlayable(handle, &state);
            if (query == VN_PREVIEW_STALE_HANDLE) {
                stale = true;
                break;
            }
            if (query != VN_PREVIEW_OK) {
                std::fprintf(stderr, "error: Preview query failed (%d)\n",
                             query);
                Vn_Term();
                return 3;
            }
            sawActive = sawActive ||
                (state.lifecycle == VN_PREVIEW_LIFECYCLE_ACTIVE &&
                 state.childLiveCount != 0);
            VnSnapshot snapshot{};
            snapshot.structSize = sizeof(snapshot);
            if (Vn_CaptureSnapshot(&snapshot) == VN_OK &&
                snapshot.busCount != 0) {
                maxMasterPeak =
                    std::max(maxMasterPeak, snapshot.busPeaks[0]);
            }
            if (!options.nullMode) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
        const VnPreviewResult stop = VnPreview_StopPlayable(handle, 10);
        if (stop != VN_PREVIEW_OK && stop != VN_PREVIEW_STALE_HANDLE) {
            std::fprintf(stderr, "error: Preview stop failed (%d)\n", stop);
            Vn_Term();
            return 3;
        }
        const auto retireDeadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(2);
        std::uint32_t retireBlock = 0;
        const std::uint32_t retireBlockLimit =
            2u * kSampleRate / kBlockFrames;
        while (!stale &&
               (options.nullMode
                    ? retireBlock < retireBlockLimit
                    : std::chrono::steady_clock::now() < retireDeadline)) {
            if (advanceOneBlock() != VN_OK) {
                std::fprintf(stderr, "error: Vn_Update failed while retiring\n");
                Vn_Term();
                return 3;
            }
            ++retireBlock;
            const VnPreviewResult query =
                VnPreview_QueryPlayable(handle, &state);
            if (query == VN_PREVIEW_STALE_HANDLE) {
                stale = true;
                break;
            }
            if (query != VN_PREVIEW_OK) {
                std::fprintf(stderr,
                             "error: Preview retire query failed (%d)\n",
                             query);
                Vn_Term();
                return 3;
            }
            if (!options.nullMode) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
        if (!stale) {
            std::fprintf(stderr,
                         "error: Preview handle did not retire within 2s\n");
            Vn_Term();
            return 3;
        }
        if (queuePreview) {
            std::printf("%s order:", ModeName(options.mode));
            for (std::uint32_t index = 0; index < heardCount; ++index) {
                std::printf(" %s", ToneName(heardTones[index]));
            }
            std::printf("\n");
        }
        std::printf(
            "%s probe: eventCount=%u lifecycle=ACTIVE:%u "
            "max master peak %.6f %s\n",
            ModeName(options.mode), eventCount, sawActive ? 1u : 0u,
            maxMasterPeak,
            eventCount != 0 && sawActive && maxMasterPeak > kSilenceFloor
                ? "non-silent" : "FAILED");
        if (options.mode == PreviewMode::VariableTransition) {
            std::printf(
                "Variable Transition transition=%s "
                "lifecycle=ACTIVE->%s\n",
                sawTransitionLocked ? "LOCKED" : "MISSING",
                stale ? "STALE" : "LIVE");
        }
        std::fflush(stdout);
        if (eventCount == 0 || !sawActive ||
            maxMasterPeak <= kSilenceFloor ||
            (options.mode == PreviewMode::VariableTransition &&
             !sawTransitionLocked)) {
            std::fprintf(stderr,
                         "error: Preview probe requires eventCount>0, "
                         "ACTIVE lifecycle, and non-silence"
                         " (transition prepared=%u token=%u applied=%u "
                         "frame=%llu "
                         "ordinal=%u "
                         "rendered=%llu)\n",
                         sawTransitionPrepared ? 1u : 0u,
                         sawTransitionToken ? 1u : 0u,
                         sawTransitionApplied ? 1u : 0u,
                         static_cast<unsigned long long>(lastTransitionFrame),
                         lastDiscreteOrdinal,
                         static_cast<unsigned long long>(
                             engine->mixer.BlocksRendered() *
                             engine->config.blockFrames));
            Vn_Term();
            return 2;
        }
        const auto gapDeadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
        std::uint32_t gapBlock = 0;
        const std::uint32_t gapBlockLimit =
            (150u * kSampleRate + 999u) / 1000u / kBlockFrames;
        while (options.nullMode
                   ? gapBlock < gapBlockLimit
                   : std::chrono::steady_clock::now() < gapDeadline) {
            if (advanceOneBlock() != VN_OK) {
                std::fprintf(stderr,
                             "error: Vn_Update failed between triggers\n");
                Vn_Term();
                return 3;
            }
            ++gapBlock;
            if (!options.nullMode) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
    }
    if (emitter != 0 && Vn_DestroyEmitter(emitter) != VN_OK) {
        std::fprintf(stderr, "error: Preview emitter destruction failed\n");
        Vn_Term();
        return 3;
    }
    Vn_Term();
    return 0;
}

}
