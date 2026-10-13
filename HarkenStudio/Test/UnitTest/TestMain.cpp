#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QSettings>
#include <QStandardPaths>

#include <gtest/gtest.h>

#include <cstdlib>

namespace {

QString& settingsRoot()
{
    static QString root;
    return root;
}

void removeSettingsRoot()
{
    QDir(settingsRoot()).removeRecursively();
    qunsetenv("HARKEN_HUB_USER_ROOT");
}

}

int main(int argc, char* argv[])
{
    testing::InitGoogleTest(&argc, argv);
#ifdef Q_OS_WIN
    if (qgetenv("QT_QPA_PLATFORM").split(';').contains("offscreen") && qEnvironmentVariableIsEmpty("QT_QPA_FONTDIR"))
        qputenv("QT_QPA_FONTDIR", qgetenv("WINDIR") + "/Fonts");
#endif
    QApplication app(argc, argv);

    // AppDataLocation is derived from the application name, and QSettings
    // callers use both the default scope and explicit product identifiers.
    // Route every user-scope INI store into one per-process root before any
    // test constructs a settings-backed singleton.
    QCoreApplication::setApplicationName(
        QStringLiteral("harken_editor_tests-%1")
            .arg(QCoreApplication::applicationPid()));
    settingsRoot() =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (settingsRoot().isEmpty()) return 1;
    QDir(settingsRoot()).removeRecursively();
    if (std::atexit(removeSettingsRoot) != 0) return 1;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       settingsRoot());
    qputenv("HARKEN_HUB_USER_ROOT",
            (settingsRoot() + QStringLiteral("/hub")).toUtf8());

    const int status = RUN_ALL_TESTS();

    QDir(settingsRoot()).removeRecursively();
    return status;
}
