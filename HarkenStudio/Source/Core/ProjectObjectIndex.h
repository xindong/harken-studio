#pragma once

#include "Utils/ObjectId.h"

#include <QHash>
#include <QString>

namespace VNX {

struct ProjectData;
struct ProjectItem;

struct ProjectObjectLocation
{
    enum class Kind { Item, Clip, Marker, Binding, Effect };
    enum class BusinessKind { None, Action, Playable, Variable };

    Kind kind = Kind::Item;
    QUuid ownerGuid;
    quint32 ownerObjectId = 0;
    QString name;
    BusinessKind businessKind = BusinessKind::None;
};

class ProjectObjectIndex
{
public:
    static ProjectObjectIndex fromProject(const ProjectData* project,
                                           const ProjectItem* excludedSubtree = nullptr);

    const ProjectObjectLocation* find(harkenstudio::ObjectId id) const
    {
        const auto found = objects_.constFind(id);
        return found == objects_.cend() ? nullptr : &*found;
    }

    QUuid projectGuid() const { return projectGuid_; }

private:
    QUuid projectGuid_;
    QHash<harkenstudio::ObjectId, ProjectObjectLocation> objects_;
};

}
