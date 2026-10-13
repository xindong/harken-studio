
#pragma once

#include "VQLTypes.h"
#include <vector>
#include <QString>

namespace VNX {

struct ProjectItem;

class IVQLDataSource
{
public:
    virtual ~IVQLDataSource() = default;

    virtual void collectRoots(const QString& rootSelector,
                              std::vector<ProjectItem*>& out) = 0;

    virtual void collectAllDescendants(ProjectItem* root,
                                       std::vector<ProjectItem*>& out) = 0;

    virtual VQLValue getProperty(const ProjectItem* item,
                                 const QString& fieldName) = 0;

    virtual bool matchesType(const ProjectItem* item,
                             const QString& typeName) = 0;
};

}
