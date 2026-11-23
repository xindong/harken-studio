#pragma once

#include "Core/Components/VMixNode.h"
#include "Core/ProjectManager.h"
#include "InspectorLayout.h"
#include "Shared/SchemaFieldPick.h"

#include <algorithm>

namespace harken::studio {

inline PropertyDescriptor sendsInspectorDescriptor(SendType type = SendType::Send)
{
    auto d = *pickSchemaField(VMixNode::schema(), QLatin1String("sends"));
    const bool sidechain = type == SendType::Sidechain;
    d.listRowFilter.insert(QStringLiteral("type"), sendTypeName(type));
    if (sidechain) {
        d.storageField = d.field;
        d.field = QStringLiteral("sidechains");
        d.label = QStringLiteral("Sidechains");
        d.section = PropertySection::Sidechain;
        d.listAddChoiceField = QStringLiteral("duckTarget");
    } else {
        d.listRowFilter.insert(QStringLiteral("duckTarget"), false);
    }
    std::erase_if(d.elementSchema, [sidechain](const auto& ed) {
        return ed.field == QLatin1String("type") || (!sidechain && ed.field == QLatin1String("duckTarget"));
    });
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
                && other.value(QStringLiteral("type")) == row.value(QStringLiteral("type"))
                && other.value(QStringLiteral("duckTarget")).toBool() == row.value(QStringLiteral("duckTarget")).toBool())
                return QObject::tr("A route with these settings already targets this bus.");
        }
        return QString{};
    };
    return withInspectorRowOrder(std::move(d));
}

}
