#include "Widgets/Monitor/MonitorSpectrumPanel.h"

#include <gtest/gtest.h>

#include <QApplication>

#include <memory>

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

}

TEST(MonitorSpectrum, DestroyUnsubscribesSharedCursor)
{
    ensureApp();
    MonitorCursorModel shared;
    shared.advanceLive(4);
    {
        auto panel = std::make_unique<MonitorSpectrumPanel>();
        panel->setSharedCursor(&shared);
        panel.reset();
    }
    EXPECT_NO_FATAL_FAILURE(shared.advanceLive(8));
}
