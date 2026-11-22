
#include "SegmentInspectorSchema.h"
#include "ContentObjectSchemaRows.h"

namespace harken::studio {

const PropertySchema& segmentInspectorSchema()
{
    static const PropertySchema s =
        contentObjectInspectorRows(ProjectItem::Type::Segment);
    return s;
}

}
