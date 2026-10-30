#pragma once

#include <QUuid>

namespace VNX {
struct ProjectData;
struct ProjectItem;
namespace VariableConsumerSemantics::Internal {
ProjectItem* findItem(ProjectData* project, const QUuid& guid);
const ProjectItem* findItem(const ProjectData* project, const QUuid& guid);
}
}
