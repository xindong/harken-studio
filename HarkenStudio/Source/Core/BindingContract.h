
#pragma once

#include "ProjectTypes.h"
#include "PropertyDescriptor.h"

#include "harken_playable_properties.g.h"

#include <cstdint>
#include <optional>

namespace VNX {

class IComponent;

enum class BindingBindRole : uint8_t
{
    None = 0,
    LiveOffset = 1,
    LiveReplace = 2,
};

enum class BindingOutputUnit : uint8_t
{
    None = 0,
    Decibel = 1,
    Cent = 2,
    Normalized = 3,
    Percent = 4,
    Dimensionless = 5,
};

enum class BindingCombine : uint8_t
{
    Add = 0,
    AddClamp = 1,
    Replace = 2,
};

enum class BindingHostClass : uint8_t
{
    Voice = 0,
    Bus = 1,
    SendElement = 2,
    EffectParam = 3,
};

struct BindingOutputRange
{
    float min = 0.0f;
    float max = 0.0f;
};

struct BindingPath
{
    uint32_t index = 0;
};

struct BindingAddress
{
    BindingHostClass hostClass = BindingHostClass::Voice;
    uint8_t slotId = 0;
    std::optional<BindingPath> path;

    bool operator==(const BindingAddress& other) const
    {
        return hostClass == other.hostClass
            && slotId == other.slotId
            && path.has_value() == other.path.has_value()
            && (!path || path->index == other.path->index);
    }

    bool operator!=(const BindingAddress& other) const
    {
        return !(*this == other);
    }
};

struct BindingContract
{
    BindingBindRole bindRole = BindingBindRole::None;
    BindingOutputUnit outputUnit = BindingOutputUnit::None;
    BindingOutputRange defaultOutputRange;
    BindingOutputRange legalOutputRange;
    BindingCombine combine = BindingCombine::Add;
    BindingHostClass hostClass = BindingHostClass::Voice;
    std::optional<BindingPath> bindPath;
    bool runtimeUnimplemented = false;
    uint32_t defaultTransitionMs = 0;
};

namespace BindingContractSemantics {

const harkenplayableproperties::PlayablePropertyMetaV1*
    playableMeta(uint8_t slotId) noexcept;

std::optional<uint8_t> slotIdFromDescriptor(const PropertyDescriptor& descriptor);

BindingHostClass hostClassOf(ProjectItemType type) noexcept;

BindingContract derive(uint8_t slotId, ProjectItemType hostType);
BindingContract derive(const PropertyDescriptor& descriptor,
                       ProjectItemType hostType);

inline bool isBindable(const BindingContract& contract)
{
    return contract.bindRole != BindingBindRole::None;
}

inline bool previewAudible(const BindingContract& contract)
{
    return isBindable(contract) && contract.hostClass == BindingHostClass::Bus;
}

BindingAddress addressOf(uint8_t slotId, ProjectItemType hostType);
std::optional<BindingAddress> addressFromDescriptor(
    const PropertyDescriptor& descriptor, ProjectItemType hostType);

QString hostClassName(BindingHostClass hostClass);
std::optional<BindingHostClass> parseHostClass(const QString& name);

QString outputUnitName(BindingOutputUnit unit);

IComponent* resolveHostComponent(ProjectItem* item, BindingHostClass hostClass);
const IComponent* resolveHostComponent(const ProjectItem* item,
                                       BindingHostClass hostClass);

std::optional<uint32_t> engineTargetId(const BindingAddress& address);
uint32_t engineTargetScopeKind(BindingHostClass hostClass);

bool supportsTransition(uint32_t scopeKind, uint32_t targetId);
bool supportsTransition(const BindingAddress& address);
uint32_t defaultTransitionMs(const BindingAddress& address);

}

}
