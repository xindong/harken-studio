
#pragma once

#include "Core/Components/VMixNode.h"
#include "Core/ProjectManager.h"
#include "Shared/SchemaFieldPick.h"

namespace VNX {

inline PropertyDescriptor mainOutputInspectorDescriptor()
{
    PropertyDescriptor d;
    if (const auto* src = pickSchemaField(VMixNode::schema(), QLatin1String("outputBusId")))
        d = *src;
    d.refSelectable = [](uint32_t hostObjectID, uint32_t candidateID) {
        return ProjectManager::instance().outputBusSelectable(hostObjectID, candidateID);
    };
    return d;
}

}
