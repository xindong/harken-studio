#pragma once

#include <QString>
#include <QStringList>

namespace VNH {

struct StudioLaunchSpec
{
    QString installPath;
    QString executablePath;
    QString projectPath;
};

struct StudioCreateSpec
{
    StudioLaunchSpec launch;
    QString projectName;
    QString projectLocation;
    QString templateId;
    QString templatePath;
};

class StudioLauncher
{
public:
    static QStringList argumentsFor(const StudioLaunchSpec& spec);
    static QStringList argumentsForCreate(const StudioCreateSpec& spec);

    static bool start(const StudioLaunchSpec& spec, QString* error);
    static bool startCreate(const StudioCreateSpec& spec, QString* error);
};

}
