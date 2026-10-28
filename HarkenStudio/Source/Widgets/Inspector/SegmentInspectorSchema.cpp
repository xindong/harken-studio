
#include "SegmentInspectorSchema.h"
#include "ContentObjectSchemaRows.h"

namespace VNX {

const PropertySchema& segmentInspectorSchema()
{
    static const PropertySchema s =
        contentObjectInspectorRows(ProjectItem::Type::Segment);
    return s;
}

}
