#pragma once

#include "Core/Components/VMixNode.h"
#include "InspectorLayout.h"

namespace VNX {

inline PropertyDescriptor effectsInspectorDescriptor()
{
    PropertyDescriptor effects;
    effects.field = QStringLiteral("effects");
    effects.ownerComponent = ComponentType::MixNode;
    effects.type = PropType::List;
    effects.label = QStringLiteral("Effects");
    effects.section = PropertySection::Effect;
    effects.flags.serialize = false;

    PropertyDescriptor effect;
    effect.field = QStringLiteral("type");
    effect.type = PropType::Enum;
    effect.label = QStringLiteral("Effect");
    for (EffectType type : {EffectType::FeedbackDelay, EffectType::ThreeBandTone,
                           EffectType::Gain, EffectType::Limiter, EffectType::Compressor})
        effect.options.push_back({EffectChainSemantics::displayName(type),
                                  EffectChainSemantics::typeName(type)});

    PropertyDescriptor level;
    level.field = QStringLiteral("wetDry");
    level.type = PropType::Float;
    level.label = QStringLiteral("Level");
    level.unit = QStringLiteral(" %");
    level.min = 0.0;
    level.max = 100.0;
    level.def = 100.0;
    level.decimals = 1;
    level.flags.serialize = false;
    level.flags.settableByName = false;
    level.toolTip = QStringLiteral("Dry/wet mix. Stored in Studio; not applied to audio processing.");

    PropertyDescriptor active;
    active.field = QStringLiteral("active");
    active.type = PropType::Bool;
    active.label = QStringLiteral("Active");
    active.flags.serialize = false;
    active.flags.settableByName = false;
    active.flags.state = true;
    effects.elementSchema = {effect, level, active};
    effects.getItem = [](const ProjectItem* item) -> QVariant {
        const auto* mix = item ? item->comp<VMixNode>() : nullptr;
        QVariantList rows;
        if (!mix) return rows;
        for (const auto& entry : mix->effects.entries()) {
            QVariantMap row;
            if (entry.typed) {
                row.insert(QStringLiteral("type"), EffectChainSemantics::typeName(entry.type));
                row.insert(QStringLiteral("wetDry"), entry.wetDry * 100.0);
                row.insert(QStringLiteral("active"), !entry.bypass);
            }
            rows.push_back(row);
        }
        return rows;
    };
    return withInspectorRowOrder(std::move(effects));
}

}
