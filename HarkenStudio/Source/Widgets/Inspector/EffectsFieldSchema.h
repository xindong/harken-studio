#pragma once

#include "Core/Components/VMixNode.h"
#include "Core/ProjectManager.h"
#include "Core/PlatformContextService.h"
#include "FieldGroupInspectorView.h"
#include "InspectorLayout.h"

namespace VNX {

inline PropertyDescriptor effectsInspectorDescriptor()
{
    PropertyDescriptor effects;
    effects.field = QStringLiteral("effects");
    effects.ownerComponent = ComponentType::MixNode;
    effects.type = PropType::List;
    effects.label = QStringLiteral("FXs");
    effects.section = PropertySection::Effect;
    effects.flags.serialize = false;

    PropertyDescriptor effect;
    effect.field = QStringLiteral("effectGuid");
    effect.type = PropType::ObjectRef;
    effect.label = QStringLiteral("Effect");
    effect.refFilter = static_cast<int>(ProjectItem::Type::Effect);

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
        const auto& context = PlatformContextService::instance().context();
        auto chain = mix->effectsForPlatform(context.all ? std::nullopt : currentWorldPlatformScope(context));
        if (PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(item->type)) {
            const auto* project = ProjectManager::instance().currentProject();
            if (!project) return rows;
            const auto view = buildFieldGroupInspectorView(*project, item->guid,
                FieldGroupId::Effect, PlatformContextService::instance().context());
            const auto effective = view.effectChain();
            if (!effective) return rows;
            chain = *effective;
        }
        for (const auto& entry : chain.entries()) {
            QVariantMap row;
            if (entry.mode == EffectMode::Custom) {
                row.insert(QStringLiteral("effectGuid"), EffectChainSemantics::displayName(entry.custom.type));
            } else {
                const auto* target = ProjectManager::instance().findItemByGuid(entry.effectGuid);
                row.insert(QStringLiteral("effectGuid"), target && target->type == ProjectItem::Type::Effect
                    ? QVariant(target->objectID) : QVariant(entry.effectGuid.toString(QUuid::WithoutBraces)));
            }
            row.insert(QStringLiteral("wetDry"), entry.wetDry * 100.0);
            row.insert(QStringLiteral("active"), !entry.bypass);
            rows.push_back(row);
        }
        return rows;
    };
    return withInspectorRowOrder(std::move(effects));
}

}
