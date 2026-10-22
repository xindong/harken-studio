#include <QApplication>
#include <QStandardPaths>

#include <gtest/gtest.h>
#include "Support/UnityProjectLock.h"

namespace HarkenHubTest { int runStateWriter(const QStringList& arguments); }

int main(int argc, char* argv[])
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
#if defined(Q_OS_WIN)
    if (qEnvironmentVariable("QT_QPA_PLATFORM") == QStringLiteral("offscreen")
        && qEnvironmentVariableIsEmpty("QT_QPA_FONTDIR"))
        qputenv("QT_QPA_FONTDIR", QStandardPaths::writableLocation(QStandardPaths::FontsLocation).toUtf8());
#endif
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    if (app.arguments().value(1) == QStringLiteral("--unity-project-lock"))
        return HarkenHubTest::runUnityProjectLock(app.arguments());
    if (app.arguments().value(1) == QStringLiteral("--update-state-writer"))
        return HarkenHubTest::runStateWriter(app.arguments());
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
