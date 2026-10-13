#include <harken/Harken.h>
#include "UnitTest/StudioObjectIdTestSupport.h"
#include "Monitor/MonitorStore.h"
#include "Monitor/WaveformBucketProjection.h"
#include "Widgets/Monitor/MonitorPanelKeys.h"
#include "Widgets/Monitor/MonitorWaveformPanel.h"
#include "Widgets/Monitor/MonitorWaveformSource.h"

#include <gtest/gtest.h>

#include <QApplication>

#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

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

TEST(MonitorWaveform, HostOwnedSourceWiresSharedCursor)
{
    ensureApp();
    MonitorCursorModel shared;
    shared.advanceLive(12);
    MonitorWaveformPanel panel;
    MonitorWaveformSource source;
    source.attach(&panel, &shared);
    EXPECT_EQ(&panel.cursorModel(), &shared);
    EXPECT_EQ(panel.cursorModel().snapshot().executeFrame, 12);
    source.attach(&panel, nullptr);
    EXPECT_NE(&panel.cursorModel(), &shared);
    EXPECT_FALSE(panel.cursorModel().snapshot().executeFrame.has_value());
}
