#pragma once

#include <harken_studio/AudioValues.h>
#include <cstdint>

namespace harkenstudiobuild {
enum class VariableKindV1 : std::uint8_t { Step = 0, Smooth = 1 };
enum class VariableScopeV1 : std::uint8_t { Global = 0, Emitter = 1 };

struct StaticImpactSummary {
    std::uint32_t propertiesPerConsumer = 0;
    std::uint32_t discreteSideReferences = 0;
    std::uint32_t continuousSideReferences = 0;
    std::uint32_t transactionsPerConsumer = 0;
    std::uint32_t eventsPerConsumer = 0;
    std::uint32_t participantsPerConsumer = 0;
    std::uint32_t targetRowsPerConsumer = 0;
    std::uint32_t referencedFlags = 0;
};

struct VariableDefinitionV1 {
    harkenstudio::ObjectId variableId{};
    VariableKindV1 kind = VariableKindV1::Smooth;
    float rangeMin = 0.0f;
    float rangeMax = 0.0f;
    float defaultValue = 0.0f;
    StaticImpactSummary impact{};
};
}
