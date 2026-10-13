
#include "PieceInspectorSchema.h"
#include "ContentObjectSchemaRows.h"

namespace VNX {

const PropertySchema& pieceInspectorSchema()
{
    static const PropertySchema s =
        contentObjectInspectorRows(ProjectItem::Type::Piece);
    return s;
}

}
