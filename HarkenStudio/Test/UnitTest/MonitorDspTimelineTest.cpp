#include "Monitor/DspTimelineProjection.h"
#include "Monitor/MonitorStore.h"
#include "Widgets/Monitor/MonitorDspTimelinePanel.h"
#include "Widgets/Monitor/MonitorDspTimelineSource.h"
#include "Widgets/Monitor/MonitorPanelKeys.h"

#include <gtest/gtest.h>

#include <QApplication>

#include <fstream>
#include <iterator>
#include <string>

using namespace VNX;

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
} // namespace

TEST(MonitorDspTimeline, SnapshotDoesNotMintLanes)
{
    ensureApp();
    auto snapshot = std::make_shared<Monitor::MonitorStoreSnapshot>();
    snapshot->generation = 9;
    MonitorDspTimelinePanel panel;
    panel.cursorModel().advanceLive(10);
    panel.bindSnapshot(snapshot);
    EXPECT_EQ(panel.visibleRowCount(), 0);
    EXPECT_EQ(panel.boundGeneration(), 9u);
}
