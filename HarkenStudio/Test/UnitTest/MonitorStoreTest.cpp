#include <harken/Harken.h>
#include "Monitor/MonitorDecoder.h"
#include "Monitor/MonitorStore.h"
#include "Monitor/support/MonitorFixtureLoader.h"

#include <gtest/gtest.h>

#include <utility>

using VNX::Monitor::decodePackedRecords;
using VNX::Monitor::MonitorDecodeBatch;
using VNX::Monitor::MonitorStore;
using VNX::Monitor::MonitorStoreBudget;
using VNX::Monitor::TestSupport::loadEditorFixture;
using VNX::Monitor::TestSupport::loadRuntimeLegalEvent;

namespace {

void resetBudget()
{
    MonitorStoreBudget::instance().resetForTests();
}
VNX::Monitor::MonitorCapabilities makeCaps(uint64_t sessionBytes, uint64_t processBytes)
{
    VNX::Monitor::MonitorCapabilities caps {};
    caps.resolvedConsumerQueueBytes = sessionBytes;
    caps.hardConsumerQueueBytes = processBytes;
    return caps;
}

MonitorDecodeBatch decodeNamed(const char* file)
{
    const auto fixture = loadEditorFixture(file);
    if (fixture.status != VN_OK)
    {
        MonitorDecodeBatch failed;
        failed.status = fixture.status;
        return failed;
    }
    return decodePackedRecords(fixture.packed.data(), fixture.packed.size(),
                               fixture.expectedRecords);
}

MonitorDecodeBatch decodeLegal()
{
    const auto fixture = loadRuntimeLegalEvent();
    EXPECT_EQ(fixture.status, VN_OK);
    return decodePackedRecords(fixture.packed.data(), fixture.packed.size(),
                               fixture.expectedRecords);
}

}

TEST(MonitorStore, SnapshotSurvivesLaterAppend)
{
    resetBudget();
    MonitorStore store(makeCaps(1ull << 20, 1ull << 20));
    ASSERT_EQ(store.appendBatch(decodeLegal()).status, VN_OK);
    const auto oldSnap = store.snapshot();
    auto extra = decodeNamed("optional_unknown.bin");
    ASSERT_EQ(store.appendBatch(std::move(extra)).status, VN_OK);
    EXPECT_EQ(oldSnap->records.size(), 1u);
    EXPECT_EQ(store.snapshot()->records.size(), 2u);
}
