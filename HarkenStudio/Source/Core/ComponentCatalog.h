
#pragma once

#include "Component.h"

#include <QString>

#include <array>
#include <optional>

namespace VNX {
namespace ComponentCatalog {

struct ComponentFacts {
    ComponentType type;
    const char* name;
};

const ComponentFacts* factsOrNull(ComponentType type);
QString componentName(ComponentType type);
std::optional<ComponentType> parseComponentName(const QString& name);
const std::array<ComponentType, 11>& allComponentTypes();

}
}
