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
                                            bool explicitInstall = false, const QString& reviewedDestination = {},
                                            const QString& reviewedIndexHash = {}, const QString& reviewedManifestHash = {});
ElevationLaunchResult launchElevatedUninstall(const QString& buildId, const QString& reviewedSnapshot = {});
ElevationLaunchResult launchElevatedRepair(const QString& buildId, const QString& transactionId, const QString& reviewedSnapshot = {},
                                          const QString& reviewedManifestHash = {});
bool pollElevatedInstall(void* processHandle, int* exitCode);
void releaseElevatedInstall(void* processHandle);

}
