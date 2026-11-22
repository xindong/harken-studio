#pragma once

#include <QString>

namespace harken::studio {

struct ProjectItem;

bool validateBusHierarchyPlacement(const ProjectItem* parent,
                                   const ProjectItem* master,
                                   QString* error = nullptr);

}
