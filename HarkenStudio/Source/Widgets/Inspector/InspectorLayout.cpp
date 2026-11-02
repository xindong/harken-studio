#include "InspectorLayout.h"
#include "Widgets/Common/TableWidget.h"

#include <QLabel>
#include <QSet>
#include <algorithm>
#include <stdexcept>

namespace VNX {

QString inspectorCategoryTitle(PropertyCategory category)
{
    switch (category) {
    case PropertyCategory::Basic: return QStringLiteral("Basic");
    case PropertyCategory::Route: return QStringLiteral("Route");
    case PropertyCategory::Spatial: return QStringLiteral("Spatial");
    case PropertyCategory::Limitation: return QStringLiteral("Limitation");
    }
    throw std::invalid_argument("Unknown Inspector category");
}

QString inspectorCategoryObjectName(PropertyCategory category)
{
    return QStringLiteral("inspectorTab") + inspectorCategoryTitle(category);
}

const std::vector<InspectorSectionInfo>& inspectorSections()
{
    static const std::vector<InspectorSectionInfo> sections = {
        {PropertySection::Playback, PropertyCategory::Basic, "playback", "Playback"},
        {PropertySection::Identity, PropertyCategory::Basic, "identity", "Identity"},
        {PropertySection::Advanced, PropertyCategory::Basic, "advanced", "Advanced"},
        {PropertySection::Source, PropertyCategory::Basic, "sources", "Source"},
        {PropertySection::Selection, PropertyCategory::Basic, "selection", "Selection"},
        {PropertySection::Step, PropertyCategory::Basic, "steps", "Step"},
        {PropertySection::Dimension, PropertyCategory::Basic, "dimensions", "Dimension"},
        {PropertySection::Label, PropertyCategory::Basic, "labels", "Label"},
        {PropertySection::Clip, PropertyCategory::Basic, "clips", "Clip"},
        {PropertySection::Custom, PropertyCategory::Basic, "custom", "Custom"},
        {PropertySection::MainOutput, PropertyCategory::Route, "main_output", "Main Output"},
        {PropertySection::Input, PropertyCategory::Route, "inputs", "Input"},
        {PropertySection::Send, PropertyCategory::Route, "send", "Send"},
        {PropertySection::Receive, PropertyCategory::Route, "receive", "Receive"},
        {PropertySection::Rating, PropertyCategory::Spatial, "rating", "Rating"},
        {PropertySection::PlaybackPriority, PropertyCategory::Limitation, "playback_priority", "Playback Priority"},
        {PropertySection::FilterLowPass, PropertyCategory::Basic, "filter_low_pass", "Low Pass"},
        {PropertySection::FilterHighPass, PropertyCategory::Basic, "filter_high_pass", "High Pass"},
        {PropertySection::FilterLowShelf, PropertyCategory::Basic, "filter_low_shelf", "Low Shelf"},
        {PropertySection::FilterHighShelf, PropertyCategory::Basic, "filter_high_shelf", "High Shelf"},
    };
    return sections;
}

const InspectorSectionInfo& inspectorSectionInfo(PropertySection section)
{
    for (const auto& info : inspectorSections())
        if (info.section == section) return info;
    throw std::invalid_argument("Inspector field requires a registered section");
}

void validateInspectorSchema(const PropertySchema& schema)
{
    QSet<QString> fields;
    for (const auto& field : schema) {
        inspectorSectionInfo(field.section);
        if (field.field.isEmpty() || field.label.isEmpty() || fields.contains(field.field))
            throw std::invalid_argument("Inspector fields require unique identifiers and labels");
        fields.insert(field.field);
    }
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

PropertyDescriptor withInspectorRowOrder(PropertyDescriptor field)
{
    PropertyDescriptor order;
    order.field = QStringLiteral("order");
    order.type = PropType::Int;
    order.label = QStringLiteral("#");
    order.flags.serialize = false;
    order.flags.settableByName = false;
    order.flags.state = true;
    field.elementSchema.insert(field.elementSchema.begin(), std::move(order));
    return field;
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
