
#include "PropertySchemaJson.h"
#include "InspectorLayout.h"

#include <QJsonArray>
#include <QJsonValue>

namespace VNX {

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

}
