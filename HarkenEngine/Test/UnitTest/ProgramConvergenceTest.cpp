// Full-chain convergence from authored project to deterministic runtime filing.
//
// A project is authored in code, compiled (front-end -> IR -> PieceProgram),
// registered against a device sample rate (the tooling program-registration
// library), and the resulting ProgramViews are filed through the planner into the
// horizon pool and consumed quantum by quantum. Two properties are pinned:
//
//   1. The resolved event frames are correct at 44.1/48/96 kHz, checked against an
//      independent integer reference computed a different way than the runtime
//      clock (direct 64-bit products here, the ClockKernel's 128-bit muldiv there),
//      so agreement is real corroboration rather than the same code twice.
//
//   2. The filed schedule is bit-identical whatever the Update cadence: the same
//      timestamped inputs posted across a cadence of one, two or four quanta
//      produce the same consumed-event digest, because the planner precomputes the
//      frames and paces materialization per quantum.
//
// The authored project exercises a musical pickup at a negative tick, a Marker start
// point, two Tracks, a Sound with a natural end (a Stop resolved from resource
// frames to device frames), and a Container clip that is a start-only placeholder
// (no Stop -- the nested-content third state).

#include "UnitTest/StudioObjectIdTestSupport.h"
#include "UnitTest/ProjectSchemaFixtures.h"

#include "harken_program_registration/ProgramRegistration.h"
#include "harken_project_schema/BuildPieceIR.h"
#include "harken_project_schema/BuildPieceProgram.h"
#include "harken_project_schema/FrontEnd.h"
#include "harken_project_schema/ResourceInput.h"

#include "harken/core/DeferredIntentPool.h"
#include "harken/core/LogicCommitCoordinator.h"
#include "harken/core/OccurrenceStatePool.h"
#include "harken/core/PlayableInstanceTable.h"
#include "harken/core/ScheduledTransactionPool.h"
#include "harken/core/TimelinePlanner.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace {

using harkenprojectschema::BuildPieceIR;
using harkenprojectschema::BuildPieceProgram;
using harkenprojectschema::CompileFrontEnd;
using harkenprojectschema::ContentKind;
using harkenprojectschema::FrontEndResult;
using harkenprojectschema::PieceIR;
using harkenprojectschema::PieceProgram;
using harkenprojectschema::ProjectInput;
using harkenprojectschema::ResourceInput;
using harkenprojectschema::ResourceTable;

namespace fx = harkenprojectschema::fixtures;

// Authoring ids pinned so the root piece and its Sound resource are addressable.
constexpr VnObjectId kPieceId = harkentest::ObjectId(1);
constexpr VnObjectId kTrack1Id = harkentest::ObjectId(2);
constexpr VnObjectId kTrack2Id = harkentest::ObjectId(3);
constexpr VnObjectId kSoundId = harkentest::ObjectId(4);
constexpr VnObjectId kSecondSoundId = harkentest::ObjectId(6);
constexpr VnObjectId kContainerId = harkentest::ObjectId(5);
// The engine sound the session registered for the project's Sound. No voice
// starts anywhere in this file, but the resolution still has to be supplied:
// registration refuses a Sound subject it cannot name an engine sound for, and
// the Container clip is not looked up at all because it starts nothing.
constexpr VnSoundId kEngineSoundId = harkentest::ObjectId(7);
constexpr std::uint32_t kStartMarkerId = 7;
constexpr std::uint32_t kExitMarkerId = 9;
constexpr std::int64_t kStartMarkerPosition = 480; // half a bar into a 4/4 bar

// The Musical fixture's tempo/meter: MusicalTime() authors one row at tick 0 with
// the model's default 120 BPM (500000 us/quarter) and 4/4.
constexpr std::int64_t kMicrosPerQuarter = 500000;
constexpr std::int64_t kPpq = 960; // ClockKernel::kClockPulsesPerQuarter

// The Sound resource: one that is a clean half second at 48 kHz, so the natural
// end rescales to round numbers at every device rate under test.
constexpr std::uint32_t kResourceRate = 48000;
constexpr std::uint64_t kResourceFrames = 24000;

// Runtime phase priorities carried as subeventPhase (block event protocol).
constexpr std::uint32_t kPhaseStart = 20;
constexpr std::uint32_t kPhaseStop = 50;

// floor(a / b) for b > 0, rounding toward negative infinity (C++ integer division
// truncates toward zero, which is wrong for the negative pickup coordinate).
std::int64_t FloorDiv(std::int64_t a, std::int64_t b) {
    std::int64_t q = a / b;
    std::int64_t r = a % b;
    if (r != 0 && ((r < 0) != (b < 0))) {
        --q;
    }
    return q;
}

// Independent reference for a musical tick's entry-relative device frame:
// floor(tick * microsPerQuarter * rate / (PPQ * 1e6)). Computed as a direct signed
// product (the magnitudes here stay far inside int64), a different route than the
// ClockKernel's saturating 128-bit path.
std::int64_t RefTickFrame(std::int64_t tick, std::int64_t rateHz) {
    return FloorDiv(tick * kMicrosPerQuarter * rateHz, kPpq * 1000000LL);
}

// Independent reference for a natural-end length crossing sample rates:
// floor(frames * toRate / fromRate). Direct product (values stay inside int64),
// not the library's portable wide-integer route.
std::uint64_t RefRescale(std::uint64_t frames, std::uint64_t fromRate,
                         std::uint64_t toRate) {
    return (frames * toRate) / fromRate;
}

// The authored project: a Musical root piece starting at a marker, owning two Tracks.
// Track 1 places the Sound at a pickup (-240), the downbeat (0) and one quarter in
// (960); Track 2 places the Container at the downbeat (start-only) and the Sound a
// bar later (1920). Ids are persisted so the piece and Sound are addressable; the
// mint high-water keeps minted clip ids clear of them.
ProjectInput BuildProject() {
    ProjectInput in;

    harkenprojectschema::InputNode piece = fx::Node(ContentKind::Piece, 1);
    piece.guid = harkentest::StudioObjectId(kPieceId);
    piece.ownedChildren = {fx::G(2), fx::G(3)};
    piece.timeSpec = fx::MusicalTime();
    piece.startPoint = fx::MarkerStart(kStartMarkerId);
    piece.markers = {
        fx::MakeMarker(50, kStartMarkerId, harkenprojectschema::MarkerKind::Custom,
                    kStartMarkerPosition),
        fx::MakeMarker(51, kExitMarkerId, harkenprojectschema::MarkerKind::Exit, 3840)};

    harkenprojectschema::InputNode track1 = fx::Node(ContentKind::Track, 2);
    track1.guid = harkentest::StudioObjectId(kTrack1Id);
    track1.ownedChildren = {fx::G(4)};
    track1.clips = {fx::Clip(201, 4, -240, 0, 0),
                    fx::Clip(202, 4, 0, 0, 1),
                    fx::Clip(203, 4, 960, 0, 2)};

    harkenprojectschema::InputNode track2 = fx::Node(ContentKind::Track, 3);
    track2.guid = harkentest::StudioObjectId(kTrack2Id);
    track2.ownedChildren = {fx::G(5), fx::G(6)};
    track2.clips = {fx::Clip(301, 5, 0, 0, 0),
                    fx::Clip(302, 6, 1920, 0, 1)};

    harkenprojectschema::InputNode sound = fx::Node(ContentKind::Sound, 4);
    sound.guid = harkentest::StudioObjectId(kSoundId);
    harkenprojectschema::InputNode container = fx::Node(ContentKind::Container, 5);
    container.guid = harkentest::StudioObjectId(kContainerId);
    harkenprojectschema::InputNode secondSound = fx::Node(ContentKind::Sound, 6);
    secondSound.guid = harkentest::StudioObjectId(kSecondSoundId);

    in.nodes = {piece, track1, track2, sound, container, secondSound};
    in.roots = {fx::G(1)};
    return in;
}

// An Absolute-timed root piece owning one Track that places the Sound at
// nanosecond 0 and a quarter second in. Exercises the nanosecond -> frame path
// (ClockKernel::TimeToFrame), which the Musical fixture does not reach.
constexpr std::int64_t kAbsClipNanos = 250000000; // 0.25 s

ProjectInput BuildAbsoluteProject() {
    ProjectInput in;

    harkenprojectschema::InputNode piece = fx::Node(ContentKind::Piece, 1);
    piece.guid = harkentest::StudioObjectId(kPieceId);
    piece.ownedChildren = {fx::G(2)};
    piece.timeSpec = fx::AbsoluteTime();
    piece.startPoint = fx::EntryStart();

    harkenprojectschema::InputNode track = fx::Node(ContentKind::Track, 2);
    track.guid = harkentest::StudioObjectId(kTrack1Id);
    track.ownedChildren = {fx::G(4)};
    track.clips = {fx::Clip(201, 4, 0, 0, 0),
                   fx::Clip(202, 4, kAbsClipNanos, 0, 1)};

    harkenprojectschema::InputNode sound = fx::Node(ContentKind::Sound, 4);
    sound.guid = harkentest::StudioObjectId(kSoundId);

    in.nodes = {piece, track, sound};
    in.roots = {fx::G(1)};
    return in;
}

ResourceTable Resources(
    const harkenprojectschema::CanonicalProject& project,
    std::uint32_t sampleRate = kResourceRate,
    std::uint64_t frameCount = kResourceFrames) {
    ResourceTable table;
    for (const harkenprojectschema::ContentObject& obj : project.objects) {
        if (obj.kind == ContentKind::Sound) {
            ResourceInput input;
            input.facts.valid = true;
            input.facts.sampleRate = sampleRate;
            input.facts.channelCount = 2;
            input.facts.frameCount = frameCount;
            input.seekable = true;
            table.emplace(obj.chassis().id, input);
        }
    }
    return table;
}

std::optional<PieceProgram> CompileProject(
    const ProjectInput& in, std::uint32_t resourceRate = kResourceRate,
    std::uint64_t resourceFrames = kResourceFrames) {
    const FrontEndResult front = CompileFrontEnd(in);
    if (!front.ok() || !front.project.has_value() || !front.facts.has_value() ||
        !front.resolution.has_value() || !front.sends.has_value() ||
        !front.effectChains.has_value()) {
        return std::nullopt;
    }
    const std::optional<PieceIR> ir =
        BuildPieceIR(*front.project, *front.facts,
                     Resources(*front.project, resourceRate, resourceFrames),
                     *front.resolution, *front.sends, *front.effectChains);
    if (!ir.has_value()) {
        return std::nullopt;
    }
    return BuildPieceProgram(*ir);
}

// One authored occurrence's expected shape, keyed by (trackId, occurrenceOrdinal)
// so a resolved view can be checked without assuming the track visitation order.
struct Expected {
    std::int64_t tick = 0;
    bool isSound = false; // Sound -> Start + Stop; Container -> Start only
};

std::map<std::pair<VnObjectId, std::uint32_t>, Expected> ExpectedOccurrences() {
    return {
        {{harkentest::ObjectId(kTrack1Id), 0}, {-240, true}},
        {{kTrack1Id, 1}, {0, true}},
        {{kTrack1Id, 2}, {960, true}},
        {{harkentest::ObjectId(kTrack2Id), 0}, {0, false}},   // Container placeholder, start-only
        {{kTrack2Id, 1}, {1920, true}},
    };
}

// FNV-1a fold of one 64-bit value into a running digest.
void Fold(std::uint64_t& hash, std::uint64_t v) {
    for (int b = 0; b < 8; ++b) {
        hash ^= (v >> (b * 8)) & 0xFFu;
        hash *= 1099511628211ull;
    }
}

constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ull;

} // namespace

// The resolved event frames are correct at every device rate, and the coverage
// shapes are present.
TEST(FullChainConvergence, ResolvedFramesMatchReferenceAcrossSampleRates) {
    const std::optional<PieceProgram> program = CompileProject(BuildProject());
    ASSERT_TRUE(program.has_value());

    const auto expected = ExpectedOccurrences();

    for (const std::int64_t rate : {44100, 48000, 96000}) {
        harken::PlayableInstanceTable instances(4);
        const harken::PlayableInstanceHandle owner =
            instances.Acquire(harken::PlayableKind::Piece, 0);
        const std::uint64_t ownerEpoch = instances.Classify(owner).scheduleEpoch;
        harken::LogicCommitCoordinator commit;

        harkenprogramreg::RegistrationParams params;
        params.soundIdMap = {{kSoundId, kEngineSoundId}, {kSecondSoundId, kEngineSoundId}};
        params.deviceSampleRateHz = rate;
        params.entryAnchorFrame = 0; // offsets are entry-relative; anchor is moot here
        params.rootPieceId = harkentest::ObjectId(kPieceId);
        params.ownerInstance = owner;
        params.ownerInstanceScheduleEpoch = ownerEpoch;

        const harkenprogramreg::RegistrationResult result =
            harkenprogramreg::RegisterPieceProgram(*program, params, commit);
        ASSERT_TRUE(result.ok) << "rate " << rate << " refusal "
                               << static_cast<int>(result.refusal);
        const harkenprogramreg::ResolvedProgram& resolved = *result.program;
        ASSERT_EQ(resolved.OccurrenceCount(), expected.size());

        std::size_t soundStops = 0;
        std::size_t containerStarts = 0;
        for (std::size_t i = 0; i < resolved.OccurrenceCount(); ++i) {
            const harkenprogramreg::OccurrenceLineage& lin = resolved.Lineage(i);
            const auto it =
                expected.find({lin.trackId, lin.occurrenceOrdinal});
            ASSERT_NE(it, expected.end())
                << "unexpected occurrence track " << lin.trackId << " ord "
                << lin.occurrenceOrdinal;
            const Expected& e = it->second;
            EXPECT_EQ(lin.pieceId, kPieceId);
            EXPECT_TRUE(lin.hasPiece);

            const harken::ProgramView& view = resolved.View(i);
            const std::int64_t start = RefTickFrame(e.tick, rate);
            ASSERT_GE(view.eventCount, 1u);
            EXPECT_EQ(view.events[0].frameOffsetFromEntry, start)
                << "rate " << rate << " tick " << e.tick << " start";
            EXPECT_EQ(view.events[0].subeventPhase, kPhaseStart);

            if (e.isSound) {
                ASSERT_EQ(view.eventCount, 2u);
                const std::int64_t stop =
                    start + static_cast<std::int64_t>(RefRescale(
                                kResourceFrames, kResourceRate,
                                static_cast<std::uint64_t>(rate)));
                EXPECT_EQ(view.events[1].frameOffsetFromEntry, stop)
                    << "rate " << rate << " tick " << e.tick << " stop";
                EXPECT_EQ(view.events[1].subeventPhase, kPhaseStop);
                ++soundStops;
            } else {
                // The Container is a start-only placeholder: no Stop event.
                EXPECT_EQ(view.eventCount, 1u)
                    << "container occurrence must be start-only";
                ++containerStarts;
            }
        }
        EXPECT_EQ(soundStops, 4u);      // four Sound placements
        EXPECT_EQ(containerStarts, 1u); // one Container placeholder
    }
}

// An Absolute-timed piece resolves nanosecond positions to frames through the
// sample rate alone, at every device rate.
TEST(FullChainConvergence, AbsolutePieceResolvesNanosecondsAcrossSampleRates) {
    const std::optional<PieceProgram> program =
        CompileProject(BuildAbsoluteProject());
    ASSERT_TRUE(program.has_value());

    for (const std::int64_t rate : {44100, 48000, 96000}) {
        harken::PlayableInstanceTable instances(4);
        const harken::PlayableInstanceHandle owner =
            instances.Acquire(harken::PlayableKind::Piece, 0);
        const std::uint64_t ownerEpoch = instances.Classify(owner).scheduleEpoch;
        harken::LogicCommitCoordinator commit;

        harkenprogramreg::RegistrationParams params;
        params.soundIdMap = {{kSoundId, kEngineSoundId}, {kSecondSoundId, kEngineSoundId}};
        params.deviceSampleRateHz = rate;
        params.entryAnchorFrame = 0;
        params.rootPieceId = harkentest::ObjectId(kPieceId);
        params.ownerInstance = owner;
        params.ownerInstanceScheduleEpoch = ownerEpoch;

        const harkenprogramreg::RegistrationResult result =
            harkenprogramreg::RegisterPieceProgram(*program, params, commit);
        ASSERT_TRUE(result.ok) << "rate " << rate << " refusal "
                               << static_cast<int>(result.refusal);
        const harkenprogramreg::ResolvedProgram& resolved = *result.program;
        ASSERT_EQ(resolved.OccurrenceCount(), 2u);

        // floor(nanos * rate / 1e9), computed the direct way as the reference.
        const std::int64_t expectStart0 = 0;
        const std::int64_t expectStart1 =
            static_cast<std::int64_t>((kAbsClipNanos * rate) / 1000000000LL);
        const std::int64_t stopDelta = static_cast<std::int64_t>(RefRescale(
            kResourceFrames, kResourceRate, static_cast<std::uint64_t>(rate)));

        for (std::size_t i = 0; i < resolved.OccurrenceCount(); ++i) {
            const harkenprogramreg::OccurrenceLineage& lin = resolved.Lineage(i);
            const harken::ProgramView& view = resolved.View(i);
            ASSERT_EQ(view.eventCount, 2u); // Sound: Start + Stop
            const std::int64_t start =
                (lin.occurrenceOrdinal == 0) ? expectStart0 : expectStart1;
            EXPECT_EQ(view.events[0].frameOffsetFromEntry, start)
                << "rate " << rate << " ord " << lin.occurrenceOrdinal;
            EXPECT_EQ(view.events[1].frameOffsetFromEntry, start + stopDelta)
                << "rate " << rate << " ord " << lin.occurrenceOrdinal;
            EXPECT_EQ(view.timeType, harken::PieceTimeType::Absolute);
        }
    }
}

// The pickup really lands before the entry downbeat, and the natural end really
// depends on the device rate -- a guard that the coverage above is not vacuous.
TEST(FullChainConvergence, PickupIsNegativeAndNaturalEndScalesWithRate) {
    // At 48 kHz one tick is 25 frames, so the -240 tick pickup is -6000 frames and
    // the half-second resource is exactly 24000 device frames.
    EXPECT_EQ(RefTickFrame(-240, 48000), -6000);
    EXPECT_EQ(RefTickFrame(0, 48000), 0);
    EXPECT_LT(RefTickFrame(-240, 48000), RefTickFrame(0, 48000));
    EXPECT_EQ(RefRescale(kResourceFrames, kResourceRate, 44100), 22050u);
    EXPECT_EQ(RefRescale(kResourceFrames, kResourceRate, 96000), 48000u);
    EXPECT_EQ(harkenprogramreg::RescaleFrameCountFloor(kResourceFrames, kResourceRate,
                                                      44100),
              22050u);
    EXPECT_EQ(harkenprogramreg::RescaleFrameCountFloor(kResourceFrames, kResourceRate,
                                                      96000),
              48000u);
}

TEST(FullChainConvergence, FrameCountRescaleUsesExactWideQuotient) {
    EXPECT_EQ(harkenprogramreg::RescaleFrameCountFloor(
                  10'000'000'000'000ull, 3'000'000, 2'999'999),
              9'999'996'666'666ull);
    EXPECT_EQ(harkenprogramreg::RescaleFrameCountFloor(1000, 44100, 48000),
              1088u);
    EXPECT_EQ(harkenprogramreg::RescaleFrameCountFloor(1000, 48000, 44100),
              918u);
    EXPECT_EQ(harkenprogramreg::RescaleFrameCountFloor(UINT64_MAX, 1, 2),
              UINT64_MAX);
}

TEST(FullChainConvergence, CheckedRescaleDistinguishesZeroFromInvalidAndOverflow) {
    std::uint64_t result = 99;
    EXPECT_TRUE(harkenprogramreg::TryRescaleFrameCountFloor(
        0, 44100, 48000, &result));
    EXPECT_EQ(0u, result);
    EXPECT_TRUE(harkenprogramreg::TryRescaleFrameCountFloor(
        147, 44100, 48000, &result));
    EXPECT_EQ(160u, result);
    EXPECT_FALSE(harkenprogramreg::TryRescaleFrameCountFloor(
        1, 0, 48000, &result));
    EXPECT_FALSE(harkenprogramreg::TryRescaleFrameCountFloor(
        UINT64_MAX, 1, 2, &result));
    EXPECT_FALSE(harkenprogramreg::TryRescaleFrameCountFloor(
        1, 1, 1, nullptr));
}

TEST(FullChainConvergence,
     ScheduleMaterializationUsesClockFloorsAndFrozenFadeReduction) {
    std::int64_t position = 0;
    EXPECT_TRUE(harkenprogramreg::TryMaterializeSchedulePosition(
        harken::ScheduleTimeKindV1::AbsoluteNanoseconds, -500000000, 0,
        48000, &position));
    EXPECT_EQ(-24000, position);
    EXPECT_TRUE(harkenprogramreg::TryMaterializeSchedulePosition(
        harken::ScheduleTimeKindV1::AbsoluteNanoseconds, 1999999999, 0,
        1, &position));
    EXPECT_EQ(1, position);
    EXPECT_TRUE(harkenprogramreg::TryMaterializeSchedulePosition(
        harken::ScheduleTimeKindV1::MusicalTicks, 960, 500000, 48000,
        &position));
    EXPECT_EQ(24000, position);
    EXPECT_TRUE(harkenprogramreg::TryMaterializeSchedulePosition(
        harken::ScheduleTimeKindV1::MusicalTicks, INT64_MAX, 20000, 48000,
        &position));
    EXPECT_EQ(INT64_MAX, position);

    std::uint64_t frames = 0;
    EXPECT_TRUE(harkenprogramreg::TryMaterializeScheduleSpan(
        harken::ScheduleTimeKindV1::AbsoluteNanoseconds, -500000000,
        1000000000, 0, 48000, &frames));
    EXPECT_EQ(48000u, frames);
    EXPECT_TRUE(harkenprogramreg::TryMaterializeScheduleSpan(
        harken::ScheduleTimeKindV1::AbsoluteNanoseconds, 0, 1999999999,
        0, 1, &frames));
    EXPECT_EQ(1u, frames);
    frames = 321;
    EXPECT_FALSE(harkenprogramreg::TryMaterializeScheduleSpan(
        harken::ScheduleTimeKindV1::MusicalTicks, 0, -1, 500000, 48000,
        &frames));
    EXPECT_EQ(321u, frames);
    position = 123;
    EXPECT_FALSE(harkenprogramreg::TryMaterializeSchedulePosition(
        static_cast<harken::ScheduleTimeKindV1>(2), 0, 0, 48000,
        &position));
    EXPECT_EQ(123, position);

    std::uint64_t fadeIn = 0;
    std::uint64_t fadeOut = 0;
    EXPECT_TRUE(harkenprogramreg::TryLowerScheduleFades(
        harken::ScheduleTimeKindV1::AbsoluteNanoseconds, 0, 166667, 166667,
        0, 48000, 10, &fadeIn, &fadeOut));
    EXPECT_EQ(5u, fadeIn);
    EXPECT_EQ(5u, fadeOut);
    EXPECT_TRUE(harkenprogramreg::TryLowerScheduleFades(
        harken::ScheduleTimeKindV1::AbsoluteNanoseconds, 0, 62500, 41667,
        0, 48000, 4, &fadeIn, &fadeOut));
    EXPECT_EQ(2u, fadeIn);
    EXPECT_EQ(2u, fadeOut);
    EXPECT_TRUE(harkenprogramreg::TryLowerScheduleFades(
        harken::ScheduleTimeKindV1::AbsoluteNanoseconds, 0, 20834, 41667,
        0, 48000, 10, &fadeIn, &fadeOut));
    EXPECT_EQ(0u, fadeIn);
    EXPECT_EQ(2u, fadeOut);
    fadeIn = 7;
    fadeOut = 8;
    EXPECT_FALSE(harkenprogramreg::TryLowerScheduleFades(
        harken::ScheduleTimeKindV1::AbsoluteNanoseconds, 0, -1, 41667,
        0, 48000, 10, &fadeIn, &fadeOut));
    EXPECT_EQ(7u, fadeIn);
    EXPECT_EQ(8u, fadeOut);
}

TEST(FullChainConvergence, NaturalEndClampsAndSaturatesWithoutWrapping) {
    const auto registerProgram = [](std::uint32_t resourceRate,
                                    std::uint64_t resourceFrames,
                                    std::int64_t deviceRate) {
        std::optional<PieceProgram> program =
            CompileProject(BuildProject(), resourceRate, resourceFrames);
        EXPECT_TRUE(program.has_value());
        if (!program.has_value()) {
            return harkenprogramreg::RegistrationResult{};
        }
        harken::PlayableInstanceTable instances(4);
        const harken::PlayableInstanceHandle owner =
            instances.Acquire(harken::PlayableKind::Piece, 0);
        harken::LogicCommitCoordinator commit;
        harkenprogramreg::RegistrationParams params;
        params.soundIdMap = {{kSoundId, kEngineSoundId}, {kSecondSoundId, kEngineSoundId}};
        params.deviceSampleRateHz = deviceRate;
        params.rootPieceId = harkentest::ObjectId(kPieceId);
        params.ownerInstance = owner;
        params.ownerInstanceScheduleEpoch =
            instances.Classify(owner).scheduleEpoch;
        return harkenprogramreg::RegisterPieceProgram(*program, params, commit);
    };

    harkenprogramreg::RegistrationResult exact =
        registerProgram(3'000'000, 10'000'000'000'000ull, 2'999'999);
    ASSERT_TRUE(exact.ok);
    ASSERT_GE(exact.program->OccurrenceCount(), 2u);
    const harken::ProgramView& exactAtEntry = exact.program->View(1);
    ASSERT_EQ(exactAtEntry.eventCount, 2u);
    EXPECT_EQ(exactAtEntry.events[0].frameOffsetFromEntry, 0);
    EXPECT_EQ(exactAtEntry.events[1].frameOffsetFromEntry,
              9'999'996'666'666ll);

    std::optional<PieceProgram> saturatedProgram = CompileProject(BuildProject());
    ASSERT_TRUE(saturatedProgram.has_value());
    auto& tracks = const_cast<std::vector<harkenprojectschema::IrTrack>&>(
        saturatedProgram->ir().tracks());
    ASSERT_GE(tracks.size(), 1u);
    ASSERT_GE(tracks[0].occurrences.size(), 3u);
    tracks[0].occurrences[2].durationFrames = UINT64_MAX;
    tracks[0].occurrences[2].resourceSampleRate = 1;
    harken::PlayableInstanceTable instances(4);
    const harken::PlayableInstanceHandle owner =
        instances.Acquire(harken::PlayableKind::Piece, 0);
    harken::LogicCommitCoordinator commit;
    harkenprogramreg::RegistrationParams params;
    params.soundIdMap = {{kSoundId, kEngineSoundId}, {kSecondSoundId, kEngineSoundId}};
    params.deviceSampleRateHz = 3'000'000;
    params.rootPieceId = harkentest::ObjectId(kPieceId);
    params.ownerInstance = owner;
    params.ownerInstanceScheduleEpoch = instances.Classify(owner).scheduleEpoch;
    harkenprogramreg::RegistrationResult saturated =
        harkenprogramreg::RegisterPieceProgram(*saturatedProgram, params, commit);
    ASSERT_TRUE(saturated.ok);
    ASSERT_GE(saturated.program->OccurrenceCount(), 3u);
    const harken::ProgramView& saturatedAfterEntry = saturated.program->View(2);
    ASSERT_EQ(saturatedAfterEntry.eventCount, 2u);
    EXPECT_GT(saturatedAfterEntry.events[0].frameOffsetFromEntry, 0);
    EXPECT_EQ(saturatedAfterEntry.events[1].frameOffsetFromEntry, INT64_MAX);
}

TEST(FullChainConvergence, EnumMappingsPreserveDeclaredValues) {
    const std::optional<PieceProgram> program = CompileProject(BuildProject());
    ASSERT_TRUE(program.has_value());
    harken::PlayableInstanceTable instances(4);
    const harken::PlayableInstanceHandle owner =
        instances.Acquire(harken::PlayableKind::Piece, 0);
    harken::LogicCommitCoordinator commit;
    harkenprogramreg::RegistrationParams params;
    params.soundIdMap = {{kSoundId, kEngineSoundId}, {kSecondSoundId, kEngineSoundId}};
    params.deviceSampleRateHz = 48000;
    params.rootPieceId = harkentest::ObjectId(kPieceId);
    params.ownerInstance = owner;
    params.ownerInstanceScheduleEpoch = instances.Classify(owner).scheduleEpoch;

    const harkenprogramreg::RegistrationResult result =
        harkenprogramreg::RegisterPieceProgram(*program, params, commit);
    ASSERT_TRUE(result.ok);
    ASSERT_GT(result.program->OccurrenceCount(), 0u);
    const harken::ProgramView& view = result.program->View(0);
    EXPECT_EQ(view.timeType, harken::PieceTimeType::Musical);
    ASSERT_NE(view.markerIndex, nullptr);
    const harken::MarkerLookup custom = view.markerIndex->ByMarkerId(harkentest::ToRuntime(fx::G(50)));
    const harken::MarkerLookup exit = view.markerIndex->ByMarkerId(harkentest::ToRuntime(fx::G(51)));
    ASSERT_TRUE(custom.present);
    ASSERT_TRUE(exit.present);
    EXPECT_EQ(custom.kind, harken::MarkerKind::Custom);
    EXPECT_EQ(exit.kind, harken::MarkerKind::Exit);
}

TEST(FullChainConvergence, OutOfDomainEnumsRefuseRegistration) {
    const auto registerCorrupt = [](bool corruptTimeKind) {
        std::optional<PieceProgram> program = CompileProject(BuildProject());
        EXPECT_TRUE(program.has_value());
        if (!program.has_value()) {
            return harkenprogramreg::RegistrationResult{};
        }
        // Deliberately corrupt the immutable compiled boundary to prove the
        // registration layer fails closed if untrusted storage crosses it.
        auto& pieces = const_cast<std::vector<harkenprojectschema::IrPiece>&>(
            program->ir().pieces());
        EXPECT_EQ(pieces.size(), 1u);
        if (pieces.empty()) {
            return harkenprogramreg::RegistrationResult{};
        }
        if (corruptTimeKind) {
            pieces[0].timeSpec.kind =
                static_cast<harkenprojectschema::PieceTimeKind>(2);
        } else {
            pieces[0].markers[0].kind =
                static_cast<harkenprojectschema::MarkerKind>(2);
        }

        harken::PlayableInstanceTable instances(4);
        const harken::PlayableInstanceHandle owner =
            instances.Acquire(harken::PlayableKind::Piece, 0);
        harken::LogicCommitCoordinator commit;
        harkenprogramreg::RegistrationParams params;
        params.soundIdMap = {{kSoundId, kEngineSoundId}, {kSecondSoundId, kEngineSoundId}};
        params.deviceSampleRateHz = 48000;
        params.rootPieceId = harkentest::ObjectId(kPieceId);
        params.ownerInstance = owner;
        params.ownerInstanceScheduleEpoch =
            instances.Classify(owner).scheduleEpoch;
        return harkenprogramreg::RegisterPieceProgram(*program, params, commit);
    };

    for (const bool corruptTimeKind : {true, false}) {
        const harkenprogramreg::RegistrationResult result =
            registerCorrupt(corruptTimeKind);
        EXPECT_FALSE(result.ok);
        EXPECT_EQ(result.refusal,
                  harkenprogramreg::RegistrationRefusal::InvalidProgram);
        EXPECT_EQ(result.program, nullptr);
    }
}

// The filed schedule is bit-identical whatever the Update cadence.
TEST(FullChainConvergence, FiledScheduleIsDeterministicAcrossUpdateCadence) {
    const std::optional<PieceProgram> program = CompileProject(BuildProject());
    ASSERT_TRUE(program.has_value());

    // One owner instance and one registration shared across the cadence runs: the
    // ProgramViews outlive every planner that files or defers them.
    harken::PlayableInstanceTable instances(4);
    const harken::PlayableInstanceHandle owner =
        instances.Acquire(harken::PlayableKind::Piece, 0);
    const std::uint64_t ownerEpoch = instances.Classify(owner).scheduleEpoch;
    harken::OccurrenceStatePool occ(4);
    harken::LogicCommitCoordinator commit;

    constexpr harken::TransportEpoch kEpoch = 1;
    constexpr std::uint32_t kQuantum = 6000;

    harkenprogramreg::RegistrationParams params;
    params.soundIdMap = {{kSoundId, kEngineSoundId}, {kSecondSoundId, kEngineSoundId}};
    params.deviceSampleRateHz = 48000;
    // Anchor the entry far enough out that every event begins beyond the first
    // window, so every play is deferred when posted and later materialized -- the
    // path the cadence invariant most needs to cover.
    params.entryAnchorFrame = 48000;
    params.rootPieceId = harkentest::ObjectId(kPieceId);
    params.ownerInstance = owner;
    params.ownerInstanceScheduleEpoch = ownerEpoch;

    const harkenprogramreg::RegistrationResult reg =
        harkenprogramreg::RegisterPieceProgram(*program, params, commit);
    ASSERT_TRUE(reg.ok);
    const harkenprogramreg::ResolvedProgram& resolved = *reg.program;
    const std::size_t n = resolved.OccurrenceCount();
    ASSERT_EQ(n, 5u);

    // Staggered submission frames across the first three quanta, so a cadence of
    // one, two and four groups the posts differently. All are well before the
    // events (which start at 42000), so no play is ever late.
    const std::array<std::uint64_t, 5> postFrames = {0, 6000, 3000, 12000, 9000};

    // The horizon must leave a writable window wider than the widest transaction
    // span: a Sound's Start-to-Stop span is one resource length (24000 frames at
    // 48 kHz), and the planner refuses a transaction whose span is not strictly
    // smaller than windowEnd - frontier. Eight pages (48000 frames) less the
    // one-quantum lead leaves a 42000-frame writable window, comfortably above it.
    auto pool_config = []() {
        harken::ScheduledTransactionPool::Config c;
        c.quantumFrames = kQuantum;
        c.scheduleHorizonFrames = kQuantum * 8;
        c.publishLeadQuanta = 1;
        c.maxScheduledTransactions = 64;
        c.maxTransactionSpanFrames = kQuantum * 8;
        c.maxIngressTransactionsPerQuantum = 64;
        c.maxFrameBatchesPerQuantum = 64;
        c.maxEventsPerQuantum = 64;
        c.maxTransactionEvents = 8;
        return c;
    };

    struct RunResult {
        std::uint64_t digest = 0;
        std::uint32_t deferredPosts = 0;
        std::uint32_t consumedEvents = 0;
    };

    auto run = [&](std::uint32_t cadence) {
        harken::ScheduledTransactionPool pool;
        EXPECT_TRUE(pool.Reset(pool_config(), 0, kEpoch));
        harken::DeferredIntentPool deferred(16, 0);
        harken::TimelinePlanner planner;
        harken::TimelinePlanner::Config pc;
        EXPECT_TRUE(planner.Reset(pc, &pool, &deferred, &instances));

        std::uint64_t hash = kFnvOffsetBasis;
        std::uint32_t deferredPosts = 0;
        std::uint32_t consumedEvents = 0;
        auto consume_events = [&](const harken::ConsumedEvent* events,
                                  std::size_t count) {
            for (std::size_t i = 0; i < count; ++i) {
                Fold(hash, events[i].executeFrame);
                Fold(hash, events[i].payload);
                Fold(hash, events[i].eventIndex);
                Fold(hash, events[i].transactionId);
            }
            consumedEvents += static_cast<std::uint32_t>(count);
        };
        auto consume_page = [&](std::uint32_t page) {
            std::array<harken::ConsumedEvent, 128> buf{};
            consume_events(buf.data(),
                           pool.ConsumePage(page, kEpoch, instances, occ,
                                            buf.data(), buf.size()));
        };
        auto consume_frontier = [&]() {
            std::array<harken::ConsumedEvent, 128> buf{};
            const std::uint64_t quantum = pool.ConsumerFrontier().load();
            if (pool.PageStamp(quantum) != quantum) {
                return;
            }
            consume_events(buf.data(),
                           pool.ConsumePageRt(quantum, buf.data(), buf.size()));
            pool.ConsumerFrontier().store(quantum + 1);
        };

        std::vector<bool> posted(n, false);
        const int totalQuanta = 30;
        for (int q = 0; q < totalQuanta; q += static_cast<int>(cadence)) {
            const std::uint64_t batchStart =
                static_cast<std::uint64_t>(q) * kQuantum;
            const std::uint64_t batchEnd =
                static_cast<std::uint64_t>(q + cadence) * kQuantum;
            for (std::size_t i = 0; i < n; ++i) {
                if (posted[i] || postFrames[i] < batchStart ||
                    postFrames[i] >= batchEnd) {
                    continue;
                }
                harken::PostInput input;
                input.currentFrame = postFrames[i];
                input.transportEpoch = kEpoch;
                input.logicCommitSequence = resolved.CommitSequence();
                input.latePolicy =
                    harken::PrePublishLatePolicy::ExecuteAtEarliestSafeFrame;
                const harken::PostResult r =
                    planner.PostPlay(resolved.View(i), input);
                if (r.status == harken::PostStatus::Deferred) {
                    ++deferredPosts;
                }
                posted[i] = true;
            }
            for (std::uint32_t c = 0; c < cadence; ++c) {
                consume_frontier();
                planner.AdvanceQuantum(kEpoch);
            }
        }
        // Fold whatever is still resident in the window.
        for (std::uint32_t p = 0; p < pool.PageCount(); ++p) {
            consume_page(p);
        }
        return RunResult{hash, deferredPosts, consumedEvents};
    };

    const RunResult c1 = run(1);
    const RunResult c2 = run(2);
    const RunResult c4 = run(4);

    // The filed schedule is bit-identical whatever the cadence.
    EXPECT_EQ(c1.digest, c2.digest);
    EXPECT_EQ(c1.digest, c4.digest);
    // And it is not empty: nine events reach the pool and are consumed exactly once
    // -- four Sound Start+Stop pairs and one lone Container Start. This is the guard
    // against a play being silently refused (a too-small window drops it and the
    // digest still matches trivially across cadence).
    EXPECT_NE(c1.digest, kFnvOffsetBasis);
    EXPECT_EQ(c1.consumedEvents, 9u);
    EXPECT_EQ(c2.consumedEvents, 9u);
    EXPECT_EQ(c4.consumedEvents, 9u);
    // The deferral+materialization path was exercised (its exact count varies with
    // cadence, as a play the window has already reached files immediately instead).
    EXPECT_GE(c1.deferredPosts, 1u);
    EXPECT_GE(c4.deferredPosts, 1u);
}
