#pragma once

#include <QString>
#include <QStringList>

namespace VNX {

enum class HubPage
{
    Projects,
    NewProject,
    EditIntegration,
};

class HubLauncher
{
public:
    static QString executablePath();
    static QString resolveExecutablePath(bool localBuild,
                                         const QString& applicationDirPath);
    static QStringList launchArguments(HubPage page,
                                       const QString& studioExecutablePath,
                                       bool revealLaunchingVersion = false,
                                       const QString& harkenProjectPath = QString());
    static bool open(HubPage page, QString* error,
                     bool revealLaunchingVersion = false,
                     const QString& harkenProjectPath = QString());
};

}
