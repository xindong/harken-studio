#include "MonitorSessionFileWrite.h"
#include "MonitorSessionRecover.h"

#include <harken/Harken.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace {

VnMonitorSessionCompareView MakeReadyView() {
    VnMonitorSessionCompareView view{};
    view.structSize = sizeof(view);
    view.status = static_cast<uint32_t>(VN_OK);
    view.hasCompleteFooter = 1;
    view.hasTraceCorrelation = 1;
    view.hasOptionalSymbols = 1;
    view.lastCompleteChunkSequence = 4;
    view.fileBytes = 256;
    view.bytesConsumed = 256;
    view.capabilityFingerprint = 11;
    view.programSemanticFingerprint = 22;
    view.runtimeLineageFingerprint = 33;
    view.tapDescriptorId = 44;
    view.measurementSegmentGeneration = 1;
    view.sampleRate = 48000;
    view.algorithmId = 7;
    view.algorithmVersion = 1;
    view.unitId = 2;
    view.targetStreamKindMask = VN_MONITOR_STREAM_TIME_SERIES;
    view.presentStreamKindMask = VN_MONITOR_STREAM_TIME_SERIES;
    view.rangeBegin = 0;
    view.rangeEnd = 100;
    return view;
}

VnMonitorSessionCompareDesc MakeDesc(const VnMonitorSessionCompareView* left,
                                     const VnMonitorSessionCompareView* right) {
    VnMonitorSessionCompareDesc desc{};
    desc.structSize = sizeof(desc);
    desc.left = left;
    desc.right = right;
    return desc;
}

VnMonitorCompareResult MakeOut() {
    VnMonitorCompareResult out{};
    out.structSize = sizeof(out);
    out.result = 0xFFFFFFFFu;
    return out;
}

VnMonitorSessionCompareView ProjectRecovered(
    const harken::monitor::SessionRecoverResult& recovered) {
    VnMonitorSessionCompareView view = MakeReadyView();
    view.status = static_cast<uint32_t>(recovered.status);
    view.recoveryNeeded = recovered.recoveryNeeded;
    view.droppedRequired = recovered.droppedRequired;
    view.requiredUnknown = recovered.status == VN_MONITOR_INCOMPATIBLE ? 1u : 0u;
    view.crcFailedRequired = recovered.droppedRequired != 0 ? 1u : 0u;
    view.truncatedAfterCommit =
        recovered.bytesConsumed < recovered.fileBytes ? 1u : 0u;
    view.hasCompleteFooter =
        recovered.commitsWritten != 0 && recovered.status == VN_OK ? 1u : 0u;
    view.hasTraceCorrelation = 0;
    for (const auto& commit : recovered.commits) {
        if (commit.kind == VN_MONITOR_CHUNK_TRACE_CORRELATION) {
            view.hasTraceCorrelation = 1;
        }
        if (commit.kind == VN_MONITOR_CHUNK_OPTIONAL_SYMBOLS) {
            view.hasOptionalSymbols = 1;
        }
    }
    view.lastCompleteChunkSequence = recovered.lastCompleteChunkSequence;
    view.fileBytes = recovered.fileBytes;
    view.bytesConsumed = recovered.bytesConsumed;
    return view;
}

} // namespace

TEST(MonitorSession, CompareRejectsNullAndWrongSize) {
    auto out = MakeOut();
    EXPECT_EQ(VN_INVALID_ARG, Vn_MonitorCompareSessions(nullptr, &out));
    EXPECT_EQ(0u, out.result);

    VnMonitorSessionCompareView left = MakeReadyView();
    VnMonitorSessionCompareView right = MakeReadyView();
    auto desc = MakeDesc(&left, &right);
    desc.structSize = 1;
    out = MakeOut();
    EXPECT_EQ(VN_INVALID_ARG, Vn_MonitorCompareSessions(&desc, &out));
    EXPECT_EQ(0u, out.result);

    out.structSize = 1;
    EXPECT_EQ(VN_INVALID_ARG, Vn_MonitorCompareSessions(nullptr, &out));
}

TEST(MonitorSession, CompareMatchingViewsAreComparable) {
    VnMonitorSessionCompareView left = MakeReadyView();
    VnMonitorSessionCompareView right = MakeReadyView();
    auto desc = MakeDesc(&left, &right);
    auto out = MakeOut();
    EXPECT_EQ(VN_OK, Vn_MonitorCompareSessions(&desc, &out));
    EXPECT_EQ(VN_MONITOR_COMPARE_COMPARABLE, out.result);
}

TEST(MonitorSession, CompareFingerprintMismatchIsIncomparable) {
    VnMonitorSessionCompareView left = MakeReadyView();
    VnMonitorSessionCompareView right = MakeReadyView();
    right.programSemanticFingerprint = 99;
    auto desc = MakeDesc(&left, &right);
    auto out = MakeOut();
    EXPECT_EQ(VN_OK, Vn_MonitorCompareSessions(&desc, &out));
    EXPECT_EQ(VN_MONITOR_COMPARE_INCOMPARABLE, out.result);
}

TEST(MonitorSession, CompareMissingIdentityIsIncomparable) {
    VnMonitorSessionCompareView left = MakeReadyView();
    VnMonitorSessionCompareView right = MakeReadyView();
    right.sampleRate = 0;
    auto desc = MakeDesc(&left, &right);
    auto out = MakeOut();
    EXPECT_EQ(VN_OK, Vn_MonitorCompareSessions(&desc, &out));
    EXPECT_EQ(VN_MONITOR_COMPARE_INCOMPARABLE, out.result);
}

TEST(MonitorSession, CompareTraceOnlyGapIsPartiallyComparable) {
    VnMonitorSessionCompareView left = MakeReadyView();
    VnMonitorSessionCompareView right = MakeReadyView();
    right.hasTraceCorrelation = 0;
    auto desc = MakeDesc(&left, &right);
    auto out = MakeOut();
    EXPECT_EQ(VN_OK, Vn_MonitorCompareSessions(&desc, &out));
    EXPECT_EQ(VN_MONITOR_COMPARE_PARTIALLY_COMPARABLE, out.result);
}

TEST(MonitorSession, CompareSymbolsOnlyGapIsPartiallyComparable) {
    VnMonitorSessionCompareView left = MakeReadyView();
    VnMonitorSessionCompareView right = MakeReadyView();
    right.hasOptionalSymbols = 0;
    auto desc = MakeDesc(&left, &right);
    auto out = MakeOut();
    EXPECT_EQ(VN_OK, Vn_MonitorCompareSessions(&desc, &out));
    EXPECT_EQ(VN_MONITOR_COMPARE_PARTIALLY_COMPARABLE, out.result);
}

TEST(MonitorSession, CompareMissingTargetStreamIsPartiallyComparable) {
    VnMonitorSessionCompareView left = MakeReadyView();
    VnMonitorSessionCompareView right = MakeReadyView();
    right.presentStreamKindMask = VN_MONITOR_STREAM_STATE;
    auto desc = MakeDesc(&left, &right);
    auto out = MakeOut();
    EXPECT_EQ(VN_OK, Vn_MonitorCompareSessions(&desc, &out));
    EXPECT_EQ(VN_MONITOR_COMPARE_PARTIALLY_COMPARABLE, out.result);
}

TEST(MonitorSession, CompareTruncatedRecoveredViewIsMissingData) {
    std::vector<std::uint8_t> header(64, 0);
    harken::monitor::PackSessionFileHeader(header.data(), 1, 1, 0, 0);
    std::vector<std::uint8_t> chunk;
    const std::uint8_t payload[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    harken::monitor::PackSessionChunk(&chunk, VN_MONITOR_CHUNK_EVENTS,
                                     VN_MONITOR_CHUNK_REQUIRED, 1, payload, 8);
    std::vector<std::uint8_t> bytes = header;
    bytes.insert(bytes.end(), chunk.begin(), chunk.end());
    bytes.resize(bytes.size() - 8);
    const auto recovered =
        harken::monitor::ScanSessionBytes(bytes.data(), bytes.size());
    ASSERT_EQ(VN_OK, recovered.status);
    ASSERT_EQ(1u, recovered.recoveryNeeded);

    VnMonitorSessionCompareView left = ProjectRecovered(recovered);
    VnMonitorSessionCompareView right = MakeReadyView();
    auto desc = MakeDesc(&left, &right);
    auto out = MakeOut();
    EXPECT_EQ(VN_OK, Vn_MonitorCompareSessions(&desc, &out));
    EXPECT_EQ(VN_MONITOR_COMPARE_MISSING_DATA, out.result);
}

TEST(MonitorSession, CompareIncompatibleRecoveredViewIsMissingData) {
    std::vector<std::uint8_t> header(64, 0);
    harken::monitor::PackSessionFileHeader(header.data(), 1, 1, 0, 0);
    std::vector<std::uint8_t> chunk;
    const std::uint8_t payload[4] = {1, 2, 3, 4};
    harken::monitor::PackSessionChunk(&chunk, 200, VN_MONITOR_CHUNK_REQUIRED, 1,
                                     payload, 4);
    std::vector<std::uint8_t> bytes = header;
    bytes.insert(bytes.end(), chunk.begin(), chunk.end());
    const auto recovered =
        harken::monitor::ScanSessionBytes(bytes.data(), bytes.size());
    ASSERT_EQ(VN_MONITOR_INCOMPATIBLE, recovered.status);

    VnMonitorSessionCompareView left = ProjectRecovered(recovered);
    VnMonitorSessionCompareView right = MakeReadyView();
    auto desc = MakeDesc(&left, &right);
    auto out = MakeOut();
    EXPECT_EQ(VN_OK, Vn_MonitorCompareSessions(&desc, &out));
    EXPECT_EQ(VN_MONITOR_COMPARE_MISSING_DATA, out.result);
}

TEST(MonitorSession, CompareGapBeatsFingerprintMismatch) {
    VnMonitorSessionCompareView left = MakeReadyView();
    VnMonitorSessionCompareView right = MakeReadyView();
    left.recoveryNeeded = 1;
    right.programSemanticFingerprint = 99;
    auto desc = MakeDesc(&left, &right);
    auto out = MakeOut();
    EXPECT_EQ(VN_OK, Vn_MonitorCompareSessions(&desc, &out));
    EXPECT_EQ(VN_MONITOR_COMPARE_MISSING_DATA, out.result);
}

TEST(MonitorSession, CompareEmptyRangeIsMissingData) {
    VnMonitorSessionCompareView left = MakeReadyView();
    VnMonitorSessionCompareView right = MakeReadyView();
    left.rangeEnd = 0;
    left.rangeBegin = 1;
    auto desc = MakeDesc(&left, &right);
    auto out = MakeOut();
    EXPECT_EQ(VN_OK, Vn_MonitorCompareSessions(&desc, &out));
    EXPECT_EQ(VN_MONITOR_COMPARE_MISSING_DATA, out.result);
}

TEST(MonitorSession, CompareZeroSampleReadyViewsAreMissingData) {
    VnMonitorSessionCompareView left = MakeReadyView();
    VnMonitorSessionCompareView right = MakeReadyView();
    left.rangeBegin = 0;
    left.rangeEnd = 0;
    right.rangeBegin = 0;
    right.rangeEnd = 0;
    auto desc = MakeDesc(&left, &right);
    auto out = MakeOut();
    EXPECT_EQ(VN_OK, Vn_MonitorCompareSessions(&desc, &out));
    EXPECT_EQ(VN_MONITOR_COMPARE_MISSING_DATA, out.result);
}

TEST(MonitorSession, CompareTraceGapWithTruncationIsMissingData) {
    VnMonitorSessionCompareView left = MakeReadyView();
    VnMonitorSessionCompareView right = MakeReadyView();
    left.hasTraceCorrelation = 0;
    right.truncatedAfterCommit = 1;
    auto desc = MakeDesc(&left, &right);
    auto out = MakeOut();
    EXPECT_EQ(VN_OK, Vn_MonitorCompareSessions(&desc, &out));
    EXPECT_EQ(VN_MONITOR_COMPARE_MISSING_DATA, out.result);
}

TEST(MonitorSession, CompareBothOptionalGapsStayPartiallyComparable) {
    VnMonitorSessionCompareView left = MakeReadyView();
    VnMonitorSessionCompareView right = MakeReadyView();
    left.hasTraceCorrelation = 0;
    right.hasOptionalSymbols = 0;
    auto desc = MakeDesc(&left, &right);
    auto out = MakeOut();
    EXPECT_EQ(VN_OK, Vn_MonitorCompareSessions(&desc, &out));
    EXPECT_EQ(VN_MONITOR_COMPARE_PARTIALLY_COMPARABLE, out.result);
}

TEST(MonitorSession, CompareClearsStaleResultOnReuse) {
    VnMonitorSessionCompareView left = MakeReadyView();
    VnMonitorSessionCompareView right = MakeReadyView();
    auto desc = MakeDesc(&left, &right);
    auto out = MakeOut();
    ASSERT_EQ(VN_OK, Vn_MonitorCompareSessions(&desc, &out));
    ASSERT_EQ(VN_MONITOR_COMPARE_COMPARABLE, out.result);
    desc.structSize = 1;
    EXPECT_EQ(VN_INVALID_ARG, Vn_MonitorCompareSessions(&desc, &out));
    EXPECT_EQ(0u, out.result);
}

TEST(MonitorSession, CompareOversizedViewIsInvalidArg) {
    VnMonitorSessionCompareView left = MakeReadyView();
    VnMonitorSessionCompareView right = MakeReadyView();
    left.structSize = sizeof(left) + 8;
    auto desc = MakeDesc(&left, &right);
    auto out = MakeOut();
    EXPECT_EQ(VN_INVALID_ARG, Vn_MonitorCompareSessions(&desc, &out));
    EXPECT_EQ(0u, out.result);
}
