#include "Platform/PlatformPaths.h"

#include <QCoreApplication>
#include <QStandardPaths>

namespace VNH {

QString studioVersionsRoot()
{
#ifdef HARKEN_HUB_TEST_OVERRIDES
    const QString overridePath = qEnvironmentVariable("HARKEN_HUB_VERSIONS_ROOT");
    if (!overridePath.isEmpty())
        return overridePath;
#endif

    return QStringLiteral("/Applications/HarkenSound");
}

QString studioBinaryRelativePath()
{
    return QStringLiteral("HarkenStudio.app/Contents/MacOS/HarkenStudio");
}

QString hubInstallDir()
{
#ifdef HARKEN_HUB_TEST_OVERRIDES
    const QString overridePath =
        qEnvironmentVariable("HARKEN_HUB_INSTALL_DIR");
    if (!overridePath.isEmpty())
        return overridePath;
#endif

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

    return QStringLiteral("/Library/Application Support/HarkenSound/HarkenHub");
}

}
