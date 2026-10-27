#pragma once

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QUuid>

namespace VNX {

struct ProjectData;
struct ProjectItem;

class ProjectItemSaveState
{
public:
    bool markSaved(const ProjectData& project);
    QList<ProjectItem*> refresh(ProjectData& project, const QSet<QUuid>* authoringWriteSet = nullptr);
    void recordPreview(const QUuid& guid);
    void invalidate();

private:
    QHash<QUuid, QJsonObject> saved_;
    QSet<QUuid> pendingPreviews_;
    bool requiresFullRefresh_ = true;
};

}
