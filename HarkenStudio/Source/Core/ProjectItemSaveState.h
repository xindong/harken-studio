#pragma once

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QUuid>

namespace VNX {

struct ProjectData;
struct ProjectItem;
class ProjectSerializer;

class ProjectItemSaveState
{
public:
    bool markSaved(const ProjectData& project);
    bool markSaved(const ProjectData& project, const ProjectSerializer& serializer,
                   const QHash<QUuid, QJsonObject>& serializedItems);
    QList<ProjectItem*> refresh(ProjectData& project, const QSet<QUuid>* authoringWriteSet = nullptr);
    void recordPreview(const QUuid& guid);
    void invalidate();

private:
    QHash<QUuid, QJsonObject> saved_;
    QSet<QUuid> pendingPreviews_;
    bool requiresFullRefresh_ = true;
};

}
