#include "AttenuationInspectorSchema.h"
#include "InspectorLayout.h"
#include "ProjectItemInspectorRows.h"
#include "Core/Components/VAttenuation.h"
#include "Shared/SchemaFieldPick.h"

namespace VNX {

const PropertySchema& attenuationInspectorSchema()
{
    static const PropertySchema s = [] {
        return PropertySchema{
            withInspectorRowOrder(*pickSchemaField(VAttenuation::schema(), QLatin1String("dimensions"))),
            notesInspectorDescriptor()};
    }();
    return s;
}

}
