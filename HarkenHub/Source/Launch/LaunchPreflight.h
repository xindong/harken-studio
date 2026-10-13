#pragma once

#include "Projects/ProjectStore.h"
#include "Transactions/InstallIndex.h"
#include "Versions/VersionScanner.h"

#include <vector>

namespace VNH {

enum class LaunchIntent
{
    DirectOpenOnly,
    UpgradeApprovedByHub,
};

QString launchIntentName(LaunchIntent intent);

enum class LaunchVerdict
{
    Ready,
    Blocked
};

struct LaunchDecision
{
    LaunchVerdict verdict = LaunchVerdict::Blocked;
    QString blockCode;
    QString reason;
    InstalledVersion version;
    LaunchIntent intent = LaunchIntent::DirectOpenOnly;
    int projectSchema = 0;
};

class LaunchPreflight
{
public:
    static LaunchDecision evaluate(const ProjectRecord& project,
                                   const std::vector<InstalledVersion>& versions,
                                   const InstallIndexSnapshot& index,
                                   LaunchIntent intent = LaunchIntent::DirectOpenOnly,
                                   const QString& selectedBuildId = {});
};

}
