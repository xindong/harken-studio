#pragma once

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QUuid>
#include <memory>

namespace VNX {

struct ProjectData;
struct ProjectItem;
class ProjectSerializer;

class ProjectItemSaveState
{
public:
    ProjectItemSaveState();
    ~ProjectItemSaveState();
    bool markSaved(const ProjectData& project);
    bool markSaved(const ProjectData& project, const ProjectSerializer& serializer,
                   const QHash<QUuid, QJsonObject>& serializedItems);
    bool acceptSaved(const ProjectData& project, const QSet<QUuid>& accepted,
                     const ProjectSerializer& serializer,
                     const QHash<QUuid, QJsonObject>& serializedItems);
    bool prepareAccepted(const ProjectData& project, const QSet<QUuid>& accepted,
                         const ProjectSerializer& serializer,
                         const QHash<QUuid, QJsonObject>& serializedItems,
                         QHash<QUuid, QJsonObject>* states);
    void publishAccepted(const QHash<QUuid, QJsonObject>& states);
    QList<ProjectItem*> refresh(ProjectData& project, const QSet<QUuid>* authoringWriteSet = nullptr);
    QList<ProjectItem*> refreshPending(ProjectData& project);
    bool ensurePrepared(const ProjectData& project);
    ProjectSerializer& serializer();
    ProjectItem* findItem(const QUuid& guid) const;
    const QSet<QUuid>& candidates() const { return candidates_; }
    bool requiresFullSave() const { return requiresFullSave_; }
    quint64 revision() const { return revision_; }
    void recordChange(ProjectItem* item);
    void recordPreview(const QUuid& guid);
    void invalidate();

private:
    bool rebuildIndex(const ProjectData& project);
    QList<ProjectItem*> refreshItems(const QList<ProjectItem*>& items);
    QHash<QUuid, QJsonObject> saved_;
    QHash<QUuid, ProjectItem*> index_;
    QList<ProjectItem*> items_;
    std::unique_ptr<ProjectSerializer> serializer_;
    QSet<QUuid> pending_;
    QSet<QUuid> candidates_;
    quint64 revision_ = 0;
    bool requiresFullRefresh_ = true;
    bool requiresFullSave_ = true;
};

}
