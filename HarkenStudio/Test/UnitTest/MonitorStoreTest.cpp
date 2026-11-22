#include <harken/Harken.h>
#include "Monitor/MonitorDecoder.h"
#include "Monitor/MonitorStore.h"
#include "Monitor/support/MonitorFixtureLoader.h"

#include <gtest/gtest.h>

#include <utility>

using harken::studio::Monitor::decodePackedRecords;
using harken::studio::Monitor::MonitorDecodeBatch;
using harken::studio::Monitor::MonitorStore;
using harken::studio::Monitor::MonitorStoreBudget;
using harken::studio::Monitor::TestSupport::loadEditorFixture;

namespace {

void resetBudget()
{
    MonitorStoreBudget::instance().resetForTests();
}
harken::studio::Monitor::MonitorCapabilities makeCaps(uint64_t sessionBytes, uint64_t processBytes)
{
    harken::studio::Monitor::MonitorCapabilities caps {};
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

}

TEST(MonitorStore, SnapshotSurvivesLaterAppend)
{
    resetBudget();
    MonitorStore store(makeCaps(1ull << 20, 1ull << 20));
    ASSERT_EQ(store.appendBatch(decodeNamed("optional_unknown.bin")).status, VN_OK);
    const auto oldSnap = store.snapshot();
    auto extra = decodeNamed("optional_unknown.bin");
    ASSERT_EQ(store.appendBatch(std::move(extra)).status, VN_OK);
    EXPECT_EQ(oldSnap->records.size(), 1u);
    EXPECT_EQ(store.snapshot()->records.size(), 2u);
}
