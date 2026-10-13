#ifndef HARKEN_PROJECT_SCHEMA_RUNTIME_IDENTITY_H
#define HARKEN_PROJECT_SCHEMA_RUNTIME_IDENTITY_H

#include "harken_project_schema/Model.h"

#include <cstdint>
#include <tuple>

namespace harkenprojectschema {

enum class RuntimeIdentityKindV1 : std::uint32_t {
    Invalid = 0,
    Sound = 1,
    Container = 2,
    Track = 3,
    Piece = 4,
    Layer = 5,
    Bus = 6,
    Action = 7,
    Variable = 8,
    Attenuation = 9,
    BusEffect = 0x100,
    PlayableEffect = 0x101,
    Binding = 0x102,
    TimelineClip = 0x103,
    TimelineMarker = 0x104,
};

inline bool isObjectRuntimeIdentityKindV1(RuntimeIdentityKindV1 kind) noexcept
{
    const auto value = static_cast<std::uint32_t>(kind);
    return value >= static_cast<std::uint32_t>(RuntimeIdentityKindV1::Sound)
        && value <= static_cast<std::uint32_t>(RuntimeIdentityKindV1::Attenuation);
}

inline bool isNestedRuntimeIdentityKindV1(RuntimeIdentityKindV1 kind) noexcept
{
    const auto value = static_cast<std::uint32_t>(kind);
    return value >= static_cast<std::uint32_t>(RuntimeIdentityKindV1::BusEffect)
        && value <= static_cast<std::uint32_t>(RuntimeIdentityKindV1::TimelineMarker);
}

struct RuntimeIdentitySourceKeyV1 {
    AuthoringGuid owningGuid{};
    RuntimeIdentityKindV1 kind = RuntimeIdentityKindV1::Invalid;
    std::uint32_t reserved = 0;
    StableId stableId = kUnmintedStableId;

    friend bool operator==(const RuntimeIdentitySourceKeyV1&,
                           const RuntimeIdentitySourceKeyV1&) = default;
    friend bool operator<(const RuntimeIdentitySourceKeyV1& left,
                          const RuntimeIdentitySourceKeyV1& right) noexcept
    {
        return std::tie(left.owningGuid, left.kind, left.reserved, left.stableId)
             < std::tie(right.owningGuid, right.kind, right.reserved, right.stableId);
    }
};

struct RuntimeIdentityClaimV1 {
    StableId stableId = kUnmintedStableId;
    RuntimeIdentityKindV1 kind = RuntimeIdentityKindV1::Invalid;
    AuthoringGuid authoringGuid{};
    RuntimeIdentitySourceKeyV1 sourceKey{};
};

}

#endif
