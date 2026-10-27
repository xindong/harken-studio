#include "Common/ObjectIdTestSupport.h"
#include "Engine.h"
#include "Common/TestUtil.h"
#include "harken/core/CompletionProgress.h"
#include "harken/core/ResamplerAdvance.h"
#include "harken/core/TimelineRuntimeInstall.h"

#include <array>
#include <vector>

namespace {

TEST(CompletionProgressRender,
     PublishesEveryLiveBlockBeforeRenderedSequenceIncludingZeroProgress) {
    VnConfig config = harkentest::MakeNullConfig(2, 256, 48000);
    harkentest::EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_NE(nullptr, engine);

    const std::vector<float> pcm(4096, 0.25f);
    const VnPCMDesc pcmDesc = harkentest::MonoDesc(pcm, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcmDesc));

    harken::TimelineSemanticTarget target{};
    target.programSemanticFingerprint = 7001;
    target.clipId = harkentest::ObjectId(17);
    target.occurrenceOrdinal = 3;
    std::array<harken::TimelineEventPayload, 2> payloads{};
    payloads[0].kind = harken::TimelineEventKind::Start;
    payloads[0].target = target;
    payloads[1].kind = harken::TimelineEventKind::Stop;
    payloads[1].target = target;
    std::array<harken::EventTemplate, 2> events{{
        {512, 1, 0, 0, 20, 0},
        {2560, 1, 1, 0, 50, 1},
    }};
    harken::ProgramView schedule{};
    schedule.transactionId = 1;
    schedule.events = events.data();
    schedule.eventCount = events.size();
    schedule.rollingGroups = true;
    const harken::TimelineProgramView program{
        1, payloads.data(), static_cast<std::uint32_t>(payloads.size())};
    harken::TimelineOccurrenceInstallView occurrence{};
    occurrence.schedule = &schedule;
    occurrence.voice.soundId = harkentest::ObjectId(1);
    occurrence.voice.outputBusId = VN_BUS_MASTER;
    occurrence.voice.target = target;
    const harken::TimelineProgramInstallView install{&program, &occurrence, 1};
    const harken::TimelineRuntimeInstallResult installed =
        harken::InstallTimelineProgram(install);
    ASSERT_EQ(harken::TimelineRuntimeInstallStatus::Installed,
              installed.status);

    harken::EngineTimelineOccurrence& live =
        engine->timelineProgramSets[installed.generation & 1u]->occurrences[0];
    live.completionProgram.kind = harken::PlayableKindV1::Sound;
    live.completionProgram.stableId = harkentest::ObjectId(1);
    live.completionProgram.world = 3;
    live.completionProgram.runtimeSemanticFingerprint = 7001;
    const harken::OccurrenceToken token = live.occurrenceToken;
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_EQ(harken::CompletionPredictability::KnownFinite,
              live.completionPredictability);
    EXPECT_EQ(token.generation,
              live.completionPredicate.occurrence.generation);
    EXPECT_EQ(4096u,
              live.completionPredicate.totalLogicalOutputFrames);

    const harken::CompletionProgressRow* row =
        engine->timelineOccurrences.CompletionProgressOf(token);
    ASSERT_NE(nullptr, row);
    harken::CompletionProgressSnapshot snapshot{};
    ASSERT_TRUE(harken::CaptureCompletionProgressExact(
        *row, token, live.arm.renderGeneration, &snapshot));
    EXPECT_EQ(0u, snapshot.progressRevision);
    EXPECT_EQ(0u, snapshot.completedThroughBlockSequence);

    engine->nullBackend.Pump(1);
    ASSERT_EQ(1u, engine->mixer.BlocksRendered());
    const VnVoiceHandle renderHandle =
        (static_cast<std::uint64_t>(live.arm.renderGeneration) << 32) |
        live.arm.renderSlot;
    const harken::VoiceRenderContext* render =
        engine->voices.Resolve(renderHandle);
    ASSERT_NE(nullptr, render);
    ASSERT_EQ(row, render->completionProgress);
    ASSERT_TRUE(harken::CaptureCompletionProgressExact(
        *row, token, live.arm.renderGeneration, &snapshot));
    EXPECT_EQ(1u, snapshot.progressRevision);
    EXPECT_EQ(1u, snapshot.completedThroughBlockSequence);
    EXPECT_EQ(0u, snapshot.logicalOutputFramesProduced);

    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1);
    ASSERT_EQ(2u, engine->mixer.BlocksRendered());
    ASSERT_TRUE(harken::CaptureCompletionProgressExact(
        *row, token, live.arm.renderGeneration, &snapshot));
    EXPECT_EQ(2u, snapshot.progressRevision);
    EXPECT_EQ(2u, snapshot.completedThroughBlockSequence);
    EXPECT_EQ(0u, snapshot.logicalOutputFramesProduced);

    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1);
    ASSERT_EQ(3u, engine->mixer.BlocksRendered());
    ASSERT_TRUE(harken::CaptureCompletionProgressExact(
        *row, token, live.arm.renderGeneration, &snapshot));
    EXPECT_EQ(3u, snapshot.progressRevision);
    EXPECT_EQ(3u, snapshot.completedThroughBlockSequence);
    EXPECT_EQ(256u, snapshot.logicalOutputFramesProduced);

    const harken::CompletionProgressSnapshot beforeExhaustion = snapshot;
    engine->voices.BeginAudioBlock();
    engine->voices.StageCompletionProgressForSilentBlock(UINT64_MAX);
    EXPECT_FALSE(engine->voices.PublishCompletionProgress(UINT64_MAX));
    ASSERT_TRUE(harken::CaptureCompletionProgressExact(
        *row, token, live.arm.renderGeneration, &snapshot));
    EXPECT_EQ(beforeExhaustion.progressRevision, snapshot.progressRevision);
    EXPECT_EQ(beforeExhaustion.completedThroughBlockSequence,
              snapshot.completedThroughBlockSequence);
    EXPECT_EQ(beforeExhaustion.logicalOutputFramesProduced,
              snapshot.logicalOutputFramesProduced);
    // The native manual block ends only after its last actual progress user.
    engine->voices.EndAudioBlock();
}

TEST(CompletionProgressRender,
     StopLocalTimingUsesResamplerIntegerPhaseNotFeedCursor) {
    VnConfig config = harkentest::MakeNullConfig(2, 256, 48000);
    harkentest::EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_NE(nullptr, engine);

    const std::vector<float> pcm(384, 0.25f);
    const VnPCMDesc pcmDesc = harkentest::MonoDesc(pcm, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcmDesc));

    harken::TimelineSemanticTarget target{};
    target.programSemanticFingerprint = 7001;
    target.clipId = harkentest::ObjectId(17);
    target.occurrenceOrdinal = 3;
    std::array<harken::TimelineEventPayload, 2> payloads{};
    payloads[0].kind = harken::TimelineEventKind::Start;
    payloads[0].target = target;
    payloads[1].kind = harken::TimelineEventKind::Stop;
    payloads[1].target = target;
    std::array<harken::EventTemplate, 2> events{{
        {512, 1, 0, 0, 20, 0},
        {2560, 1, 1, 0, 50, 1},
    }};
    harken::ProgramView schedule{};
    schedule.transactionId = 1;
    schedule.events = events.data();
    schedule.eventCount = events.size();
    schedule.rollingGroups = true;
    const harken::TimelineProgramView program{
        1, payloads.data(), static_cast<std::uint32_t>(payloads.size())};
    harken::TimelineOccurrenceInstallView occurrence{};
    occurrence.schedule = &schedule;
    occurrence.voice.soundId = harkentest::ObjectId(1);
    occurrence.voice.loopCount = 3;
    occurrence.voice.outputBusId = VN_BUS_MASTER;
    occurrence.voice.target = target;
    const harken::TimelineProgramInstallView install{&program, &occurrence, 1};
    const harken::TimelineRuntimeInstallResult installed =
        harken::InstallTimelineProgram(install);
    ASSERT_EQ(harken::TimelineRuntimeInstallStatus::Installed,
              installed.status);

    harken::EngineTimelineOccurrence& live =
        engine->timelineProgramSets[installed.generation & 1u]->occurrences[0];
    live.completionProgram.kind = harken::PlayableKindV1::Sound;
    live.completionProgram.stableId = harkentest::ObjectId(1);
    live.completionProgram.world = 3;
    live.completionProgram.runtimeSemanticFingerprint = 7001;
    const harken::OccurrenceToken token = live.occurrenceToken;
    ASSERT_EQ(VN_OK, Vn_Update());
    for (std::uint32_t block = 0; block < 3; ++block) {
        engine->nullBackend.Pump(1);
        ASSERT_EQ(VN_OK, Vn_Update());
    }
    const VnVoiceHandle renderHandle =
        (static_cast<std::uint64_t>(live.arm.renderGeneration) << 32) |
        live.arm.renderSlot;
    const harken::VoiceRenderContext* render =
        engine->voices.Resolve(renderHandle);
    ASSERT_NE(nullptr, render);
    const harken::CompletionProgressRow* row =
        engine->timelineOccurrences.CompletionProgressOf(token);
    ASSERT_NE(nullptr, row);
    harken::CompletionProgressSnapshot snapshot{};
    ASSERT_TRUE(harken::CaptureCompletionProgressExact(
        *row, token, live.arm.renderGeneration, &snapshot));
    EXPECT_EQ(render->srcCursor, snapshot.sourceFeedCursor);
    EXPECT_EQ(render->resampler.SourceFrame(), snapshot.resamplerSourceFrame);
    EXPECT_DOUBLE_EQ(render->resampler.SourceFraction(),
                     snapshot.resamplerSourceFraction);

    EXPECT_EQ(render->resampler.phaseHigh, snapshot.resamplerPhaseHigh);
    EXPECT_EQ(render->resampler.phaseLow, snapshot.resamplerPhaseLow);
    EXPECT_EQ(256u, snapshot.logicalOutputFramesProduced);
    EXPECT_GT(snapshot.completedLoopPasses, 0u);
    EXPECT_LT(snapshot.loopsRemaining, 3u);
    // Prefetch has wrapped; the first pass has not finished playing.
    EXPECT_EQ(384u, snapshot.nextLoopEndOutputFrame);

}

} // namespace
