#pragma once

#include "Launch/LaunchPreflight.h"
#include "Versions/StudioCapabilities.h"

#include <QString>
#include <QStringList>

namespace VNH {

struct StudioLaunchSpec
{
    QString installPath;
    QString executablePath;
    QString projectPath;
    QString serverName;
    QString sessionToken;
    QString expectedBuildId;
    QString projectOpenRequestId;
    QString projectOpenApproval;
    LaunchIntent intent = LaunchIntent::DirectOpenOnly;
    int capabilitiesVersion = kStudioCapabilitiesVersion;
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

    bool start(const StudioLaunchSpec& spec, QString* error);
    bool startCreate(const StudioCreateSpec& spec, QString* error);
    qint64 pid() const { return pid_; }
    bool stillRunning() const;
    bool terminate();

private:
    qint64 pid_ = 0;
};

}
