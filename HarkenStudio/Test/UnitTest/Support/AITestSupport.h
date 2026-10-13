#pragma once

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <functional>

namespace VNX::TestSupport {

inline void ensureApp()
{
    if (qApp) return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

inline bool spinUntil(const std::function<bool()>& predicate, int timeoutMs = 1000)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return predicate();
}

}
