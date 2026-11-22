#include "EffectInspectorSchema.h"

#include "ProjectItemInspectorRows.h"

namespace harken::studio {

const PropertySchema& effectInspectorSchema()
{
    static const PropertySchema schema{notesInspectorDescriptor()};
    return schema;
}

}
