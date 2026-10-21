#pragma once

#include "Engine.h"

#include <gtest/gtest.h>

namespace harkentest {

inline void ExpectTimelineInstallIdentity(
    const harken::Engine& engine, VnVoiceHandle render,
    harken::PlayableInstanceHandle root, const harken::TimelineSemanticTarget& target,
    std::uint64_t onset, harken::TimelineArmToken arm) {
    harken::EngineTimelineVoiceInstallIdentity captured{};
    ASSERT_TRUE(engine.CaptureTimelineVoiceInstallIdentity(render, &captured));
    EXPECT_EQ(engine.voices.Capacity(), engine.timelineVoiceInstallIdentities.Capacity());
    EXPECT_GE(engine.timelineVoiceInstallIdentities.size(), engine.voices.ConstructedSlotCount());
    EXPECT_EQ(render, captured.renderHandle);
    EXPECT_EQ(root, captured.rootInstance);
    EXPECT_EQ(onset, captured.onsetFrame);
    EXPECT_EQ(arm.slot, captured.startArm.slot);
    EXPECT_EQ(arm.generation, captured.startArm.generation);
    EXPECT_EQ(arm.renderSlot, captured.startArm.renderSlot);
    EXPECT_EQ(arm.renderGeneration, captured.startArm.renderGeneration);
    EXPECT_TRUE(captured.releaseObserver.Valid());
    EXPECT_EQ(static_cast<std::uint32_t>(render), captured.releaseObserver.slot);
    EXPECT_EQ(target.programSemanticFingerprint, captured.target.programSemanticFingerprint);
    EXPECT_EQ(target.clipGuidHigh, captured.target.clipGuidHigh);
    EXPECT_EQ(target.clipGuidLow, captured.target.clipGuidLow);
    EXPECT_EQ(target.subjectGuidHigh, captured.target.subjectGuidHigh);
    EXPECT_EQ(target.subjectGuidLow, captured.target.subjectGuidLow);
    EXPECT_EQ(target.trackId, captured.target.trackId);
    EXPECT_EQ(target.clipId, captured.target.clipId);
    EXPECT_EQ(target.subjectId, captured.target.subjectId);
    EXPECT_EQ(target.loopIteration, captured.target.loopIteration);
    EXPECT_EQ(target.occurrenceOrdinal, captured.target.occurrenceOrdinal);
}

} // namespace harkentest
