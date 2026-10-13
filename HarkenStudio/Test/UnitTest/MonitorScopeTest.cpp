#include <harken/Harken.h>
#include "UnitTest/StudioObjectIdTestSupport.h"
#include "Monitor/MonitorStore.h"
#include "Monitor/ScopeTriggerProjection.h"
#include "Widgets/Monitor/MonitorPanelKeys.h"
#include "Widgets/Monitor/MonitorScopePanel.h"
#include "Widgets/Monitor/MonitorScopeSource.h"

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

TEST(MonitorScope, HostOwnedSourceWiresSharedCursor)
{
    ensureApp();
    MonitorCursorModel shared;
    shared.advanceLive(12);
    MonitorScopePanel panel;
    MonitorScopeSource source;
    source.attach(&panel, &shared);
    EXPECT_EQ(&panel.cursorModel(), &shared);
    source.attach(&panel, nullptr);
    EXPECT_NE(&panel.cursorModel(), &shared);
}
