
#pragma once

#include "Core/Components/VMixNode.h"
#include "InspectorLayout.h"
#include "Core/ProjectManager.h"
#include "Shared/SchemaFieldPick.h"

namespace VNX {

inline PropertyDescriptor sendsInspectorDescriptor()
{
    PropertyDescriptor d;
    if (const auto* src = pickSchemaField(VMixNode::schema(), QLatin1String("sends")))
        d = *src;
    d.listAddChoiceField = QStringLiteral("mode");
    d.editElement = [](const QVariantMap& current, const QString& field, const QVariant& value) {
        auto row = current;
        if (field == QLatin1String("mode") && current.value(field).toInt() != value.toInt()) {
            auto volumes = current.value(QStringLiteral("modeVolumes")).toMap();
            volumes.insert(QString::number(current.value(field).toInt()),
                           current.value(QStringLiteral("sendLevel")));
            const auto next = QString::number(value.toInt());
            if (volumes.contains(next)) row[QStringLiteral("sendLevel")] = volumes.take(next);
            row[QStringLiteral("modeVolumes")] = volumes;
        }
        row[field] = value;
        return row;
    };
    for (auto& ed : d.elementSchema)
        if (ed.field == QLatin1String("targetBusID"))
            ed.refSelectableForElement = [](uint32_t hostObjectID, const QVariantMap& row, uint32_t candidateID) {
                return ProjectManager::instance().sendTargetStructurallySelectable(
                    hostObjectID, candidateID, static_cast<SendMode>(row.value(QStringLiteral("mode")).toInt()));
            };
    d.validateElement = [](const QVariantList& rows, int row, const QString& field) {
        if (field != QLatin1String("targetBusID") && field != QLatin1String("mode")) return QString{};
        const auto element = rows[row].toMap();
        const auto target = element.value(QStringLiteral("targetBusID"));
        const int mode = element.value(QStringLiteral("mode")).toInt();
        for (int i = 0; i < rows.size(); ++i)
            if (i != row && rows[i].toMap().value(QStringLiteral("targetBusID")) == target
                && rows[i].toMap().value(QStringLiteral("mode")).toInt() == mode)
                return QObject::tr("Send targets must be unique within each mode");
        return QString{};
    };
    return withInspectorRowOrder(std::move(d));
}

}
