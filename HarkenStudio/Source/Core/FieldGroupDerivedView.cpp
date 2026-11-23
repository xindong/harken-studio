#include "FieldGroupDerivedView.h"

#include "Tooling/Harkenizer/Core/CanonicalRuntimeProjectAdapterPrivate.h"
#include "FieldGroupProjectAdapter.h"
#include "Components/VMixNode.h"
#include "Components/VSpatial.h"

namespace harken::studio {

bool refreshFieldGroupDerivedViews(ProjectData& project)
{
    const auto source = harken::studio::bridge::canonicalRuntimeProjectSourceIndex(project);
    const auto view = buildFieldGroupProjectView(project, *source);
    if (!view.ok() || !source->valid || project.busses.empty()
        || !project.busses.front().rootItem)
        return false;
    const auto* master = project.busses.front().rootItem.get();
    harken::studio::project_schema::AuthoringGuid masterGuid{};
    for (const auto& [guid, item] : source->byGuid)
        if (item == master) masterGuid = guid;
    FieldGroupResolver resolver(view.definitions, masterGuid);
    struct Pending {
        ProjectItem* item;
        uint32_t busId;
        harken::studio::types::ObjectId busRef;
        float outputVolume;
        std::vector<Send> sends;
        SpatialRatingFieldGroup rating;
        harken::studio::types::ObjectId attenuationRef;
    };
    std::vector<Pending> pending;
    for (const auto& definition : view.definitions) {
        const auto output = resolver.Resolve(definition.guid, {}, FieldGroupId::Output);
        const auto send = resolver.Resolve(definition.guid, {}, FieldGroupId::Send);
        const auto spatial = resolver.Resolve(definition.guid, {}, FieldGroupId::SpatialRating);
        const auto effects = resolver.Resolve(definition.guid, {}, FieldGroupId::Effect);
        const auto conversion = resolver.Resolve(definition.guid, {}, FieldGroupId::Conversion);
        if (!output.ok() || !send.ok() || !spatial.ok() || !effects.ok() || !conversion.ok()) return false;
        const auto settingsRef = std::get<ConversionFieldGroup>(conversion.value->payload).settingsRef;
        if (!harken::studio::project_schema::IsNull(settingsRef)) {
            const auto settings = source->byGuid.find(settingsRef);
            if (settings == source->byGuid.end()
                || settings->second->type != ProjectItem::Type::Conversion)
                return false;
        }
        const auto& outputFields = std::get<OutputFieldGroup>(output.value->payload);
        const auto& rating = std::get<SpatialRatingFieldGroup>(spatial.value->payload);
        const auto bus = source->byGuid.find(outputFields.outputBusRef);
        if (bus == source->byGuid.end() || bus->second->type != ProjectItem::Type::Bus)
            return false;
        harken::studio::types::ObjectId attenuationRef{};
        if (!harken::studio::project_schema::IsNull(rating.spatialAttenuationRef)) {
            const auto attenuation = source->byGuid.find(rating.spatialAttenuationRef);
            if (attenuation == source->byGuid.end()
                || attenuation->second->type != ProjectItem::Type::Attenuation)
                return false;
            attenuationRef = attenuation->second->runtimeId();
        }
        Pending row{source->byGuid.at(definition.guid), bus->second->objectID,
                    bus->second->runtimeId(), outputFields.outputBusVolume, {}, rating,
                    attenuationRef};
        for (const auto& authored : std::get<SendFieldGroup>(send.value->payload).sends) {
            const auto target = source->byGuid.find(authored.targetBusID);
            if (target == source->byGuid.end() || target->second->type != ProjectItem::Type::Bus)
                return false;
            row.sends.push_back({target->second->objectID,
                                VMixNode::dbToLinear(authored.sendLevel), authored.type,
                                authored.duckTarget});
        }
        pending.push_back(std::move(row));
    }
    for (auto& row : pending) {
        if (auto* mix = row.item->comp<VMixNode>()) {
            mix->outputBusId = row.busId;
            mix->outputBusRef = row.busRef;
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
