#pragma once

#include "ProjectIssue.h"

#include <QString>
#include <QStringList>

namespace harken::studio {

struct ProjectLocation
{
    ProjectIssue issue = ProjectIssue::None;
    QString projectFilePath;
    QStringList candidates;

    bool ok() const { return issue == ProjectIssue::None; }
};

class ProjectLocator
{
public:
    static ProjectLocation locate(const QString& inputPath);
};

}
