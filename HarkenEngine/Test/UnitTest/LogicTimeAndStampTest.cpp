// Time-coordinate and stamp contract tests.
//
// The three coordinates are separate types, so most of the mistakes they prevent
// are compile errors rather than test failures. What remains testable is the
// arithmetic between them and the two-part visibility rule for a Variable write:
// a commit order alone cannot answer "as of frame N", and an effective frame
// alone cannot order two writes that land in the same frame.

#include "Common/ObjectIdTestSupport.h"
#include "harken/core/LogicReasonCode.h"
#include "harken/core/LogicStamps.h"
#include "harken/core/TimeCoordinates.h"

#include <gtest/gtest.h>

#include <cstdint>

using harken::DeviceFrame;
using harken::ExecuteFrame;
using harken::GraphFrame;
using harken::LogicReasonCode;
using harken::LogicReasonCodeName;
using harken::ResolvedVariableSnapshotStamp;
using harken::TelemetryCorrelation;
using harken::TimeCoordinateObservation;
using harken::VariableSnapshotReader;
using harken::VariableWriteStamp;
using harken::WriteVisibleAt;

namespace {

VariableWriteStamp Write(std::uint64_t sequence, std::uint64_t frame)
{
    VariableWriteStamp stamp;
    stamp.commitSequence = sequence;
    stamp.effectiveFrame = ExecuteFrame{frame};
    return stamp;
}

ResolvedVariableSnapshotStamp Cut(std::uint64_t sequence, std::uint64_t frame)
{
    ResolvedVariableSnapshotStamp stamp;
    stamp.commitSequence = sequence;
    stamp.decisionFrame = ExecuteFrame{frame};
    return stamp;
}

} // namespace

// The graph runs ahead of logic and the device presents behind the graph. The
// distances live beside the position so a conversion cannot pair this block's
// position with a distance measured in another.
TEST(TimeCoordinates, DerivesGraphAndDevicePositionsFromOneObservation)
{
    TimeCoordinateObservation now;
    now.execute = ExecuteFrame{1000};
    now.publishLeadFrames = 256;
    now.presentationLatencyFrames = 480;

    EXPECT_EQ(now.Graph(), GraphFrame{1256});
    EXPECT_EQ(now.Device(), DeviceFrame{776});
}

// Early in a session the graph has not run far enough for a presented position
// to exist. Clamping reports "nothing presented yet" rather than wrapping into an
// enormous frame number that would read as the far future.
TEST(TimeCoordinates, ClampsThePresentedPositionInsteadOfWrapping)
{
    TimeCoordinateObservation start;
    start.execute = ExecuteFrame{0};
    start.publishLeadFrames = 128;
    start.presentationLatencyFrames = 512;

    EXPECT_EQ(start.Device(), DeviceFrame{0});
    EXPECT_EQ(start.Graph(), GraphFrame{128});
}

// Advancing moves the position and leaves the distances alone, which is the
// reason they are stored separately from it.
TEST(TimeCoordinates, AdvancingQuantaLeavesTheDistancesUnchanged)
{
    TimeCoordinateObservation now;
    now.publishLeadFrames = 100;
    now.presentationLatencyFrames = 50;
    now.AdvanceQuanta(128, 3);

    EXPECT_EQ(now.execute, ExecuteFrame{384});
    EXPECT_EQ(now.Graph(), GraphFrame{484});
    EXPECT_EQ(now.Device(), DeviceFrame{434});
}

// Both halves of the rule are load-bearing. A write committed before the cut but
// effective later has not started applying; a write effective earlier but
// committed after the cut belongs to a decision that had not happened yet.
TEST(LogicStamps, WriteVisibilityNeedsBothOrderAndEffectiveFrame)
{
    const ResolvedVariableSnapshotStamp cut = Cut(10, 1000);

    EXPECT_TRUE(WriteVisibleAt(Write(9, 900), cut));
    EXPECT_TRUE(WriteVisibleAt(Write(10, 1000), cut));
    // Committed in time, not yet in force.
    EXPECT_FALSE(WriteVisibleAt(Write(9, 1001), cut));
    // In force by frame, but from a commit the cut had not seen.
    EXPECT_FALSE(WriteVisibleAt(Write(11, 900), cut));
}

// Two writes landing in the same frame are still ordered, which an effective
// frame on its own could not express.
TEST(LogicStamps, OrdersTwoWritesThatShareAnEffectiveFrame)
{
    const VariableWriteStamp earlier = Write(4, 500);
    const VariableWriteStamp later = Write(5, 500);

    EXPECT_TRUE(WriteVisibleAt(earlier, Cut(4, 500)));
    EXPECT_FALSE(WriteVisibleAt(later, Cut(4, 500)));
    EXPECT_TRUE(WriteVisibleAt(later, Cut(5, 500)));
}

// An unstamped record must not read as visible, or a value nobody committed would
// participate in a decision.
TEST(LogicStamps, TreatsAnUnstampedWriteOrCutAsNotVisible)
{
    EXPECT_FALSE(WriteVisibleAt(VariableWriteStamp{}, Cut(10, 1000)));
    EXPECT_FALSE(
        WriteVisibleAt(Write(1, 1), ResolvedVariableSnapshotStamp{}));
}

// The reader deliberately has no "current value" call: an evaluator that could
// read the present would eventually do so, and two decisions in one tick would
// stop agreeing. An unwired reader answers "no value" rather than misbehaving.
TEST(LogicStamps, UnwiredSnapshotReaderReportsNoValue)
{
    VariableSnapshotReader reader;
    float value = 42.0f;
    EXPECT_FALSE(reader.Read(harkentest::ObjectId(7), Cut(1, 1), &value));
    EXPECT_FLOAT_EQ(value, 42.0f);
}

// Attribution has to survive a stripped symbol sidecar, so a record with no
// instance and no commit is reported as unattributable rather than published as
// though it described something.
TEST(LogicStamps, CorrelationIsAttributableOnlyWithAnInstanceAndACommit)
{
    TelemetryCorrelation empty;
    EXPECT_FALSE(empty.Attributable());

    TelemetryCorrelation missingCommit;
    missingCommit.arrangingInstance = 5;
    EXPECT_FALSE(missingCommit.Attributable());

    TelemetryCorrelation complete;
    complete.arrangingInstance = 5;
    complete.commitSequence = 9;
    complete.clipId = harkentest::ObjectId(77);
    complete.occurrenceOrdinal = 2;
    complete.executeFrame = ExecuteFrame{2048};
    EXPECT_TRUE(complete.Attributable());
}

// Names exist so two reports of one condition read alike, and a code outside the
// table still returns a string, because a report must not lose its reason to a
// null check.
//
// No spelling is asserted here, deliberately. These names are a private
// diagnostic convenience, not a protocol: pinning the literals would make any
// consumer that started matching on them look supported, and would turn a
// reworded diagnostic into a broken contract. What is asserted is completeness --
// that every defined code has a row of its own.
TEST(LogicReasonCode, NamesEveryDefinedCodeDistinctlyFromTheFallback)
{
    // The fallback any undefined number resolves to. Read rather than spelled,
    // so the comparisons below stay valid if the wording changes.
    const char* fallback =
        LogicReasonCodeName(static_cast<LogicReasonCode>(9999));
    ASSERT_NE(fallback, nullptr);

    for (std::uint32_t i = 0; i < harken::kLogicReasonCodeCount; ++i) {
        const char* name = LogicReasonCodeName(static_cast<LogicReasonCode>(i));
        ASSERT_NE(name, nullptr);
        // A defined code reaching the fallback is a code that was added without
        // a row -- the failure this case exists to catch.
        EXPECT_STRNE(name, fallback);
        EXPECT_STRNE(name, "");
    }

    // Distinct rows, so a row copied and not edited is caught too: two codes
    // sharing a name would make one condition unreportable.
    for (std::uint32_t a = 0; a < harken::kLogicReasonCodeCount; ++a) {
        for (std::uint32_t b = a + 1; b < harken::kLogicReasonCodeCount; ++b) {
            EXPECT_STRNE(LogicReasonCodeName(static_cast<LogicReasonCode>(a)),
                         LogicReasonCodeName(static_cast<LogicReasonCode>(b)));
        }
    }
}
