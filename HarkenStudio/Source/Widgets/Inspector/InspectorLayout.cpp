#include "InspectorLayout.h"
#include "Widgets/Common/TableWidget.h"

#include <QLabel>
#include <algorithm>
#include <stdexcept>

namespace VNX {

QString inspectorCategoryObjectName(PropertyCategory category)
{
    return QStringLiteral("inspectorTab") + inspectorCategoryTitle(category);
}

void applyInspectorTableDescriptor(TableWidget* table, const PropertySchema& schema,
                                   const QString& field)
{
    const auto found = std::find_if(schema.begin(), schema.end(), [&](const auto& descriptor) {
        return descriptor.field == field && descriptor.type == PropType::List;
    });
    if (found == schema.end())
        throw std::invalid_argument("Inspector table requires a list field descriptor");
    applyInspectorTableDescriptor(table, *found);
}

void applyInspectorTableDescriptor(TableWidget* table, const PropertyDescriptor& field)
{
    if (table->columnCount() != static_cast<int>(field.elementSchema.size()))
        throw std::invalid_argument("Inspector table column schema mismatch");
    table->setProperty("inspectorField", field.field);
    table->setAccessibleName(field.label);
    for (size_t i = 0; i < field.elementSchema.size(); ++i) {
        auto* cell = table->headerCell(static_cast<int>(i));
        const auto& column = field.elementSchema[i];
        cell->setText(column.label);
        cell->setProperty("inspectorColumn", column.field);
        cell->setProperty("inspectorState", column.flags.state);
    }
}

}
