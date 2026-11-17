#include "Platform/Elevation.h"

namespace VNH {

ElevationLaunchResult launchElevatedInstall(const QString& transactionId,
                                            bool explicitInstall, const QString& reviewedDestination, const QString& reviewedIndexHash,
                                            const QString& reviewedManifestHash)
{
    QStringList arguments{QStringLiteral("--install-package"), transactionId};
    if (explicitInstall)
        arguments.append(QStringLiteral("--explicit-install"));
    if (!reviewedDestination.isEmpty())
        arguments << QStringLiteral("--reviewed-destination") << reviewedDestination;
    if (!reviewedIndexHash.isEmpty()) arguments << QStringLiteral("--reviewed-index") << reviewedIndexHash;
    if (!reviewedManifestHash.isEmpty()) arguments << QStringLiteral("--reviewed-manifest") << reviewedManifestHash;
    return launchElevatedCommand(arguments);
}

ElevationLaunchResult launchElevatedUninstall(const QString& buildId, const QString& reviewedSnapshot)
{
    QStringList arguments{QStringLiteral("--uninstall-version"), buildId};
    if (!reviewedSnapshot.isEmpty()) arguments << QStringLiteral("--reviewed-snapshot") << reviewedSnapshot;
    return launchElevatedCommand(arguments);
}

ElevationLaunchResult launchElevatedRepair(const QString& buildId, const QString& transactionId, const QString& reviewedSnapshot,
                                          const QString& reviewedManifestHash)
{
    QStringList arguments{QStringLiteral("--repair-version"), buildId, QStringLiteral("--transaction-id"), transactionId};
    if (!reviewedSnapshot.isEmpty()) arguments << QStringLiteral("--reviewed-snapshot") << reviewedSnapshot;
    if (!reviewedManifestHash.isEmpty()) arguments << QStringLiteral("--reviewed-manifest") << reviewedManifestHash;
    return launchElevatedCommand(arguments);
}

}
