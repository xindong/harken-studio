
#include "ActionInspectorSchema.h"
#include "ProjectItemInspectorRows.h"
#include "Core/Components/VAction.h"

namespace VNX {

#if defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"
#  pragma clang diagnostic ignored "-Wmissing-field-initializers"
#endif

const PropertySchema& actionInspectorSchema()
{
    static const PropertySchema s = [] {
        PropertyDescriptor steps;
        steps.field = QStringLiteral("steps");
        steps.ownerComponent = ComponentType::Action;
        steps.type = PropType::List;
        steps.label = QStringLiteral("Steps");
        steps.section = PropertySection::Step;
        steps.flags.serialize = false;
        steps.elementSchema = {
            {.field="order", .type=PropType::Int, .label="#",
             .flags={.serialize=false, .settableByName=false, .state=true}},
            {.field="type", .type=PropType::Enum, .label="Type"},
            {.field="targetObjectID", .type=PropType::ObjectRef, .label="Target"},
            {.field="delaySec", .type=PropType::Float, .label="Delay", .unit=" s"},
            {.field="details", .type=PropType::Struct,
             .flags={.serialize=false, .settableByName=false, .state=true}},
        };
        steps.getItem = [](const ProjectItem* item) -> QVariant {
            const auto* action = item ? item->comp<VAction>() : nullptr;
            return action ? action->toJson().value(QStringLiteral("steps")).toVariant() : QVariant();
        };
        return PropertySchema{std::move(steps), notesInspectorDescriptor()};
    }();
    return s;
}

#if defined(__clang__)
#  pragma clang diagnostic pop
#endif

}
