
#include "ContainerInspectorSchema.h"
#include "ContentObjectSchemaRows.h"
#include "Core/ProjectTypes.h"
#include "Core/Components/VSelection.h"
#include "Core/ContainerBehaviorSemantics.h"
#include "Shared/SchemaFieldPick.h"

namespace VNX {

#if defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#  pragma clang diagnostic ignored "-Wmissing-field-initializers"
#endif

const PropertySchema& containerInspectorSchema()
{
    static const PropertySchema s = [] {
        auto rows = contentObjectInspectorRows(ProjectItem::Type::Container);
        rows.push_back(*pickSchemaField(VSelection::schema(), QLatin1String("mode")));
        rows.push_back(*pickSchemaField(VSelection::schema(), QLatin1String("variableResponse")));
        rows.push_back(*pickSchemaField(VSelection::schema(), QLatin1String("selections")));
        rows.push_back(*pickSchemaField(VSelection::schema(), QLatin1String("playType")));

        PropertyDescriptor behavior;
        behavior.field = QStringLiteral("behavior");
        behavior.ownerComponent = ComponentType::Selection;
        behavior.type = PropType::Struct;
        behavior.label = QStringLiteral("Behavior");
        behavior.section = PropertySection::Selection;
        behavior.flags.serialize = false;
        behavior.elementSchema = {
            {.field="discipline", .type=PropType::Enum, .label="Discipline"},
            {.field="avoidRepeat", .type=PropType::Int, .label="Min Repeat Gap"},
            {.field="memoryScope", .type=PropType::Enum, .label="Memory Scope"},
            {.field="resumeFromLast", .type=PropType::Bool, .label="Resume From Last"},
        };
        behavior.getItem = [](const ProjectItem* item) -> QVariant {
            const auto* selection = item ? item->comp<VSelection>() : nullptr;
            return selection ? ContainerBehaviorSemantics::toJson(selection->behavior).toVariantMap() : QVariant();
        };
        rows.push_back(std::move(behavior));
        return rows;
    }();
    return s;
}

#if defined(__clang__)
#  pragma clang diagnostic pop
#endif

}
