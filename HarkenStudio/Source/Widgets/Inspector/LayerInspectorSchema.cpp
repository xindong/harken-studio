
#include "LayerInspectorSchema.h"
#include "ContentObjectSchemaRows.h"

namespace VNX {

const PropertySchema& layerInspectorSchema()
{
    static const PropertySchema s =
        contentObjectInspectorRows(ProjectItem::Type::Layer);
    return s;
}

}
