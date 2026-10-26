#include "UnitTest/StudioObjectIdTestSupport.h"
#include "RuntimeBridge/NativeDiagnostics.h"

#include <harken/Harken.h>

#include <gtest/gtest.h>

namespace Bridge = HarkenEditor::Bridge;

namespace {

VnSnapshot makeBlankSnapshot()
{
    VnSnapshot snapshot{};
    snapshot.structSize = sizeof(VnSnapshot);
    return snapshot;
}

}

TEST(DebugHonestyMapper, RenderPlanPreservesFullWidthBusIds)
{
    VnSnapshot snapshot = makeBlankSnapshot();
    snapshot.busCount = 1;
    snapshot.busIds[0] = harkentest::ObjectId((uint64_t{1} << 32) | 7u);

    Bridge::RenderPlanInfo plan;
    EXPECT_TRUE(Bridge::mapSnapshotToRenderPlan(snapshot, plan));
    ASSERT_EQ(plan.busCount, 1u);
    EXPECT_EQ(plan.busOrder[0], snapshot.busIds[0]);
}
