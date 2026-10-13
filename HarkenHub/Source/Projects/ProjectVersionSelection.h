#pragma once

#include "Projects/ProjectProbe.h"
#include "Projects/ProjectStore.h"
#include "Versions/VersionScanner.h"

namespace VNH {

struct ProjectVersionSelection
{
    QString buildId;
    QString source;
    QString errorCode;
    QString reason;

    static ProjectVersionSelection resolve(
        const ProjectRecord& project, const ProjectProbeResult& probe,
        const std::vector<InstalledVersion>& versions,
        const QString& selectedBuildId = {});
};

}
