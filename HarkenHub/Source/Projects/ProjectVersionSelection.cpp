#include "Projects/ProjectVersionSelection.h"

#include <QCoreApplication>

namespace VNH {

ProjectVersionSelection ProjectVersionSelection::resolve(
    const ProjectRecord& project, const ProjectProbeResult& probe,
    const std::vector<InstalledVersion>& versions, const QString& selectedBuildId)
{
    const auto text = [](const char* message) {
        return QCoreApplication::translate("HarkenHub", message);
    };
    ProjectVersionSelection result;
    result.buildId = selectedBuildId.isEmpty() ? project.preferredStudioBuildId : selectedBuildId;
    if (!result.buildId.isEmpty()) {
        result.source = text("User selection");
        for (const auto& version : versions)
            if (version.buildId == result.buildId)
                return result;
        result.errorCode = QStringLiteral("VersionUnavailable");
        result.reason = text("The selected Studio is not installed. Install it or choose another version.");
        return result;
    }
    std::vector<const InstalledVersion*> candidates;
    if (!probe.savedStudioVersion.isEmpty()) {
        result.source = text("Project recommendation");
        for (const auto& version : versions)
            if (version.available && version.semanticVersion == probe.savedStudioVersion)
                candidates.push_back(&version);
    } else if (!project.lastKnownGoodStudioBuildId.isEmpty()) {
        result.source = text("Last successful open");
        for (const auto& version : versions)
            if (version.available
                && (version.buildId == project.lastKnownGoodStudioBuildId
                    || version.expectedBuildId() == project.lastKnownGoodStudioBuildId))
                candidates.push_back(&version);
    }
    if (candidates.size() == 1) {
        result.buildId = candidates.front()->buildId;
        return result;
    }
    result.errorCode = QStringLiteral("VersionSelectionRequired");
    if (candidates.size() > 1)
        result.reason = text("Several installations match this project. Choose a specific Studio installation.");
    else if (!probe.savedStudioVersion.isEmpty())
        result.reason = text("The project was saved with Studio %1, which is not available. Install it or choose another version.")
                            .arg(probe.savedStudioVersion);
    else
        result.reason = text("Choose a Studio version to open this project.");
    return result;
}

}
