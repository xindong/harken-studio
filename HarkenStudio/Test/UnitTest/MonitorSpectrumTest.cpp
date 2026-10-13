#include <harken/Harken.h>
#include "UnitTest/StudioObjectIdTestSupport.h"
#include "Monitor/MonitorStore.h"
#include "Monitor/SpectrumDisplayProjection.h"
#include "Widgets/Monitor/MonitorPanelKeys.h"
#include "Widgets/Monitor/MonitorSpectrumPanel.h"
#include "Widgets/Monitor/MonitorSpectrumSource.h"

#include <gtest/gtest.h>

#include <QApplication>

#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
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

TEST(MonitorSpectrum, HostOwnedSourceWiresSharedCursor)
{
    ensureApp();
    MonitorCursorModel shared;
    shared.advanceLive(12);
    MonitorSpectrumPanel panel;
    MonitorSpectrumSource source;
    source.attach(&panel, &shared);
    EXPECT_EQ(&panel.cursorModel(), &shared);
    EXPECT_EQ(panel.cursorModel().snapshot().executeFrame, 12);
    source.attach(&panel, nullptr);
    EXPECT_NE(&panel.cursorModel(), &shared);
    EXPECT_FALSE(panel.cursorModel().snapshot().executeFrame.has_value());
}

TEST(MonitorSpectrum, DestroyUnsubscribesSharedCursor)
{
    ensureApp();
    MonitorCursorModel shared;
    shared.advanceLive(4);
    {
        auto panel = std::make_unique<MonitorSpectrumPanel>();
        panel->setSharedCursor(&shared);
        EXPECT_EQ(&panel->cursorModel(), &shared);
        panel.reset();
    }
    EXPECT_NO_FATAL_FAILURE(shared.advanceLive(8));
    EXPECT_EQ(shared.snapshot().executeFrame, 8);
}
