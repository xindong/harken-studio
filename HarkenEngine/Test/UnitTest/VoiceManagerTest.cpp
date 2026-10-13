#include "Common/SignalTestSupport.h"

#include "harken/core/VoiceManager.h"

#include <algorithm>
#include <limits>

namespace {

using Manager = harken::VoiceManager;

VnVoiceHandle Handle(std::uint32_t slot, std::uint32_t generation) {
    return (static_cast<std::uint64_t>(generation) << 32) | slot;
}

Manager::RenderRequest Request(std::uint32_t priority, double audibility,
                               VnVoiceHandle publicHandle = 1) {
    Manager::RenderRequest request;
    request.publicHandle = publicHandle;
    request.priority = priority;
    request.audibilityDb = audibility;
    return request;
}

Manager::LiveCandidate Candidate(std::uint32_t slot,
                                 std::uint32_t generation,
                                 std::uint32_t priority,
                                 std::uint64_t startOrder,
                                 double audibility) {
    Manager::LiveCandidate candidate;
    candidate.renderHandle = Handle(slot, generation);
    candidate.publicHandle = Handle(slot + 10u, 1u);
    candidate.priority = priority;
    candidate.startOrder = startOrder;
    candidate.audibilityDb = audibility;
    return candidate;
}

TEST(VoiceManager, FreeRenderAndDirectVirtualAreDistinctDecisions) {
    Manager manager(2, 4);
    Manager::FreeRender free;
    free.available = true;
    free.slot = 1;
    free.generation = 7;

    Manager::Decision decision =
        manager.Decide(Request(128, 0.0), true, true, -40.0, 1, free,
                       nullptr, 0);
    EXPECT_EQ(Manager::DecisionKind::UseFreeRender, decision.kind);
    EXPECT_EQ(1u, decision.renderSlot);
    EXPECT_EQ(7u, decision.renderGeneration);

    decision =
        manager.Decide(Request(128, -80.0), true, true, -40.0, 1, free,
                       nullptr, 0);
    EXPECT_EQ(Manager::DecisionKind::DirectVirtual, decision.kind);

    Manager::RenderRequest stream = Request(128, -80.0);
    stream.residency = Manager::Residency::RenderRequired;
    decision = manager.Decide(stream, true, true, -40.0, 1, free, nullptr, 0);
    EXPECT_EQ(Manager::DecisionKind::UseFreeRender, decision.kind);
}

TEST(VoiceManager, RefuseAndStealPoliciesPreserveLegacyComparators) {
    Manager manager(3, 3);
    const Manager::LiveCandidate candidates[] = {
        Candidate(0, 2, 100, 9, -12.0),
        Candidate(1, 4, 100, 3, -30.0),
        Candidate(2, 1, 200, 1, -80.0),
    };
    const Manager::FreeRender noFree;

    Manager::Decision decision =
        manager.Decide(Request(100, 0.0), false, false, 0.0, 0, noFree,
                       candidates, 3);
    EXPECT_EQ(Manager::DecisionKind::Refuse, decision.kind);

    decision =
        manager.Decide(Request(100, 0.0), false, false, 0.0, 1, noFree,
                       candidates, 3);
    EXPECT_EQ(Manager::DecisionKind::Steal, decision.kind);
    EXPECT_EQ(candidates[1].renderHandle, decision.victimRenderHandle);

    decision =
        manager.Decide(Request(100, 0.0), false, false, 0.0, 2, noFree,
                       candidates, 3);
    EXPECT_EQ(Manager::DecisionKind::Steal, decision.kind);
    EXPECT_EQ(candidates[1].renderHandle, decision.victimRenderHandle);
}

TEST(VoiceManager, ProtectedAndNonFiniteCandidatesFailClosed) {
    Manager manager(2, 2);
    Manager::LiveCandidate candidates[] = {
        Candidate(0, 1, 100, 1, -80.0),
        Candidate(1, 1, 100, 2, -20.0),
    };
    candidates[0].protection = Manager::Protection::Protected;
    const Manager::FreeRender noFree;
    Manager::Decision decision =
        manager.Decide(Request(100, 0.0), false, false, 0.0, 1, noFree,
                       candidates, 2);
    EXPECT_EQ(candidates[1].renderHandle, decision.victimRenderHandle);

    candidates[1].audibilityDb =
        std::numeric_limits<double>::quiet_NaN();
    decision =
        manager.Decide(Request(100, 0.0), false, false, 0.0, 1, noFree,
                       candidates, 2);
    EXPECT_EQ(Manager::DecisionKind::Refuse, decision.kind);

    Manager::FreeRender free;
    free.available = true;
    Manager::RenderRequest nonFinite =
        Request(100, std::numeric_limits<double>::quiet_NaN());
    decision = manager.Decide(nonFinite, true, true, -40.0, 1, free,
                              nullptr, 0);
    EXPECT_EQ(Manager::DecisionKind::UseFreeRender, decision.kind);

    decision = manager.Decide(nonFinite, true, true, -40.0, 1, noFree,
                              candidates, 2);
    EXPECT_EQ(Manager::DecisionKind::Refuse, decision.kind);
}

TEST(VoiceManager, ExactClaimHasSingleOwnerAndRejectsGenerationReuse) {
    Manager manager(1, 2);
    const VnVoiceHandle victim = Handle(0, 7);
    const VnVoiceHandle claimant = Handle(1, 3);
    EXPECT_TRUE(manager.PublishClaim(victim, claimant));
    EXPECT_FALSE(manager.PublishClaim(victim, Handle(2, 1)));
    EXPECT_TRUE(manager.IsSlotClaimed(0));
    EXPECT_FALSE(manager.IsClaimReady(claimant, 8, true));
    EXPECT_TRUE(manager.IsClaimReady(claimant, 8, false));
    EXPECT_FALSE(manager.IsClaimReady(claimant, 9, false));

    manager.ReleaseClaim(claimant);
    EXPECT_FALSE(manager.IsSlotClaimed(0));
    EXPECT_TRUE(manager.PublishClaim(Handle(0, 9), Handle(2, 1)));
}

TEST(VoiceManager, RequestAndParkTiesAreStable) {
    Manager manager(3, 4);
    manager.AddRequest(Request(100, -20.0, 9));
    manager.AddRequest(Request(200, -40.0, 8));
    manager.AddRequest(Request(200, -10.0, 7));
    manager.AddRequest(Request(200, -10.0, 6));
    manager.SortRequests();
    ASSERT_EQ(4u, manager.Requests().size());
    EXPECT_EQ(6u, manager.Requests()[0].publicHandle);
    EXPECT_EQ(7u, manager.Requests()[1].publicHandle);
    EXPECT_EQ(8u, manager.Requests()[2].publicHandle);
    EXPECT_EQ(9u, manager.Requests()[3].publicHandle);

    manager.ClearCandidates();
    manager.AddCandidate(Candidate(2, 1, 100, 1, -40.0));
    manager.AddCandidate(Candidate(1, 1, 50, 2, -10.0));
    manager.AddCandidate(Candidate(0, 1, 50, 3, -30.0));
    manager.SortParkCandidates();
    ASSERT_EQ(3u, manager.Candidates().size());
    EXPECT_EQ(0u, static_cast<std::uint32_t>(
                      manager.Candidates()[0].renderHandle));
    EXPECT_EQ(1u, static_cast<std::uint32_t>(
                      manager.Candidates()[1].renderHandle));
    EXPECT_EQ(2u, static_cast<std::uint32_t>(
                      manager.Candidates()[2].renderHandle));
}

} // namespace
