
#pragma once

#include "ProjectTypes.h"

#include <QColor>
#include <QIcon>

namespace VNX {

QIcon iconForItemType(ProjectItem::Type type);
QIcon iconForItemType(ProjectItem::Type type, const QColor& tint);
const char* iconLogicalIdForItem(const ProjectItem& item);
QIcon iconForItem(const ProjectItem& item);
QIcon iconForItem(const ProjectItem& item, const QColor& tint);

}
