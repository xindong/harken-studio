#include "Core/AttenuationStore.h"
#include "Core/Components/VAttenuation.h"
#include "Core/ProjectTypes.h"

namespace VNX {

std::vector<const VAttenuation*> AttenuationStore::all(const ProjectData& project)
{
    std::vector<const VAttenuation*> out;
    out.reserve(project.attenuationItems.size());
    for (const auto& binding : project.attenuationItems)
    {
        const ProjectItem* item = binding.rootItem.get();
        if (!item || item->type != ProjectItem::Type::Attenuation)
            continue;
        if (const auto* comp = item->comp<VAttenuation>())
            out.push_back(comp);
    }
    return out;
}

harkenstudio::ObjectId AttenuationStore::runtimeId(const ProjectData& project, const VAttenuation& attenuation)
{
    for (const auto& binding : project.attenuationItems)
    {
        const auto* item = binding.rootItem.get();
        if (item && item->type == ProjectItem::Type::Attenuation
            && item == attenuation.hostItem())
            return item->runtimeId();
    }
    return {};
}

}
