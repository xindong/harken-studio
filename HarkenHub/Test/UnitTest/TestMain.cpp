#include <QApplication>
#include <QStandardPaths>

#include <gtest/gtest.h>

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
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
