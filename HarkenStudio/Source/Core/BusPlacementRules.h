#pragma once

#include <QString>

namespace VNX {

struct ProjectItem;

bool validateBusHierarchyPlacement(const ProjectItem* parent,
                                   const ProjectItem* master,
                                   QString* error = nullptr);

}
