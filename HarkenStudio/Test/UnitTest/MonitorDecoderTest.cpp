#include <harken/Harken.h>
#include "Monitor/MonitorDecoder.h"
#include "Monitor/support/MonitorFixtureLoader.h"

#include <gtest/gtest.h>

using harken::studio::Monitor::decodePackedRecords;
using harken::studio::Monitor::TestSupport::loadEditorFixture;

TEST(MonitorDecoder, EveryFixtureDecodesToItsDeclaredOutcome)
{
    for (const char* name : {"optional_unknown.bin", "required_unknown.bin", "short_header.bin",
                             "truncated_record.bin", "legal_then_required_unknown.bin",
                             "count_bytes_mismatch.bin", "native_layout.bin", "be_count_prefix.bin",
                             "empty_batch.bin"}) {
        SCOPED_TRACE(name);
        const auto fixture = loadEditorFixture(name);
        if (fixture.status != VN_OK) {
            EXPECT_EQ(fixture.expected, "incompatible");
            continue;
        }
        const auto decoded = decodePackedRecords(fixture.packed.data(), fixture.packed.size(),
                                                 fixture.expectedRecords);
        if (fixture.expected == "incompatible") {
            EXPECT_NE(decoded.status, VN_OK);
            EXPECT_EQ(decoded.recordsWritten, 0u);
        } else {
            EXPECT_EQ(decoded.status, VN_OK);
            EXPECT_EQ(decoded.records.size(), decoded.recordsWritten);
        }
    }
}
