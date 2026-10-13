#include "BusPlacementRules.h"

#include "Components/VBus.h"
#include "ProjectTypes.h"

#include <cstddef>
#include <cstdint>

namespace VNX {

bool validateBusHierarchyPlacement(const ProjectItem* parent,
                                   const ProjectItem* master,
                                   QString* error)
{
    auto fail = [error](const QString& message) {
        if (error)
            *error = message;
        return false;
    };

    if (!parent)
        return fail(QStringLiteral("Bus parent not found."));

    uint32_t depth = 0;
    for (const ProjectItem* current = parent; current;) {
        ++depth;
        if (current == master || !current->parent)
            break;
        current = current->parent;
    }
    if (depth >= 8)
        return fail(QStringLiteral("Bus hierarchy depth limit (8) exceeded."));

    size_t busChildCount = 0;
    for (const auto& child : parent->children) {
        if (child && child->comp<VBus>())
            ++busChildCount;
    }
    if (busChildCount >= 16)
        return fail(QStringLiteral("Maximum children per bus (16) exceeded."));

    return true;
}

}
