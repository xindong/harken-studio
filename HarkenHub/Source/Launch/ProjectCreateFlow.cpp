#include "Launch/ProjectCreateFlow.h"
#include "Launch/StudioLauncher.h"
#include "Versions/LocalBuildRegistry.h"
#include "Versions/StudioCapabilities.h"

#include <QCoreApplication>
#include <algorithm>

namespace VNH {

namespace {

QString hubText(const char* source)
{
    return QCoreApplication::translate("HarkenHub", source);
}

}

bool ProjectCreateFlow::create(const ProjectCreateRequest& request, QString* error)
{
    InstalledVersion selectedVersion = request.version;
    if (selectedVersion.capabilitiesVersion != 1
        && selectedVersion.capabilitiesVersion != kStudioCapabilitiesVersion) {
        *error = hubText("This Studio can open existing projects, but project creation from Hub is unavailable.");
        return false;
    }
    if (selectedVersion.isLocalBuild()) {
        const std::vector<InstalledVersion> localBuilds =
            VersionScanner::scanLocalBuilds();
        const auto current = std::find_if(
            localBuilds.begin(), localBuilds.end(),
            [&selectedVersion](const InstalledVersion& candidate) {
                return candidate.buildId == selectedVersion.buildId;
            });
        if (current == localBuilds.end() || !current->available
            || current->executableFingerprint.isEmpty()
            || LocalBuildRegistry::fileFingerprint(current->executablePath) != current->executableFingerprint) {
            QString recheckError;
            if (current != localBuilds.end() && current->available)
                LocalBuildRegistry::requestRecheck(current->executablePath, current->executableFingerprint, &recheckError);
            *error = recheckError.isEmpty()
                ? hubText("This local build changed and is being checked again. Try again shortly.")
                : recheckError;
            return false;
        }
        selectedVersion = *current;
    }

    StudioCreateSpec spec;
    spec.launch.installPath = selectedVersion.installPath;
    spec.launch.executablePath = selectedVersion.executablePath;
    spec.projectName = request.name;
    spec.projectLocation = request.location;
    spec.templateId = request.templateId;
    spec.templatePath = request.templatePath;
    return StudioLauncher::startCreate(spec, error);
}

}
