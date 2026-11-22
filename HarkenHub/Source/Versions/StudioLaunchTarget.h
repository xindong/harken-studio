#pragma once

#include <QSet>
#include <QString>

#include <vector>

#include "Versions/VersionScanner.h"

namespace harken::hub {

class StudioLaunchTarget
{
public:
    static const InstalledVersion* pick(
        const std::vector<InstalledVersion>& versions,
        const QString& pinnedBuildId,
        const QSet<QString>& excludedBuildIds);
    static QString pinnedBuildId();
    static void setPinnedBuildId(const QString& buildId);
};

}
