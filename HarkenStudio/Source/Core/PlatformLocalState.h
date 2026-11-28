#pragma once

#include "ActionManager.h"
#include "FieldGroups.h"
#include "ProjectPlatform.h"

#include <QSet>
#include <QString>
#include <QVariant>

namespace harken::studio {

struct ProjectData;
struct ProjectItem;

struct PlatformLocalAddress
{
    enum class Kind { Property, Filter, GroupField, SourceActive, RowCell, EffectChain, Selections };

    Kind kind = Kind::Property;
    QString field;
    FieldGroupId group = FieldGroupId::Output;
    bool canInherit = false;
    QUuid row;
    QString cell;

    bool operator==(const PlatformLocalAddress&) const = default;
};

namespace PlatformLocalState {

QSet<ProjectPlatformId> localPlatforms(const ProjectData& project, const ProjectItem& item,
                                       const PlatformLocalAddress& address);
bool hasScalarValue(const PlatformLocalAddress& address);
QVariant effectiveValue(const ProjectData& project, ProjectItem& item,
                        const PlatformLocalAddress& address, const ProjectPlatformId& platform);
ActionResult setLocal(const ProjectData& project, ProjectItem& item,
                      const PlatformLocalAddress& address, const ProjectPlatformId& platform,
                      const QObject* originator = nullptr);
ActionResult clearLocal(ProjectItem& item, const PlatformLocalAddress& address,
                        const ProjectPlatformId& platform, const QObject* originator = nullptr);

}
}
