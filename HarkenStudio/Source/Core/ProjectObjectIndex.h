#pragma once

#include "Utils/ObjectId.h"

#include <QHash>
#include <QString>

namespace harken::studio {

struct ProjectData;
struct ProjectItem;

struct ProjectObjectLocation
{
    enum class Kind { Item, Clip, Binding, Effect };
    enum class BusinessKind { None, Action, Playable, Variable };

    Kind kind = Kind::Item;
    QUuid ownerGuid;
    quint32 ownerObjectId = 0;
    QString name;
    BusinessKind businessKind = BusinessKind::None;
    QString ownerIconId;
};

class ProjectObjectIndex
{
public:
    static ProjectObjectIndex fromProject(const ProjectData* project,
                                           const ProjectItem* excludedSubtree = nullptr);

    const ProjectObjectLocation* find(harken::studio::types::ObjectId id) const
    {
        const auto found = objects_.constFind(id);
        return found == objects_.cend() ? nullptr : &*found;
    }

    QUuid projectGuid() const { return projectGuid_; }

private:
    QUuid projectGuid_;
    QHash<harken::studio::types::ObjectId, ProjectObjectLocation> objects_;
};

}
