#include "ReferenceSlotResolution.h"

#include "Components/VMixNode.h"
#include "Components/VSpatial.h"
#include "Components/VTrack.h"
#include "ItemTypeCatalog.h"
#include "PlayablePropertyAuthoringV1.h"
#include "PlatformContextService.h"
#include "ProjectManager.h"

#include <QSet>

namespace VNX::ReferenceSlotResolution {
namespace {

std::shared_ptr<const FieldGroupEffectiveSnapshot> currentSnapshot(const ProjectItem* item)
{
    const auto* project = ProjectManager::instance().currentProject();
    if (!item || !project
        || !PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(item->type))
        return {};
    return std::make_shared<FieldGroupEffectiveSnapshot>(
        buildFieldGroupEffectiveSnapshot(*project,
            toFieldGroupScope(currentWorldPlatformScope(PlatformContextService::instance().context()))));
}

}

ResolutionScope buildResolutionScope(const ProjectItem* item)
{
    return buildResolutionScope(item, currentSnapshot(item));
}

const ProjectItem* resolveOutputBusSlotOwner(const ProjectItem* item)
{
    return resolveOutputBusSlotOwner(buildResolutionScope(item));
}

uint32_t resolveEffectiveOutputBusId(const ProjectItem* item)
{
    return resolveEffectiveOutputBusId(buildResolutionScope(item));
}

AttenuationSlot resolveEffectiveAttenuation(const ProjectItem* item)
{
    return resolveEffectiveAttenuation(buildResolutionScope(item));
}

SendSlot resolveEffectiveSends(const ProjectItem* item)
{
    return resolveEffectiveSends(buildResolutionScope(item));
}

std::vector<ResolvedParameterBinding> resolveParameterBindings(
    const ProjectItem* item, const BindingAddress& address)
{
    return resolveParameterBindings(buildResolutionScope(item), address);
}

ResolutionScope buildResolutionScope(const ProjectItem* item,
                                     const PlacementContext& placement,
                                     const PlacementItemResolver& resolveItem)
{
    return buildResolutionScope(item, placement, resolveItem, currentSnapshot(item));
}

}
