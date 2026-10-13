
#include "VariableInspectorSchema.h"
#include "InspectorLayout.h"
#include "Core/Components/VVariable.h"
#include "ProjectItemInspectorRows.h"

namespace VNX {

const PropertySchema& variableInspectorSchema()
{
    // rangeMin / rangeMax stay in the component schema as the serialized and
    // by-name-settable truth; the page edits the declared domain through the
    // single "range" row, so the two bound rows drop out here.
    static const PropertySchema schema = [] {
        PropertySchema rows;
        for (const PropertyDescriptor& d : VVariable::schema())
        {
            if (d.flags.advanced
                || d.field == QLatin1String("rangeMin")
                || d.field == QLatin1String("rangeMax"))
                continue;
            rows.push_back(d);
        }
        return withProjectItemInspectorRows(std::move(rows));
    }();
    return schema;
}

}
