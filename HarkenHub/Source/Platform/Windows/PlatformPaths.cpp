#include "Platform/PlatformPaths.h"

#include <QCoreApplication>
#include <QDir>
#include <QSettings>
#include <QStandardPaths>

namespace harken::hub {

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

QString registeredHubInstallDir()
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
    return QDir::fromNativeSeparators(machine.value(QStringLiteral("InstallDir")).toString());
}

QString hubInstallDir()
{
    const QString registered = registeredHubInstallDir();
    return registered.isEmpty() ? QCoreApplication::applicationDirPath() : registered;
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
