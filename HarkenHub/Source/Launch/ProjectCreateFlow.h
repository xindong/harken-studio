#pragma once

#include "Versions/VersionScanner.h"

#include <QString>
#include <functional>

class QWidget;

namespace VNH {

struct ProjectCreateRequest
{
    InstalledVersion version;
    QString name;
    QString location;
    QString templateId;
    QString templatePath;
};

struct ProjectCreateOutcome
{
    enum class Result
    {
        Created,
        Started,
        Unknown,
        Failed,
        Declined,
        Busy,
    };

    Result result = Result::Failed;
    QString projectPath;
};

class ProjectCreateFlow
{
public:
    static ProjectCreateOutcome create(const ProjectCreateRequest& request,
                                       QWidget* dialogParent,
                                       std::function<void(const ProjectCreateOutcome&)> onFinished = {});
};

}
