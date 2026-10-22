#include "Common/ObjectIdTestSupport.h"
#include "Common/TestUtil.h"

#include "Engine.h"
#include "MonitorCaptureHubTestHooks.h"
#include "Common/MonitorExecutionTestSupport.h"
#include "SharedFrontierTrace.h"
#include "harken/core/CurrentWorld.h"
#include "harken_preview/PreviewAbi.h"

#include <gtest/gtest.h>

#include <array>
#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

constexpr VnObjectId kSound = harkentest::ObjectId(8101);
constexpr VnObjectId kSecondSound = harkentest::ObjectId(8105);
constexpr VnObjectId kContainer = harkentest::ObjectId(8102);
constexpr VnObjectId kTrack = harkentest::ObjectId(8103);
constexpr VnObjectId kPiece = harkentest::ObjectId(8104);

harken::PlayablePropertyPayloadViewV1 CanonicalPayload() {
    harken::PlayablePropertyPayloadViewV1 payload{};
    payload.values[3].bits = 1;
    payload.values[11].bits = std::bit_cast<std::uint32_t>(1.0f);
    payload.values[16].bits = std::bit_cast<std::uint32_t>(1.0f);
    return payload;
}

harken::PlayablePropertyPayloadViewV1 ResolvedDefinitionPayload() {
    auto payload = CanonicalPayload();
    payload.presentMask = harken::kResolvedSectionGroupPropertyMaskV1;
    payload.values[6].objectId = VN_BUS_MASTER;
    return payload;
}

struct PreviewWorldImage {
    std::array<harken::PlayablePropertyProgramViewV1, 2> properties{};
    harken::PlayableOccurrencePropertyViewV1 occurrence{};
    harken::ContainerChildProgramViewV2 child{};
    harken::ContainerProgramViewV3 container{};
    harken::ContainerWorldInstallViewV1 world{};

    explicit PreviewWorldImage(
        std::uint64_t soundRevision,
        harken::ContainerModeV1 mode = harken::ContainerModeV1::Random,
        harken::RandomDisciplineV1 discipline =
            harken::RandomDisciplineV1::Draw) {
        properties[0].contentPropertyKind =
            harken::playableproperties::ContentPropertyKindV1::Sound;
        properties[0].stableId = harkentest::ObjectId(kSound);
        properties[0].contentRevision = soundRevision;
        properties[0].routeStableId = VN_BUS_MASTER;
        properties[0].compatibilityFingerprint = 1;
        properties[0].payload = ResolvedDefinitionPayload();
        properties[0].programSemanticFingerprint =
            harken::ComputePlayablePropertyFingerprintV1(properties[0]);

        properties[1].contentPropertyKind =
            harken::playableproperties::ContentPropertyKindV1::Container;
        properties[1].authoredStableOrder = 1;
        properties[1].stableId = harkentest::ObjectId(kContainer);
        properties[1].contentRevision = 1;
        properties[1].routeStableId = VN_BUS_MASTER;
        properties[1].compatibilityFingerprint = 2;
        properties[1].payload = ResolvedDefinitionPayload();
        properties[1].programSemanticFingerprint =
            harken::ComputePlayablePropertyFingerprintV1(properties[1]);

        child.playable.kind = harken::PlayableKindV1::Sound;
        child.playable.stableId = harkentest::ObjectId(kSound);
        child.playable.expectedProgramSemanticFingerprint =
            properties[0].programSemanticFingerprint;
        child.playable.contentRevision = soundRevision;
        child.playable.routeStableId = VN_BUS_MASTER;
        child.playable.compatibilityFingerprint = 1;
        child.stableChildOrdinal = 7;
        child.frequencyWeight =
            mode == harken::ContainerModeV1::Random &&
                    discipline == harken::RandomDisciplineV1::Draw
                ? 1u
                : 0u;
        child.orderWeight =
            mode == harken::ContainerModeV1::Random &&
                    discipline == harken::RandomDisciplineV1::Rotation
                ? 1u
                : 0u;

        occurrence.parentKind =
            harken::playableproperties::ContentPropertyKindV1::Container;
        occurrence.parentStableId = harkentest::ObjectId(kContainer);
        occurrence.parentProgramSemanticFingerprint =
            properties[1].programSemanticFingerprint;
        occurrence.stableOccurrenceOrdinal = child.stableChildOrdinal;
        occurrence.childPlayable = child.playable;
        occurrence.payload = CanonicalPayload();
        occurrence.occurrenceSemanticFingerprint =
            harken::ComputePlayableOccurrenceFingerprintV1(occurrence);

        container.containerStableId = harkentest::ObjectId(kContainer);
        container.programSemanticFingerprint =
            properties[1].programSemanticFingerprint;
        container.mode = mode;
        container.randomDiscipline = discipline;
        container.memoryScope = harken::MemoryScopeV1::Global;
        container.childCount = 1;
        container.children = &child;

        world.playableProperties = properties.data();
        world.playablePropertyCount = properties.size();
        world.playableOccurrences = &occurrence;
        world.playableOccurrenceCount = 1;
        world.containers = &container;
        world.containerCount = 1;
        world.identity.capabilityMask =
            harken::kWorldCapabilityPlayableProperties |
            (mode == harken::ContainerModeV1::Sequence
                 ? harken::kWorldCapabilitySequence
                 : discipline == harken::RandomDisciplineV1::Rotation
                       ? harken::kWorldCapabilityRandomRotation
                       : harken::kWorldCapabilityRandomDraw);
        world.identity.runtimeSemanticFingerprint =
            harken::ComputeCurrentWorldFingerprintV4(world);
    }
};

struct PreviewScheduleWorldImage {
    std::array<harken::PlayablePropertyProgramViewV1, 3> properties{};
    harken::PlayableOccurrencePropertyViewV1 occurrence{};
    harken::ScheduleClipPlacementV1 placement{};
    harken::TrackScheduleProgramViewV1 track{};
    harken::ScheduleTrackRefV1 trackRef{};
    harken::PieceScheduleProgramViewV1 piece{};
    harken::ContainerWorldInstallViewV1 world{};

    explicit PreviewScheduleWorldImage(std::uint64_t soundRevision) {
        const std::array<harken::playableproperties::ContentPropertyKindV1, 3>
            kinds = {
                harken::playableproperties::ContentPropertyKindV1::Sound,
                harken::playableproperties::ContentPropertyKindV1::Track,
                harken::playableproperties::ContentPropertyKindV1::Piece};
        const std::array<VnObjectId, 3> ids = {
            kSound, kTrack, kPiece};
        for (std::uint32_t index = 0; index < properties.size(); ++index) {
            properties[index].contentPropertyKind = kinds[index];
            properties[index].stableId = harkentest::ObjectId(ids[index]);
            properties[index].authoredStableOrder = index;
            properties[index].contentRevision =
                index == 0 ? soundRevision : 100u + index;
            properties[index].routeStableId = VN_BUS_MASTER;
            properties[index].compatibilityFingerprint = index + 1u;
            properties[index].payload = ResolvedDefinitionPayload();
            properties[index].programSemanticFingerprint =
                harken::ComputePlayablePropertyFingerprintV1(
                    properties[index]);
        }
        const auto makeRef = [&](std::uint32_t index,
                                 harken::PlayableKindV1 kind) {
            harken::PlayableRefV1 ref{};
            ref.kind = kind;
            ref.stableId = properties[index].stableId;
            ref.expectedProgramSemanticFingerprint =
                properties[index].programSemanticFingerprint;
            ref.contentRevision = properties[index].contentRevision;
            ref.routeStableId = properties[index].routeStableId;
            ref.compatibilityFingerprint =
                properties[index].compatibilityFingerprint;
            return ref;
        };
        placement.stableOccurrenceOrdinal = 7;
        placement.content = makeRef(0, harken::PlayableKindV1::Sound);
        placement.clipId = harkentest::ObjectId(91);
        placement.clipGuidLow = 92;
        track.rootDefinition = makeRef(1, harken::PlayableKindV1::Track);
        track.placementCount = 1;
        track.placements = &placement;
        track.scheduleSemanticFingerprint =
            harken::ComputeTrackScheduleFingerprintV1(track);
        trackRef.trackDefinition = track.rootDefinition;
        trackRef.trackScheduleSemanticFingerprint =
            track.scheduleSemanticFingerprint;
        piece.rootDefinition = makeRef(2, harken::PlayableKindV1::Piece);
        piece.trackRefCount = 1;
        piece.trackRefs = &trackRef;
        piece.scheduleSemanticFingerprint =
            harken::ComputePieceScheduleFingerprintV1(piece);
        occurrence.parentKind =
            harken::playableproperties::ContentPropertyKindV1::Track;
        occurrence.parentStableId = track.rootDefinition.stableId;
        occurrence.parentProgramSemanticFingerprint =
            track.rootDefinition.expectedProgramSemanticFingerprint;
        occurrence.stableOccurrenceOrdinal =
            placement.stableOccurrenceOrdinal;
        occurrence.childPlayable = placement.content;
        occurrence.payload = CanonicalPayload();
        occurrence.occurrenceSemanticFingerprint =
            harken::ComputePlayableOccurrenceFingerprintV1(occurrence);
        world.playableProperties = properties.data();
        world.playablePropertyCount = properties.size();
        world.playableOccurrences = &occurrence;
        world.playableOccurrenceCount = 1;
        world.trackSchedules = &track;
        world.trackScheduleCount = 1;
        world.pieceSchedules = &piece;
        world.pieceScheduleCount = 1;
        world.identity.capabilityMask =
            harken::kWorldCapabilityPlayableProperties;
        world.identity.runtimeSemanticFingerprint =
            harken::ComputeCurrentWorldFingerprintV4(world);
    }
};

struct PreviewVariableControlWorldImage {
    harken::VariableDefinitionV1 variable{
        harkentest::ObjectId(41), harken::VariableKindV1::Step, 0.0f, 8.0f, 3.0f};
    std::array<harken::PlayablePropertyProgramViewV1, 2> properties{};
    harken::PlayableOccurrencePropertyViewV1 occurrence{};
    harken::ContainerChildProgramViewV2 child{};
    harken::GridAxisV1 axis{0, 0, variable.variableId};
    harken::DiscreteGridCellV1 cell{
        harken::DiscreteCellKindV1::Exact, 3};
    harken::DiscreteGridRowViewV1 row{};
    harken::ContinuousGridCellV1 continuousCell{};
    harken::ContinuousGridRowViewV1 continuousRow{};
    harken::ContainerProgramViewV3 container{};
    harken::ContainerWorldInstallViewV1 world{};

    explicit PreviewVariableControlWorldImage(
        std::uint64_t soundRevision,
        bool continuousOnly = false) {
        properties[0].contentPropertyKind =
            harken::playableproperties::ContentPropertyKindV1::Sound;
        properties[0].stableId = harkentest::ObjectId(kSound);
        properties[0].contentRevision = soundRevision;
        properties[0].routeStableId = VN_BUS_MASTER;
        properties[0].compatibilityFingerprint = 1;
        properties[0].payload = ResolvedDefinitionPayload();
        properties[0].programSemanticFingerprint =
            harken::ComputePlayablePropertyFingerprintV1(properties[0]);
        properties[1].contentPropertyKind =
            harken::playableproperties::ContentPropertyKindV1::Container;
        properties[1].authoredStableOrder = 1;
        properties[1].stableId = harkentest::ObjectId(kContainer);
        properties[1].contentRevision = 1;
        properties[1].routeStableId = VN_BUS_MASTER;
        properties[1].compatibilityFingerprint = 2;
        properties[1].payload = ResolvedDefinitionPayload();
        properties[1].programSemanticFingerprint =
            harken::ComputePlayablePropertyFingerprintV1(properties[1]);

        child.playable.kind = harken::PlayableKindV1::Sound;
        child.playable.stableId = harkentest::ObjectId(kSound);
        child.playable.expectedProgramSemanticFingerprint =
            properties[0].programSemanticFingerprint;
        child.playable.contentRevision = soundRevision;
        child.playable.routeStableId = VN_BUS_MASTER;
        child.playable.compatibilityFingerprint = 1;
        child.stableChildOrdinal = 7;

        occurrence.parentKind =
            harken::playableproperties::ContentPropertyKindV1::Container;
        occurrence.parentStableId = harkentest::ObjectId(kContainer);
        occurrence.parentProgramSemanticFingerprint =
            properties[1].programSemanticFingerprint;
        occurrence.stableOccurrenceOrdinal = child.stableChildOrdinal;
        occurrence.childPlayable = child.playable;
        occurrence.payload = CanonicalPayload();
        occurrence.occurrenceSemanticFingerprint =
            harken::ComputePlayableOccurrenceFingerprintV1(occurrence);

        row.stableRowOrdinal = 1;
        row.childStableOrdinal = child.stableChildOrdinal;
        row.cellCount = 1;
        row.cells = &cell;
        container.containerStableId = harkentest::ObjectId(kContainer);
        container.programSemanticFingerprint =
            properties[1].programSemanticFingerprint;
        container.mode = harken::ContainerModeV1::VariableControl;
        container.childCount = 1;
        container.children = &child;
        if (continuousOnly) {
            continuousCell.kind = harken::ContinuousCellKindV1::Interval;
            continuousCell.lower = 0.0f;
            continuousCell.upper = 8.0f;
            continuousCell.lowerInclusive = 1;
            continuousCell.upperInclusive = 1;
            continuousRow.stableRowOrdinal = 1;
            continuousRow.childStableOrdinal = child.stableChildOrdinal;
            continuousRow.cellCount = 1;
            continuousRow.cells = &continuousCell;
            container.continuousAxisCount = 1;
            container.continuousAxes = &axis;
            container.continuousRowCount = 1;
            container.continuousRows = &continuousRow;
            container.evaluationPeriodFrames = 64;
            container.envelopeFrames = 2;
            container.maxActiveRows = 1;
            container.maxLiveRows = 1;
        } else {
            container.discreteAxisCount = 1;
            container.discreteAxes = &axis;
            container.discreteRowCount = 1;
            container.discreteRows = &row;
        }

        world.variables = &variable;
        world.variableCount = 1;
        world.playableProperties = properties.data();
        world.playablePropertyCount = properties.size();
        world.playableOccurrences = &occurrence;
        world.playableOccurrenceCount = 1;
        world.containers = &container;
        world.containerCount = 1;
        world.identity.capabilityMask =
            harken::kWorldCapabilityVariableDefinitions |
            harken::kWorldCapabilityPlayableProperties |
            harken::kWorldCapabilityVariableControl;
        world.identity.runtimeSemanticFingerprint =
            harken::ComputeCurrentWorldFingerprintV4(world);
    }
};

struct PreviewDualVariableControlWorldImage {
    std::array<harken::VariableDefinitionV1, 2> variables{{
        {harkentest::ObjectId(41), harken::VariableKindV1::Step, 0.0f, 8.0f, 3.0f},
        {harkentest::ObjectId(42), harken::VariableKindV1::Smooth, 0.0f, 8.0f, 2.0f},
    }};
    std::array<harken::PlayablePropertyProgramViewV1, 3> properties{};
    std::array<harken::PlayableOccurrencePropertyViewV1, 4> occurrences{};
    std::array<harken::ContainerChildProgramViewV2, 4> children{};
    harken::GridAxisV1 discreteAxis{1, 0, harkentest::ObjectId(41)};
    harken::GridAxisV1 continuousAxis{1, 0, harkentest::ObjectId(42)};
    std::array<harken::DiscreteGridCellV1, 2> discreteCells{{
        {harken::DiscreteCellKindV1::Exact, 3},
        {harken::DiscreteCellKindV1::Exact, 7},
    }};
    std::array<harken::DiscreteGridRowViewV1, 2> discreteRows{};
    std::array<harken::ContinuousGridCellV1, 2> continuousCells{};
    std::array<harken::ContinuousGridRowViewV1, 2> continuousRows{};
    harken::ContainerProgramViewV3 container{};
    harken::ContainerWorldInstallViewV1 world{};

    PreviewDualVariableControlWorldImage(std::uint64_t firstRevision,
                                         std::uint64_t secondRevision) {
        const std::array<VnObjectId, 2> soundIds{
            kSound, kSecondSound};
        const std::array<std::uint64_t, 2> revisions{
            firstRevision, secondRevision};
        for (std::uint32_t index = 0; index < 2; ++index) {
            auto& property = properties[index];
            property.contentPropertyKind =
                harken::playableproperties::ContentPropertyKindV1::Sound;
            property.authoredStableOrder = index;
            property.stableId = harkentest::ObjectId(soundIds[index]);
            property.contentRevision = revisions[index];
            property.routeStableId = VN_BUS_MASTER;
            property.compatibilityFingerprint = index + 1u;
            property.payload = ResolvedDefinitionPayload();
            property.programSemanticFingerprint =
                harken::ComputePlayablePropertyFingerprintV1(property);
        }
        auto& root = properties[2];
        root.contentPropertyKind =
            harken::playableproperties::ContentPropertyKindV1::Container;
        root.authoredStableOrder = 2;
        root.stableId = harkentest::ObjectId(kContainer);
        root.contentRevision = 1;
        root.routeStableId = VN_BUS_MASTER;
        root.compatibilityFingerprint = 3;
        root.payload = ResolvedDefinitionPayload();
        root.programSemanticFingerprint =
            harken::ComputePlayablePropertyFingerprintV1(root);

        for (std::uint32_t index = 0; index < children.size(); ++index) {
            const std::uint32_t soundIndex = index & 1u;
            auto& child = children[index];
            child.playable.kind = harken::PlayableKindV1::Sound;
            child.playable.stableId = properties[soundIndex].stableId;
            child.playable.expectedProgramSemanticFingerprint =
                properties[soundIndex].programSemanticFingerprint;
            child.playable.contentRevision =
                properties[soundIndex].contentRevision;
            child.playable.routeStableId = VN_BUS_MASTER;
            child.playable.compatibilityFingerprint =
                properties[soundIndex].compatibilityFingerprint;
            child.stableChildOrdinal = index + 1u;
            child.authoredStableOrder = index;

            auto& occurrence = occurrences[index];
            occurrence.parentKind = harken::playableproperties::
                ContentPropertyKindV1::Container;
            occurrence.parentStableId = harkentest::ObjectId(kContainer);
            occurrence.parentProgramSemanticFingerprint =
                root.programSemanticFingerprint;
            occurrence.stableOccurrenceOrdinal = child.stableChildOrdinal;
            occurrence.childPlayable = child.playable;
            occurrence.payload = CanonicalPayload();
            occurrence.occurrenceSemanticFingerprint =
                harken::ComputePlayableOccurrenceFingerprintV1(occurrence);
        }
        for (std::uint32_t index = 0; index < discreteRows.size(); ++index) {
            auto& row = discreteRows[index];
            row.stableRowOrdinal = index + 1u;
            row.authoredStableOrder = index;
            row.childStableOrdinal = index + 1u;
            row.cellCount = 1;
            row.cells = &discreteCells[index];
        }
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
        for (std::uint32_t index = 0;
             index < continuousRows.size(); ++index) {
            auto& row = continuousRows[index];
            row.stableRowOrdinal = index + 1u;
            row.authoredStableOrder = index;
            row.childStableOrdinal = index + 3u;
            row.cellCount = 1;
            row.cells = &continuousCells[index];
        }

        container.containerStableId = harkentest::ObjectId(kContainer);
        container.programSemanticFingerprint =
            root.programSemanticFingerprint;
        container.mode = harken::ContainerModeV1::VariableControl;
        container.memoryScope = harken::MemoryScopeV1::Emitter;
        container.childCount = children.size();
        container.children = children.data();
        container.discreteTiming = 1;
        container.discreteAxisCount = 1;
        container.discreteAxes = &discreteAxis;
        container.discreteRowCount = discreteRows.size();
        container.discreteRows = discreteRows.data();
        container.continuousAxisCount = 1;
        container.continuousAxes = &continuousAxis;
        container.continuousRowCount = continuousRows.size();
        container.continuousRows = continuousRows.data();
        container.evaluationPeriodFrames = 64;
        container.envelopeFrames = 64;
        container.maxActiveRows = continuousRows.size();
        container.maxLiveRows = 2;

        world.variables = variables.data();
        world.variableCount = variables.size();
        world.playableProperties = properties.data();
        world.playablePropertyCount = properties.size();
        world.playableOccurrences = occurrences.data();
        world.playableOccurrenceCount = occurrences.size();
        world.containers = &container;
        world.containerCount = 1;
        world.identity.capabilityMask =
            harken::kWorldCapabilityVariableDefinitions |
            harken::kWorldCapabilityPlayableProperties |
            harken::kWorldCapabilityVariableControl;
        world.identity.runtimeSemanticFingerprint =
            harken::ComputeCurrentWorldFingerprintV4(world);
    }
};

VnPreviewPlayPlayableDescV2 Desc(std::uint32_t kind,
                                 VnObjectId stableId) {
    VnPreviewPlayPlayableDescV2 desc{};
    desc.structSize = sizeof(desc);
    desc.playableKind = kind;
    desc.stableId = stableId;
    return desc;
}

struct PreviewSession {
    VnConfig config = harkentest::MakeNullConfig(2, 64, 48000);
    harkentest::EngineSession session{config};
    std::vector<float> samples = std::vector<float>(4096, 0.25f);

    PreviewSession() {
        EXPECT_EQ(session.InitResult(), VN_OK);
        const VnPCMDesc pcm = harkentest::MonoDesc(samples, 48000);
        EXPECT_EQ(Vn_RegisterPCM(kSound, &pcm), VN_OK);
    }
};

bool PumpUntilNonSilent(harken::Engine& engine) {
    for (std::uint32_t block = 0; block < 16; ++block) {
        if (Vn_Update() != VN_OK) {
            return false;
        }
        engine.nullBackend.Pump(1);
        const float* output = engine.nullBackend.LastBlock();
        if (std::any_of(output, output + 2u * engine.config.blockFrames,
                        [](float sample) { return sample != 0.0f; })) {
            return true;
        }
    }
    return false;
}

#if HARKEN_DEVELOPMENT_PREVIEW

void ExpectPublicPreviewAudible(
    harken::ContainerModeV1 mode,
    harken::RandomDisciplineV1 discipline =
        harken::RandomDisciplineV1::Draw) {
    PreviewSession fixture;
    harken::Engine* engine = fixture.session.Engine();
    ASSERT_NE(engine, nullptr);
    const harken::PCMView media = engine->registry.Lookup(kSound);
    PreviewWorldImage image(media.contentRevision, mode, discipline);
    ASSERT_EQ(harken::InstallContainerWorldV1(*engine, image.world).status,
              harken::WorldInstallStatus::Ok);
    VnPreviewPlayPlayableDescV2 desc =
        Desc(VN_PREVIEW_PLAYABLE_KIND_CONTAINER, kContainer);
    VnPreviewPlayableHandle handle = 0;
    ASSERT_EQ(VnPreview_PlayPlayable(&desc, &handle), VN_PREVIEW_OK);
    ASSERT_TRUE(PumpUntilNonSilent(*engine));
    ASSERT_EQ(Vn_Update(), VN_OK);
    VnPreviewPlayableStateV2 state{};
    state.structSize = sizeof(state);
    ASSERT_EQ(VnPreview_QueryPlayable(handle, &state), VN_PREVIEW_OK);
    EXPECT_NE(state.lastAppliedBlockSequence, 0u);
}

void ExpectPublicSchedulePreviewAudible(std::uint32_t kind,
                                        VnObjectId stableId,
                                        bool repeatSameClip = false) {
    PreviewSession fixture;
    harken::Engine* engine = fixture.session.Engine();
    ASSERT_NE(engine, nullptr);
    const harken::PCMView media = engine->registry.Lookup(kSound);
    PreviewScheduleWorldImage image(media.contentRevision);
    std::array<harken::ScheduleClipPlacementV1, 2> placements{image.placement, image.placement};
    std::array<harken::PlayableOccurrencePropertyViewV1, 2> occurrences{image.occurrence, image.occurrence};
    placements[1].stablePlacementOrdinal = 1;
    placements[1].stableOccurrenceOrdinal = 8;
    placements[1].clipId = harkentest::ObjectId(93);
    placements[1].clipGuidLow = 94;
    occurrences[1].stableOccurrenceOrdinal = 8;
    occurrences[1].occurrenceSemanticFingerprint = harken::ComputePlayableOccurrenceFingerprintV1(occurrences[1]);
    image.track.placements = placements.data();
    image.track.placementCount = repeatSameClip ? 1u : placements.size();
    image.track.scheduleSemanticFingerprint = harken::ComputeTrackScheduleFingerprintV1(image.track);
    image.trackRef.trackScheduleSemanticFingerprint = image.track.scheduleSemanticFingerprint;
    image.piece.scheduleSemanticFingerprint = harken::ComputePieceScheduleFingerprintV1(image.piece);
    image.world.playableOccurrences = occurrences.data();
    image.world.playableOccurrenceCount = image.track.placementCount;
    image.world.identity.runtimeSemanticFingerprint = harken::ComputeCurrentWorldFingerprintV4(image.world);
    ASSERT_EQ(harken::InstallContainerWorldV1(*engine, image.world).status,
              harken::WorldInstallStatus::Ok);
    const auto capture = harkentest::monitor::OpenIdentityEvents();
    VnPreviewPlayPlayableDescV2 desc = Desc(kind, stableId);
    VnPreviewPlayableHandle handle = 0;
    ASSERT_EQ(VnPreview_PlayPlayable(&desc, &handle), VN_PREVIEW_OK);
    EXPECT_NE(handle, 0u);
    if (repeatSameClip) {
        VnPreviewPlayableHandle second = 0;
        ASSERT_EQ(VnPreview_PlayPlayable(&desc, &second), VN_PREVIEW_OK);
        EXPECT_NE(handle, second);
    }
    EXPECT_TRUE(PumpUntilNonSilent(*engine));
    const auto rows = harkentest::monitor::ReadIdentityEvents(capture);
    std::vector<std::uint64_t> voices;
    std::vector<std::uint64_t> requests;
    std::vector<std::uint32_t> ordinals;
    for (const auto& row : rows) {
        if (row.fact.factKind != VN_MONITOR_FACT_VOICE_LIVE) continue;
        EXPECT_EQ(VN_MONITOR_CONTENT_SOUND, row.fact.contentKind);
        EXPECT_EQ(kSound, row.header.stableId);
        EXPECT_EQ(media.contentRevision, row.fact.contentRevision);
        EXPECT_TRUE(harken::monitor::FactHas(row.fact, VN_MONITOR_FACT_FIELD_PLAYBACK_REQUEST));
        EXPECT_NE(0u, row.fact.playbackRequest.sequence);
        EXPECT_EQ(VN_MONITOR_OPERATION_PLAY_PREVIEW, row.fact.playbackRequest.operation);
        EXPECT_EQ(static_cast<unsigned>(harken::MonitorRequestOrigin::StudioPreview), row.fact.playbackRequest.origin);
        EXPECT_EQ(image.properties[0].programSemanticFingerprint, row.header.programSemanticFingerprint);
        EXPECT_TRUE(harken::monitor::FactHas(row.fact, VN_MONITOR_FACT_FIELD_CLIP));
        EXPECT_TRUE(harken::monitor::FactHas(row.fact, VN_MONITOR_FACT_FIELD_OCCURRENCE));
        EXPECT_EQ(row.header.telemetry.occurrenceOrdinal == 7 ? placements[0].clipId : placements[1].clipId,
            row.header.telemetry.clipId);
        voices.push_back(row.fact.logicalVoice);
        requests.push_back(row.fact.playbackRequest.sequence);
        ordinals.push_back(row.header.telemetry.occurrenceOrdinal);
    }
    ASSERT_EQ(2u, voices.size());
    EXPECT_NE(voices[0], voices[1]);
    EXPECT_EQ(!repeatSameClip, requests[0] == requests[1]);
    std::sort(ordinals.begin(), ordinals.end());
    EXPECT_EQ((repeatSameClip ? std::vector<std::uint32_t>{7, 7} :
        std::vector<std::uint32_t>{7, 8}), ordinals);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(capture));
}

TEST(PreviewAbi, LayoutAndPinnedVocabularyMatchTheTypedV2Contract) {
    EXPECT_EQ(sizeof(VnPreviewPlayPlayableDescV2), 40u);
    EXPECT_EQ(sizeof(VnPreviewResetPlayableDescV2), 40u);
    EXPECT_EQ(sizeof(VnPreviewPlayableStateV2), 72u);
    EXPECT_EQ(VN_PREVIEW_UNKNOWN_ID, 14);
    EXPECT_EQ(VN_PREVIEW_NO_VOICE, 15);
    EXPECT_EQ(VN_PREVIEW_BACKPRESSURE, 16);
    EXPECT_EQ(VN_PREVIEW_REVISION_CONFLICT, 17);
    EXPECT_EQ(VN_PREVIEW_PLAYABLE_KIND_SOUND, 0);
    EXPECT_EQ(VN_PREVIEW_PLAYABLE_KIND_CONTAINER, 1);
    EXPECT_EQ(VN_PREVIEW_PLAYABLE_KIND_TRACK, 2);
    EXPECT_EQ(VN_PREVIEW_PLAYABLE_KIND_PIECE, 3);
}

TEST(PreviewAbi, PlayClearsOutputOnEveryRefusal) {
    VnPreviewPlayableHandle handle = 99;
    EXPECT_EQ(VnPreview_PlayPlayable(nullptr, &handle),
              VN_PREVIEW_INVALID_ARG);
    EXPECT_EQ(handle, 0u);

    VnPreviewPlayPlayableDescV2 desc =
        Desc(VN_PREVIEW_PLAYABLE_KIND_SOUND, kSound);
    desc.structSize = 1;
    handle = 99;
    EXPECT_EQ(VnPreview_PlayPlayable(&desc, &handle),
              VN_PREVIEW_STRUCT_SIZE_MISMATCH);
    EXPECT_EQ(handle, 0u);
}

TEST(PreviewAbi, PlaybackPathsPublishRenderedOccurrencesAndClearAfterStop) {
    PreviewSession fixture;
    auto* engine = fixture.session.Engine();
    ASSERT_NE(engine, nullptr);
    PreviewWorldImage image(engine->registry.Lookup(kSound).contentRevision);
    ASSERT_EQ(harken::InstallContainerWorldV1(*engine, image.world).status,
              harken::WorldInstallStatus::Ok);
    auto desc = Desc(VN_PREVIEW_PLAYABLE_KIND_CONTAINER, kContainer);
    VnPreviewPlayableHandle handle = 0;
    ASSERT_EQ(VnPreview_PlayPlayable(&desc, &handle), VN_PREVIEW_OK);
    uint32_t required = 99;
    EXPECT_EQ(VnPreview_CapturePlaybackPaths(nullptr, 0, &required), VN_PREVIEW_OK);
    EXPECT_EQ(required, 0u);
    ASSERT_TRUE(PumpUntilNonSilent(*engine));
    EXPECT_EQ(VnPreview_CapturePlaybackPaths(nullptr, 0, &required), VN_PREVIEW_RESULT_CAPACITY);
    ASSERT_EQ(required, 1u);
    VnPreviewPlaybackPathV1 path{};
    ASSERT_EQ(VnPreview_CapturePlaybackPaths(&path, 1, &required), VN_PREVIEW_OK);
    ASSERT_EQ(path.rowCount, 3u);
    EXPECT_EQ(path.structSize, sizeof(path));
    EXPECT_EQ(path.soundId, kSound);
    EXPECT_EQ(path.outputBusId, VN_BUS_MASTER);
    EXPECT_EQ(path.rows[0].stableId, kContainer);
    EXPECT_EQ(path.rows[0].occurrenceOrdinal, UINT32_MAX);
    EXPECT_EQ(path.rows[1].stableId, kContainer);
    EXPECT_EQ(path.rows[1].occurrenceOrdinal, 7u);
    EXPECT_EQ(path.rows[2].stableId, kSound);
    const auto firstVoice = path.voiceInstance;
    ASSERT_EQ(VnPreview_StopPlayable(handle, 0), VN_PREVIEW_OK);
    for (uint32_t block = 0; block < 128 && required != 0; ++block) {
        ASSERT_EQ(Vn_Update(), VN_OK);
        engine->nullBackend.Pump(1);
        ASSERT_EQ(VnPreview_CapturePlaybackPaths(&path, 1, &required), VN_PREVIEW_OK);
    }
    ASSERT_EQ(required, 0u);
    ASSERT_EQ(VnPreview_PlayPlayable(&desc, &handle), VN_PREVIEW_OK);
    EXPECT_EQ(VnPreview_CapturePlaybackPaths(nullptr, 0, &required), VN_PREVIEW_OK);
    EXPECT_EQ(required, 0u);
    ASSERT_TRUE(PumpUntilNonSilent(*engine));
    ASSERT_EQ(VnPreview_CapturePlaybackPaths(&path, 1, &required), VN_PREVIEW_OK);
    EXPECT_EQ(required, 1u);
    EXPECT_NE(path.voiceInstance, firstVoice);
}

TEST(PreviewAbi, EventSessionCapturesRealPreviewLifecycleWithoutControllingPlayback) {
    PreviewSession fixture;
    auto* engine = fixture.session.Engine();
    ASSERT_NE(engine, nullptr);
    PreviewWorldImage image(engine->registry.Lookup(kSound).contentRevision);
    ASSERT_EQ(harken::InstallContainerWorldV1(*engine, image.world).status,
              harken::WorldInstallStatus::Ok);
    VnMonitorSessionDesc capture{};
    capture.structSize = sizeof(capture);
    capture.requestedStreamKindMask = VN_MONITOR_STREAM_EVENT;
    capture.requestedPermissionMask = VN_MONITOR_PERM_OBSERVE_EVENTS;
    capture.consumerQueueBytes = 65536;
    VnMonitorSessionHandle session = 0;
    ASSERT_EQ(VN_OK, Vn_MonitorOpenEventSession(&capture, &session));
    const auto identitySession = harkentest::monitor::OpenIdentityEvents();
    auto desc = Desc(VN_PREVIEW_PLAYABLE_KIND_CONTAINER, kContainer);
    VnPreviewPlayableHandle handle = 0;
    ASSERT_EQ(VN_PREVIEW_OK, VnPreview_PlayPlayable(&desc, &handle));
    const auto firstRoot = handle & ~(UINT64_C(1) << 63);
    ASSERT_TRUE(PumpUntilNonSilent(*engine));
    ASSERT_EQ(VN_PREVIEW_OK, VnPreview_StopPlayable(handle, 0));
    for (int block = 0; block < 8; ++block) {
        engine->nullBackend.Pump(1);
        ASSERT_EQ(VN_OK, Vn_Update());
    }
    ASSERT_EQ(VN_PREVIEW_OK, VnPreview_PlayPlayable(&desc, &handle));
    const auto secondRoot = handle & ~(UINT64_C(1) << 63);
    ASSERT_TRUE(PumpUntilNonSilent(*engine));
    ASSERT_EQ(VN_OK, Vn_MonitorSealEventSession(session));
    ASSERT_EQ(VN_OK, Vn_Update());
    harken::monitor::DrainOnceForTests();
    std::vector<unsigned char> bytes(65536);
    VnMonitorPollDesc poll{};
    poll.structSize = sizeof(poll);
    poll.buffer = bytes.data();
    poll.bufferBytes = bytes.size();
    VnMonitorPollResult result{};
    result.structSize = sizeof(result);
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(session, &poll, &result));
    bool played = false, stopped = false, started = false, retired = false;
    for (std::uint64_t offset = 0; offset < result.bytesWritten;) {
        std::uint32_t kind = 0;
        std::uint64_t recordBytes = 0;
        std::memcpy(&kind, bytes.data() + offset, sizeof(kind));
        std::memcpy(&recordBytes, bytes.data() + offset + 16, sizeof(recordBytes));
        ASSERT_GE(recordBytes, VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES);
        if (kind == VN_MONITOR_RECORD_EVENT_DETAIL) {
            VnMonitorEventDetail detail{};
            std::memcpy(&detail, bytes.data() + offset + VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES,
                        sizeof(detail));
            if (detail.category == VN_MONITOR_EVENT_CATEGORY_OPERATION) {
                played |= detail.code == VN_MONITOR_OPERATION_PLAY_PREVIEW && detail.result == VN_PREVIEW_OK;
                stopped |= detail.code == VN_MONITOR_OPERATION_STOP_PREVIEW && detail.result == VN_PREVIEW_OK;
            } else if (detail.category == VN_MONITOR_EVENT_CATEGORY_LIFECYCLE) {
                started |= detail.code == VN_MONITOR_LIFECYCLE_PLAYABLE_START;
                retired |= detail.code == VN_MONITOR_LIFECYCLE_PLAYABLE_RETIRED;
            }
        }
        offset += recordBytes;
    }
    EXPECT_TRUE(played);
    EXPECT_TRUE(stopped);
    EXPECT_TRUE(started);
    EXPECT_TRUE(retired);
    const auto rows = harkentest::monitor::ReadIdentityEvents(identitySession);
    std::uint64_t firstRequest = 0, secondRequest = 0;
    for (const auto& row : rows) {
        if (row.fact.factKind != VN_MONITOR_FACT_PLAYABLE_ACTIVE) continue;
        if (row.header.instanceHandle == firstRoot) firstRequest = row.fact.playbackRequest.sequence;
        if (row.header.instanceHandle == secondRoot) secondRequest = row.fact.playbackRequest.sequence;
    }
    ASSERT_NE(0u, firstRequest);
    ASSERT_NE(0u, secondRequest);
    EXPECT_NE(firstRequest, secondRequest);
    bool soundStopped = false;
    for (const auto& row : rows) {
        if (row.fact.root != firstRoot && row.fact.root != secondRoot) continue;
        EXPECT_EQ(row.fact.root == firstRoot ? firstRequest : secondRequest,
            row.fact.playbackRequest.sequence);
        if (row.fact.factKind != VN_MONITOR_FACT_PLAYABLE_RETIRED ||
            row.fact.contentKind != VN_MONITOR_CONTENT_SOUND) continue;
        soundStopped = true;
        EXPECT_EQ(firstRoot, row.fact.root);
        EXPECT_TRUE(harken::monitor::FactHas(row.fact, VN_MONITOR_FACT_FIELD_TERMINAL_KIND));
        EXPECT_EQ(VN_MONITOR_TERMINAL_REQUESTED_STOP, row.fact.terminalKind);
        EXPECT_EQ(VN_OK, row.fact.result);
    }
    EXPECT_TRUE(soundStopped);
    ASSERT_EQ(VN_OK, Vn_MonitorCloseSession(identitySession));
    ASSERT_EQ(VN_OK, Vn_MonitorCloseSession(session));
    EXPECT_TRUE(PumpUntilNonSilent(*engine));
}

TEST(PreviewAbi, AudibilityParkingPreservesPlayablePathsAndExcludesReturnedRawVoices) {
    auto config = harkentest::MakeNullConfig(2, 64, 48000);
    config.maxParkedVoices = 2;
    config.audibilityParkEnabled = 1;
    config.parkThresholdDb = -40.0f;
    harkentest::EngineSession session(config);
    ASSERT_EQ(session.InitResult(), VN_OK);
    auto* engine = session.Engine();
    ASSERT_NE(engine, nullptr);
    const std::vector<float> samples(48000, 0.25f);
    const auto pcm = harkentest::MonoDesc(samples, 48000);
    ASSERT_EQ(Vn_RegisterPCM(kSound, &pcm), VN_OK);

    PreviewWorldImage image(engine->registry.Lookup(kSound).contentRevision);
    constexpr auto volumeSlot = static_cast<std::size_t>(
        harken::playableproperties::PlayablePropertyIdV1::Volume) - 1u;
    image.properties[0].payload.presentMask |= UINT64_C(1) << volumeSlot;
    image.properties[0].payload.values[volumeSlot].bits =
        std::bit_cast<std::uint32_t>(-80.0f);
    image.properties[0].programSemanticFingerprint =
        harken::ComputePlayablePropertyFingerprintV1(image.properties[0]);
    image.child.playable.expectedProgramSemanticFingerprint =
        image.properties[0].programSemanticFingerprint;
    image.occurrence.childPlayable = image.child.playable;
    image.occurrence.occurrenceSemanticFingerprint =
        harken::ComputePlayableOccurrenceFingerprintV1(image.occurrence);
    image.world.identity.runtimeSemanticFingerprint =
        harken::ComputeCurrentWorldFingerprintV4(image.world);
    ASSERT_EQ(harken::InstallContainerWorldV1(*engine, image.world).status,
              harken::WorldInstallStatus::Ok);

    auto desc = Desc(VN_PREVIEW_PLAYABLE_KIND_CONTAINER, kContainer);
    VnPreviewPlayableHandle handle = 0;
    ASSERT_EQ(VnPreview_PlayPlayable(&desc, &handle), VN_PREVIEW_OK);
    VnPreviewPlaybackPathV1 path{};
    std::uint32_t required = 0;
    for (std::uint32_t block = 0; block < 32 && required == 0; ++block) {
        ASSERT_EQ(Vn_Update(), VN_OK);
        engine->nullBackend.Pump(1);
        ASSERT_EQ(VnPreview_CapturePlaybackPaths(&path, 1, &required), VN_PREVIEW_OK);
    }
    ASSERT_EQ(required, 1u);
    const auto playableRender = path.voiceInstance;
    ASSERT_LT(engine->params.ResolvedValue(playableRender, harken::kTargetVoiceVolume),
              config.parkThresholdDb);
    ASSERT_EQ(engine->ledger.OwnerForRender(playableRender).visibility,
              harken::VoiceLedger::PublicVisibility::PlayableOwnedInternal);
    ASSERT_EQ(path.rowCount, 3u);
    EXPECT_EQ(path.rows[1].stableId, kContainer);
    EXPECT_EQ(path.rows[1].occurrenceOrdinal, 7u);

    const auto rawDesc = harkentest::MakePlayDesc(kSound);
    VnVoiceHandle raw = 0;
    ASSERT_EQ(harkentest::PlayRaw(&rawDesc, &raw), VN_OK);
    const auto originalRawRender = harkentest::RenderHandle(engine, raw);
    ASSERT_NE(originalRawRender, 0u);
    engine->nullBackend.Pump(1);
    ASSERT_EQ(Vn_SetVoiceVolume(raw, -80.0f, 0), VN_OK);
    for (std::uint32_t block = 0; block < 8; ++block) {
        ASSERT_EQ(Vn_Update(), VN_OK);
        engine->nullBackend.Pump(1);
        ASSERT_EQ(VnPreview_CapturePlaybackPaths(&path, 1, &required), VN_PREVIEW_OK);
        ASSERT_EQ(required, 1u);
        EXPECT_EQ(path.voiceInstance, playableRender);
    }
    ASSERT_EQ(engine->ledger.Classify(raw).kind, harken::VoiceLedger::HandleKind::Parked);
    ASSERT_EQ(engine->ledger.ParkedCount(), 1u);
    ASSERT_EQ(engine->voiceParkedCount, 1u);

    ASSERT_EQ(Vn_SetVoiceVolume(raw, 0.0f, 0), VN_OK);
    for (std::uint32_t block = 0; block < 8; ++block) {
        ASSERT_EQ(Vn_Update(), VN_OK);
        engine->nullBackend.Pump(1);
        ASSERT_EQ(VnPreview_CapturePlaybackPaths(&path, 1, &required), VN_PREVIEW_OK);
        ASSERT_EQ(required, 1u);
        EXPECT_EQ(path.voiceInstance, playableRender);
    }
    ASSERT_EQ(engine->ledger.Classify(raw).kind, harken::VoiceLedger::HandleKind::Live);
    EXPECT_NE(harkentest::RenderHandle(engine, raw), originalRawRender);
    ASSERT_EQ(engine->ledger.ParkedCount(), 0u);
    ASSERT_EQ(engine->voiceReturnedCount, 1u);

    ASSERT_EQ(VnPreview_StopPlayable(handle, 0), VN_PREVIEW_OK);
    for (std::uint32_t block = 0; block < 32; ++block) {
        ASSERT_EQ(Vn_Update(), VN_OK);
        engine->nullBackend.Pump(1);
        ASSERT_EQ(VnPreview_CapturePlaybackPaths(&path, 1, &required), VN_PREVIEW_OK);
    }
    ASSERT_EQ(required, 0u);
    ASSERT_EQ(Vn_SetVoiceVolume(raw, -80.0f, 0), VN_OK);
    for (std::uint32_t block = 0; block < 8; ++block) {
        ASSERT_EQ(Vn_Update(), VN_OK);
        engine->nullBackend.Pump(1);
    }
    ASSERT_EQ(engine->ledger.Classify(raw).kind, harken::VoiceLedger::HandleKind::Parked);
    ASSERT_EQ(Vn_SetVoiceVolume(raw, 0.0f, 0), VN_OK);
    for (std::uint32_t block = 0; block < 8; ++block) {
        ASSERT_EQ(Vn_Update(), VN_OK);
        engine->nullBackend.Pump(1);
        ASSERT_EQ(VnPreview_CapturePlaybackPaths(&path, 1, &required), VN_PREVIEW_OK);
        EXPECT_EQ(required, 0u);
    }
    ASSERT_EQ(engine->ledger.Classify(raw).kind, harken::VoiceLedger::HandleKind::Live);
    const auto returnedRawRender = harkentest::RenderHandle(engine, raw);
    EXPECT_EQ(static_cast<std::uint32_t>(returnedRawRender),
              static_cast<std::uint32_t>(playableRender));
    EXPECT_NE(returnedRawRender, playableRender);
    EXPECT_EQ(engine->voiceParkedCount, 2u);
    EXPECT_EQ(engine->voiceReturnedCount, 2u);
}

TEST(PreviewAbi, PlaybackPathsKeepTrackPlacementAndPieceIdentity) {
    PreviewSession fixture;
    auto* engine = fixture.session.Engine();
    ASSERT_NE(engine, nullptr);
    PreviewScheduleWorldImage image(engine->registry.Lookup(kSound).contentRevision);
    ASSERT_EQ(harken::InstallContainerWorldV1(*engine, image.world).status,
              harken::WorldInstallStatus::Ok);
    auto desc = Desc(VN_PREVIEW_PLAYABLE_KIND_PIECE, kPiece);
    VnPreviewPlayableHandle handle = 0;
    ASSERT_EQ(VnPreview_PlayPlayable(&desc, &handle), VN_PREVIEW_OK);
    ASSERT_TRUE(PumpUntilNonSilent(*engine));
    VnPreviewPlaybackPathV1 path{};
    uint32_t required = 0;
    ASSERT_EQ(VnPreview_CapturePlaybackPaths(&path, 1, &required), VN_PREVIEW_OK);
    ASSERT_EQ(required, 1u);
    ASSERT_EQ(path.rowCount, 4u);
    EXPECT_EQ(path.rows[0].stableId, kPiece);
    EXPECT_EQ(path.rows[1].stableId, kTrack);
    EXPECT_EQ(path.rows[2].stableId, kTrack);
    EXPECT_EQ(path.rows[2].occurrenceOrdinal, 7u);
    EXPECT_EQ(path.rows[3].stableId, kSound);
}

TEST(PreviewAbi, PlaybackCaptureRejectsInvalidBuffersAndReportsUnavailableRuntime) {
    EXPECT_EQ(VnPreview_CapturePlaybackPaths(nullptr, 0, nullptr), VN_PREVIEW_INVALID_ARG);
    uint32_t required = 99;
    EXPECT_EQ(VnPreview_CapturePlaybackPaths(nullptr, 1, &required), VN_PREVIEW_INVALID_ARG);
    EXPECT_EQ(required, 0u);
    EXPECT_EQ(VnPreview_CapturePlaybackPaths(nullptr, 0, &required), VN_PREVIEW_HOST_ERROR);
    EXPECT_EQ(required, 0u);
}

TEST(PreviewAbi, PlaybackPathsObserveVoicesBeyondTheLegacySnapshotAndExcludeRawVoices) {
    const auto config = harkentest::MakeNullConfig(VN_SNAPSHOT_MAX_VOICES + 1, 64, 48000);
    harkentest::EngineSession session(config);
    ASSERT_EQ(session.InitResult(), VN_OK);
    auto* engine = session.Engine();
    ASSERT_NE(engine, nullptr);
    const std::vector<float> samples(48000, 0.001f);
    const auto pcm = harkentest::MonoDesc(samples, 48000);
    ASSERT_EQ(Vn_RegisterPCM(kSound, &pcm), VN_OK);
    PreviewWorldImage image(engine->registry.Lookup(kSound).contentRevision);
    ASSERT_EQ(harken::InstallContainerWorldV1(*engine, image.world).status,
              harken::WorldInstallStatus::Ok);
    const auto raw = harkentest::MakePlayDesc(kSound);
    for (uint32_t index = 0; index < VN_SNAPSHOT_MAX_VOICES; ++index) {
        VnVoiceHandle handle = 0;
        ASSERT_EQ(harkentest::PlayRaw(&raw, &handle), VN_OK);
    }
    auto desc = Desc(VN_PREVIEW_PLAYABLE_KIND_CONTAINER, kContainer);
    VnPreviewPlayableHandle handle = 0;
    ASSERT_EQ(VnPreview_PlayPlayable(&desc, &handle), VN_PREVIEW_OK);
    VnPreviewPlaybackPathV1 path{};
    uint32_t required = 0;
    for (uint32_t block = 0; block < 32 && required == 0; ++block) {
        ASSERT_EQ(Vn_Update(), VN_OK);
        engine->nullBackend.Pump(1);
        ASSERT_EQ(VnPreview_CapturePlaybackPaths(&path, 1, &required), VN_PREVIEW_OK);
    }
    ASSERT_EQ(required, 1u);
    EXPECT_GE(static_cast<uint32_t>(path.voiceInstance), VN_SNAPSHOT_MAX_VOICES);
    EXPECT_EQ(path.soundId, kSound);
}

TEST(PreviewAbi, ResetRefusesBeforeRuntimeMutation) {
    EXPECT_EQ(VnPreview_ResetPlayable(nullptr), VN_PREVIEW_INVALID_ARG);
    VnPreviewResetPlayableDescV2 desc{};
    desc.structSize = 1;
    EXPECT_EQ(VnPreview_ResetPlayable(&desc), VN_PREVIEW_STRUCT_SIZE_MISMATCH);
}

TEST(PreviewAbi, SequenceResetClearsPreviewSelectionMemory) {
    PreviewSession fixture;
    harken::Engine* engine = fixture.session.Engine();
    ASSERT_NE(engine, nullptr);
    const harken::PCMView media = engine->registry.Lookup(kSound);
    PreviewWorldImage image(media.contentRevision,
                            harken::ContainerModeV1::Sequence);
    ASSERT_EQ(harken::InstallContainerWorldV1(*engine, image.world).status,
              harken::WorldInstallStatus::Ok);

    VnEmitterHandle emitter = 0;
    ASSERT_EQ(Vn_CreateEmitter(&emitter), VN_OK);
    VnPreviewPlayPlayableDescV2 play{};
    play.structSize = sizeof(play);
    play.playableKind = VN_PREVIEW_PLAYABLE_KIND_CONTAINER;
    play.stableId = harkentest::ObjectId(kContainer);
    play.emitterHandle = emitter;
    VnPreviewPlayableHandle handle = 0;
    ASSERT_EQ(VnPreview_PlayPlayable(&play, &handle), VN_PREVIEW_OK);
    ASSERT_TRUE(PumpUntilNonSilent(*engine));
    ASSERT_EQ(Vn_Update(), VN_OK);

    harken::SelectionMemoryKeyV1 key{};
    key.world = engine->variables.CurrentWorldGeneration();
    key.containerStableId = harkentest::ObjectId(kContainer);
    key.programSemanticFingerprint =
        image.container.programSemanticFingerprint;
    key.memoryScope = harken::MemoryScopeV1::Global;
    const harken::SelectionMemorySnapshotV1 before =
        engine->selectionMemory.Snapshot(key);
    ASSERT_TRUE(before.found);
    EXPECT_EQ(before.pendingCount, 0u);
    EXPECT_NE(before.selectionOrdinal, 0u);

    VnPreviewResetPlayableDescV2 reset{};
    reset.structSize = sizeof(reset);
    reset.playableKind = VN_PREVIEW_PLAYABLE_KIND_CONTAINER;
    reset.stableId = harkentest::ObjectId(kContainer);
    reset.emitterHandle = emitter;
    ASSERT_EQ(VnPreview_ResetPlayable(&reset), VN_PREVIEW_OK);
    const harken::SelectionMemorySnapshotV1 after =
        engine->selectionMemory.Snapshot(key);
    ASSERT_TRUE(after.found);
    EXPECT_EQ(after.selectionOrdinal, 0u);
    EXPECT_GT(after.resetEpoch, before.resetEpoch);
}

TEST(PreviewAbi, TypedContainerPlayReturnsTaggedRootAndLogicOwnedState) {
    PreviewSession fixture;
    harken::Engine* engine = fixture.session.Engine();
    ASSERT_NE(engine, nullptr);
    const harken::PCMView media = engine->registry.Lookup(kSound);
    ASSERT_TRUE(media.Valid());
    PreviewWorldImage image(media.contentRevision);
    ASSERT_EQ(harken::InstallContainerWorldV1(*engine, image.world).status,
              harken::WorldInstallStatus::Ok);

    const std::uint64_t issued = engine->logicCommits.Current();
    VnPreviewPlayPlayableDescV2 desc =
        Desc(VN_PREVIEW_PLAYABLE_KIND_CONTAINER, kContainer);
    VnPreviewPlayableHandle handle = 0;
    ASSERT_EQ(VnPreview_PlayPlayable(&desc, &handle), VN_PREVIEW_OK);
    ASSERT_NE(handle, 0u);
    EXPECT_NE(handle & (std::uint64_t{1} << 63), 0u);

    VnPreviewPlayableStateV2 state{};
    state.structSize = sizeof(state);
    ASSERT_EQ(VnPreview_QueryPlayable(handle, &state), VN_PREVIEW_OK);
    EXPECT_EQ(state.playableKind, VN_PREVIEW_PLAYABLE_KIND_CONTAINER);
    EXPECT_EQ(state.lifecycle, VN_PREVIEW_LIFECYCLE_PREPARING);
    EXPECT_EQ(state.reason, VN_PREVIEW_REASON_NONE);
    EXPECT_EQ(state.stableId, kContainer);
    EXPECT_NE(state.worldGeneration, 0u);
    EXPECT_EQ(state.acceptedLogicTicket, issued + 1u);
    EXPECT_EQ(state.lastAppliedBlockSequence, 0u);
    EXPECT_EQ(state.pendingFutureCount, 1u);
}

TEST(PreviewAbi, QueryRefusalsLeaveEveryCallerByteUntouched) {
    VnPreviewPlayableStateV2 state{};
    std::memset(&state, 0xA5, sizeof(state));
    state.structSize = sizeof(state);
    const VnPreviewPlayableStateV2 before = state;
    EXPECT_EQ(VnPreview_QueryPlayable(7, &state), VN_PREVIEW_INVALID_ARG);
    EXPECT_EQ(std::memcmp(&state, &before, sizeof(state)), 0);

    state.structSize = 1;
    const VnPreviewPlayableStateV2 wrongSize = state;
    EXPECT_EQ(VnPreview_QueryPlayable(std::uint64_t{1} << 63, &state),
              VN_PREVIEW_STRUCT_SIZE_MISMATCH);
    EXPECT_EQ(std::memcmp(&state, &wrongSize, sizeof(state)), 0);
}

TEST(PreviewAbi, MissingScheduleIdentitiesRefuseBeforeRuntimeMutation) {
    PreviewSession fixture;
    harken::Engine* engine = fixture.session.Engine();
    ASSERT_NE(engine, nullptr);
    const std::uint64_t issued = engine->logicCommits.Current();
    const std::uint32_t freeInstances = engine->playableInstances.FreeCount();
    const std::uint64_t head = engine->commands.PublishedHead();
    for (const std::uint32_t kind : {VN_PREVIEW_PLAYABLE_KIND_TRACK,
                                     VN_PREVIEW_PLAYABLE_KIND_PIECE}) {
        VnPreviewPlayPlayableDescV2 desc = Desc(kind, harkentest::ObjectId(44));
        VnPreviewPlayableHandle handle = 99;
        EXPECT_EQ(VnPreview_PlayPlayable(&desc, &handle),
                  VN_PREVIEW_UNKNOWN_ID);
        EXPECT_EQ(handle, 0u);
    }
    EXPECT_EQ(engine->logicCommits.Current(), issued);
    EXPECT_EQ(engine->playableInstances.FreeCount(), freeInstances);
    EXPECT_EQ(engine->commands.PublishedHead(), head);
}

TEST(PreviewAbi, KindScopedLookupReportsUnknownIdWithoutCrossKindFallback) {
    PreviewSession fixture;
    harken::Engine* engine = fixture.session.Engine();
    ASSERT_NE(engine, nullptr);
    const harken::PCMView media = engine->registry.Lookup(kSound);
    PreviewWorldImage image(media.contentRevision);
    ASSERT_EQ(harken::InstallContainerWorldV1(*engine, image.world).status,
              harken::WorldInstallStatus::Ok);

    VnPreviewPlayPlayableDescV2 desc =
        Desc(VN_PREVIEW_PLAYABLE_KIND_SOUND, kContainer);
    VnPreviewPlayableHandle handle = 99;
    EXPECT_EQ(VnPreview_PlayPlayable(&desc, &handle),
              VN_PREVIEW_UNKNOWN_ID);
    EXPECT_EQ(handle, 0u);
}

TEST(PreviewAbi, StopRejectsVoiceShapedHandlesAndIsIdempotentForRoot) {
    PreviewSession fixture;
    harken::Engine* engine = fixture.session.Engine();
    ASSERT_NE(engine, nullptr);
    const harken::PCMView media = engine->registry.Lookup(kSound);
    PreviewWorldImage image(media.contentRevision);
    ASSERT_EQ(harken::InstallContainerWorldV1(*engine, image.world).status,
              harken::WorldInstallStatus::Ok);
    VnPreviewPlayPlayableDescV2 desc =
        Desc(VN_PREVIEW_PLAYABLE_KIND_SOUND, kSound);
    VnPreviewPlayableHandle handle = 0;
    ASSERT_EQ(VnPreview_PlayPlayable(&desc, &handle), VN_PREVIEW_OK);

    EXPECT_EQ(VnPreview_StopPlayable(7, 10), VN_PREVIEW_INVALID_ARG);
    EXPECT_EQ(VnPreview_StopPlayable(handle, 10), VN_PREVIEW_OK);
    EXPECT_EQ(VnPreview_StopPlayable(handle, 10), VN_PREVIEW_OK);
    VnPreviewPlayableStateV2 state{};
    state.structSize = sizeof(state);
    ASSERT_EQ(VnPreview_QueryPlayable(handle, &state), VN_PREVIEW_OK);
    EXPECT_EQ(state.lifecycle, VN_PREVIEW_LIFECYCLE_CANCELLING);
    EXPECT_EQ(state.reason, VN_PREVIEW_REASON_STOP_REQUESTED);
    EXPECT_EQ(state.pendingFutureCount, 0u);
}

TEST(PreviewAbi, DirectSoundKeepsOneExecutionIdentityAcrossConcurrentAndRepeatedPlayback) {
    PreviewSession fixture;
    harken::Engine* engine = fixture.session.Engine();
    ASSERT_NE(engine, nullptr);
    PreviewWorldImage image(engine->registry.Lookup(kSound).contentRevision);
    ASSERT_EQ(harken::InstallContainerWorldV1(*engine, image.world).status,
              harken::WorldInstallStatus::Ok);
    const auto instanceBaseline = engine->playableInstances.LiveCount();
    const auto voiceBaseline = engine->voices.ActiveCount();
    const auto armBaseline = engine->timelineArms.LiveCount();
    const auto pendingBaseline = engine->ledger.PendingStartCount();
    const auto parkedBaseline = engine->ledger.ParkedCount();
    const auto capture = harkentest::monitor::OpenIdentityEvents();
    std::vector<harkentest::monitor::IdentityRecord> rows;
    const auto desc = Desc(VN_PREVIEW_PLAYABLE_KIND_SOUND, kSound);
    std::array<VnPreviewPlayableHandle, 3> handles{};
    ASSERT_EQ(VnPreview_PlayPlayable(&desc, &handles[0]), VN_PREVIEW_OK);
    ASSERT_EQ(VnPreview_PlayPlayable(&desc, &handles[1]), VN_PREVIEW_OK);
    EXPECT_NE(handles[0], handles[1]);
    EXPECT_EQ(instanceBaseline + 2u, engine->playableInstances.LiveCount());
    ASSERT_TRUE(PumpUntilNonSilent(*engine));
    harkentest::monitor::CollectIdentityEvents(capture, rows);
    ASSERT_EQ(VnPreview_StopPlayable(handles[0], 0), VN_PREVIEW_OK);

    const auto finishPlaybacks = [&]() {
        for (unsigned block = 0; block != 256; ++block) {
            ASSERT_EQ(Vn_Update(), VN_OK);
            engine->nullBackend.Pump(1);
            harkentest::monitor::CollectIdentityEvents(capture, rows);
            bool allRetired = true;
            for (const auto handle : handles) {
                if (handle == 0) continue;
                VnPreviewPlayableStateV2 state{};
                state.structSize = sizeof(state);
                const auto status = VnPreview_QueryPlayable(handle, &state);
                ASSERT_TRUE(status == VN_PREVIEW_OK ||
                            status == VN_PREVIEW_STALE_HANDLE);
                allRetired &= status == VN_PREVIEW_STALE_HANDLE;
            }
            if (allRetired) return;
        }
        FAIL() << "Sound playback did not retire after its audio completed";
    };
    ASSERT_NO_FATAL_FAILURE(finishPlaybacks());
    EXPECT_EQ(instanceBaseline, engine->playableInstances.LiveCount());
    EXPECT_EQ(voiceBaseline, engine->voices.ActiveCount());
    ASSERT_EQ(VnPreview_PlayPlayable(&desc, &handles[2]), VN_PREVIEW_OK);
    EXPECT_NE(handles[0], handles[2]);
    EXPECT_NE(handles[1], handles[2]);
    ASSERT_TRUE(PumpUntilNonSilent(*engine));
    harkentest::monitor::CollectIdentityEvents(capture, rows);
    ASSERT_NO_FATAL_FAILURE(finishPlaybacks());

    std::array<std::uint64_t, 3> requests{}, voices{};
    unsigned soundActiveCount = 0;
    unsigned soundRetiredCount = 0;
    for (const auto& row : rows) {
        if (row.fact.contentKind != VN_MONITOR_CONTENT_SOUND) continue;
        soundActiveCount += row.fact.factKind == VN_MONITOR_FACT_PLAYABLE_ACTIVE;
        soundRetiredCount += row.fact.factKind == VN_MONITOR_FACT_PLAYABLE_RETIRED;
    }
    EXPECT_EQ(handles.size(), soundActiveCount);
    EXPECT_EQ(handles.size(), soundRetiredCount);
    for (std::size_t index = 0; index != handles.size(); ++index) {
        const auto root = handles[index] & ~(UINT64_C(1) << 63);
        unsigned activeCount = 0, startedCount = 0, retiredCount = 0, liveVoiceCount = 0;
        for (const auto& row : rows) {
            if (row.fact.root != root) continue;
            if (row.fact.factKind != VN_MONITOR_FACT_PLAYABLE_ACTIVE &&
                row.fact.factKind != VN_MONITOR_FACT_PLAYABLE_STARTED &&
                row.fact.factKind != VN_MONITOR_FACT_PLAYABLE_RETIRED &&
                row.fact.factKind != VN_MONITOR_FACT_VOICE_LIVE) continue;
            EXPECT_EQ(kSound, row.header.stableId);
            EXPECT_EQ(VN_MONITOR_CONTENT_SOUND, row.fact.contentKind);
            EXPECT_TRUE(harken::monitor::FactHas(
                row.fact, VN_MONITOR_FACT_FIELD_PLAYBACK_REQUEST));
            EXPECT_NE(0u, row.fact.playbackRequest.sequence);
            if (requests[index] == 0) requests[index] = row.fact.playbackRequest.sequence;
            EXPECT_EQ(requests[index], row.fact.playbackRequest.sequence);
            if (row.fact.factKind == VN_MONITOR_FACT_VOICE_LIVE) {
                ++liveVoiceCount;
                EXPECT_TRUE(harken::monitor::FactHas(
                    row.fact, VN_MONITOR_FACT_FIELD_SERVICE_LEAF));
                EXPECT_EQ(root, row.fact.serviceLeaf);
                voices[index] = row.fact.logicalVoice;
                EXPECT_NE(0u, voices[index]);
                continue;
            }
            EXPECT_EQ(root, row.header.instanceHandle);
            EXPECT_EQ(0u, row.header.playableInstanceHandle);
            if (row.fact.factKind == VN_MONITOR_FACT_PLAYABLE_ACTIVE) {
                ++activeCount;
            } else if (row.fact.factKind == VN_MONITOR_FACT_PLAYABLE_STARTED) {
                ++startedCount;
            } else {
                ++retiredCount;
                EXPECT_EQ(1u, startedCount);
                EXPECT_TRUE(harken::monitor::FactHas(
                    row.fact, VN_MONITOR_FACT_FIELD_TERMINAL_KIND));
                EXPECT_EQ(index == 0 ? VN_MONITOR_TERMINAL_REQUESTED_STOP :
                                      VN_MONITOR_TERMINAL_NATURAL_COMPLETION,
                          row.fact.terminalKind);
            }
        }
        EXPECT_EQ(1u, activeCount);
        EXPECT_EQ(1u, startedCount);
        EXPECT_EQ(1u, retiredCount);
        EXPECT_EQ(1u, liveVoiceCount);
        for (std::size_t previous = 0; previous != index; ++previous) {
            EXPECT_NE(requests[previous], requests[index]);
            EXPECT_NE(voices[previous], voices[index]);
        }
    }
    EXPECT_EQ(instanceBaseline, engine->playableInstances.LiveCount());
    EXPECT_EQ(voiceBaseline, engine->voices.ActiveCount());
    EXPECT_EQ(armBaseline, engine->timelineArms.LiveCount());
    EXPECT_EQ(pendingBaseline, engine->ledger.PendingStartCount());
    EXPECT_EQ(parkedBaseline, engine->ledger.ParkedCount());
    EXPECT_FALSE(engine->runtimeRecreateRequired);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(capture));
}

TEST(PreviewAbi, RandomDrawRunsThroughThePublicPreviewAndProducesAudio) {
    ExpectPublicPreviewAudible(harken::ContainerModeV1::Random);
}

TEST(PreviewAbi, RandomRotationRunsThroughThePublicPreviewAndProducesAudio) {
    ExpectPublicPreviewAudible(harken::ContainerModeV1::Random,
                               harken::RandomDisciplineV1::Rotation);
}

TEST(PreviewAbi, SequenceRunsThroughThePublicPreviewAndProducesAudio) {
    ExpectPublicPreviewAudible(harken::ContainerModeV1::Sequence);
}

TEST(PreviewAbi, TrackAndPieceRunThroughPublicPreviewAndProduceAudio) {
    ExpectPublicSchedulePreviewAudible(VN_PREVIEW_PLAYABLE_KIND_TRACK,
                                       kTrack);
    ExpectPublicSchedulePreviewAudible(VN_PREVIEW_PLAYABLE_KIND_PIECE,
                                       kPiece);
}

TEST(PreviewAbi, RepeatedScheduleClipKeepsDistinctVoiceInstances) {
    ExpectPublicSchedulePreviewAudible(VN_PREVIEW_PLAYABLE_KIND_TRACK, kTrack, true);
    ExpectPublicSchedulePreviewAudible(VN_PREVIEW_PLAYABLE_KIND_PIECE, kPiece, true);
}

TEST(PreviewAbi, VariableControlRunsThroughFirstPublicPreviewAndProducesAudio) {
    PreviewSession fixture;
    harken::Engine* engine = fixture.session.Engine();
    ASSERT_NE(engine, nullptr);
    const harken::PCMView media = engine->registry.Lookup(kSound);
    PreviewVariableControlWorldImage image(media.contentRevision);
    ASSERT_EQ(harken::InstallContainerWorldV1(*engine, image.world).status,
              harken::WorldInstallStatus::Ok);

    VnPreviewPlayPlayableDescV2 desc =
        Desc(VN_PREVIEW_PLAYABLE_KIND_CONTAINER, kContainer);
    VnPreviewPlayableHandle handle = 0;
    ASSERT_EQ(VnPreview_PlayPlayable(&desc, &handle), VN_PREVIEW_OK);
    EXPECT_NE(handle, 0u);
    EXPECT_TRUE(PumpUntilNonSilent(*engine));
}

TEST(PreviewAbi,
     VariableControlInitialEmitterPendingWriteBackpressuresThenUsesAppliedBank) {
    VnConfig config = harkentest::MakeNullConfig(16, 64, 48000);
    harkentest::EngineSession session(config);
    ASSERT_EQ(session.InitResult(), VN_OK);
    harken::Engine* engine = session.Engine();
    ASSERT_NE(engine, nullptr);
    std::vector<float> firstSamples(65536, 0.25f);
    std::vector<float> secondSamples(65536, -0.25f);
    const VnPCMDesc firstPcm =
        harkentest::MonoDesc(firstSamples, 48000);
    const VnPCMDesc secondPcm =
        harkentest::MonoDesc(secondSamples, 48000);
    ASSERT_EQ(Vn_RegisterPCM(kSound, &firstPcm), VN_OK);
    ASSERT_EQ(Vn_RegisterPCM(kSecondSound, &secondPcm), VN_OK);
    PreviewDualVariableControlWorldImage image(
        engine->registry.Lookup(kSound).contentRevision,
        engine->registry.Lookup(kSecondSound).contentRevision);
    image.world.identity.runtimeSemanticFingerprint =
        harken::ComputeCurrentWorldFingerprintV4(image.world);
    ASSERT_EQ(harken::InstallContainerWorldV1(*engine, image.world).status,
              harken::WorldInstallStatus::Ok);
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    ASSERT_EQ(VN_OK, Vn_SetEmitterValue(emitter, harkentest::ObjectId(41), 7.0f));
    ASSERT_EQ(1u, engine->variables.PendingCount());

    const harken::LogicCommitSequence ticketBefore =
        engine->logicCommits.Current();
    const std::uint32_t voiceBefore = engine->voices.ActiveCount();
    const std::uint32_t armBefore = engine->timelineArms.LiveCount();
    const std::uint32_t decisionBefore = engine->decisions.LiveCount();
    const std::uint32_t groupBefore = engine->armGroups.LiveCount();
    const std::uint32_t selectionBefore =
        engine->selectionMemory.FreeDeltaCount();
    const std::uint32_t instanceBefore =
        engine->playableInstances.LiveCount();
    VnPreviewPlayPlayableDescV2 desc =
        Desc(VN_PREVIEW_PLAYABLE_KIND_CONTAINER, kContainer);
    desc.emitterHandle = emitter;
    VnPreviewPlayableHandle handle = 0;

    EXPECT_EQ(VN_PREVIEW_BACKPRESSURE,
              VnPreview_PlayPlayable(&desc, &handle));
    EXPECT_EQ(0u, handle);
    EXPECT_EQ(ticketBefore, engine->logicCommits.Current());
    EXPECT_EQ(voiceBefore, engine->voices.ActiveCount());
    EXPECT_EQ(armBefore, engine->timelineArms.LiveCount());
    EXPECT_EQ(decisionBefore, engine->decisions.LiveCount());
    EXPECT_EQ(groupBefore, engine->armGroups.LiveCount());
    EXPECT_EQ(selectionBefore, engine->selectionMemory.FreeDeltaCount());
    EXPECT_EQ(instanceBefore, engine->playableInstances.LiveCount());
    EXPECT_TRUE(std::none_of(
        engine->playableServices.begin(), engine->playableServices.end(),
        [](const harken::EnginePlayableServiceRow& row) {
            return row.phase != harken::EnginePlayableStartPhase::Free;
        }));

    harkentest::AdvanceVariableWrite(engine);
    ASSERT_EQ(VN_PREVIEW_OK,
              VnPreview_PlayPlayable(&desc, &handle));
    ASSERT_NE(0u, handle);
    const harken::PlayableInstanceHandle internal =
        handle & ~(std::uint64_t{1} << 63);
    const auto row = std::find_if(
        engine->playableServices.begin(), engine->playableServices.end(),
        [&](const harken::EnginePlayableServiceRow& candidate) {
            return candidate.rootInstance == internal;
        });
    ASSERT_NE(engine->playableServices.end(), row);
    ASSERT_EQ(1u, row->variableControlDiscreteMemberCount);
    EXPECT_EQ(2u, engine->ReadPlayableMember(
                      row->scheduleStorageBase).voice.Facts().target.occurrenceOrdinal);
    EXPECT_FALSE(engine->runtimeRecreateRequired);
}

TEST(PreviewAbi, ContinuousOnlyVariableControlStartsOnFirstPublicPreview) {
    PreviewSession fixture;
    harken::Engine* engine = fixture.session.Engine();
    ASSERT_NE(engine, nullptr);
    const harken::PCMView media = engine->registry.Lookup(kSound);
    PreviewVariableControlWorldImage image(media.contentRevision, true);
    ASSERT_EQ(harken::InstallContainerWorldV1(*engine, image.world).status,
              harken::WorldInstallStatus::Ok);

    VnPreviewPlayPlayableDescV2 desc =
        Desc(VN_PREVIEW_PLAYABLE_KIND_CONTAINER, kContainer);
    VnPreviewPlayableHandle handle = 0;
    ASSERT_EQ(VnPreview_PlayPlayable(&desc, &handle), VN_PREVIEW_OK);
    EXPECT_TRUE(PumpUntilNonSilent(*engine));
}

TEST(PreviewAbi,
     DualVariableControlRetargetStopRetiresThePublicPreviewHandle) {
    VnConfig config = harkentest::MakeNullConfig(16, 64, 48000);
    harkentest::EngineSession session(config);
    ASSERT_EQ(session.InitResult(), VN_OK);
    harken::Engine* engine = session.Engine();
    ASSERT_NE(engine, nullptr);
    std::vector<float> firstSamples(65536, 0.25f);
    std::vector<float> secondSamples(65536, -0.25f);
    const VnPCMDesc firstPcm =
        harkentest::MonoDesc(firstSamples, 48000);
    const VnPCMDesc secondPcm =
        harkentest::MonoDesc(secondSamples, 48000);
    ASSERT_EQ(Vn_RegisterPCM(kSound, &firstPcm), VN_OK);
    ASSERT_EQ(Vn_RegisterPCM(kSecondSound, &secondPcm), VN_OK);
    PreviewDualVariableControlWorldImage image(
        engine->registry.Lookup(kSound).contentRevision,
        engine->registry.Lookup(kSecondSound).contentRevision);
    ASSERT_EQ(harken::InstallContainerWorldV1(*engine, image.world).status,
              harken::WorldInstallStatus::Ok);
    const std::uint32_t voiceBaseline = engine->voices.ActiveCount();
    const std::uint32_t armBaseline = engine->timelineArms.LiveCount();
    const std::uint32_t decisionBaseline = engine->decisions.LiveCount();
    const std::uint32_t groupBaseline = engine->armGroups.LiveCount();
    const std::uint32_t instanceBaseline =
        engine->playableInstances.LiveCount();

    VnPreviewPlayPlayableDescV2 desc =
        Desc(VN_PREVIEW_PLAYABLE_KIND_CONTAINER, kContainer);
    VnPreviewPlayableHandle handle = 0;
    ASSERT_EQ(VnPreview_PlayPlayable(&desc, &handle), VN_PREVIEW_OK);
    ASSERT_NE(handle, 0u);
    VnPreviewPlayableStateV2 state{};
    state.structSize = sizeof(state);
    bool sawActive = false;
    for (std::uint32_t block = 0; block < 75; ++block) {
        if (block == 37) {
            ASSERT_EQ(Vn_SetValue(harkentest::ObjectId(41), 7.0f), VN_OK);
            ASSERT_EQ(Vn_SetValue(harkentest::ObjectId(42), 7.0f), VN_OK);
        }
        ASSERT_EQ(Vn_Update(), VN_OK);
        engine->nullBackend.Pump(1);
        ASSERT_EQ(VnPreview_QueryPlayable(handle, &state), VN_PREVIEW_OK);
        sawActive = sawActive ||
            state.lifecycle == VN_PREVIEW_LIFECYCLE_ACTIVE;
    }
    ASSERT_TRUE(sawActive);
    ASSERT_EQ(VnPreview_StopPlayable(handle, 10), VN_PREVIEW_OK);
    bool stale = false;
    std::uint32_t failureBlock = UINT32_MAX;
    for (std::uint32_t block = 0; block < 1500 && !stale; ++block) {
        ASSERT_EQ(Vn_Update(), VN_OK);
        engine->nullBackend.Pump(1);
        stale = VnPreview_QueryPlayable(handle, &state) ==
            VN_PREVIEW_STALE_HANDLE;
        if (engine->runtimeRecreateRequired) {
            failureBlock = block;
            break;
        }
    }
    if (!stale) {
        const std::uint64_t internalHandle =
            handle & ~(std::uint64_t{1} << 63);
        const auto row = std::find_if(
            engine->playableServices.begin(),
            engine->playableServices.end(),
            [&](const harken::EnginePlayableServiceRow& candidate) {
                return candidate.phase !=
                           harken::EnginePlayableStartPhase::Free &&
                    candidate.rootInstance == internalHandle;
            });
        ASSERT_NE(row, engine->playableServices.end());
        harken::VariableControlInstanceSnapshot instance{};
        ASSERT_TRUE(engine->variableControlInstances.Capture(
            row->variableControlInstance, &instance));
        harken::DecisionCommittedSnapshot discreteRetarget{};
        harken::DecisionCommittedSnapshot continuousRetarget{};
        harken::ArmGroupSnapshot discreteRetargetGroup{};
        harken::ArmGroupSnapshot continuousRetargetGroup{};
        const bool discreteRetargetCaptured =
            engine->decisions.CaptureCommitted(
            row->variableControlDiscreteRetargetDecision,
            &discreteRetarget);
        const bool continuousRetargetCaptured =
            engine->decisions.CaptureCommitted(
            row->variableControlRetargetDecision,
            &continuousRetarget);
        const bool discreteGroupCaptured = engine->armGroups.Capture(
            row->variableControlDiscreteRetargetArmGroup,
            &discreteRetargetGroup);
        const bool continuousGroupCaptured = engine->armGroups.Capture(
            row->variableControlRetargetArmGroup,
            &continuousRetargetGroup);
        ADD_FAILURE()
            << "failureBlock=" << failureBlock
            << " phase=" << static_cast<unsigned>(row->phase)
            << " lifecycle="
            << static_cast<unsigned>(row->controlLifecycle)
            << " initialDiscrete="
            << row->variableControlDiscreteDecisionCommitted << "/"
            << row->variableControlDiscreteCleanupStarted
            << " initialContinuous="
            << row->variableControlContinuousDecisionCommitted << "/"
            << row->variableControlContinuousCleanupStarted
            << " discreteRetarget="
            << row->variableControlDiscreteRetargetPrepared << "/"
            << row->variableControlDiscreteRetargetDecisionCommitted << "/"
            << row->variableControlDiscreteRetargetApplied << "/"
            << row->variableControlDiscreteRetargetCleanupStarted << "/"
            << row->variableControlDiscreteRetargetSourceExited
            << " continuousRetarget="
            << row->variableControlRetargetPrepared << "/"
            << row->variableControlRetargetDecisionCommitted << "/"
            << row->variableControlRetargetApplied << "/"
            << row->variableControlRetargetCleanupStarted << "/"
            << row->variableControlRetargetExitsIssued << "/"
            << row->variableControlRetargetMembershipFinalized
            << " sideStates="
            << static_cast<unsigned>(instance.discrete.state) << "/"
            << static_cast<unsigned>(instance.continuous.state)
            << " audioStates="
            << discreteRetargetCaptured << ":"
            << static_cast<unsigned>(discreteRetarget.audioState) << "/"
            << continuousRetargetCaptured << ":"
            << static_cast<unsigned>(continuousRetarget.audioState)
            << " groupStates="
            << discreteGroupCaptured << ":"
            << static_cast<unsigned>(discreteRetargetGroup.state) << "/"
            << discreteRetargetGroup.tombstonedCount << "/"
            << discreteRetargetGroup.memberCount << ":"
            << continuousGroupCaptured << ":"
            << static_cast<unsigned>(continuousRetargetGroup.state) << "/"
            << continuousRetargetGroup.tombstonedCount << "/"
            << continuousRetargetGroup.memberCount;
    }
    EXPECT_EQ(engine->voices.ActiveCount(), voiceBaseline);
    EXPECT_EQ(engine->timelineArms.LiveCount(), armBaseline);
    ASSERT_NO_FATAL_FAILURE(ExpectTransferredDecisionsReclaimed(*engine, decisionBaseline));
    EXPECT_EQ(engine->armGroups.LiveCount(), groupBaseline);
    EXPECT_EQ(engine->playableInstances.LiveCount(), instanceBaseline);
    EXPECT_FALSE(engine->runtimeRecreateRequired);
}

#else

TEST(PreviewAbi, ClosedGateRefusesBeforeParsingOrRuntimeMutation) {
    EXPECT_EQ(VnPreview_CapturePlaybackPaths(nullptr, 0, nullptr),
              VN_PREVIEW_DEVELOPMENT_API_DISABLED);
    VnPreviewPlayableHandle handle = 99;
    EXPECT_EQ(VnPreview_PlayPlayable(nullptr, &handle),
              VN_PREVIEW_DEVELOPMENT_API_DISABLED);
    EXPECT_EQ(handle, 0u);
    EXPECT_EQ(VnPreview_ResetPlayable(nullptr),
              VN_PREVIEW_DEVELOPMENT_API_DISABLED);
    EXPECT_EQ(VnPreview_StopPlayable(7, 10),
              VN_PREVIEW_DEVELOPMENT_API_DISABLED);
    VnPreviewPlayableStateV2 state{};
    std::memset(&state, 0xA5, sizeof(state));
    const VnPreviewPlayableStateV2 before = state;
    EXPECT_EQ(VnPreview_QueryPlayable(7, &state),
              VN_PREVIEW_DEVELOPMENT_API_DISABLED);
    EXPECT_EQ(std::memcmp(&state, &before, sizeof(state)), 0);
}

#endif

}
