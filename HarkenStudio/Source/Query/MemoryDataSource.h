
#pragma once

#include "IVQLDataSource.h"

namespace VNX {

struct ProjectData;

class MemoryDataSource : public IVQLDataSource
{
public:
    void setProject(ProjectData* p) { project_ = p; }
    ProjectData* project() const { return project_; }

    void collectRoots(const QString& rootSelector,
                      std::vector<ProjectItem*>& out) override;

    void collectAllDescendants(ProjectItem* root,
                               std::vector<ProjectItem*>& out) override;

    VQLValue getProperty(const ProjectItem* item,
                         const QString& fieldName) override;

    bool matchesType(const ProjectItem* item,
                     const QString& typeName) override;

private:
    ProjectData* project_ = nullptr;
};

}
