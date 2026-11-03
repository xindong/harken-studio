#pragma once

#include "Versions/VersionScanner.h"

#include <QString>

namespace VNH {

struct ProjectCreateRequest
{
    InstalledVersion version;
    QString name;
    QString location;
    QString templateId;
    QString templatePath;
};

class ProjectCreateFlow
{
public:
    static bool create(const ProjectCreateRequest& request, QString* error);
};

}
