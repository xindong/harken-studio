#include "EffectInspectorSchema.h"

#include "Core/Components/VEffect.h"
#include "ProjectItemInspectorRows.h"

namespace VNX {

const PropertySchema& effectInspectorSchema()
{
    static const PropertySchema schema = [] {
        PropertyDescriptor type;
        type.field = QStringLiteral("type");
        type.ownerComponent = ComponentType::Effect;
        type.type = PropType::Enum;
        type.label = QStringLiteral("Type");
        type.section = PropertySection::Status;
        type.options = {{EffectChainSemantics::displayName(EffectType::Limiter),
                         EffectChainSemantics::typeName(EffectType::Limiter)}};
        type.flags.readOnly = true;
        type.flags.serialize = false;
        type.flags.settableByName = false;
        type.getItem = [](const ProjectItem* item) -> QVariant {
            const auto* effect = item ? item->comp<VEffect>() : nullptr;
            return effect ? QVariant(EffectChainSemantics::typeName(effect->type)) : QVariant();
        };
        return PropertySchema{type, notesInspectorDescriptor()};
    }();
    return schema;
}

}
