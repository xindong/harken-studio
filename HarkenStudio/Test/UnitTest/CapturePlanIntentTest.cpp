#include "UnitTest/StudioObjectIdTestSupport.h"
#include "Monitor/CapturePlanIntent.h"

#include "harken/Harken.h"

#include <gtest/gtest.h>

using VNX::Monitor::CaptureBudgetPreview;
using VNX::Monitor::CapturePlanFace;
using VNX::Monitor::CapturePlanIntent;
using VNX::Monitor::canonicalIntent;
using VNX::Monitor::capturePlanFaceText;
using VNX::Monitor::generationUnchanged;
using VNX::Monitor::hideMeasurement;
using VNX::Monitor::intentsEqual;
using VNX::Monitor::keepPreviousPreview;
using VNX::Monitor::measurementIdAllowed;
using VNX::Monitor::previewFromEffective;
using VNX::Monitor::previewMatchesCorrelation;
using VNX::Monitor::shouldApply;

TEST(CapturePlanIntent, CanonicalSortsAndDedups)
{
    CapturePlanIntent raw;
    raw.measurementIds = {9, 1, 9, 3};
    raw.streamKindMask = VN_MONITOR_STREAM_OPTIONAL_AUDIO;
    const auto next = canonicalIntent(raw);
    ASSERT_EQ(next.measurementIds.size(), 3u);
    EXPECT_EQ(next.measurementIds[0], 1u);
    EXPECT_EQ(next.measurementIds[1], 3u);
    EXPECT_EQ(next.measurementIds[2], 9u);
}

TEST(CapturePlanIntent, HideWaveformIdOnEmptyIntentIsNoOp)
{
    CapturePlanIntent empty;
    empty.streamKindMask = VN_MONITOR_STREAM_OPTIONAL_AUDIO;
    const auto next = hideMeasurement(empty, 11);
    EXPECT_TRUE(next.measurementIds.empty());
    EXPECT_TRUE(intentsEqual(empty, next));
}

TEST(CapturePlanIntent, UnpublishedMeasurementIdsAreRejected)
{
    EXPECT_FALSE(measurementIdAllowed(11));
    CapturePlanIntent intent;
    intent.measurementIds = {11};
    intent.streamKindMask = VN_MONITOR_STREAM_OPTIONAL_AUDIO;
    VNX::Monitor::MonitorEffectiveCapturePlan accepted {};
    accepted.activationState = VN_MONITOR_PLAN_ACTIVE;
    const auto preview = previewFromEffective(VN_OK, &accepted, 134217728ull, 3, intent);
    EXPECT_EQ(preview.face, CapturePlanFace::UnpublishedMeasurement);
    EXPECT_FALSE(shouldApply(preview));
    EXPECT_EQ(capturePlanFaceText(preview), "UnpublishedMeasurement");
}

TEST(CapturePlanIntent, EmptyMeasurementStillBillsAudioChunk)
{
    CapturePlanIntent intent;
    intent.streamKindMask = VN_MONITOR_STREAM_OPTIONAL_AUDIO;
    VNX::Monitor::MonitorEffectiveCapturePlan accepted {};
    accepted.capturePlanGeneration = 4;
    accepted.activationState = VN_MONITOR_PLAN_PENDING;
    accepted.accepted.bytesPerSecondBudget = 0;
    const auto preview = previewFromEffective(VN_OK, &accepted, 134217728ull, 7, intent);
    EXPECT_EQ(preview.face, CapturePlanFace::EmptyMeasurement);
    EXPECT_TRUE(preview.chargesAudioChunk);
    EXPECT_TRUE(shouldApply(preview));
    EXPECT_EQ(capturePlanFaceText(preview), "EmptyMeasurement audio-chunk billed cap=134217728");
}

TEST(CapturePlanIntent, PendingIsNotReady)
{
    CapturePlanIntent intent;
    intent.measurementIds = {};
    intent.streamKindMask = VN_MONITOR_STREAM_OPTIONAL_AUDIO;
    VNX::Monitor::MonitorEffectiveCapturePlan accepted {};
    accepted.capturePlanGeneration = 8;
    accepted.previousGeneration = 7;
    accepted.effectiveFromExecuteFrame = 42;
    accepted.activationState = VN_MONITOR_PLAN_PENDING;
    const auto preview = previewFromEffective(VN_OK, &accepted, 134217728ull, 9, intent);
    EXPECT_EQ(preview.face, CapturePlanFace::EmptyMeasurement);
    EXPECT_EQ(preview.activationState, static_cast<uint32_t>(VN_MONITOR_PLAN_PENDING));
    EXPECT_EQ(preview.effectiveFromExecuteFrame, 42u);
    EXPECT_TRUE(previewMatchesCorrelation(preview, 9));
    EXPECT_FALSE(previewMatchesCorrelation(preview, 8));
}

TEST(CapturePlanIntent, BudgetExceededKeepsPreviousGeneration)
{
    CaptureBudgetPreview previous;
    previous.capturePlanGeneration = 5;
    previous.activationState = VN_MONITOR_PLAN_ACTIVE;
    CapturePlanIntent intent;
    const auto failed = keepPreviousPreview(previous, VN_MONITOR_BUDGET_EXCEEDED, intent);
    EXPECT_EQ(failed.face, CapturePlanFace::BudgetExceeded);
    EXPECT_EQ(failed.capturePlanGeneration, 5u);
    EXPECT_TRUE(generationUnchanged(previous, failed));
    EXPECT_FALSE(shouldApply(failed));
}

TEST(CapturePlanIntent, PermissionDeniedDoesNotLookLikeBudget)
{
    CapturePlanIntent intent;
    const auto preview = previewFromEffective(
        VN_MONITOR_PERMISSION_DENIED, nullptr, 0, 1, intent);
    EXPECT_EQ(preview.face, CapturePlanFace::PermissionDenied);
    EXPECT_NE(capturePlanFaceText(preview).find("PermissionDenied"), std::string::npos);
}

TEST(CapturePlanIntent, UnsupportedHasHighestFacePriority)
{
    CapturePlanIntent intent;
    const auto preview = previewFromEffective(
        VN_MONITOR_UNSUPPORTED, nullptr, 0, 1, intent);
    EXPECT_EQ(preview.face, CapturePlanFace::Unsupported);
}
