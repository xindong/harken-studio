#include "FieldGroupDerivedView.h"

#include "Tooling/Harkenizer/Core/CanonicalRuntimeProjectAdapterPrivate.h"
#include "FieldGroupProjectAdapter.h"
#include "Components/VMixNode.h"
#include "Components/VSpatial.h"

namespace VNX {

bool refreshFieldGroupDerivedViews(ProjectData& project)
{
    const auto source = HarkenEditor::Bridge::canonicalRuntimeProjectSourceIndex(project);
    const auto view = buildFieldGroupProjectView(project, *source);
    if (!view.ok() || !source->valid || project.busses.empty()
        || !project.busses.front().rootItem)
        return false;
    const auto* master = project.busses.front().rootItem.get();
    harkenprojectschema::AuthoringGuid masterGuid{};
    for (const auto& [guid, item] : source->byGuid)
        if (item == master) masterGuid = guid;
    FieldGroupResolver resolver(view.definitions, masterGuid);
    struct Pending {
        ProjectItem* item;
        uint32_t busId;
        harkenstudio::ObjectId busRef;
        float outputVolume;
        std::vector<Send> sends;
        SpatialRatingFieldGroup rating;
        harkenstudio::ObjectId attenuationRef;
    };
    std::vector<Pending> pending;
    for (const auto& definition : view.definitions) {
        const auto output = resolver.Resolve(definition.guid, {}, FieldGroupId::MainOutput);
        const auto send = resolver.Resolve(definition.guid, {}, FieldGroupId::Send);
        const auto spatial = resolver.Resolve(definition.guid, {}, FieldGroupId::SpatialRating);
        if (!output.ok() || !send.ok() || !spatial.ok()) return false;
        const auto& main = std::get<MainOutputFieldGroup>(output.value->payload);
        const auto& rating = std::get<SpatialRatingFieldGroup>(spatial.value->payload);
        const auto bus = source->byGuid.find(main.mainOutputBusRef);
        if (bus == source->byGuid.end() || bus->second->type != ProjectItem::Type::Bus)
            return false;
        harkenstudio::ObjectId attenuationRef{};
        if (!harkenprojectschema::IsNull(rating.spatialAttenuationRef)) {
            const auto attenuation = source->byGuid.find(rating.spatialAttenuationRef);
            if (attenuation == source->byGuid.end()
                || attenuation->second->type != ProjectItem::Type::Attenuation)
                return false;
            attenuationRef = attenuation->second->runtimeId();
        }
        Pending row{source->byGuid.at(definition.guid), bus->second->objectID,
                    bus->second->runtimeId(), main.outputBusVolume, {}, rating,
                    attenuationRef};
        for (const auto& authored : std::get<SendFieldGroup>(send.value->payload).sends) {
            const auto target = source->byGuid.find(authored.targetBusID);
            if (target == source->byGuid.end() || target->second->type != ProjectItem::Type::Bus)
                return false;
            row.sends.push_back({target->second->objectID, VMixNode::dbToLinear(authored.sendLevel)});
        }
        pending.push_back(std::move(row));
    }
    for (auto& row : pending) {
        if (auto* mix = row.item->comp<VMixNode>()) {
            mix->outputBusId = row.busId;
            mix->mainOutputBusRef = row.busRef;
            mix->outputBusVolume = row.outputVolume;
            mix->sends = std::move(row.sends);
        }
        if (auto* spatial = row.item->comp<VSpatial>()) {
            spatial->panning = row.rating.spatialPanning;
            spatial->is3DEnabled = row.rating.spatialIs3DEnabled;
            spatial->attenuationDistanceScaling = row.rating.spatialAttenuationDistanceScaling;
            spatial->attenuationRef = row.attenuationRef;
        }
    }
    return true;
}

}
