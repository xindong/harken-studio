#pragma once

#include "VQLTypes.h"

#include <QString>

namespace VNX {

struct ProjectItem;
struct ProjectData;

namespace VQLFieldCatalog {

bool isKnownField(const QString& fieldName);
bool project(const ProjectItem* item, const QString& fieldName,
             QString* outputName, VQLValue* value, const ProjectData* projectData = nullptr);

}
}
