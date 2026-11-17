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

ElevationLaunchResult launchElevatedUninstall(const QString& buildId, const QString& reviewedSnapshot)
{
    QStringList arguments{QStringLiteral("--uninstall-version"), buildId};
    if (!reviewedSnapshot.isEmpty()) arguments << QStringLiteral("--reviewed-snapshot") << reviewedSnapshot;
    return launchElevatedCommand(arguments);
}

ElevationLaunchResult launchElevatedRepair(const QString& buildId, const QString& transactionId, const QString& reviewedSnapshot)
{
    QStringList arguments{QStringLiteral("--repair-version"), buildId, QStringLiteral("--transaction-id"), transactionId};
    if (!reviewedSnapshot.isEmpty()) arguments << QStringLiteral("--reviewed-snapshot") << reviewedSnapshot;
    return launchElevatedCommand(arguments);
}

}
