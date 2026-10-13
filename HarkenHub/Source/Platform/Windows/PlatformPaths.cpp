#include "Platform/PlatformPaths.h"

#include <QCoreApplication>
#include <QDir>
#include <QSettings>
#include <QStandardPaths>

namespace VNH {

QString studioVersionsRoot()
{
#ifdef HARKEN_HUB_TEST_OVERRIDES
    const QString overridePath = qEnvironmentVariable("HARKEN_HUB_VERSIONS_ROOT");
    if (!overridePath.isEmpty())
        return overridePath;
#endif

    const QString programFiles =
        qEnvironmentVariable("ProgramFiles", QStringLiteral("C:/Program Files"));
    return programFiles + QStringLiteral("/HarkenSound/HarkenStudio/Versions");
}

QString studioBinaryRelativePath()
{
    return QStringLiteral("HarkenStudio.exe");
}

QString hubInstallDir()
{
#ifdef HARKEN_HUB_TEST_OVERRIDES
    const QString overridePath =
        qEnvironmentVariable("HARKEN_HUB_INSTALL_DIR");
    if (!overridePath.isEmpty())
        return overridePath;
#endif

    const QSettings machine(
        QStringLiteral("HKEY_LOCAL_MACHINE\\Software\\HarkenSound\\HarkenHub"),
        QSettings::NativeFormat);
    const QString registered =
        machine.value(QStringLiteral("InstallDir")).toString();
    if (!registered.isEmpty())
        return QDir::fromNativeSeparators(registered);

    return QCoreApplication::applicationDirPath();
}

QString userDataDir()
{
#ifdef HARKEN_HUB_TEST_OVERRIDES
    const QString overridePath = qEnvironmentVariable("HARKEN_HUB_USER_ROOT");
    if (!overridePath.isEmpty())
        return overridePath;
#endif

    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
}

QString machineDataDir()
{
#ifdef HARKEN_HUB_TEST_OVERRIDES
    const QString overridePath = qEnvironmentVariable("HARKEN_HUB_MACHINE_ROOT");
    if (!overridePath.isEmpty())
        return overridePath;
#endif

    const QString programData =
        qEnvironmentVariable("ProgramData", QStringLiteral("C:/ProgramData"));
    return programData + QStringLiteral("/HarkenSound/HarkenHub");
}

}
