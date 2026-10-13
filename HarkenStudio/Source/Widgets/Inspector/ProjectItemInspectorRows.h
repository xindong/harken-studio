#pragma once

#include "Core/ProjectTypes.h"
#include "Core/PropertyDescriptor.h"

namespace VNX {

inline PropertyDescriptor notesInspectorDescriptor()
{
    PropertyDescriptor descriptor;
    descriptor.field = QStringLiteral("notes");
    descriptor.type = PropType::String;
    descriptor.label = QStringLiteral("Notes");
    descriptor.section=PropertySection::Custom;
    descriptor.flags.multiline = true;
    descriptor.flags.serialize = false;
    descriptor.getItem = [](const ProjectItem* item) -> QVariant {
        return item ? QVariant(item->notes) : QVariant();
    };
    return descriptor;
}

inline PropertySchema withProjectItemInspectorRows(PropertySchema schema)
{
    schema.push_back(notesInspectorDescriptor());
    return schema;
}

}
