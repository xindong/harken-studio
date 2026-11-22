
#include "LayerInspectorSchema.h"
#include "ContentObjectSchemaRows.h"

namespace harken::studio {

const PropertySchema& layerInspectorSchema()
{
    static const PropertySchema s =
        contentObjectInspectorRows(ProjectItem::Type::Layer);
    return s;
}

}
