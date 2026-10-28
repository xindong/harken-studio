
#include "ComponentCatalog.h"

namespace VNX {
namespace ComponentCatalog {

namespace {

// Single word table: the serialization face and the IPC tag face share it.
constexpr ComponentFacts kComponentFacts[] = {
    { ComponentType::MixNode,      "mixnode" },
    { ComponentType::Audio,        "audio" },
    { ComponentType::Selection,    "selection" },
    { ComponentType::Bus,          "bus" },
    { ComponentType::Action,       "action" },
    { ComponentType::Variable,     "variable" },
    { ComponentType::Attenuation,  "attenuation" },
    { ComponentType::Spatial,      "spatial" },
    { ComponentType::Track,        "tracktimeline" },
    { ComponentType::Segment,        "segmenttimeline" },
    { ComponentType::Sound,        "sound" },
};

constexpr char kUnknownName[] = "unknown";

}

const ComponentFacts* factsOrNull(ComponentType type)
{
    for (const ComponentFacts& facts : kComponentFacts) {
        if (facts.type == type) return &facts;
    }
    return nullptr;
}

QString componentName(ComponentType type)
{
    const ComponentFacts* facts = factsOrNull(type);
    return QString::fromLatin1(facts ? facts->name : kUnknownName);
}

std::optional<ComponentType> parseComponentName(const QString& name)
{
    for (const ComponentFacts& facts : kComponentFacts) {
        if (name.compare(QLatin1String(facts.name), Qt::CaseSensitive) == 0)
            return facts.type;
    }
    return std::nullopt;
}

const std::array<ComponentType, 11>& allComponentTypes()
{
    static const std::array<ComponentType, 11> types = {
        ComponentType::MixNode, ComponentType::Audio, ComponentType::Selection,
        ComponentType::Bus, ComponentType::Action, ComponentType::Variable,
        ComponentType::Attenuation, ComponentType::Spatial, ComponentType::Track,
        ComponentType::Segment, ComponentType::Sound,
    };
    return types;
}

}
}
