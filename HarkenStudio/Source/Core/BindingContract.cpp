
#include "BindingContract.h"

#include "Components/VMixNode.h"
#include "PlayablePropertyAuthoringV1.h"

#include <harken_studio_build/BindingTargets.h>

#include <QLatin1String>
#include <QString>

namespace VNX {
namespace BindingContractSemantics {
namespace {

using harkenplayableproperties::PlayablePropertyCombineRoleV1;
using harkenplayableproperties::PlayablePropertyIdV1;
using harkenplayableproperties::PlayablePropertyMetaV1;
using harkenplayableproperties::PlayablePropertyPhaseRoleV1;
using harkenplayableproperties::PlayablePropertyWireV1;
using harkenplayableproperties::kPlayablePropertyRegistryV1;

struct ProductGate
{
    uint8_t slotId;
    bool runtimeUnimplemented;
};

constexpr ProductGate kProductGates[] = {
    {static_cast<uint8_t>(PlayablePropertyIdV1::MakeUpGain), false},
    {static_cast<uint8_t>(PlayablePropertyIdV1::OutputBusVolume), false},
    {static_cast<uint8_t>(PlayablePropertyIdV1::SpatialPanning), true},
    {static_cast<uint8_t>(PlayablePropertyIdV1::SpatialAttenuationDistanceScaling), true},
};

const ProductGate* productGateFor(uint8_t slotId) noexcept
{
    for (const ProductGate& gate : kProductGates)
        if (gate.slotId == slotId) return &gate;
    return nullptr;
}

BindingOutputRange defaultRangeFor(BindingOutputUnit unit)
{
    switch (unit) {
    case BindingOutputUnit::Decibel:      return {-96.0f, 24.0f};
    case BindingOutputUnit::Cent:          return {-4800.0f, 4800.0f};
    case BindingOutputUnit::Normalized:    return {0.0f, 1.0f};
    case BindingOutputUnit::Percent:       return {0.0f, 100.0f};
    case BindingOutputUnit::Dimensionless: return {0.0f, 1.0f};
    case BindingOutputUnit::None:          return {};
    }
    return {};
}

BindingOutputRange legalRangeFor(BindingOutputUnit unit)
{
    switch (unit) {
    case BindingOutputUnit::Decibel:      return {-96.0f, 24.0f};
    case BindingOutputUnit::Cent:          return {-4800.0f, 4800.0f};
    case BindingOutputUnit::Normalized:    return {0.0f, 1.0f};
    case BindingOutputUnit::Percent:       return {0.0f, 100.0f};
    case BindingOutputUnit::Dimensionless: return {0.0f, 1.0f};
    case BindingOutputUnit::None:          return {};
    }
    return {};
}

BindingContract gatedNone(BindingHostClass hostClass, bool runtimeUnimplemented)
{
    BindingContract contract;
    contract.hostClass = hostClass;
    contract.bindRole = BindingBindRole::None;
    contract.runtimeUnimplemented = runtimeUnimplemented;
    return contract;
}

} // namespace

const PlayablePropertyMetaV1* playableMeta(uint8_t slotId) noexcept
{
    if (!PlayablePropertyAuthoringV1::fieldById(slotId)) return nullptr;
    for (const PlayablePropertyMetaV1& meta : kPlayablePropertyRegistryV1)
        if (static_cast<uint8_t>(meta.id) == slotId) return &meta;
    return nullptr;
}

std::optional<uint8_t> slotIdFromDescriptor(const PropertyDescriptor& descriptor)
{
    const auto* field = PlayablePropertyAuthoringV1::fieldByName(descriptor.field);
    if (!field && !descriptor.jsonKey.isEmpty())
        field = PlayablePropertyAuthoringV1::fieldByJsonKey(descriptor.jsonKey);
    if (!field)
        field = PlayablePropertyAuthoringV1::fieldByName(descriptor.jsonKey);
    if (!field) return std::nullopt;
    return field->id;
}

BindingHostClass hostClassOf(ProjectItemType type) noexcept
{
    return type == ProjectItemType::Bus
        ? BindingHostClass::Bus
        : BindingHostClass::Voice;
}

BindingContract derive(uint8_t slotId, ProjectItemType hostType)
{
    const PlayablePropertyMetaV1* meta = playableMeta(slotId);
    if (!meta) return {};

    BindingContract contract;
    contract.hostClass = hostClassOf(hostType);

    const bool firstAudibleContinuous =
        meta->phaseRole == PlayablePropertyPhaseRoleV1::FirstAudible
        && meta->wire == PlayablePropertyWireV1::F32;
    if (!firstAudibleContinuous)
        return contract;

    const bool busOutputVolume =
        contract.hostClass == BindingHostClass::Bus
        && slotId == static_cast<uint8_t>(PlayablePropertyIdV1::OutputBusVolume);
    if (!busOutputVolume) {
        if (const ProductGate* gate = productGateFor(slotId))
            return gatedNone(contract.hostClass, gate->runtimeUnimplemented);
    }

    if (busOutputVolume) {
        contract.bindRole = BindingBindRole::LiveOffset;
        contract.outputUnit = BindingOutputUnit::Decibel;
        contract.combine = BindingCombine::Add;
        contract.defaultOutputRange = defaultRangeFor(contract.outputUnit);
        contract.legalOutputRange = legalRangeFor(contract.outputUnit);
    } else switch (meta->combineRole) {
    case PlayablePropertyCombineRoleV1::DbSum:
        contract.bindRole = BindingBindRole::LiveOffset;
        contract.outputUnit = BindingOutputUnit::Decibel;
        contract.combine = BindingCombine::Add;
        contract.defaultOutputRange = defaultRangeFor(contract.outputUnit);
        contract.legalOutputRange = legalRangeFor(contract.outputUnit);
        break;
    case PlayablePropertyCombineRoleV1::CentSumClamp:
        contract.bindRole = BindingBindRole::LiveOffset;
        contract.outputUnit = BindingOutputUnit::Cent;
        contract.combine = BindingCombine::AddClamp;
        contract.defaultOutputRange = defaultRangeFor(contract.outputUnit);
        contract.legalOutputRange = legalRangeFor(contract.outputUnit);
        break;
    default:
        return contract;
    }

    BindingAddress address;
    address.hostClass = contract.hostClass;
    address.slotId = slotId;
    address.path = contract.bindPath;
    if (!engineTargetId(address))
        return gatedNone(contract.hostClass, true);

    contract.defaultTransitionMs = defaultTransitionMs(address);
    return contract;
}

BindingContract derive(const PropertyDescriptor& descriptor,
                       ProjectItemType hostType)
{
    const auto slotId = slotIdFromDescriptor(descriptor);
    if (slotId) return derive(*slotId, hostType);

    BindingContract contract;
    contract.hostClass = hostClassOf(hostType);
    if (descriptor.field == QLatin1String("spatializationMix")) {
        contract.runtimeUnimplemented = true;
    }
    return contract;
}

BindingAddress addressOf(uint8_t slotId, ProjectItemType hostType)
{
    const BindingContract contract = derive(slotId, hostType);
    BindingAddress address;
    address.hostClass = contract.hostClass;
    address.slotId = slotId;
    address.path = contract.bindPath;
    return address;
}

std::optional<BindingAddress> addressFromDescriptor(
    const PropertyDescriptor& descriptor, ProjectItemType hostType)
{
    const auto slotId = slotIdFromDescriptor(descriptor);
    if (!slotId) return std::nullopt;
    return addressOf(*slotId, hostType);
}

QString hostClassName(BindingHostClass hostClass)
{
    switch (hostClass) {
    case BindingHostClass::Voice:       return QStringLiteral("voice");
    case BindingHostClass::Bus:         return QStringLiteral("bus");
    case BindingHostClass::SendElement: return QStringLiteral("send");
    case BindingHostClass::EffectParam: return QStringLiteral("effect");
    }
    return {};
}

std::optional<BindingHostClass> parseHostClass(const QString& name)
{
    if (name == QLatin1String("voice")) return BindingHostClass::Voice;
    if (name == QLatin1String("bus")) return BindingHostClass::Bus;
    if (name == QLatin1String("send")) return BindingHostClass::SendElement;
    if (name == QLatin1String("effect")) return BindingHostClass::EffectParam;
    return std::nullopt;
}

QString outputUnitName(BindingOutputUnit unit)
{
    switch (unit) {
    case BindingOutputUnit::Decibel:      return QStringLiteral("dB");
    case BindingOutputUnit::Cent:          return QStringLiteral("cent");
    case BindingOutputUnit::Normalized:    return QStringLiteral("normalized");
    case BindingOutputUnit::Percent:       return QStringLiteral("%");
    case BindingOutputUnit::Dimensionless: return {};
    case BindingOutputUnit::None:          return {};
    }
    return {};
}

IComponent* resolveHostComponent(ProjectItem* item, BindingHostClass hostClass)
{
    if (!item) return nullptr;
    switch (hostClass) {
    case BindingHostClass::Voice:
    case BindingHostClass::Bus:
        return item->comp<VMixNode>();
    case BindingHostClass::SendElement:
    case BindingHostClass::EffectParam:
        return nullptr;
    }
    return nullptr;
}

const IComponent* resolveHostComponent(const ProjectItem* item,
                                       BindingHostClass hostClass)
{
    if (!item) return nullptr;
    switch (hostClass) {
    case BindingHostClass::Voice:
    case BindingHostClass::Bus:
        return item->comp<VMixNode>();
    case BindingHostClass::SendElement:
    case BindingHostClass::EffectParam:
        return nullptr;
    }
    return nullptr;
}

std::optional<uint32_t> engineTargetId(const BindingAddress& address)
{
    if (address.path.has_value()) return std::nullopt;
    switch (address.hostClass) {
    case BindingHostClass::Voice:
        if (address.slotId == static_cast<uint8_t>(PlayablePropertyIdV1::Volume))
            return harkenstudiobuild::kTargetVoiceVolume;
        if (address.slotId == static_cast<uint8_t>(PlayablePropertyIdV1::Pitch))
            return harkenstudiobuild::kTargetVoicePitch;
        return std::nullopt;
    case BindingHostClass::Bus:
        if (address.slotId == static_cast<uint8_t>(PlayablePropertyIdV1::Volume))
            return harkenstudiobuild::kBusTargetBusVolume;
        if (address.slotId == static_cast<uint8_t>(PlayablePropertyIdV1::OutputBusVolume))
            return harkenstudiobuild::kBusTargetBusOutputVolume;
        return std::nullopt;
    case BindingHostClass::SendElement:
    case BindingHostClass::EffectParam:
        return std::nullopt;
    }
    return std::nullopt;
}

uint32_t engineTargetScopeKind(BindingHostClass hostClass)
{
    return hostClass == BindingHostClass::Bus
        ? harkenstudiobuild::kTargetScopeBus
        : harkenstudiobuild::kTargetScopeVoice;
}

bool supportsTransition(uint32_t scopeKind, uint32_t targetId)
{
    const auto* meta = harkenstudiobuild::FindTargetMeta(scopeKind, targetId);
    return meta && meta->supportsTransition;
}

bool supportsTransition(const BindingAddress& address)
{
    const auto targetId = engineTargetId(address);
    if (!targetId) return false;
    return supportsTransition(engineTargetScopeKind(address.hostClass), *targetId);
}

uint32_t defaultTransitionMs(const BindingAddress& address)
{
    const auto target = engineTargetId(address);
    if (!target) return 0;
    const auto scope = engineTargetScopeKind(address.hostClass);
    return ((scope == harkenstudiobuild::kTargetScopeVoice
             && (*target == harkenstudiobuild::kTargetVoiceVolume || *target == harkenstudiobuild::kTargetVoicePitch))
            || (scope == harkenstudiobuild::kTargetScopeBus
                && (*target == harkenstudiobuild::kBusTargetBusVolume
                    || *target == harkenstudiobuild::kBusTargetBusOutputVolume)))
        ? 20u : 0u;
}

}
}
