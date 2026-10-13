#include "Widgets/TimeSeries/MonitorCursorMark.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QColor>

namespace {

void ensureApp()
{
    if (qApp)
        return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

VNX::MonitorCursorSnapshot makeSnap(
    VNX::MonitorCursorMode mode,
    std::optional<std::int64_t> executeFrame,
    VNX::MonitorCursorUncertaintyKind kind,
    bool historyUnavailable = false)
{
    VNX::MonitorCursorSnapshot snap;
    snap.mode = mode;
    snap.executeFrame = executeFrame;
    snap.uncertaintyKind = kind;
    snap.historyUnavailable = historyUnavailable;
    return snap;
}

} // namespace

TEST(MonitorCursorMark, UnreadyHasNoLineAndFixedText)
{
    ensureApp();
    VNX::MonitorCursorMark mark;
    mark.updateData(makeSnap(
        VNX::MonitorCursorMode::FollowLive,
        std::nullopt,
        VNX::MonitorCursorUncertaintyKind::Unknown));
    EXPECT_FALSE(mark.showsLine());
}

TEST(MonitorCursorMark, ExactAndEstimateUseDifferentTextAndColor)
{
    ensureApp();
    VNX::MonitorCursorMark exact;
    exact.updateData(makeSnap(
        VNX::MonitorCursorMode::FollowLive,
        8,
        VNX::MonitorCursorUncertaintyKind::Exact));
    VNX::MonitorCursorMark estimate;
    estimate.updateData(makeSnap(
        VNX::MonitorCursorMode::FollowLive,
        8,
        VNX::MonitorCursorUncertaintyKind::Estimate));
    EXPECT_TRUE(exact.showsLine());
    EXPECT_TRUE(estimate.showsLine());
    EXPECT_NE(exact.uncertaintyColor(), estimate.uncertaintyColor());
}

TEST(MonitorCursorMark, HistoryUnavailableKeepsTextOffGapAndHidesLine)
{
    ensureApp();
    VNX::MonitorCursorMark mark;
    mark.updateData(makeSnap(
        VNX::MonitorCursorMode::LockHistory,
        8,
        VNX::MonitorCursorUncertaintyKind::Exact,
        true));
    EXPECT_FALSE(mark.showsLine());
    EXPECT_EQ(mark.displayedUncertainty(), VNX::MonitorCursorUncertaintyKind::Unknown);
}

TEST(MonitorCursorMark, UnknownWithExecuteFrameHasDistinctColor)
{
    ensureApp();
    VNX::MonitorCursorMark unknown;
    unknown.updateData(makeSnap(
        VNX::MonitorCursorMode::FollowLive,
        8,
        VNX::MonitorCursorUncertaintyKind::Unknown));
    VNX::MonitorCursorMark exact;
    exact.updateData(makeSnap(
        VNX::MonitorCursorMode::FollowLive,
        8,
        VNX::MonitorCursorUncertaintyKind::Exact));
    VNX::MonitorCursorMark estimate;
    estimate.updateData(makeSnap(
        VNX::MonitorCursorMode::FollowLive,
        8,
        VNX::MonitorCursorUncertaintyKind::Estimate));
    EXPECT_TRUE(unknown.showsLine());
    EXPECT_NE(unknown.uncertaintyColor(), exact.uncertaintyColor());
    EXPECT_NE(unknown.uncertaintyColor(), estimate.uncertaintyColor());
}
