#pragma once

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QUuid>

namespace VNX {

struct ProjectData;
struct ProjectItem;

class ProjectItemSaveState
{
public:
    bool markSaved(const ProjectData& project);
    QList<ProjectItem*> refresh(ProjectData& project) const;

private:
    QHash<QUuid, QJsonObject> saved_;
};

}
