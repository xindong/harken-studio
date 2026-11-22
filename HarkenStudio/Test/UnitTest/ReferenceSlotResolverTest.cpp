#include "UnitTest/StudioObjectIdTestSupport.h"
#include "harken_reference_slots/ReferenceSlotResolver.h"

#include <gtest/gtest.h>

#include <cstdint>

using harkenreferenceslots::ReferenceSlot;
using harkenreferenceslots::ReferenceSlotSource;
using harkenreferenceslots::ResolutionOutcome;
using harkenreferenceslots::ResolveReferenceSlot;

namespace {

constexpr std::uint32_t kCeiling = 8;

bool PlacementAncestor(void*, harken::studio::types::ObjectId subjectId,
                       std::uint32_t, harken::studio::types::ObjectId* outObjectId)
{
    *outObjectId = subjectId;
    return true;
}

}

TEST(ReferenceSlotResolution, TerminatesOnASourceThatNeverEndsItsChain)
{
    ReferenceSlotSource source;
    source.placementAncestor = &PlacementAncestor;
    const auto resolution =
        ResolveReferenceSlot(harkentest::StudioObjectId(1), ReferenceSlot::OutputBus, source, kCeiling);

    EXPECT_NE(resolution.outcome, ResolutionOutcome::Resolved);
}
