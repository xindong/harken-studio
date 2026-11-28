#pragma once

#include "Core/Components/VMixNode.h"
#include "Core/ProjectManager.h"
#include "InspectorLayout.h"
#include "Shared/SchemaFieldPick.h"

namespace harken::studio {

inline PropertyDescriptor routesInspectorDescriptor(SendType type)
{
    const bool sidechain = type == SendType::Sidechain;
    auto d = *pickSchemaField(VMixNode::schema(),
        sidechain ? QLatin1String("sidechains") : QLatin1String("sends"));
    if (sidechain) d.listAddChoiceField = QStringLiteral("duckTarget");
    for (auto& ed : d.elementSchema)
        if (ed.field == QLatin1String("targetBusID"))
            ed.refSelectableForElement = [type](uint32_t host, const QVariantMap& row, uint32_t target) {
                return ProjectManager::instance().sendTargetStructurallySelectable(
                    host, target, type, row.value(QStringLiteral("duckTarget")).toBool());
            };
    d.validateElement = [](const QVariantList& rows, int index, const QString&) {
        const auto row = rows[index].toMap();
        for (int i = 0; i < rows.size(); ++i) {
            if (i == index) continue;
            const auto other = rows[i].toMap();
            if (other.value(QStringLiteral("targetBusID")) == row.value(QStringLiteral("targetBusID"))
                && other.value(QStringLiteral("duckTarget")).toBool() == row.value(QStringLiteral("duckTarget")).toBool())
                return QObject::tr("A route with these settings already targets this bus.");
        }
        return QString{};
    };
    return withInspectorRowOrder(std::move(d));
}

}
