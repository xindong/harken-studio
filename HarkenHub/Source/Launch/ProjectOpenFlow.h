#pragma once

#include "Launch/LaunchPreflight.h"
#include "Versions/VersionScanner.h"

#include <QString>

#include <vector>

class QWidget;

namespace VNH {

class ProjectStore;

struct ProjectOpenOutcome
{
    enum class Result
    {
        Started,
        Declined,
        Failed,
        Blocked,
    };

    Result result = Result::Failed;
    QString blockCode;
    QString error;
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
                                   const QString& selectedBuildId = {});
    static bool isVersionBlock(const QString& blockCode);
};

}
