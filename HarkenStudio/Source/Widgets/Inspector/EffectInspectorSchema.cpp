#include "EffectInspectorSchema.h"

#include "ProjectItemInspectorRows.h"

namespace VNX {

const PropertySchema& effectInspectorSchema()
{
    static const PropertySchema schema{notesInspectorDescriptor()};
    return schema;
}

}
