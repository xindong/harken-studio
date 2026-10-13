#pragma once

#include "BindingSemantics.h"
#include "PlacementContext.h"
#include "ProjectTypes.h"
#include "FieldGroupProjectAdapter.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace VNX {
namespace ReferenceSlotResolution {

using PlacementItemResolver = std::function<const ProjectItem*(const QUuid&)>;

enum class ResolutionSource : uint8_t
{
    None = 0,
    Self,
    Placement,
    Organization
};

struct ResolutionCandidate
{
    const ProjectItem* item = nullptr;
    ResolutionSource source = ResolutionSource::None;
    QString via;
};

struct ResolutionScope
{
    const ProjectItem* subject = nullptr;
    std::shared_ptr<const FieldGroupEffectiveSnapshot> effectiveGroups;
    std::vector<const ProjectItem*> placementChain;
    std::vector<const ProjectItem*> organizationChain;
    std::vector<ResolutionCandidate> candidates;
    bool placementContextProvided = false;
    bool placementContextValid = true;
    QString placementContextError;
};

ResolutionScope buildResolutionScope(const ProjectItem* item);
ResolutionScope buildResolutionScope(
    const ProjectItem* item,
    std::shared_ptr<const FieldGroupEffectiveSnapshot> effectiveGroups);
ResolutionScope buildResolutionScope(const ProjectData& project, const ProjectItem* item,
                                     FieldGroupScope platform = {});
ResolutionScope buildResolutionScope(const ProjectItem* item,
                                     const PlacementContext& placement,
                                     const PlacementItemResolver& resolveItem);
ResolutionScope buildResolutionScope(const ProjectData& project, const ProjectItem* item,
                                     const PlacementContext& placement,
                                     const PlacementItemResolver& resolveItem,
                                     FieldGroupScope platform = {});

QString resolutionSourceName(ResolutionSource source);

struct SlotResolution
{
    const ProjectItem* owner = nullptr;
    ResolutionSource source = ResolutionSource::None;
    QString via;
    FieldGroupScope authoringScope;
    const ProjectItem* target = nullptr;
    bool valid = true;
    QString error;
};

SlotResolution resolveOutputBusSlot(const ResolutionScope& scope);
const ProjectItem* resolveOutputBusSlotOwner(const ResolutionScope& scope);
const ProjectItem* resolveOutputBusSlotOwner(const ProjectItem* item);

uint32_t resolveEffectiveOutputBusId(const ResolutionScope& scope);
uint32_t resolveEffectiveOutputBusId(const ProjectItem* item);

struct AttenuationSlot : SlotResolution
{
    harkenstudio::ObjectId ref{};
};

AttenuationSlot resolveEffectiveAttenuation(const ResolutionScope& scope);
AttenuationSlot resolveEffectiveAttenuation(const ProjectItem* item);

struct SendSlot : SlotResolution
{
    std::vector<Send> sends;
};

SendSlot resolveEffectiveSends(const ResolutionScope& scope);
SendSlot resolveEffectiveSends(const ProjectItem* item);

struct ResolvedParameterBinding
{
    const ProjectItem* owner = nullptr;
    const ParameterBindingDef* binding = nullptr;
    ResolutionSource source = ResolutionSource::None;
    QString via;
};

std::vector<ResolvedParameterBinding> resolveParameterBindings(
    const ResolutionScope& scope, const BindingAddress& address);
std::vector<ResolvedParameterBinding> resolveParameterBindings(
    const ProjectItem* item, const BindingAddress& address);

}
}
