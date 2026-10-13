#pragma once

#include "Launch/LaunchPreflight.h"
#include "Versions/VersionScanner.h"

#include <QString>

#include <vector>
#include <functional>

class QWidget;

namespace VNH {

class ProjectStore;

struct ProjectOpenOutcome
{
    enum class Result
    {
        Launched,
        Started,
        Unknown,
        Declined,
        Failed,
        Blocked,
        Busy
    };

    Result result = Result::Failed;
    QString blockCode;
};

class ProjectOpenFlow
{
public:
    static bool ensureRegistered(ProjectStore* store, const QString& path,
                                 QString* canonicalPath, QString* error);
    static ProjectOpenOutcome open(ProjectStore* store,
                                   const QString& canonicalPath,
                                   const std::vector<InstalledVersion>& versions,
                                   QWidget* dialogParent,
                                   const QString& selectedBuildId = {},
                                   std::function<void(const ProjectOpenOutcome&)> onFinished = {});
    static bool isVersionBlock(const QString& blockCode);
};

}
