#include "Platform/Elevation.h"

namespace VNH {

ElevationLaunchResult launchElevatedInstall(const QString& transactionId,
                                            bool explicitInstall)
{
    QStringList arguments{QStringLiteral("--install-package"), transactionId};
    if (explicitInstall)
        arguments.append(QStringLiteral("--explicit-install"));
    return launchElevatedCommand(arguments);
}

ElevationLaunchResult launchElevatedUninstall(const QString& buildId)
{
    return launchElevatedCommand(
        {QStringLiteral("--uninstall-version"), buildId});
}

ElevationLaunchResult launchElevatedRepair(const QString& buildId, const QString& transactionId)
{
    return launchElevatedCommand(
        {QStringLiteral("--repair-version"), buildId,
         QStringLiteral("--transaction-id"), transactionId});
}

}
