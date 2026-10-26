#include "Widgets/Monitor/MonitorPerformancePanel.h"
#include "Widgets/Monitor/MonitorVoicesPanel.h"

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

TEST(MonitorSharedCursor, SharedCursorMayDieBeforePanel)
{
    ensureApp();
    auto shared = std::make_unique<MonitorCursorModel>();
    auto performance = std::make_unique<MonitorPerformancePanel>();
    auto voices = std::make_unique<MonitorVoicesPanel>();
    performance->setSharedCursor(shared.get());
    voices->setSharedCursor(shared.get());
    shared.reset();
    performance.reset();
    voices.reset();
}
