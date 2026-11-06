
#include "PropertySchemaJson.h"
#include "InspectorLayout.h"
#include "Core/ComponentCatalog.h"
#include "Core/ItemTypeCatalog.h"
#include "SoundInspectorSchema.h"
#include "ContainerInspectorSchema.h"
#include "TrackInspectorSchema.h"
#include "SegmentInspectorSchema.h"
#include "LayerInspectorSchema.h"
#include "BusInspectorSchema.h"
#include "ActionInspectorSchema.h"
#include "VariableInspectorSchema.h"
#include "AttenuationInspectorSchema.h"
#include "EffectInspectorSchema.h"

#include <QJsonArray>
#include <QJsonValue>
#include <QSet>
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

const std::vector<InspectorSectionInfo>& inspectorSections()
{
    static const std::vector<InspectorSectionInfo> sections = {
        {PropertySection::Playback, PropertyCategory::Basic, "playback", "Playback"},
        {PropertySection::Status, PropertyCategory::Basic, "status", "Status"},
        {PropertySection::Advanced, PropertyCategory::Basic, "advanced", "Advanced"},
        {PropertySection::Source, PropertyCategory::Basic, "sources", "Source"},
        {PropertySection::Selection, PropertyCategory::Basic, "selection", "Selection"},
        {PropertySection::Step, PropertyCategory::Basic, "steps", "Step"},
        {PropertySection::Dimension, PropertyCategory::Basic, "dimensions", "Dimension"},
        {PropertySection::Label, PropertyCategory::Basic, "labels", "Label"},
        {PropertySection::Clip, PropertyCategory::Basic, "clips", "Clip"},
        {PropertySection::Effect, PropertyCategory::Basic, "effects", "FX"},
        {PropertySection::Custom, PropertyCategory::Basic, "custom", "Custom"},
        {PropertySection::Output, PropertyCategory::Route, "output", "Output"},
        {PropertySection::Input, PropertyCategory::Route, "inputs", "Input"},
        {PropertySection::Send, PropertyCategory::Route, "send", "Send"},
        {PropertySection::Receive, PropertyCategory::Route, "receive", "Receive"},
        {PropertySection::Rating, PropertyCategory::Spatial, "rating", "Rating"},
        {PropertySection::PlaybackPriority, PropertyCategory::Limitation, "playback_priority", "Playback Priority"},
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


namespace {

QString widgetString(const PropertyDescriptor& d)
{
    if (d.flags.segmented && !d.segments.empty())
        return QStringLiteral("value_segmented");

    switch (d.type)
    {
        case PropType::Float:     return QStringLiteral("slider");
        case PropType::Int:       return QStringLiteral("int_spinbox");
        case PropType::Bool:      return QStringLiteral("checkbox");
        case PropType::Enum:      return QStringLiteral("combo");
        case PropType::String:    return d.flags.multiline
            ? QStringLiteral("multiline_text") : QStringLiteral("text");
        case PropType::Color:     return QStringLiteral("color_picker");
        case PropType::ObjectRef: return QStringLiteral("object_ref");
        case PropType::List:      return QStringLiteral("list");
        case PropType::Range:     return QStringLiteral("range");
        case PropType::Struct:    return QStringLiteral("struct");
    }
    return QStringLiteral("text");
}

QJsonObject rowToJson(const PropertyDescriptor& d)
{
    QJsonObject row;
    row[QStringLiteral("type")]  = widgetString(d);
    row[QStringLiteral("field")] = d.field;
    row[QStringLiteral("label")] = d.label;
    row[QStringLiteral("showLabel")] = d.type != PropType::List;
    row[QStringLiteral("readOnly")] = d.flags.readOnly;
    if (d.type == PropType::List)
        row[QStringLiteral("tableKind")] = d.listPresentation == ListPresentation::Display
            ? QStringLiteral("display") : QStringLiteral("config");
    if (d.flags.state) row[QStringLiteral("state")] = true;
    if (d.min.isValid()) row[QStringLiteral("min")] = d.min.toDouble();
    if (d.max.isValid()) row[QStringLiteral("max")] = d.max.toDouble();
    if (!d.unit.isEmpty()) row[QStringLiteral("suffix")] = d.unit;
    row[QStringLiteral("decimals")] = d.decimals;
    if (d.displayScale != 1.0)
        row[QStringLiteral("displayScale")] = d.displayScale;

    if (d.type == PropType::Enum)
    {
        QJsonArray opts;
        for (const auto& o : d.options)
        {
            QJsonObject oo;
            oo[QStringLiteral("text")]  = o.text;
            oo[QStringLiteral("value")] = QJsonValue::fromVariant(o.value);
            opts.append(oo);
        }
        row[QStringLiteral("options")] = opts;
    }
    if (!d.elementSchema.empty()) {
        QJsonArray children;
        for (const auto& element : d.elementSchema)
            children.append(rowToJson(element));
        row[d.type == PropType::List ? QStringLiteral("columns") : QStringLiteral("children")] = children;
    }
    return row;
}

}

QJsonObject propertySchemaToJson(const PropertySchema& schema,
                                 const QString& component,
                                 const QStringList& itemTypes)
{
    validateInspectorSchema(schema);
    QJsonArray sections;
    for (const auto& info : inspectorSections())
    {
        QJsonArray children;
        for (const auto& field : schema)
            if (field.section == info.section) children.append(rowToJson(field));
        if (children.isEmpty()) continue;
        QJsonObject sec;
        sec[QStringLiteral("type")]     = QStringLiteral("collapsible");
        sec[QStringLiteral("id")]       = info.key;
        sec[QStringLiteral("title")]    = info.title;
        sec[QStringLiteral("category")] = inspectorCategoryTitle(info.category);
        sec[QStringLiteral("expanded")] = true;
        sec[QStringLiteral("children")] = children;
        sections.append(sec);
    }

    QJsonObject root;
    root[QStringLiteral("component")] = component;
    QJsonArray types;
    for (const auto& t : itemTypes) types.append(t);
    root[QStringLiteral("itemTypes")] = types;
    root[QStringLiteral("sections")]  = sections;
    return root;
}

QJsonObject inspectorSchemaToJson(const QString& schemaId)
{
    const auto type = ItemTypeCatalog::parseTypeName(schemaId, Qt::CaseSensitive, false);
    if (!type)
        return {};

    const PropertySchema* schema = nullptr;
    ComponentType component = ComponentType::Audio;
    switch (*type)
    {
        case ProjectItem::Type::Sound:
            schema = &soundInspectorSchema();       component = ComponentType::Audio;       break;
        case ProjectItem::Type::Container:
            schema = &containerInspectorSchema();   component = ComponentType::Audio;       break;
        case ProjectItem::Type::Track:
            schema = &trackInspectorSchema();       component = ComponentType::Audio;       break;
        case ProjectItem::Type::Segment:
            schema = &segmentInspectorSchema();       component = ComponentType::Audio;       break;
        case ProjectItem::Type::Layer:
            schema = &layerInspectorSchema();       component = ComponentType::MixNode;     break;
        case ProjectItem::Type::Bus:
            schema = &busInspectorSchema();         component = ComponentType::Bus;         break;
        case ProjectItem::Type::Action:
            schema = &actionInspectorSchema();      component = ComponentType::Action;      break;
        case ProjectItem::Type::Variable:
            schema = &variableInspectorSchema();    component = ComponentType::Variable;    break;
        case ProjectItem::Type::Attenuation:
            schema = &attenuationInspectorSchema(); component = ComponentType::Attenuation; break;
        case ProjectItem::Type::Effect:
            schema = &effectInspectorSchema();      component = ComponentType::Effect;      break;
        default:
            return {};
    }

    return propertySchemaToJson(
        *schema, ComponentCatalog::componentName(component),
        {ItemTypeCatalog::ipcName(*type)});
}

}
