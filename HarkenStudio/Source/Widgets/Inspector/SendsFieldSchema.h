
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
    for (auto& ed : d.elementSchema)
        if (ed.field == QLatin1String("targetBusID"))
            ed.refSelectable = [](uint32_t hostObjectID, uint32_t candidateID) {
                return ProjectManager::instance().sendTargetStructurallySelectable(hostObjectID, candidateID);
            };
    d.validateElement = [](const QVariantList& rows, int row, const QString& field) {
        if (field != QLatin1String("targetBusID")) return QString{};
        const auto target = rows[row].toMap().value(field);
        for (int i = 0; i < rows.size(); ++i)
            if (i != row && rows[i].toMap().value(field) == target)
                return QObject::tr("Send targets must be unique");
        return QString{};
    };
    return withInspectorRowOrder(std::move(d));
}

}
