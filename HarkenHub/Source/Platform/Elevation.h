#pragma once

#include <QString>
#include <QStringList>

namespace VNH {

struct ElevationLaunchResult
{
    bool ok = false;
    bool canceled = false;
    QString error;
    void* processHandle = nullptr;
};

ElevationLaunchResult launchElevatedCommand(const QStringList& arguments);
bool startInstallerDetached(const QString& program,
                            const QStringList& arguments, QString* error);
ElevationLaunchResult launchElevatedInstall(const QString& transactionId,
                                            bool explicitInstall = false);
ElevationLaunchResult launchElevatedUninstall(const QString& buildId);
ElevationLaunchResult launchElevatedRepair(const QString& buildId);
bool pollElevatedInstall(void* processHandle, int* exitCode);
void releaseElevatedInstall(void* processHandle);

}
