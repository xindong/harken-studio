#include "Versions/StudioLaunchTarget.h"

#include <QSettings>
#include <QVersionNumber>

namespace VNH {

namespace {

QString pinnedBuildIdKey()
{
    return QStringLiteral("launchStudioBuildId");
}

}

const InstalledVersion* StudioLaunchTarget::pick(
    const std::vector<InstalledVersion>& versions,
    const QString& pinnedBuildId,
    const QSet<QString>& excludedBuildIds)
{
    if (!pinnedBuildId.isEmpty() &&
        !excludedBuildIds.contains(pinnedBuildId)) {
        for (const auto& version : versions) {
            if (version.buildId == pinnedBuildId)
                return &version;
        }
    }

    const InstalledVersion* best = nullptr;
    QVersionNumber bestNumber;
    for (const auto& version : versions) {
        if (excludedBuildIds.contains(version.buildId))
            continue;
        const QVersionNumber number =
            QVersionNumber::fromString(version.semanticVersion);
        if (!best) {
            best = &version;
            bestNumber = number;
            continue;
        }
        const int comparison = QVersionNumber::compare(number, bestNumber);
        if (comparison > 0 ||
            (comparison == 0 && version.buildId > best->buildId)) {
            best = &version;
            bestNumber = number;
        }
    }
    return best;
}

QString StudioLaunchTarget::pinnedBuildId()
{
    return QSettings().value(pinnedBuildIdKey()).toString();
}

void StudioLaunchTarget::setPinnedBuildId(const QString& buildId)
{
    QSettings settings;
    if (buildId.isEmpty())
        settings.remove(pinnedBuildIdKey());
    else
        settings.setValue(pinnedBuildIdKey(), buildId);
}

}
