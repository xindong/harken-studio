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
        std::vector<BusRoute> sends;
        std::vector<BusRoute> sidechains;
        SpatialRatingFieldGroup rating;
        harken::studio::types::ObjectId attenuationRef;
    };
    std::vector<Pending> pending;
    for (const auto& definition : view.definitions) {
        const auto output = resolver.Resolve(definition.guid, {}, FieldGroupId::Output);
        const auto send = resolver.Resolve(definition.guid, {}, FieldGroupId::Send);
        const auto sidechain = resolver.Resolve(definition.guid, {}, FieldGroupId::Sidechain);
        const auto spatial = resolver.Resolve(definition.guid, {}, FieldGroupId::SpatialRating);
        const auto effects = resolver.Resolve(definition.guid, {}, FieldGroupId::Effect);
        const auto conversion = resolver.Resolve(definition.guid, {}, FieldGroupId::Conversion);
        if (!output.ok() || !send.ok() || !sidechain.ok() || !spatial.ok() || !effects.ok()
            || !conversion.ok()) return false;
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
                    bus->second->runtimeId(), outputFields.outputBusVolume, {}, {}, rating,
                    attenuationRef};
        const auto busId = [&](harken::studio::project_schema::AuthoringGuid guid) -> std::optional<uint32_t> {
            const auto target = source->byGuid.find(guid);
            if (target == source->byGuid.end() || target->second->type != ProjectItem::Type::Bus) return std::nullopt;
            return target->second->objectID;
        };
        const auto derive = [&](const auto& rows, std::vector<BusRoute>& routes) {
            for (const auto& authored : rows) {
                const auto target = busId(authored.targetBusID);
                if (!target) return false;
                BusRoute route{fromFieldGroupGuid(authored.id), *target, authored.sendLevel, false, {}};
                if constexpr (requires { authored.duckTarget; }) route.duckTarget = authored.duckTarget;
                for (const auto& [platform, cells] : authored.platforms) {
                    BusRouteCells derived;
                    derived.sendLevelDb = cells.sendLevel;
                    if constexpr (requires { cells.duckTarget; }) derived.duckTarget = cells.duckTarget;
                    route.platforms.emplace(fromFieldGroupGuid(platform), derived);
                }
                routes.push_back(std::move(route));
            }
            return true;
        };
        if (!derive(std::get<SendFieldGroup>(send.value->payload).sends, row.sends)
            || !derive(std::get<SidechainFieldGroup>(sidechain.value->payload).sidechains, row.sidechains))
            return false;
        pending.push_back(std::move(row));
    }
    for (auto& row : pending) {
        if (auto* mix = row.item->comp<VMixNode>()) {
            mix->outputBusId = row.busId;
            mix->outputBusRef = row.busRef;
            mix->outputBusVolume = row.outputVolume;
            mix->sends = std::move(row.sends);
            mix->sidechains = std::move(row.sidechains);
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
