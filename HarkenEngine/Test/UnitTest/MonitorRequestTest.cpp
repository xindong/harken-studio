#include "Common/TestUtil.h"
#include "Common/ObjectIdTestSupport.h"
#include "HarkenApiInternal.h"
#include "harken/core/MonitorRequest.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <thread>
#include <vector>

TEST(MonitorRequest, SaturationNeverWrapsAndUnavailableKeepsOrigin) {
    harken::MonitorRequestSource source(71, UINT64_MAX - 1);
    const auto last = source.Begin(harken::MonitorRequestOrigin::NativeAPI, 1);
    EXPECT_EQ(last.key.engineInstance, 71u);
    EXPECT_EQ(last.key.sequence, UINT64_MAX);
    for (unsigned i = 0; i != 3; ++i) {
        const auto missing = source.Begin(harken::MonitorRequestOrigin::StudioPreview, 3);
        EXPECT_FALSE(missing.key.Valid());
        EXPECT_EQ(missing.origin, harken::MonitorRequestOrigin::StudioPreview);
        EXPECT_EQ(missing.operation, 3u);
    }
    EXPECT_EQ(source.LastSequence(), UINT64_MAX);
    EXPECT_EQ(source.UnavailableCount(), 3u);
    harken::MonitorRequestSource unavailable(0);
    EXPECT_FALSE(unavailable.Begin(harken::MonitorRequestOrigin::NativeAPI, 1).key.Valid());
    EXPECT_EQ(unavailable.UnavailableCount(), 1u);
    EXPECT_EQ(unavailable.LastSequence(), 0u);
}

TEST(MonitorRequest, ConcurrentAllocationIsDistinctWithinOneEngine) {
    harken::MonitorRequestSource source(81);
    std::array<std::uint64_t, 1024> sequences{};
    std::array<std::thread, 8> callers;
    for (unsigned caller = 0; caller != callers.size(); ++caller) {
        callers[caller] = std::thread([&, caller] {
            for (unsigned i = 0; i != 128; ++i) {
                sequences[caller * 128 + i] =
                    source.Begin(harken::MonitorRequestOrigin::NativeAPI, 1).key.sequence;
            }
        });
    }
    for (auto& caller : callers) caller.join();
    std::sort(sequences.begin(), sequences.end());
    for (unsigned i = 0; i != sequences.size(); ++i) EXPECT_EQ(sequences[i], i + 1u);
    EXPECT_EQ(source.UnavailableCount(), 0u);
}

TEST(MonitorRequest, NativeRefusalsAllocateOnceAndEngineReinitChangesIncarnation) {
    (void)Vn_Term();
    EXPECT_FALSE(harken::detail::BeginMonitorRequest(
        harken::MonitorRequestOrigin::NativeAPI, 1).key.Valid());
    std::uint64_t previousInstance = 0;
    for (unsigned incarnation = 0; incarnation != 2; ++incarnation) {
        harkentest::EngineSession session(harkentest::MakeNullConfig(2, 256, 48000));
        ASSERT_EQ(session.InitResult(), VN_OK);
        auto& source = session.Engine()->monitorRequests;
        ASSERT_NE(source.EngineInstance(), 0u);
        EXPECT_NE(source.EngineInstance(), previousInstance);
        previousInstance = source.EngineInstance();
        const auto before = source.LastSequence();
        EXPECT_EQ(Vn_PostAction(nullptr, nullptr), VN_INVALID_ARG);
        EXPECT_EQ(Vn_PostActionByName(nullptr, nullptr), VN_INVALID_ARG);
        EXPECT_EQ(Vn_PostActionPinned(nullptr, nullptr), VN_INVALID_ARG);
        EXPECT_EQ(Vn_PostActionResolved(nullptr, 0, 0, nullptr), VN_INVALID_ARG);
        EXPECT_EQ(source.LastSequence(), before + 4);
        EXPECT_EQ(source.EngineInstance(), previousInstance);
    }
}

TEST(MonitorRequest, PreparedChildrenInheritBeforeRootMetadataAndRetirementClearsIdentity) {
    harken::PlayableInstanceTable table(2);
    harken::MonitorRequestSource requests(101);
    const auto play = requests.Begin(harken::MonitorRequestOrigin::StudioPreview, 3);
    const auto unrelated = requests.Begin(harken::MonitorRequestOrigin::NativeAPI, 1);
    harken::PreparedPlayableInstance root, child;
    ASSERT_TRUE(table.PrepareAcquire(harken::PlayableKind::Container, 0, &root, play));
    ASSERT_TRUE(table.PrepareAcquire(harken::PlayableKind::Sound, root.Handle(), &child, unrelated));
    const auto rootHandle = root.Handle();
    const auto childHandle = child.Handle();
    harken::MonitorRequestContext captured{};
    ASSERT_TRUE(table.CapturePlaybackRequest(childHandle, &captured));
    EXPECT_EQ(captured.key, play.key);
    EXPECT_EQ(captured.origin, play.origin);
    harken::PlayableRootMetadata metadata{};
    metadata.actionFiring = 91;
    ASSERT_TRUE(table.SetPreparedRootMetadata(root, metadata));
    ASSERT_TRUE(table.CommitPrepared(&root));
    ASSERT_TRUE(table.CommitPrepared(&child));
    ASSERT_TRUE(table.CapturePlaybackRequest(childHandle, &captured));
    EXPECT_EQ(captured.key, play.key);
    ASSERT_TRUE(table.AdvanceLifecycle(childHandle, harken::PlayableLifecycle::Retired));
    ASSERT_TRUE(table.AdvanceLifecycle(rootHandle, harken::PlayableLifecycle::Retired));
    ASSERT_TRUE(table.PrepareAcquire(harken::PlayableKind::Sound, 0, &root));
    EXPECT_NE(root.Handle(), rootHandle);
    EXPECT_FALSE(table.CapturePlaybackRequest(rootHandle, &captured));
    ASSERT_TRUE(table.CapturePlaybackRequest(root.Handle(), &captured));
    EXPECT_FALSE(captured.key.Valid());
    ASSERT_TRUE(table.CancelPrepared(&root));
}

TEST(MonitorRequest, LogicalVoiceKeepsRequestAcrossPendingParkResumeAndReuse) {
    harken::VoiceLedger ledger(1);
    harken::MonitorRequestSource requests(102);
    const auto play = requests.Begin(harken::MonitorRequestOrigin::StudioPreview, 18);
    VnVoiceHandle logical = 0;
    std::uint32_t slot = UINT32_MAX;
    ASSERT_TRUE(ledger.AcquirePending(&logical, &slot, 128, play));
    harken::VoiceLedger::MonitorIdentity identity{};
    ASSERT_TRUE(ledger.CaptureMonitorIdentity(slot, &identity));
    EXPECT_EQ(identity.playbackRequest.key, play.key);
    EXPECT_EQ(identity.logicalHandle, logical);
    ledger.ParkPending(slot);
    const VnVoiceHandle firstRender = (UINT64_C(5) << 32);
    ledger.ReturnPark(slot, firstRender);
    std::uint32_t parkedSlot = UINT32_MAX;
    ASSERT_TRUE(ledger.Park(firstRender, &parkedSlot));
    EXPECT_EQ(parkedSlot, slot);
    const VnVoiceHandle secondRender = (UINT64_C(7) << 32);
    ledger.ReturnPark(slot, secondRender);
    ASSERT_TRUE(ledger.CaptureMonitorIdentity(slot, &identity));
    EXPECT_EQ(identity.logicalHandle, logical);
    EXPECT_EQ(identity.renderHandle, secondRender);
    EXPECT_EQ(identity.playbackRequest.key, play.key);
    const harken::MonitorControlCause stop{
        requests.Begin(harken::MonitorRequestOrigin::NativeAPI, 5)};
    const harken::MonitorControlCause repeated{
        requests.Begin(harken::MonitorRequestOrigin::NativeAPI, 5)};
    ledger.MarkTerminating(secondRender, stop);
    ledger.MarkTerminating(secondRender, repeated);
    ASSERT_TRUE(ledger.CaptureMonitorIdentity(slot, &identity));
    EXPECT_EQ(identity.playbackRequest.key, play.key);
    EXPECT_EQ(identity.controlCause.request.key, stop.request.key);
    ledger.Reap(secondRender);
    EXPECT_FALSE(ledger.CaptureMonitorIdentity(slot, &identity));
    VnVoiceHandle reused = 0;
    ASSERT_TRUE(ledger.Acquire(&reused, &slot));
    EXPECT_NE(reused, logical);
    ASSERT_TRUE(ledger.CaptureMonitorIdentity(slot, &identity));
    EXPECT_FALSE(identity.playbackRequest.key.Valid());
    EXPECT_FALSE(identity.controlCause.request.key.Valid());
}

TEST(MonitorRequest, RawStartCopiesExplicitRequestWithoutMintingInternalCall) {
    harkentest::EngineSession session(harkentest::MakeNullConfig(2, 256, 48000));
    ASSERT_EQ(session.InitResult(), VN_OK);
    auto& engine = *session.Engine();
    const auto sound = harkentest::ObjectId(3101);
    std::vector<float> samples(8192, 0.25f);
    const auto pcm = harkentest::MonoDesc(samples, 48000);
    ASSERT_EQ(Vn_RegisterPCM(sound, &pcm), VN_OK);
    const auto play = engine.monitorRequests.Begin(harken::MonitorRequestOrigin::StudioPreview, 18);
    auto desc = harkentest::MakePlayDesc(sound);
    VnVoiceHandle voice = 0;
    ASSERT_EQ(harken::detail::PlayVoiceRaw(&desc, &voice, play), VN_OK);
    const auto resolved = engine.ledger.Classify(voice);
    harken::VoiceLedger::MonitorIdentity identity{};
    ASSERT_TRUE(engine.ledger.CaptureMonitorIdentity(resolved.ledgerSlot, &identity));
    EXPECT_EQ(identity.playbackRequest.key, play.key);
    EXPECT_EQ(identity.logicalHandle, voice);
    EXPECT_EQ(identity.parent.internalToken, 0u);
    EXPECT_EQ(engine.monitorRequests.LastSequence(), play.key.sequence);
}

TEST(MonitorRequest, StopAndStopAllPreservePlaybackAndFirstAcceptedCause) {
    harkentest::EngineSession session(harkentest::MakeNullConfig(2, 256, 48000));
    ASSERT_EQ(session.InitResult(), VN_OK);
    auto& engine = *session.Engine();
    const auto sound = harkentest::ObjectId(3102);
    std::vector<float> samples(8192, 0.25f);
    const auto pcm = harkentest::MonoDesc(samples, 48000);
    ASSERT_EQ(Vn_RegisterPCM(sound, &pcm), VN_OK);
    auto desc = harkentest::MakePlayDesc(sound);
    std::array<harken::MonitorRequestContext, 2> requests{};
    std::array<VnVoiceHandle, 2> voices{};
    std::array<std::uint32_t, 2> slots{};
    for (unsigned i = 0; i != 2; ++i) {
        requests[i] = engine.monitorRequests.Begin(harken::MonitorRequestOrigin::StudioPreview, 18);
        ASSERT_EQ(harken::detail::PlayVoiceRaw(&desc, &voices[i], requests[i]), VN_OK);
        slots[i] = engine.ledger.Classify(voices[i]).ledgerSlot;
    }
    ASSERT_EQ(Vn_Stop(voices[0], 0), VN_OK);
    const harken::MonitorRequestKey firstStop{engine.monitorRequests.EngineInstance(), engine.monitorRequests.LastSequence()};
    ASSERT_EQ(Vn_Stop(voices[0], 0), VN_OK);
    EXPECT_EQ(engine.monitorRequests.LastSequence(), firstStop.sequence + 1);
    harken::VoiceLedger::MonitorIdentity untouched{};
    ASSERT_TRUE(engine.ledger.CaptureMonitorIdentity(slots[1], &untouched));
    EXPECT_FALSE(untouched.controlCause.request.key.Valid());
    ASSERT_EQ(Vn_StopAll(0), VN_OK);
    const harken::MonitorRequestKey allStop{engine.monitorRequests.EngineInstance(), engine.monitorRequests.LastSequence()};
    for (unsigned i = 0; i != 2; ++i) {
        harken::VoiceLedger::MonitorIdentity identity{};
        ASSERT_TRUE(engine.ledger.CaptureMonitorIdentity(slots[i], &identity));
        EXPECT_EQ(identity.playbackRequest.key, requests[i].key);
        EXPECT_EQ(identity.controlCause.request.key, i == 0 ? firstStop : allStop);
        EXPECT_EQ(identity.controlCause.request.origin, harken::MonitorRequestOrigin::NativeAPI);
    }
}

TEST(MonitorRequest, InvalidControlTargetsAndEmptyStopAllEachAllocateOnce) {
    harkentest::EngineSession session(harkentest::MakeNullConfig(2, 256, 48000));
    ASSERT_EQ(session.InitResult(), VN_OK);
    auto& source = session.Engine()->monitorRequests;
    const auto before = source.LastSequence();
    EXPECT_EQ(Vn_Stop(0, 0), VN_STALE_HANDLE);
    EXPECT_EQ(Vn_StopActionFiring(0, 0), VN_STALE_HANDLE);
    EXPECT_EQ(Vn_StopEmitter(0, 0), VN_STALE_HANDLE);
    EXPECT_EQ(Vn_StopAll(0), VN_OK);
    EXPECT_EQ(source.LastSequence(), before + 4);
}

TEST(MonitorRequest, PlayableCauseKeepsNaturalResultsAndFirstUnavailableDecision) {
    harken::PlayableInstanceTable table(4);
    const auto root = table.Acquire(harken::PlayableKind::Container, 0);
    const auto natural = table.Acquire(harken::PlayableKind::Sound, root);
    const auto active = table.Acquire(harken::PlayableKind::Sound, root);
    const auto unrelated = table.Acquire(harken::PlayableKind::Sound, 0);
    ASSERT_NE(unrelated, 0u);
    ASSERT_TRUE(table.SetTerminalOutcome(root, {harken::PlayableTerminalKind::NaturalCompletion}));
    ASSERT_TRUE(table.SetTerminalOutcome(natural, {harken::PlayableTerminalKind::NaturalCompletion}));
    harken::MonitorControlCause unknown{};
    unknown.parentPlayable = root;
    ASSERT_EQ(table.BumpScheduleEpochChecked(root, &unknown).status, harken::IssueStatus::Ok);
    table.VisitPublishedSubtree(root, [&](auto handle) { table.RecordControlCause(handle, unknown); });
    harken::MonitorControlCause captured{};
    EXPECT_FALSE(table.CaptureControlCause(root, &captured));
    EXPECT_FALSE(table.CaptureControlCause(natural, &captured));
    EXPECT_FALSE(table.CaptureControlCause(unrelated, &captured));
    ASSERT_TRUE(table.CaptureControlCause(active, &captured));
    EXPECT_EQ(captured.parentPlayable, root);
    EXPECT_FALSE(captured.request.key.Valid());
    harken::MonitorRequestSource requests(103);
    const harken::MonitorControlCause later{requests.Begin(harken::MonitorRequestOrigin::NativeAPI, 5)};
    table.VisitPublishedSubtree(root, [&](auto handle) { table.RecordControlCause(handle, later); });
    ASSERT_TRUE(table.CaptureControlCause(active, &captured));
    EXPECT_FALSE(captured.request.key.Valid());
    EXPECT_EQ(captured.parentPlayable, root);
    ASSERT_TRUE(table.AdvanceLifecycle(active, harken::PlayableLifecycle::Retired));
    const auto reused = table.Acquire(harken::PlayableKind::Sound, root);
    ASSERT_NE(reused, 0u);
    EXPECT_NE(reused, active);
    EXPECT_FALSE(table.CaptureControlCause(active, &captured));
    EXPECT_FALSE(table.CaptureControlCause(reused, &captured));
}
