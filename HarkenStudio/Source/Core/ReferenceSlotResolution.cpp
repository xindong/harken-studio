#include "ReferenceSlotResolution.h"

#include "Components/VMixNode.h"
#include "Components/VSpatial.h"
#include "Components/VTrack.h"
#include "ItemTypeCatalog.h"
#include "PlayablePropertyAuthoringV1.h"
#include "PlatformContextService.h"
#include "ProjectManager.h"

#include <QSet>

namespace VNX {
namespace ReferenceSlotResolution {
namespace {

QString viaText(ResolutionSource source, const ProjectItem* item)
{
    switch (source) {
    case ResolutionSource::Self:
        return QStringLiteral("via self");
    case ResolutionSource::Placement:
        return QStringLiteral("via placement %1").arg(item ? item->name : QString());
    case ResolutionSource::Organization:
        return QStringLiteral("via organization %1").arg(item ? item->name : QString());
    case ResolutionSource::None:
        return {};
    }
    return {};
}

class CandidateBuilder
{
public:
    void append(const ProjectItem* item, ResolutionSource source,
                std::vector<const ProjectItem*>* chain = nullptr)
    {
        if (!item || seen(item)) return;
        remember(item);
        if (chain) chain->push_back(item);
        candidates.push_back({item, source, viaText(source, item)});
    }

    std::vector<ResolutionCandidate> candidates;

private:
    bool seen(const ProjectItem* item) const
    {
        return item->guid.isNull() ? pointerIds_.contains(item)
                                   : guids_.contains(item->guid);
    }

    void remember(const ProjectItem* item)
    {
        if (item->guid.isNull()) pointerIds_.insert(item);
        else guids_.insert(item->guid);
    }

    QSet<QUuid> guids_;
    QSet<const ProjectItem*> pointerIds_;
};

struct PlacementMaterialization
{
    bool valid = true;
    QString error;
    std::vector<const ProjectItem*> items;
};

PlacementMaterialization materializePlacement(
    const ProjectItem* subject, const PlacementContext& placement,
    const PlacementItemResolver& resolveItem)
{
    PlacementMaterialization result;
    if (!subject) {
        result.valid = false;
        result.error = QStringLiteral("Placement context has no subject.");
        return result;
    }
    if (!resolveItem) {
        result.valid = false;
        result.error = QStringLiteral("Placement context has no item resolver.");
        return result;
    }

    QUuid expectedTarget = subject->guid;
    if (subject->type == ProjectItem::Type::Track && subject->parent
        && subject->parent->type == ProjectItem::Type::Segment) {
        result.items.push_back(subject->parent);
        expectedTarget = subject->parent->guid;
    }

    const auto& occurrences = placement.occurrences();
    for (size_t index = 0; index < occurrences.size(); ++index) {
        const PlacementOccurrence& occurrence = occurrences[index];
        if (occurrence.trackGuid.isNull() || occurrence.clipGuid.isNull()
            || occurrence.targetGuid.isNull()) {
            result.valid = false;
            result.error = QStringLiteral("Placement context contains an empty identity.");
            result.items.clear();
            return result;
        }
        const ProjectItem* track = resolveItem(occurrence.trackGuid);
        if (!track) {
            result.valid = false;
            result.error = QStringLiteral("Placement Track is unavailable.");
            result.items.clear();
            return result;
        }
        if (track->type != ProjectItem::Type::Track) {
            result.valid = false;
            result.error = QStringLiteral("Placement host is not a Track.");
            result.items.clear();
            return result;
        }
        const auto* timeline = track->comp<VTrack>();
        if (!timeline) {
            result.valid = false;
            result.error = QStringLiteral("Placement Track has no timeline.");
            result.items.clear();
            return result;
        }
        const Clip* clip = nullptr;
        for (const Clip& candidate : timeline->clips.items) {
            if (candidate.clipGuid == occurrence.clipGuid) {
                clip = &candidate;
                break;
            }
        }
        if (!clip) {
            result.valid = false;
            result.error = QStringLiteral("Placement Clip is unavailable.");
            result.items.clear();
            return result;
        }
        if (clip->targetGuid != occurrence.targetGuid
            || occurrence.targetGuid != expectedTarget) {
            result.valid = false;
            result.error = QStringLiteral("Placement Clip target no longer matches the context.");
            result.items.clear();
            return result;
        }

        result.items.push_back(track);
        const ProjectItem* segment = track->parent
            && track->parent->type == ProjectItem::Type::Segment
            ? track->parent : nullptr;
        if (segment) result.items.push_back(segment);

        if (index + 1 < occurrences.size()) {
            if (!segment) {
                result.valid = false;
                result.error = QStringLiteral("Placement drill path is disconnected.");
                result.items.clear();
                return result;
            }
            expectedTarget = segment->guid;
        }
    }
    return result;
}

ResolutionScope makeScope(const ProjectItem* item,
                          const PlacementMaterialization* placement)
{
    ResolutionScope scope;
    scope.subject = item;
    CandidateBuilder builder;
    builder.append(item, ResolutionSource::Self);

    if (placement) {
        scope.placementContextProvided = true;
        scope.placementContextValid = placement->valid;
        scope.placementContextError = placement->error;
        if (placement->valid) {
            for (const ProjectItem* candidate : placement->items)
                builder.append(candidate, ResolutionSource::Placement,
                               &scope.placementChain);
        }
    }

    QSet<const ProjectItem*> ancestors;
    for (const ProjectItem* current = item ? item->parent : nullptr;
         current; current = current->parent) {
        if (ancestors.contains(current)) break;
        ancestors.insert(current);
        if (ItemTypeCatalog::hasMixingCapability(current->type))
            builder.append(current, ResolutionSource::Organization,
                           &scope.organizationChain);
    }
    scope.candidates = std::move(builder.candidates);
    return scope;
}

template <typename HasOwnValue>
SlotResolution resolveSlot(const ResolutionScope& scope, HasOwnValue hasOwnValue)
{
    for (const ResolutionCandidate& candidate : scope.candidates) {
        if (candidate.item && hasOwnValue(*candidate.item))
            return {candidate.item, candidate.source, candidate.via};
    }
    return {};
}

const EffectiveDefinitionFieldGroups* effectiveDefinition(
    const ResolutionScope& scope, SlotResolution& result)
{
    if (!scope.effectiveGroups) {
        result.valid = false;
        result.error = QStringLiteral("Field group resolution requires a project");
        return nullptr;
    }
    const auto& snapshot = *scope.effectiveGroups;
    if (!snapshot.ok()) {
        result.valid = false;
        result.error = snapshot.diagnostics.front().message;
        return nullptr;
    }
    const auto guid = toFieldGroupGuid(scope.subject->guid);
    const auto object = snapshot.objects.find(guid);
    const auto definition = snapshot.definitions.find(guid);
    if (object == snapshot.objects.end() || object->second != scope.subject
        || definition == snapshot.definitions.end()) {
        result.valid = false;
        result.error = QStringLiteral("Field group definition does not belong to this project");
        return nullptr;
    }
    return &definition->second;
}

void applySource(const ResolutionScope& scope, const FieldGroupSource& source,
                 SlotResolution& result)
{
    result.authoringScope = source.scope;
    if (source.isDefault) return;
    result.owner = scope.effectiveGroups->objects.at(source.definition);
    result.source = result.owner == scope.subject
        ? ResolutionSource::Self : ResolutionSource::Organization;
    result.via = viaText(result.source, result.owner);
}

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

ResolutionScope buildResolutionScope(
    const ProjectItem* item,
    std::shared_ptr<const FieldGroupEffectiveSnapshot> effectiveGroups)
{
    auto scope = makeScope(item, nullptr);
    scope.effectiveGroups = std::move(effectiveGroups);
    return scope;
}

ResolutionScope buildResolutionScope(const ProjectData& project, const ProjectItem* item,
                                     FieldGroupScope platform)
{
    return buildResolutionScope(item, std::make_shared<FieldGroupEffectiveSnapshot>(
        buildFieldGroupEffectiveSnapshot(project, platform)));
}

ResolutionScope buildResolutionScope(const ProjectItem* item,
                                     const PlacementContext& placement,
                                     const PlacementItemResolver& resolveItem)
{
    const PlacementMaterialization materialized = materializePlacement(
        item, placement, resolveItem);
    auto scope = makeScope(item, &materialized);
    scope.effectiveGroups = currentSnapshot(item);
    return scope;
}

ResolutionScope buildResolutionScope(const ProjectData& project, const ProjectItem* item,
                                     const PlacementContext& placement,
                                     const PlacementItemResolver& resolveItem,
                                     FieldGroupScope platform)
{
    const auto materialized = materializePlacement(item, placement, resolveItem);
    auto scope = makeScope(item, &materialized);
    scope.effectiveGroups = std::make_shared<FieldGroupEffectiveSnapshot>(
        buildFieldGroupEffectiveSnapshot(project, platform));
    return scope;
}

QString resolutionSourceName(ResolutionSource source)
{
    switch (source) {
    case ResolutionSource::Self:
        return QStringLiteral("self");
    case ResolutionSource::Placement:
        return QStringLiteral("placement");
    case ResolutionSource::Organization:
        return QStringLiteral("organization");
    case ResolutionSource::None:
        return QStringLiteral("none");
    }
    return QStringLiteral("none");
}

SlotResolution resolveOutputBusSlot(const ResolutionScope& scope)
{
    if (scope.subject
        && PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(scope.subject->type)) {
        SlotResolution result;
        const auto* effective = effectiveDefinition(scope, result);
        if (!effective) return result;
        applySource(scope, effective->mainOutputSource, result);
        result.target = scope.effectiveGroups->objects.at(effective->mainOutput.mainOutputBusRef);
        return result;
    }
    return resolveSlot(scope, [](const ProjectItem& candidate) {
        const auto* mix = candidate.comp<VMixNode>();
        return mix && mix->outputBusId != 0;
    });
}

const ProjectItem* resolveOutputBusSlotOwner(const ResolutionScope& scope)
{
    return resolveOutputBusSlot(scope).owner;
}

const ProjectItem* resolveOutputBusSlotOwner(const ProjectItem* item)
{
    return resolveOutputBusSlotOwner(buildResolutionScope(item));
}

uint32_t resolveEffectiveOutputBusId(const ResolutionScope& scope)
{
    const auto resolved = resolveOutputBusSlot(scope);
    if (!resolved.valid) return 0;
    if (resolved.target) return resolved.target->objectID;
    const ProjectItem* owner = resolved.owner;
    return owner ? owner->comp<VMixNode>()->outputBusId : kMasterBusID;
}

uint32_t resolveEffectiveOutputBusId(const ProjectItem* item)
{
    return resolveEffectiveOutputBusId(buildResolutionScope(item));
}

AttenuationSlot resolveEffectiveAttenuation(const ResolutionScope& scope)
{
    if (scope.subject
        && PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(scope.subject->type)) {
        AttenuationSlot result;
        const auto* effective = effectiveDefinition(scope, result);
        if (!effective) return result;
        applySource(scope, effective->spatialRatingSource, result);
        if (!harkenprojectschema::IsNull(effective->spatialRating.spatialAttenuationRef)) {
            result.target = scope.effectiveGroups->objects.at(effective->spatialRating.spatialAttenuationRef);
            result.ref = result.target->runtimeId();
        }
        return result;
    }
    const SlotResolution resolved = resolveSlot(scope, [](const ProjectItem& candidate) {
        const auto* spatial = candidate.comp<VSpatial>();
        return spatial && candidate.playablePropertyPresence.Has(
            harkenplayableproperties::PlayablePropertyIdV1::SpatialAttenuationRef);
    });
    AttenuationSlot slot;
    if (!resolved.owner) return slot;
    const auto* spatial = resolved.owner->comp<VSpatial>();
    slot.owner = resolved.owner;
    slot.source = resolved.source;
    slot.via = resolved.via;
    slot.ref = spatial->attenuationRef;
    return slot;
}

AttenuationSlot resolveEffectiveAttenuation(const ProjectItem* item)
{
    return resolveEffectiveAttenuation(buildResolutionScope(item));
}

SendSlot resolveEffectiveSends(const ResolutionScope& scope)
{
    SendSlot result;
    if (scope.subject
        && PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(scope.subject->type)) {
        const auto* effective = effectiveDefinition(scope, result);
        if (!effective) return result;
        applySource(scope, effective->sendSource, result);
        for (const auto& row : effective->send.sends) {
            const auto* target = scope.effectiveGroups->objects.at(row.targetBusID);
            result.sends.push_back({target->objectID, VMixNode::dbToLinear(row.sendLevel)});
        }
    } else if (const auto* mix = scope.subject ? scope.subject->comp<VMixNode>() : nullptr) {
        result.owner = scope.subject;
        result.source = ResolutionSource::Self;
        result.via = viaText(result.source, result.owner);
        result.sends = mix->sends;
    }
    return result;
}

SendSlot resolveEffectiveSends(const ProjectItem* item)
{
    return resolveEffectiveSends(buildResolutionScope(item));
}

std::vector<ResolvedParameterBinding> resolveParameterBindings(
    const ResolutionScope& scope, const BindingAddress& address)
{
    std::vector<ResolvedParameterBinding> resolved;
    for (const ResolutionCandidate& candidate : scope.candidates) {
        const auto* mix = static_cast<const VMixNode*>(
            BindingContractSemantics::resolveHostComponent(
                candidate.item, address.hostClass));
        if (!mix) continue;
        const std::vector<const ParameterBindingDef*> matches =
            mix->bindings.findAll(address);
        if (matches.empty()) continue;
        for (const ParameterBindingDef* binding : matches)
            resolved.push_back(
                {candidate.item, binding, candidate.source, candidate.via});
    }
    return resolved;
}

std::vector<ResolvedParameterBinding> resolveParameterBindings(
    const ProjectItem* item, const BindingAddress& address)
{
    return resolveParameterBindings(buildResolutionScope(item), address);
}

}
}
