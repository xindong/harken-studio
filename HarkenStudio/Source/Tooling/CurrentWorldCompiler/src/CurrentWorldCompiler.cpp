#include "harken_current_world_compiler/CurrentWorldCompiler.h"
#include "harken_studio_build/BindingMaterialization.h"
#include "harken_studio_build/EffectEncoding.h"

#include <harken_studio_build/WorldValidation.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <tuple>

namespace harkencurrentworldcompiler {

static_assert(harkenprojectschema::kTargetCapabilityVariableDefinitionsV1
              == harkenstudiobuild::kWorldCapabilityVariableDefinitions);
static_assert(harkenprojectschema::kTargetCapabilityPlayablePropertiesV1
              == harkenstudiobuild::kWorldCapabilityPlayableProperties);
static_assert(harkenprojectschema::kTargetCapabilityRandomDrawV1
              == harkenstudiobuild::kWorldCapabilityRandomDraw);
static_assert(harkenprojectschema::kTargetCapabilityRandomRotationV1
              == harkenstudiobuild::kWorldCapabilityRandomRotation);
static_assert(harkenprojectschema::kTargetCapabilitySequenceV1
              == harkenstudiobuild::kWorldCapabilitySequence);
static_assert(harkenprojectschema::kTargetCapabilityRandomQueueV1
              == harkenstudiobuild::kWorldCapabilityRandomQueue);
static_assert(harkenprojectschema::kTargetCapabilitySequenceQueueV1
              == harkenstudiobuild::kWorldCapabilitySequenceQueue);
static_assert(harkenprojectschema::kTargetCapabilityVariableControlV1
              == harkenstudiobuild::kWorldCapabilityVariableControl);
static_assert(harkenprojectschema::kTargetCapabilityTransitionV1
              == harkenstudiobuild::kWorldCapabilityTransition);
static_assert(harkenprojectschema::kTargetCapabilityActionDefinitionsV1
              == harkenstudiobuild::kWorldCapabilityActionDefinitions);
namespace {

using PropertyId = harkenplayableproperties::PlayablePropertyIdV1;

std::size_t Slot(PropertyId id) noexcept {
    return static_cast<std::size_t>(id) - 1u;
}

float CanonicalF32(float value) noexcept {
    return value == 0.0f ? 0.0f : value;
}

bool CanonicalActionTime(float value, float maximum, double scale,
                         harkenstudiobuild::AuthoredTimeRationalV1* out) noexcept {
    if (out == nullptr || !std::isfinite(value) || value < 0.0f ||
        value > maximum) {
        return false;
    }
    const double nanoseconds = static_cast<double>(value) * scale;
    const double rounded = std::floor(nanoseconds + 0.5);
    if (!std::isfinite(rounded) ||
        rounded > static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
        return false;
    }
    out->domain = harkenstudiobuild::AuthoredTimeDomainV1::AbsoluteNanoseconds;
    out->numerator = static_cast<std::int64_t>(rounded);
    out->denominator = 1;
    return true;
}

std::uint32_t CanonicalF32Bits(std::uint32_t bits) noexcept {
    return bits == UINT32_C(0x80000000) ? 0u : bits;
}

harkenstudiobuild::playableproperties::ContentPropertyKindV1 RuntimeKind(
    harkenprojectschema::ContentKind kind) noexcept {
    return static_cast<harkenstudiobuild::playableproperties::ContentPropertyKindV1>(
        static_cast<std::uint32_t>(kind));
}

const harkenprojectschema::ContentObject* FindObject(
    const harkenprojectschema::CanonicalProject& project,
    harkenstudio::ObjectId stableId) noexcept {
    const auto found = std::lower_bound(
        project.objects.begin(), project.objects.end(), stableId,
        [](const harkenprojectschema::ContentObject& object,
           harkenstudio::ObjectId id) { return object.chassis().id < id; });
    return found != project.objects.end() && found->chassis().id == stableId
               ? &*found
               : nullptr;
}

const DefinitionMaterializationV1* FindMaterialization(
    const std::vector<DefinitionMaterializationV1>& rows,
    harkenplayableproperties::ContentPropertyKindV1 kind,
    harkenstudio::ObjectId stableId) noexcept {
    const auto found = std::find_if(
        rows.begin(), rows.end(), [&](const DefinitionMaterializationV1& row) {
            return row.kind == kind && row.stableId == stableId;
        });
    return found == rows.end() ? nullptr : &*found;
}


harkenstudiobuild::PlayableRefV1 MakeRef(
    const harkenstudiobuild::PlayablePropertyProgramViewV1& property) noexcept {
    harkenstudiobuild::PlayableRefV1 ref{};
    ref.kind = static_cast<harkenstudiobuild::PlayableKindV1>(
        static_cast<std::uint32_t>(property.contentPropertyKind) - 1u);
    ref.stableId = property.stableId;
    ref.expectedProgramSemanticFingerprint =
        property.programSemanticFingerprint;
    ref.contentRevision = property.contentRevision;
    ref.routeStableId = property.routeStableId;
    ref.compatibilityFingerprint = property.compatibilityFingerprint;
    return ref;
}

} // namespace

void OwningCurrentWorldV1::OwnedPayload::Refresh() noexcept {
    view.sendCount = static_cast<std::uint32_t>(sends.size());
    view.sends = sends.empty() ? nullptr : sends.data();
    view.effectNodeCount = static_cast<std::uint32_t>(effectNodes.size());
    view.effectNodes = effectNodes.empty() ? nullptr : effectNodes.data();
    for (std::size_t i = 0; i < effectNodes.size(); ++i) {
        effectNodes[i].paramCount =
            static_cast<std::uint32_t>(effectParams[i].size());
        effectNodes[i].params =
            effectParams[i].empty() ? nullptr : effectParams[i].data();
    }
    view.effectEdgeCount = static_cast<std::uint32_t>(effectEdges.size());
    view.effectEdges = effectEdges.empty() ? nullptr : effectEdges.data();
}

void OwningCurrentWorldV1::OwnedProperty::Refresh() noexcept {
    payload.Refresh();
    view.organizationLayerAncestorCount =
        static_cast<std::uint32_t>(ancestors.size());
    view.organizationLayerAncestors =
        ancestors.empty() ? nullptr : ancestors.data();
    view.structuralAncestorCount =
        static_cast<std::uint32_t>(structuralAncestors.size());
    view.structuralAncestors = structuralAncestors.empty()
        ? nullptr : structuralAncestors.data();
    view.payload = payload.view;
}

void OwningCurrentWorldV1::OwnedOccurrence::Refresh() noexcept {
    payload.Refresh();
    view.payload = payload.view;
}

void OwningCurrentWorldV1::OwnedContainer::Refresh() noexcept {
    view.curveCount = static_cast<std::uint32_t>(curves.size());
    view.curves = curves.empty() ? nullptr : curves.data();
    view.childCount = static_cast<std::uint32_t>(children.size());
    view.children = children.empty() ? nullptr : children.data();
    view.discreteAxisCount = static_cast<std::uint32_t>(discreteAxes.size());
    view.discreteAxes = discreteAxes.empty() ? nullptr : discreteAxes.data();
    view.discreteRowCount = static_cast<std::uint32_t>(discreteRows.size());
    view.discreteRows = discreteRows.empty() ? nullptr : discreteRows.data();
    for (std::size_t i = 0; i < discreteRows.size(); ++i) {
        discreteRows[i].cellCount =
            static_cast<std::uint32_t>(discreteCells[i].size());
        discreteRows[i].cells =
            discreteCells[i].empty() ? nullptr : discreteCells[i].data();
    }
    view.continuousAxisCount =
        static_cast<std::uint32_t>(continuousAxes.size());
    view.continuousAxes =
        continuousAxes.empty() ? nullptr : continuousAxes.data();
    view.continuousRowCount =
        static_cast<std::uint32_t>(continuousRows.size());
    view.continuousRows =
        continuousRows.empty() ? nullptr : continuousRows.data();
    for (std::size_t i = 0; i < continuousRows.size(); ++i) {
        continuousRows[i].cellCount =
            static_cast<std::uint32_t>(continuousCells[i].size());
        continuousRows[i].cells = continuousCells[i].empty()
            ? nullptr : continuousCells[i].data();
    }
}

void OwningCurrentWorldV1::OwnedTrackSchedule::Refresh() noexcept {
    view.placementCount = static_cast<std::uint32_t>(placements.size());
    view.placements = placements.empty() ? nullptr : placements.data();
}

void OwningCurrentWorldV1::OwnedPieceSchedule::Refresh() noexcept {
    view.trackRefCount = static_cast<std::uint32_t>(trackRefs.size());
    view.trackRefs = trackRefs.empty() ? nullptr : trackRefs.data();
    view.markerCount = static_cast<std::uint32_t>(markers.size());
    view.markers = markers.empty() ? nullptr : markers.data();
}

void OwningCurrentWorldV1::OwnedTransition::Refresh() noexcept {
    view.downwardPathStepCount =
        static_cast<std::uint32_t>(downwardPathSteps.size());
    view.downwardPathSteps = downwardPathSteps.empty()
        ? nullptr : downwardPathSteps.data();
    view.ruleCount = static_cast<std::uint32_t>(rules.size());
    view.rules = rules.empty() ? nullptr : rules.data();
}

void OwningCurrentWorldV1::OwnedAction::Refresh() noexcept {
    view.stepCount = static_cast<std::uint32_t>(steps.size());
    view.steps = steps.empty() ? nullptr : steps.data();
}

void OwningCurrentWorldV1::Refresh() noexcept {
    propertyViews_.clear();
    propertyViews_.reserve(properties_.size());
    for (auto& row : properties_) {
        row.Refresh();
        propertyViews_.push_back(row.view);
    }
    occurrenceViews_.clear();
    occurrenceViews_.reserve(occurrences_.size());
    for (auto& row : occurrences_) {
        row.Refresh();
        occurrenceViews_.push_back(row.view);
    }
    trackScheduleViews_.clear();
    trackScheduleViews_.reserve(trackSchedules_.size());
    for (auto& row : trackSchedules_) {
        row.Refresh();
        trackScheduleViews_.push_back(row.view);
    }
    pieceScheduleViews_.clear();
    pieceScheduleViews_.reserve(pieceSchedules_.size());
    for (auto& row : pieceSchedules_) {
        row.Refresh();
        pieceScheduleViews_.push_back(row.view);
    }
    containerViews_.clear();
    containerViews_.reserve(containers_.size());
    for (auto& row : containers_) {
        row.Refresh();
        containerViews_.push_back(row.view);
    }
    transitionViews_.clear();
    transitionViews_.reserve(transitions_.size());
    for (auto& row : transitions_) {
        row.Refresh();
        transitionViews_.push_back(row.view);
    }
    actionViews_.clear();
    actionViews_.reserve(actions_.size());
    for (auto& row : actions_) {
        row.Refresh();
        actionViews_.push_back(row.view);
    }
    view_.variables = variables_.empty() ? nullptr : variables_.data();
    view_.variableCount = static_cast<std::uint32_t>(variables_.size());
    view_.playableProperties =
        propertyViews_.empty() ? nullptr : propertyViews_.data();
    view_.playablePropertyCount = static_cast<std::uint32_t>(properties_.size());
    view_.playableOccurrences =
        occurrenceViews_.empty() ? nullptr : occurrenceViews_.data();
    view_.playableOccurrenceCount =
        static_cast<std::uint32_t>(occurrences_.size());
    view_.trackSchedules =
        trackScheduleViews_.empty() ? nullptr : trackScheduleViews_.data();
    view_.trackScheduleCount =
        static_cast<std::uint32_t>(trackSchedules_.size());
    view_.pieceSchedules =
        pieceScheduleViews_.empty() ? nullptr : pieceScheduleViews_.data();
    view_.pieceScheduleCount =
        static_cast<std::uint32_t>(pieceSchedules_.size());
    view_.containers =
        containerViews_.empty() ? nullptr : containerViews_.data();
    view_.containerCount = static_cast<std::uint32_t>(containers_.size());
    view_.transitions =
        transitionViews_.empty() ? nullptr : transitionViews_.data();
    view_.transitionCount =
        static_cast<std::uint32_t>(transitionViews_.size());
    view_.actions = actionViews_.empty() ? nullptr : actionViews_.data();
    view_.actionCount = static_cast<std::uint32_t>(actionViews_.size());
}

CompileResultV1 CompileCurrentWorldV1(
    const harkenprojectschema::CanonicalProject& project,
    const std::vector<harkenstudiobuild::VariableDefinitionV1>& variables,
    const std::vector<DefinitionMaterializationV1>& materializations,
    const std::vector<ContainerAuthoringV1>& authoredContainers,
    const harkenstudiobuild::WorldCapacitySummaryV1& capacity,
    const std::vector<TransitionProgramAuthoringV1>& authoredTransitions,
    const std::vector<ActionProgramAuthoringV1>& authoredActions,
    const harkenbuscompiler::CompiledBusIdSetV1* busIds) {
    CompileResultV1 result{};
    if (busIds != nullptr
        && (busIds->ids.empty()
            || harkenstudio::IsNull(busIds->ids.front())
            || !std::is_sorted(busIds->ids.begin(), busIds->ids.end())
            || std::adjacent_find(busIds->ids.begin(), busIds->ids.end())
                != busIds->ids.end())) {
        result.status = CompileStatusV1::InvalidProject;
        return result;
    }
    auto world = std::make_unique<OwningCurrentWorldV1>();
    world->view_.identity.projectId = project.projectId;
    world->view_.capacity = capacity;
    world->variables_ = variables;
    for (auto& variable : world->variables_) {
        variable.rangeMin = CanonicalF32(variable.rangeMin);
        variable.rangeMax = CanonicalF32(variable.rangeMax);
        variable.defaultValue = CanonicalF32(variable.defaultValue);
    }

    std::set<std::pair<std::uint32_t, harkenstudio::ObjectId>> materializationKeys;
    for (const auto& row : materializations) {
        const auto numericKind = static_cast<std::uint32_t>(row.kind);
        const auto* object = FindObject(project, row.stableId);
        if (row.stableId == harkenstudio::ObjectId{} ||
            numericKind < static_cast<std::uint32_t>(
                              harkenplayableproperties::ContentPropertyKindV1::Sound) ||
            numericKind > static_cast<std::uint32_t>(
                              harkenplayableproperties::ContentPropertyKindV1::Piece) ||
            object == nullptr ||
            static_cast<std::uint32_t>(object->kind) != numericKind ||
            !materializationKeys.emplace(numericKind, row.stableId).second) {
            result.status = CompileStatusV1::InvalidMaterialization;
            result.relatedStableId = row.stableId;
            return result;
        }
    }

    std::map<harkenstudio::ObjectId, harkenstudio::ObjectId> parentByChild;
    for (const auto& object : project.objects) {
        std::vector<harkenstudio::ObjectId> children;
        switch (object.kind) {
            case harkenprojectschema::ContentKind::Layer:
                children = object.layer.ownedChildren;
                break;
            case harkenprojectschema::ContentKind::Container:
                children = object.container.ownedChildren;
                break;
            case harkenprojectschema::ContentKind::Track:
                children = object.track.ownedChildren;
                break;
            case harkenprojectschema::ContentKind::Piece:
                children = object.piece.tracks;
                break;
            case harkenprojectschema::ContentKind::Sound:
                break;
        }
        for (const harkenstudio::ObjectId child : children) {
            if (child == harkenstudio::ObjectId{} || parentByChild.contains(child)) {
                result.status = CompileStatusV1::InvalidProject;
                result.relatedStableId = child;
                return result;
            }
            parentByChild.emplace(child, object.chassis().id);
        }
    }

    const auto buildPayload = [&](const harkenprojectschema::PlayablePropertyPayloadV1& source,
                                  OwningCurrentWorldV1::OwnedPayload& output) {
        output.view.presentMask = source.presence.bits;
        output.view.values[Slot(PropertyId::LoopCount)].bits = 1;
        output.view.values[Slot(PropertyId::SpatialAttenuationDistanceScaling)]
            .bits = std::bit_cast<std::uint32_t>(1.0f);
        output.view.values[Slot(PropertyId::SpatializationMix)]
            .bits = std::bit_cast<std::uint32_t>(1.0f);
        const auto present = [&](PropertyId id) { return source.presence.Has(id); };
        const auto setF32 = [&](PropertyId id, float value) {
            if (present(id)) {
                output.view.values[Slot(id)].bits =
                    std::bit_cast<std::uint32_t>(CanonicalF32(value));
            }
        };
        const auto setU32 = [&](PropertyId id, std::uint32_t value) {
            if (present(id)) output.view.values[Slot(id)].bits = value;
        };
        const auto setObjectId = [&](PropertyId id, harkenstudio::ObjectId value) {
            if (present(id)) output.view.values[Slot(id)].objectId = value;
        };
        setF32(PropertyId::Volume, source.volume);
        setF32(PropertyId::Pitch, source.pitch);
        setF32(PropertyId::MakeUpGain, source.makeUpGain);
        setU32(PropertyId::LoopCount, source.loopCount);
        setU32(PropertyId::StreamingMode, source.streamingMode);
        setF32(PropertyId::OutputBusVolume, source.outputBusVolume);
        setObjectId(PropertyId::MainOutputBusRef, source.mainOutputBusRef);
        setF32(PropertyId::SpatialPanning, source.spatial.panning);
        setF32(PropertyId::SpatialSpread, source.spatial.spread);
        setU32(PropertyId::SpatialIs3DEnabled,
               source.spatial.is3DEnabled ? 1u : 0u);
        setF32(PropertyId::SpatialAttenuationDistanceScaling,
               source.spatial.attenuationDistanceScaling);
        setObjectId(PropertyId::SpatialAttenuationRef,
                    source.spatial.attenuationRef);
        setF32(PropertyId::SpatializationMix,
               source.spatial.spatializationMix);
        if (present(PropertyId::Sends)) {
            output.sends.reserve(source.sends.size());
            for (std::size_t i = 0; i < source.sends.size(); ++i) {
                if (i > UINT32_MAX) return false;
                harkenstudiobuild::SendV1 row{};
                row.stableSendOrdinal = static_cast<std::uint32_t>(i);
                row.targetBusId = source.sends[i].targetBusId;
                row.sendLevel = CanonicalF32(source.sends[i].sendLevel);
                output.sends.push_back(row);
            }
        }
        if (present(PropertyId::SignalGraph)) {
            output.effectNodes.reserve(source.signalGraph.nodes.size());
            output.effectParams.reserve(source.signalGraph.nodes.size());
            auto sourceNodes = source.signalGraph.nodes;
            std::sort(sourceNodes.begin(), sourceNodes.end(),
                      [](const auto& a, const auto& b) {
                          return a.nodeId < b.nodeId;
                      });
            for (const auto& sourceNode : sourceNodes) {
                if (harkenstudiobuild::FindPackEffectSchemaByType(sourceNode.effectType) == nullptr) {
                    return false;
                }
                harkenstudiobuild::EffectNodeV1 node{};
                node.nodeId = sourceNode.nodeId;
                node.effectId = sourceNode.effectId;
                node.effectType = sourceNode.effectType;
                output.effectNodes.push_back(node);
                auto params = sourceNode.params;
                std::sort(params.begin(), params.end(),
                          [](const auto& a, const auto& b) {
                              return a.paramId < b.paramId;
                          });
                auto& destinationParams = output.effectParams.emplace_back();
                destinationParams.reserve(params.size());
                for (const auto& param : params) {
                    destinationParams.push_back(harkenstudiobuild::EffectParamV1{
                        param.paramId, CanonicalF32Bits(param.valueBits)});
                }
            }
            output.effectEdges.reserve(source.signalGraph.edges.size());
            for (const auto& edge : source.signalGraph.edges) {
                output.effectEdges.push_back(
                    harkenstudiobuild::EffectEdgeV1{edge.fromNodeId, edge.toNodeId});
            }
            std::sort(output.effectEdges.begin(), output.effectEdges.end(),
                      [](const auto& a, const auto& b) {
                          return std::tie(a.fromNodeId, a.toNodeId) <
                                 std::tie(b.fromNodeId, b.toNodeId);
                      });
        }
        output.Refresh();
        return true;
    };

    world->properties_.reserve(project.objects.size());
    for (const auto& object : project.objects) {
        OwningCurrentWorldV1::OwnedProperty row{};
        row.view.contentPropertyKind = RuntimeKind(object.kind);
        row.view.stableId = object.chassis().id;
        const DefinitionMaterializationV1* materialization = nullptr;
        if (object.kind != harkenprojectschema::ContentKind::Layer) {
            materialization = FindMaterialization(
                materializations,
                static_cast<harkenplayableproperties::ContentPropertyKindV1>(
                    static_cast<std::uint32_t>(object.kind)),
                row.view.stableId);
        }
        if (busIds != nullptr) {
            const auto containsBus = [busIds](harkenstudio::ObjectId id) {
                return id == harkenstudio::ObjectId{} || std::binary_search(
                    busIds->ids.begin(), busIds->ids.end(), id);
            };
            const auto& chassis = object.chassis();
            if (!containsBus(chassis.mainOutputBusRef)) {
                result.status = CompileStatusV1::InvalidProject;
                result.relatedStableId = chassis.mainOutputBusRef;
                return result;
            }
            for (const auto& send : chassis.sends) {
                if (!containsBus(send.targetBusId)) {
                    result.status = CompileStatusV1::InvalidProject;
                    result.relatedStableId = send.targetBusId;
                    return result;
                }
            }
        }
        if (row.view.stableId == harkenstudio::ObjectId{} ||
            !buildPayload(object.chassis(), row.payload)) {
            result.status = CompileStatusV1::InvalidProject;
            result.relatedStableId = object.chassis().id;
            return result;
        }
        if (object.kind != harkenprojectschema::ContentKind::Layer) {
            if (materialization == nullptr) {
                result.status = CompileStatusV1::MissingMaterialization;
                result.relatedStableId = row.view.stableId;
                return result;
            }
            if (materialization->contentRevision == 0) {
                result.status = CompileStatusV1::InvalidMaterialization;
                result.relatedStableId = row.view.stableId;
                return result;
            }
            row.view.contentRevision = materialization->contentRevision;
            row.view.routeStableId = materialization->routeStableId;
            row.view.compatibilityFingerprint =
                materialization->compatibilityFingerprint;
        }

        std::vector<harkenstudio::ObjectId> nearestFirst;
        auto parent = parentByChild.find(row.view.stableId);
        std::set<harkenstudio::ObjectId> visitedParents;
        while (parent != parentByChild.end()) {
            if (!visitedParents.insert(parent->second).second) {
                result.status = CompileStatusV1::InvalidProject;
                result.relatedStableId = parent->second;
                return result;
            }
            const auto* ancestor = FindObject(project, parent->second);
            if (ancestor == nullptr) {
                result.status = CompileStatusV1::InvalidProject;
                result.relatedStableId = parent->second;
                return result;
            }
            nearestFirst.push_back(ancestor->chassis().id);
            parent = parentByChild.find(ancestor->chassis().id);
        }
        for (auto it = nearestFirst.rbegin(); it != nearestFirst.rend(); ++it) {
            const auto* ancestor = FindObject(project, *it);
            if (ancestor == nullptr) {
                result.status = CompileStatusV1::InvalidProject;
                result.relatedStableId = *it;
                return result;
            }
            row.structuralAncestors.push_back(
                harkenstudiobuild::StructuralAncestorRefV1{
                    RuntimeKind(ancestor->kind), 0, *it, 0, {}});
            if (ancestor->kind == harkenprojectschema::ContentKind::Layer) {
                row.ancestors.push_back(harkenstudiobuild::LayerAncestorRefV1{*it, 0});
            }
        }
        row.Refresh();
        world->properties_.push_back(std::move(row));
    }

    std::sort(world->properties_.begin(), world->properties_.end(),
              [](const auto& a, const auto& b) {
                  return std::pair(static_cast<std::uint32_t>(
                                       a.view.contentPropertyKind),
                                   a.view.stableId) <
                         std::pair(static_cast<std::uint32_t>(
                                       b.view.contentPropertyKind),
                                   b.view.stableId);
              });
    if (world->properties_.size() > UINT32_MAX) {
        result.status = CompileStatusV1::Capacity;
        return result;
    }
    for (std::size_t i = 0; i < world->properties_.size(); ++i) {
        world->properties_[i].view.authoredStableOrder =
            static_cast<std::uint32_t>(i);
    }
    world->Refresh();
    std::map<std::pair<std::uint32_t, harkenstudio::ObjectId>, std::size_t> propertyIndex;
    for (std::size_t i = 0; i < world->properties_.size(); ++i) {
        propertyIndex.emplace(
            std::pair(static_cast<std::uint32_t>(
                          world->properties_[i].view.contentPropertyKind),
                      world->properties_[i].view.stableId),
            i);
    }
    for (auto& row : world->properties_) {
        for (auto& ancestor : row.structuralAncestors) {
            const auto found = propertyIndex.find({
                static_cast<std::uint32_t>(ancestor.contentPropertyKind),
                ancestor.stableId});
            if (found == propertyIndex.end()) {
                result.status = CompileStatusV1::InvalidProject;
                result.relatedStableId = ancestor.stableId;
                return result;
            }
            ancestor.payload = harkenstudiobuild::MakeStructuralAncestorPayloadV1(
                world->properties_[found->second].payload.view);
        }
        row.Refresh();
    }
    std::vector<std::uint8_t> fingerprintState(world->properties_.size(), 0);
    std::function<bool(std::size_t)> fingerprintProperty = [&](std::size_t index) {
        if (fingerprintState[index] == 2) return true;
        if (fingerprintState[index] == 1) return false;
        fingerprintState[index] = 1;
        auto& row = world->properties_[index];
        for (auto& ancestor : row.structuralAncestors) {
            const auto found = propertyIndex.find(
                {static_cast<std::uint32_t>(
                     ancestor.contentPropertyKind), ancestor.stableId});
            if (found == propertyIndex.end() || !fingerprintProperty(found->second)) {
                return false;
            }
            ancestor.programSemanticFingerprint =
                world->properties_[found->second].view.programSemanticFingerprint;
        }
        for (auto& ancestor : row.ancestors) {
            const auto found = propertyIndex.find(
                {static_cast<std::uint32_t>(
                     harkenstudiobuild::playableproperties::ContentPropertyKindV1::Layer),
                 ancestor.layerStableId});
            if (found == propertyIndex.end() ||
                !fingerprintProperty(found->second)) {
                return false;
            }
            ancestor.layerProgramSemanticFingerprint =
                world->properties_[found->second].view.programSemanticFingerprint;
        }
        row.Refresh();
        row.view.programSemanticFingerprint =
            harkenstudiobuild::ComputePlayablePropertyFingerprintV1(row.view);
        fingerprintState[index] = 2;
        return true;
    };
    for (std::size_t i = 0; i < world->properties_.size(); ++i) {
        if (!fingerprintProperty(i)) {
            result.status = CompileStatusV1::InvalidProject;
            result.relatedStableId = world->properties_[i].view.stableId;
            return result;
        }
    }

    const auto propertyFor = [&](harkenprojectschema::ContentKind kind,
                                 harkenstudio::ObjectId stableId)
        -> const harkenstudiobuild::PlayablePropertyProgramViewV1* {
        const auto found = propertyIndex.find(
            {static_cast<std::uint32_t>(kind), stableId});
        return found == propertyIndex.end()
                   ? nullptr
                   : &world->properties_[found->second].view;
    };

    for (const auto& object : project.objects) {
        if (object.kind == harkenprojectschema::ContentKind::Track) {
            const auto* parent = propertyFor(object.kind, object.chassis().id);
            for (const auto& clip : object.track.clips) {
                const auto* child = propertyFor(clip.contentRef.kind,
                                                clip.contentRef.target);
                if (parent == nullptr || child == nullptr) {
                    result.status = CompileStatusV1::InvalidProject;
                    result.relatedStableId = clip.contentRef.target;
                    return result;
                }
                OwningCurrentWorldV1::OwnedOccurrence row{};
                row.view.parentKind = parent->contentPropertyKind;
                row.view.stableOccurrenceOrdinal = clip.stableOccurrenceOrdinal;
                row.view.parentStableId = parent->stableId;
                row.view.parentProgramSemanticFingerprint =
                    parent->programSemanticFingerprint;
                row.view.childPlayable = MakeRef(*child);
                if (!buildPayload(clip, row.payload)) {
                    result.status = CompileStatusV1::InvalidProject;
                    result.relatedStableId = clip.id;
                    return result;
                }
                row.Refresh();
                row.view.occurrenceSemanticFingerprint =
                    harkenstudiobuild::ComputePlayableOccurrenceFingerprintV1(row.view);
                world->occurrences_.push_back(std::move(row));
            }
        } else if (object.kind == harkenprojectschema::ContentKind::Container) {
            const auto* parent = propertyFor(object.kind, object.chassis().id);
            for (const auto& childOccurrence : object.container.children) {
                const auto* child = propertyFor(
                    childOccurrence.childPlayableRef.kind,
                    childOccurrence.childPlayableRef.target);
                if (parent == nullptr || child == nullptr) {
                    result.status = CompileStatusV1::InvalidProject;
                    result.relatedStableId =
                        childOccurrence.childPlayableRef.target;
                    return result;
                }
                OwningCurrentWorldV1::OwnedOccurrence row{};
                row.view.parentKind = parent->contentPropertyKind;
                row.view.stableOccurrenceOrdinal =
                    childOccurrence.stableOccurrenceOrdinal;
                row.view.parentStableId = parent->stableId;
                row.view.parentProgramSemanticFingerprint =
                    parent->programSemanticFingerprint;
                row.view.childPlayable = MakeRef(*child);
                if (!buildPayload(childOccurrence, row.payload)) {
                    result.status = CompileStatusV1::InvalidProject;
                    result.relatedStableId = parent->stableId;
                    return result;
                }
                row.Refresh();
                row.view.occurrenceSemanticFingerprint =
                    harkenstudiobuild::ComputePlayableOccurrenceFingerprintV1(row.view);
                world->occurrences_.push_back(std::move(row));
            }
        }
    }
    std::sort(world->occurrences_.begin(), world->occurrences_.end(),
              [](const auto& a, const auto& b) {
                  return std::tuple(static_cast<std::uint32_t>(a.view.parentKind),
                                    a.view.parentStableId,
                                    a.view.stableOccurrenceOrdinal) <
                         std::tuple(static_cast<std::uint32_t>(b.view.parentKind),
                                    b.view.parentStableId,
                                    b.view.stableOccurrenceOrdinal);
              });

    for (const auto& object : project.objects) {
        if (object.kind != harkenprojectschema::ContentKind::Track) continue;
        const auto* root = propertyFor(object.kind, object.chassis().id);
        if (root == nullptr || object.track.clips.size() > UINT32_MAX) {
            result.status = CompileStatusV1::InvalidProject;
            result.relatedStableId = object.chassis().id;
            return result;
        }
        OwningCurrentWorldV1::OwnedTrackSchedule row{};
        row.view.rootDefinition = MakeRef(*root);
        auto clips = object.track.clips;
        std::sort(clips.begin(), clips.end(), [](const auto& a, const auto& b) {
            return std::tie(a.time, a.semanticOrderKey, a.id, a.guid.high,
                            a.guid.low) <
                   std::tie(b.time, b.semanticOrderKey, b.id, b.guid.high,
                            b.guid.low);
        });
        row.placements.reserve(clips.size());
        for (std::size_t i = 0; i < clips.size(); ++i) {
            const auto& clip = clips[i];
            const auto* content = propertyFor(clip.contentRef.kind,
                                              clip.contentRef.target);
            if (content == nullptr) {
                result.status = CompileStatusV1::InvalidProject;
                result.relatedStableId = clip.contentRef.target;
                return result;
            }
            harkenstudiobuild::ScheduleClipPlacementV1 placement{};
            placement.stablePlacementOrdinal = static_cast<std::uint32_t>(i);
            placement.stableOccurrenceOrdinal = clip.stableOccurrenceOrdinal;
            placement.content = MakeRef(*content);
            placement.authoredStart = clip.time;
            placement.authoredDuration = clip.duration;
            placement.authoredFadeIn = clip.fades.inLength;
            placement.authoredFadeOut = clip.fades.outLength;
            placement.clipId = clip.id;
            placement.clipGuidHigh = clip.guid.high;
            placement.clipGuidLow = clip.guid.low;
            row.placements.push_back(placement);
        }
        row.Refresh();
        row.view.scheduleSemanticFingerprint =
            harkenstudiobuild::ComputeTrackScheduleFingerprintV1(row.view);
        world->trackSchedules_.push_back(std::move(row));
    }
    std::sort(world->trackSchedules_.begin(), world->trackSchedules_.end(),
              [](const auto& a, const auto& b) {
                  return a.view.rootDefinition.stableId <
                         b.view.rootDefinition.stableId;
              });
    world->Refresh();

    const auto trackScheduleFor = [&](harkenstudio::ObjectId stableId)
        -> const harkenstudiobuild::TrackScheduleProgramViewV1* {
        const auto found = std::lower_bound(
            world->trackScheduleViews_.begin(), world->trackScheduleViews_.end(),
            stableId, [](const auto& row, harkenstudio::ObjectId id) {
                return row.rootDefinition.stableId < id;
            });
        return found == world->trackScheduleViews_.end() ||
                       found->rootDefinition.stableId != stableId
                   ? nullptr
                   : &*found;
    };

    for (const auto& object : project.objects) {
        if (object.kind != harkenprojectschema::ContentKind::Piece) continue;
        const auto* root = propertyFor(object.kind, object.chassis().id);
        if (root == nullptr || object.piece.tracks.size() > UINT32_MAX ||
            object.piece.markers.size() > UINT32_MAX) {
            result.status = CompileStatusV1::InvalidProject;
            result.relatedStableId = object.chassis().id;
            return result;
        }
        OwningCurrentWorldV1::OwnedPieceSchedule row{};
        row.view.rootDefinition = MakeRef(*root);
        row.view.startPointKind =
            static_cast<std::uint32_t>(object.piece.startPoint.kind);
        row.view.startMarkerId = object.piece.startPoint.markerId;
        if (object.piece.timeSpec.kind ==
            harkenprojectschema::PieceTimeKind::Absolute) {
            row.view.timeKind = harkenstudiobuild::ScheduleTimeKindV1::AbsoluteNanoseconds;
        } else {
            if (object.piece.timeSpec.tempoMap.size() != 1 ||
                object.piece.timeSpec.meterMap.size() != 1 ||
                object.piece.timeSpec.tempoMap[0].tick != 0 ||
                object.piece.timeSpec.meterMap[0].tick != 0) {
                result.status = CompileStatusV1::InvalidProject;
                result.relatedStableId = object.chassis().id;
                return result;
            }
            row.view.timeKind = harkenstudiobuild::ScheduleTimeKindV1::MusicalTicks;
            row.view.microsPerQuarter =
                object.piece.timeSpec.tempoMap[0].microsPerQuarter;
            row.view.meterNumerator =
                object.piece.timeSpec.meterMap[0].numerator;
            row.view.meterDenominator =
                object.piece.timeSpec.meterMap[0].denominator;
        }
        auto trackIds = object.piece.tracks;
        std::sort(trackIds.begin(), trackIds.end());
        row.trackRefs.reserve(trackIds.size());
        for (std::size_t i = 0; i < trackIds.size(); ++i) {
            const auto* track = trackScheduleFor(trackIds[i]);
            if (track == nullptr) {
                result.status = CompileStatusV1::InvalidProject;
                result.relatedStableId = trackIds[i];
                return result;
            }
            row.trackRefs.push_back(harkenstudiobuild::ScheduleTrackRefV1{
                track->rootDefinition, track->scheduleSemanticFingerprint,
                static_cast<std::uint32_t>(i), 0});
        }
        auto markers = object.piece.markers;
        std::sort(markers.begin(), markers.end(), [](const auto& a, const auto& b) {
            return std::pair(a.markerId, a.id) < std::pair(b.markerId, b.id);
        });
        row.markers.reserve(markers.size());
        for (const auto& marker : markers) {
            row.markers.push_back(harkenstudiobuild::ScheduleMarkerV1{
                marker.id, marker.markerId, static_cast<std::uint32_t>(marker.kind),
                marker.position});
        }
        row.Refresh();
        row.view.scheduleSemanticFingerprint =
            harkenstudiobuild::ComputePieceScheduleFingerprintV1(row.view);
        world->pieceSchedules_.push_back(std::move(row));
    }
    std::sort(world->pieceSchedules_.begin(), world->pieceSchedules_.end(),
              [](const auto& a, const auto& b) {
                  return a.view.rootDefinition.stableId <
                         b.view.rootDefinition.stableId;
              });

    std::set<harkenstudio::ObjectId> consumedAuthoring;
    for (const auto& object : project.objects) {
        if (object.kind != harkenprojectschema::ContentKind::Container) continue;
        const auto authored = std::find_if(
            authoredContainers.begin(), authoredContainers.end(),
            [&](const ContainerAuthoringV1& row) {
                return row.containerStableId == object.chassis().id;
            });
        if (authored == authoredContainers.end()) {
            result.status = CompileStatusV1::MissingContainerAuthoring;
            result.relatedStableId = object.chassis().id;
            return result;
        }
        if (!consumedAuthoring.insert(authored->containerStableId).second ||
            authored->children.size() != object.container.children.size()) {
            result.status = CompileStatusV1::InvalidContainerAuthoring;
            result.relatedStableId = object.chassis().id;
            return result;
        }
        OwningCurrentWorldV1::OwnedContainer row{};
        row.view.containerStableId = authored->containerStableId;
        row.view.mode = authored->mode;
        row.view.randomDiscipline = authored->randomDiscipline;
        row.view.memoryScope = authored->memoryScope;
        row.view.retriggerPolicy = authored->retriggerPolicy;
        row.view.resetPolicy = authored->resetPolicy;
        row.view.sequenceEndPolicy = authored->sequenceEndPolicy;
        row.view.queueCycle = authored->queueCycle;
        row.view.queueCycleCount = authored->queueCycleCount;
        row.view.queueSplice = authored->queueSplice;
        row.view.resumePosition = authored->resumePosition ? 1u : 0u;
        row.view.avoidLastN = authored->avoidLastN;
        row.view.initialSequenceCursor = authored->initialSequenceCursor;
        row.view.discreteTiming = authored->discreteTiming;
        row.view.continuousTiming = authored->continuousTiming;
        row.view.continuousNormalization = authored->continuousNormalization;
        row.view.evaluationPeriodFrames = authored->evaluationPeriodFrames;
        row.view.envelopeFrames = authored->envelopeFrames;
        row.view.maxActiveRows = authored->maxActiveRows;
        row.view.maxLiveRows = authored->maxLiveRows;
        row.view.transitionProgramIndex = authored->transitionProgramIndex;
        row.view.authoredStableOrder = authored->authoredStableOrder;
        row.curves = authored->curves;
        std::sort(row.curves.begin(), row.curves.end(),
                  [](const auto& a, const auto& b) {
                      return a.curveLocalId < b.curveLocalId;
                  });
        row.discreteAxes = authored->discreteAxes;
        std::sort(row.discreteAxes.begin(), row.discreteAxes.end(),
                  [](const auto& a, const auto& b) {
                      return a.stableAxisOrdinal < b.stableAxisOrdinal;
                  });
        auto discreteRows = authored->discreteRows;
        std::sort(discreteRows.begin(), discreteRows.end(),
                  [](const auto& a, const auto& b) {
                      return a.stableRowOrdinal < b.stableRowOrdinal;
                  });
        row.discreteRows.resize(discreteRows.size());
        row.discreteCells.resize(discreteRows.size());
        for (std::size_t gridRow = 0; gridRow < discreteRows.size();
             ++gridRow) {
            if (discreteRows[gridRow].cells.size() !=
                row.discreteAxes.size()) {
                result.status = CompileStatusV1::InvalidContainerAuthoring;
                result.relatedStableId = object.chassis().id;
                return result;
            }
            row.discreteRows[gridRow].stableRowOrdinal =
                discreteRows[gridRow].stableRowOrdinal;
            row.discreteRows[gridRow].authoredStableOrder =
                discreteRows[gridRow].authoredStableOrder;
            row.discreteRows[gridRow].childStableOrdinal =
                discreteRows[gridRow].childStableOrdinal;
            row.discreteCells[gridRow].reserve(row.discreteAxes.size());
            for (const auto& axis : row.discreteAxes) {
                if (axis.authoredColumnOrder >=
                    discreteRows[gridRow].cells.size()) {
                    result.status = CompileStatusV1::InvalidContainerAuthoring;
                    result.relatedStableId = object.chassis().id;
                    return result;
                }
                row.discreteCells[gridRow].push_back(
                    discreteRows[gridRow].cells[axis.authoredColumnOrder]);
            }
        }
        row.continuousAxes = authored->continuousAxes;
        std::sort(row.continuousAxes.begin(), row.continuousAxes.end(),
                  [](const auto& a, const auto& b) {
                      return a.stableAxisOrdinal < b.stableAxisOrdinal;
                  });
        auto continuousRows = authored->continuousRows;
        std::sort(continuousRows.begin(), continuousRows.end(),
                  [](const auto& a, const auto& b) {
                      return a.stableRowOrdinal < b.stableRowOrdinal;
                  });
        row.continuousRows.resize(continuousRows.size());
        row.continuousCells.resize(continuousRows.size());
        for (std::size_t gridRow = 0; gridRow < continuousRows.size();
             ++gridRow) {
            if (continuousRows[gridRow].cells.size() !=
                row.continuousAxes.size()) {
                result.status = CompileStatusV1::InvalidContainerAuthoring;
                result.relatedStableId = object.chassis().id;
                return result;
            }
            row.continuousRows[gridRow].stableRowOrdinal =
                continuousRows[gridRow].stableRowOrdinal;
            row.continuousRows[gridRow].authoredStableOrder =
                continuousRows[gridRow].authoredStableOrder;
            row.continuousRows[gridRow].childStableOrdinal =
                continuousRows[gridRow].childStableOrdinal;
            row.continuousCells[gridRow].reserve(row.continuousAxes.size());
            for (const auto& axis : row.continuousAxes) {
                if (axis.authoredColumnOrder >=
                    continuousRows[gridRow].cells.size()) {
                    result.status = CompileStatusV1::InvalidContainerAuthoring;
                    result.relatedStableId = object.chassis().id;
                    return result;
                }
                row.continuousCells[gridRow].push_back(
                    continuousRows[gridRow]
                        .cells[axis.authoredColumnOrder]);
            }
        }
        std::set<std::uint32_t> authoredOrdinals;
        auto authoredChildren = authored->children;
        std::sort(authoredChildren.begin(), authoredChildren.end(),
                  [](const auto& a, const auto& b) {
                      return a.stableOccurrenceOrdinal <
                             b.stableOccurrenceOrdinal;
                  });
        for (const auto& authoredChild : authoredChildren) {
            if (!authoredOrdinals.insert(
                    authoredChild.stableOccurrenceOrdinal).second) {
                result.status = CompileStatusV1::InvalidContainerAuthoring;
                result.relatedStableId = object.chassis().id;
                return result;
            }
            const auto childOccurrence = std::find_if(
                object.container.children.begin(), object.container.children.end(),
                [&](const harkenprojectschema::ContainerChildOccurrence& child) {
                    return child.stableOccurrenceOrdinal ==
                           authoredChild.stableOccurrenceOrdinal;
                });
            if (childOccurrence == object.container.children.end()) {
                result.status = CompileStatusV1::InvalidContainerAuthoring;
                result.relatedStableId = object.chassis().id;
                return result;
            }
            const auto* child = propertyFor(
                childOccurrence->childPlayableRef.kind,
                childOccurrence->childPlayableRef.target);
            if (child == nullptr) {
                result.status = CompileStatusV1::InvalidProject;
                result.relatedStableId =
                    childOccurrence->childPlayableRef.target;
                return result;
            }
            harkenstudiobuild::ContainerChildProgramViewV2 compiled{};
            compiled.playable = MakeRef(*child);
            compiled.stableChildOrdinal =
                authoredChild.stableOccurrenceOrdinal;
            compiled.authoredStableOrder = authoredChild.authoredStableOrder;
            compiled.frequencyWeight = authoredChild.frequencyWeight;
            compiled.orderWeight = authoredChild.orderWeight;
            compiled.queueStopPolicy = authoredChild.queueStopPolicy;
            const bool queue = row.view.mode == harkenstudiobuild::ContainerModeV1::RandomQueue ||
                               row.view.mode == harkenstudiobuild::ContainerModeV1::SequenceQueue;
            const bool inlineCompletionAllowed =
                queue && compiled.playable.kind == harkenstudiobuild::PlayableKindV1::Sound;
            const bool inactiveCompletion =
                authoredChild.completion.completionClass ==
                    harkenstudiobuild::AuthoredCompletionClassV1::SignalOnly &&
                authoredChild.completion.leaves.empty();
            if (!inlineCompletionAllowed && !inactiveCompletion) {
                result.status = CompileStatusV1::InvalidContainerAuthoring;
                result.relatedStableId = row.view.containerStableId;
                return result;
            }
            if (queue &&
                (compiled.playable.kind == harkenstudiobuild::PlayableKindV1::Track ||
                 compiled.playable.kind == harkenstudiobuild::PlayableKindV1::Piece)) {
                std::uint64_t scheduleFingerprint = 0;
                if (compiled.playable.kind == harkenstudiobuild::PlayableKindV1::Track) {
                    const auto* schedule =
                        trackScheduleFor(compiled.playable.stableId);
                    if (schedule == nullptr) {
                        result.status = CompileStatusV1::InvalidProject;
                        result.relatedStableId = compiled.playable.stableId;
                        return result;
                    }
                    scheduleFingerprint = schedule->scheduleSemanticFingerprint;
                } else {
                    const auto schedule = std::lower_bound(
                        world->pieceSchedules_.begin(),
                        world->pieceSchedules_.end(), compiled.playable.stableId,
                        [](const auto& candidate, harkenstudio::ObjectId stableId) {
                            return candidate.view.rootDefinition.stableId <
                                   stableId;
                        });
                    if (schedule == world->pieceSchedules_.end() ||
                        schedule->view.rootDefinition.stableId !=
                            compiled.playable.stableId) {
                        result.status = CompileStatusV1::InvalidProject;
                        result.relatedStableId = compiled.playable.stableId;
                        return result;
                    }
                    scheduleFingerprint =
                        schedule->view.scheduleSemanticFingerprint;
                }
                compiled.timingHint = harkenstudiobuild::MakeScheduleReferenceTimingHint(
                    compiled.playable.kind, compiled.playable.stableId,
                    scheduleFingerprint);
            } else if (queue &&
                       compiled.playable.kind == harkenstudiobuild::PlayableKindV1::Sound) {
                if (authoredChild.completion.completionClass ==
                        harkenstudiobuild::AuthoredCompletionClassV1::FiniteMediaCandidate &&
                    (authoredChild.completion.leaves.size() != 1 ||
                     authoredChild.completion.leaves[0].value.content.kind !=
                         harkenstudiobuild::PlayableKindV1::Sound ||
                     authoredChild.completion.leaves[0].value.content.stableId !=
                         compiled.playable.stableId)) {
                    result.status = CompileStatusV1::InvalidContainerAuthoring;
                    result.relatedStableId = row.view.containerStableId;
                    return result;
                }
                if (authoredChild.completion.completionClass ==
                    harkenstudiobuild::AuthoredCompletionClassV1::FiniteMediaCandidate) {
                    compiled.timingHint.kind =
                        harkenstudiobuild::ChildTimingHintKindV2::InlineSound;
                    const auto& source =
                        authoredChild.completion.leaves[0].value;
                    auto& hint = compiled.timingHint.payload.inlineSound;
                    hint.startOffset = source.startOffset;
                    hint.duration = source.duration;
                    hint.loopCount = source.loopCount;
                    hint.loopRegionPresent = source.loopRegionPresent;
                    hint.reserved = source.reserved2;
                    hint.loopStart = source.loopStart;
                    hint.loopEnd = source.loopEnd;
                }
            }
            row.children.push_back(compiled);
        }
        row.Refresh();
        const auto* containerDefinition = propertyFor(
            harkenprojectschema::ContentKind::Container,
            row.view.containerStableId);
        if (containerDefinition == nullptr) {
            result.status = CompileStatusV1::InvalidProject;
            result.relatedStableId = row.view.containerStableId;
            return result;
        }
        row.view.programSemanticFingerprint =
            containerDefinition->programSemanticFingerprint;
        world->containers_.push_back(std::move(row));
    }
    if (consumedAuthoring.size() != authoredContainers.size()) {
        result.status = CompileStatusV1::InvalidContainerAuthoring;
        for (const auto& row : authoredContainers) {
            if (!consumedAuthoring.contains(row.containerStableId)) {
                result.relatedStableId = row.containerStableId;
                break;
            }
        }
        return result;
    }
    std::sort(world->containers_.begin(), world->containers_.end(),
              [](const auto& a, const auto& b) {
                  return a.view.containerStableId < b.view.containerStableId;
              });

    harkenstudio::ObjectId advancedTransitionOwner{};
    world->transitions_.reserve(authoredTransitions.size());
    for (const auto& authored : authoredTransitions) {
        OwningCurrentWorldV1::OwnedTransition row{};
        row.view.ownerContainerStableId = authored.ownerContainerStableId;
        row.view.defaultRuleIndex = authored.defaultRuleIndex;
        row.view.maxContainerDescent = authored.maxContainerDescent;
        row.view.maxOwnerAscent = authored.maxOwnerAscent;
        row.view.watchFallback = authored.watchFallback;
        row.downwardPathSteps = authored.downwardPathSteps;
        row.rules = authored.rules;
        const bool defaultRulePresent =
            row.view.defaultRuleIndex < row.rules.size();
        const std::uint32_t defaultRuleOrdinal = defaultRulePresent
            ? row.rules[row.view.defaultRuleIndex].stableRuleOrdinal : 0;
        std::sort(row.rules.begin(), row.rules.end(),
                  [](const auto& a, const auto& b) {
                      return a.stableRuleOrdinal < b.stableRuleOrdinal;
                  });
        if (defaultRulePresent) {
            const auto defaultRule = std::find_if(
                row.rules.begin(), row.rules.end(),
                [defaultRuleOrdinal](const auto& rule) {
                    return rule.stableRuleOrdinal == defaultRuleOrdinal;
                });
            row.view.defaultRuleIndex = static_cast<std::uint32_t>(
                std::distance(row.rules.begin(), defaultRule));
        }
        const auto resolveRef = [&](harkenstudiobuild::PlayableRefV1* ref) {
            if (ref == nullptr || ref->reserved != 0 || ref->stableId == harkenstudio::ObjectId{} ||
                static_cast<std::uint32_t>(ref->kind) >
                    static_cast<std::uint32_t>(harkenstudiobuild::PlayableKindV1::Piece)) {
                return false;
            }
            const auto kind = static_cast<harkenprojectschema::ContentKind>(
                static_cast<std::uint32_t>(ref->kind) + 1u);
            const auto* property = propertyFor(kind, ref->stableId);
            if (property == nullptr) return false;
            const harkenstudiobuild::PlayableRefV1 resolved = MakeRef(*property);
            const bool identityOnly =
                ref->expectedProgramSemanticFingerprint == 0 &&
                ref->contentRevision == 0 && ref->routeStableId == harkenstudio::ObjectId{} &&
                ref->compatibilityFingerprint == 0;
            if (!identityOnly &&
                (ref->expectedProgramSemanticFingerprint !=
                     resolved.expectedProgramSemanticFingerprint ||
                 ref->contentRevision != resolved.contentRevision ||
                 ref->routeStableId != resolved.routeStableId ||
                 ref->compatibilityFingerprint !=
                     resolved.compatibilityFingerprint)) {
                return false;
            }
            *ref = resolved;
            return true;
        };
        bool refsValid = true;
        for (auto& step : row.downwardPathSteps) {
            refsValid = resolveRef(&step.playable) && refsValid;
        }
        for (auto& rule : row.rules) {
            if (rule.sourceAny == 0) {
                refsValid = resolveRef(&rule.source) && refsValid;
            }
            if (rule.targetAny == 0) {
                refsValid = resolveRef(&rule.target) && refsValid;
            }
            if (rule.bridgePresent != 0) {
                refsValid = resolveRef(&rule.bridge) && refsValid;
            }
        }
        if (!refsValid) {
            result.status = CompileStatusV1::InvalidCanonicalWorld;
            result.relatedStableId = authored.ownerContainerStableId;
            return result;
        }
        row.Refresh();
        const auto classification =
            harkenstudiobuild::ClassifyTransitionProgramV1(row.view);
        if (classification == harkenstudiobuild::TransitionProgramClassV1::Advanced) {
            if (advancedTransitionOwner == harkenstudio::ObjectId{}) {
                advancedTransitionOwner = authored.ownerContainerStableId;
            }
        }
        if (classification == harkenstudiobuild::TransitionProgramClassV1::Invalid) {
            result.status = CompileStatusV1::InvalidCanonicalWorld;
            result.relatedStableId = authored.ownerContainerStableId;
            return result;
        }
        world->transitions_.push_back(std::move(row));
    }
    std::sort(world->transitions_.begin(), world->transitions_.end(),
              [](const auto& a, const auto& b) {
                  return a.view.ownerContainerStableId <
                         b.view.ownerContainerStableId;
              });
    for (auto& container : world->containers_) {
        if (container.view.transitionProgramIndex == UINT32_MAX) continue;
        const std::uint32_t authoredIndex =
            container.view.transitionProgramIndex;
        if (authoredIndex >= authoredTransitions.size() ||
            authoredTransitions[authoredIndex].ownerContainerStableId !=
                container.view.containerStableId) {
            result.status = CompileStatusV1::InvalidContainerAuthoring;
            result.relatedStableId = container.view.containerStableId;
            return result;
        }
        const auto transition = std::lower_bound(
            world->transitions_.begin(), world->transitions_.end(),
            container.view.containerStableId,
            [](const auto& row, harkenstudio::ObjectId stableId) {
                return row.view.ownerContainerStableId < stableId;
            });
        if (transition == world->transitions_.end() ||
            transition->view.ownerContainerStableId !=
                container.view.containerStableId) {
            result.status = CompileStatusV1::InvalidContainerAuthoring;
            result.relatedStableId = container.view.containerStableId;
            return result;
        }
        container.view.transitionProgramIndex =
            static_cast<std::uint32_t>(
                std::distance(world->transitions_.begin(), transition));
    }

    world->actions_.reserve(authoredActions.size());
    for (const auto& authored : authoredActions) {
        OwningCurrentWorldV1::OwnedAction row{};
        row.view.actionStableId = authored.actionStableId;
        if (row.view.actionStableId == harkenstudio::ObjectId{} || authored.steps.empty() ||
            authored.steps.size() > harkenstudiobuild::kMaxActionSteps) {
            result.status = CompileStatusV1::InvalidActionAuthoring;
            result.relatedStableId = authored.actionStableId;
            return result;
        }
        row.steps.reserve(authored.steps.size());
        for (std::size_t index = 0; index < authored.steps.size(); ++index) {
            const auto& source = authored.steps[index];
            if (source.authoredEventIndex != index ||
                source.target.stableId == harkenstudio::ObjectId{}) {
                result.status = CompileStatusV1::InvalidActionAuthoring;
                result.relatedStableId = authored.actionStableId;
                return result;
            }
            harkenstudiobuild::ActionStepProgramViewV1 step{};
            step.opcode = source.opcode;
            step.authoredEventIndex = source.authoredEventIndex;
            step.target.kind = source.target.kind;
            step.target.stableId = source.target.stableId;
            const harkenstudiobuild::AuthoredTimeRationalV1 zeroTime{
                harkenstudiobuild::AuthoredTimeDomainV1::AbsoluteNanoseconds, 0, 1};
            step.delay = zeroTime;
            step.fade = zeroTime;
            step.cooldown = zeroTime;
            if ((source.opcode == harkenstudiobuild::ActionOpcodeV1::Play ||
                 source.opcode == harkenstudiobuild::ActionOpcodeV1::Stop) &&
                static_cast<std::uint32_t>(source.target.kind) <=
                    static_cast<std::uint32_t>(harkenstudiobuild::ActionTargetKindV1::Piece)) {
                const auto contentKind =
                    static_cast<harkenprojectschema::ContentKind>(
                        static_cast<std::uint32_t>(source.target.kind) + 1u);
                const auto* target =
                    propertyFor(contentKind, source.target.stableId);
                if (target == nullptr) {
                    result.status = CompileStatusV1::InvalidActionAuthoring;
                    result.relatedStableId = authored.actionStableId;
                    return result;
                }
                step.target.expectedProgramSemanticFingerprint =
                    target->programSemanticFingerprint;
                if (source.opcode == harkenstudiobuild::ActionOpcodeV1::Play) {
                    if (!CanonicalActionTime(source.delayMilliseconds, 60000.0f,
                                             1000000.0, &step.delay) ||
                        !CanonicalActionTime(source.cooldownSeconds, 60.0f,
                                             1000000000.0, &step.cooldown) ||
                        !harkenstudiobuild::ActionQuantizeTimingSupportedV1(
                            source.opcode, source.quantizeTiming)) {
                        result.status = CompileStatusV1::InvalidActionAuthoring;
                        result.relatedStableId = authored.actionStableId;
                        return result;
                    }
                    step.quantizeTiming = source.quantizeTiming;
                    if (source.quantizeTiming !=
                        harkenstudiobuild::ActionQuantizeTimingV1::Immediate) {
                        if (source.quantizeReference.stableId == harkenstudio::ObjectId{} ||
                            static_cast<std::uint32_t>(
                                source.quantizeReference.kind) >
                                static_cast<std::uint32_t>(
                                    harkenstudiobuild::ActionTargetKindV1::Piece)) {
                            result.status =
                                CompileStatusV1::InvalidActionAuthoring;
                            result.relatedStableId = authored.actionStableId;
                            return result;
                        }
                        const auto referenceKind =
                            static_cast<harkenprojectschema::ContentKind>(
                                static_cast<std::uint32_t>(
                                    source.quantizeReference.kind) +
                                1u);
                        const auto* reference = propertyFor(
                            referenceKind, source.quantizeReference.stableId);
                        if (reference == nullptr ||
                            (source.ifSilent !=
                                 harkenstudiobuild::ActionIfSilentV1::Immediate &&
                             source.ifSilent !=
                                 harkenstudiobuild::ActionIfSilentV1::Drop)) {
                            result.status =
                                CompileStatusV1::InvalidActionAuthoring;
                            result.relatedStableId = authored.actionStableId;
                            return result;
                        }
                        step.quantizeReference.kind =
                            source.quantizeReference.kind;
                        step.quantizeReference.stableId =
                            source.quantizeReference.stableId;
                        step.quantizeReference
                            .expectedProgramSemanticFingerprint =
                            reference->programSemanticFingerprint;
                        step.ifSilent = source.ifSilent;
                    }
                    if (step.cooldown.numerator != 0) {
                        if (source.cooldownScope !=
                                harkenstudiobuild::ActionCooldownScopeV1::Global &&
                            source.cooldownScope !=
                                harkenstudiobuild::ActionCooldownScopeV1::PerEmitter) {
                            result.status =
                                CompileStatusV1::InvalidActionAuthoring;
                            result.relatedStableId = authored.actionStableId;
                            return result;
                        }
                        step.cooldownScope = source.cooldownScope;
                    }
                } else {
                    if (!CanonicalActionTime(source.delayMilliseconds, 60000.0f,
                                             1000000.0, &step.delay) ||
                        !CanonicalActionTime(source.fadeMilliseconds, 60000.0f,
                                             1000000.0, &step.fade) ||
                        !CanonicalActionTime(source.cooldownSeconds, 60.0f,
                                             1000000000.0, &step.cooldown) ||
                        !harkenstudiobuild::ActionQuantizeTimingSupportedV1(
                            source.opcode, source.quantizeTiming)) {
                        result.status = CompileStatusV1::InvalidActionAuthoring;
                        result.relatedStableId = authored.actionStableId;
                        return result;
                    }
                    step.quantizeTiming = source.quantizeTiming;
                    if (source.quantizeTiming !=
                        harkenstudiobuild::ActionQuantizeTimingV1::Immediate) {
                        if (source.quantizeReference.stableId != harkenstudio::ObjectId{}) {
                            if (static_cast<std::uint32_t>(
                                    source.quantizeReference.kind) >
                                static_cast<std::uint32_t>(
                                    harkenstudiobuild::ActionTargetKindV1::Piece)) {
                                result.status =
                                    CompileStatusV1::InvalidActionAuthoring;
                                result.relatedStableId = authored.actionStableId;
                                return result;
                            }
                            const auto referenceKind =
                                static_cast<harkenprojectschema::ContentKind>(
                                    static_cast<std::uint32_t>(
                                        source.quantizeReference.kind) +
                                    1u);
                            const auto* reference = propertyFor(
                                referenceKind,
                                source.quantizeReference.stableId);
                            if (reference == nullptr) {
                                result.status =
                                    CompileStatusV1::InvalidActionAuthoring;
                                result.relatedStableId = authored.actionStableId;
                                return result;
                            }
                            step.quantizeReference.kind =
                                source.quantizeReference.kind;
                            step.quantizeReference.stableId =
                                source.quantizeReference.stableId;
                            step.quantizeReference
                                .expectedProgramSemanticFingerprint =
                                reference->programSemanticFingerprint;
                        }
                        if (source.ifSilent !=
                                harkenstudiobuild::ActionIfSilentV1::Immediate &&
                            source.ifSilent !=
                                harkenstudiobuild::ActionIfSilentV1::Drop) {
                            result.status =
                                CompileStatusV1::InvalidActionAuthoring;
                            result.relatedStableId = authored.actionStableId;
                            return result;
                        }
                        step.ifSilent = source.ifSilent;
                    }
                    if (step.cooldown.numerator != 0) {
                        if (source.cooldownScope !=
                                harkenstudiobuild::ActionCooldownScopeV1::Global &&
                            source.cooldownScope !=
                                harkenstudiobuild::ActionCooldownScopeV1::PerEmitter) {
                            result.status =
                                CompileStatusV1::InvalidActionAuthoring;
                            result.relatedStableId = authored.actionStableId;
                            return result;
                        }
                        step.cooldownScope = source.cooldownScope;
                    }
                }
            } else if (source.opcode == harkenstudiobuild::ActionOpcodeV1::PostAction &&
                       source.target.kind ==
                           harkenstudiobuild::ActionTargetKindV1::Action) {
                if (!CanonicalActionTime(source.delayMilliseconds, 60000.0f,
                                         1000000.0, &step.delay) ||
                    !CanonicalActionTime(source.cooldownSeconds, 60.0f,
                                         1000000000.0, &step.cooldown) ||
                    !harkenstudiobuild::ActionQuantizeTimingSupportedV1(
                        source.opcode, source.quantizeTiming)) {
                    result.status = CompileStatusV1::InvalidActionAuthoring;
                    result.relatedStableId = authored.actionStableId;
                    return result;
                }
                step.quantizeTiming = source.quantizeTiming;
                if (source.quantizeTiming !=
                    harkenstudiobuild::ActionQuantizeTimingV1::Immediate) {
                    if (source.quantizeReference.stableId == harkenstudio::ObjectId{} ||
                        static_cast<std::uint32_t>(
                            source.quantizeReference.kind) >
                            static_cast<std::uint32_t>(
                                harkenstudiobuild::ActionTargetKindV1::Piece)) {
                        result.status = CompileStatusV1::InvalidActionAuthoring;
                        result.relatedStableId = authored.actionStableId;
                        return result;
                    }
                    const auto referenceKind =
                        static_cast<harkenprojectschema::ContentKind>(
                            static_cast<std::uint32_t>(
                                source.quantizeReference.kind) +
                            1u);
                    const auto* reference = propertyFor(
                        referenceKind, source.quantizeReference.stableId);
                    if (reference == nullptr ||
                        (source.ifSilent !=
                             harkenstudiobuild::ActionIfSilentV1::Immediate &&
                         source.ifSilent != harkenstudiobuild::ActionIfSilentV1::Drop)) {
                        result.status = CompileStatusV1::InvalidActionAuthoring;
                        result.relatedStableId = authored.actionStableId;
                        return result;
                    }
                    step.quantizeReference.kind =
                        source.quantizeReference.kind;
                    step.quantizeReference.stableId =
                        source.quantizeReference.stableId;
                    step.quantizeReference
                        .expectedProgramSemanticFingerprint =
                        reference->programSemanticFingerprint;
                    step.ifSilent = source.ifSilent;
                }
                if (step.cooldown.numerator != 0) {
                    if (source.cooldownScope !=
                            harkenstudiobuild::ActionCooldownScopeV1::Global &&
                        source.cooldownScope !=
                            harkenstudiobuild::ActionCooldownScopeV1::PerEmitter) {
                        result.status = CompileStatusV1::InvalidActionAuthoring;
                        result.relatedStableId = authored.actionStableId;
                        return result;
                    }
                    step.cooldownScope = source.cooldownScope;
                }
            } else if (source.opcode == harkenstudiobuild::ActionOpcodeV1::Reset &&
                       source.target.kind ==
                           harkenstudiobuild::ActionTargetKindV1::Container) {
                const auto* target = propertyFor(
                    harkenprojectschema::ContentKind::Container,
                    source.target.stableId);
                if (target == nullptr ||
                    !CanonicalActionTime(source.delayMilliseconds, 60000.0f,
                                         1000000.0, &step.delay) ||
                    (source.resetScope != harkenstudiobuild::ActionScopeV1::Global &&
                     source.resetScope != harkenstudiobuild::ActionScopeV1::Emitter)) {
                    result.status = CompileStatusV1::InvalidActionAuthoring;
                    result.relatedStableId = authored.actionStableId;
                    return result;
                }
                step.target.expectedProgramSemanticFingerprint =
                    target->programSemanticFingerprint;
                step.scope = source.resetScope;
            } else if (source.opcode == harkenstudiobuild::ActionOpcodeV1::SetVariable &&
                       source.target.kind ==
                           harkenstudiobuild::ActionTargetKindV1::Variable) {
                const auto variable = std::find_if(
                    world->variables_.begin(), world->variables_.end(),
                    [&](const harkenstudiobuild::VariableDefinitionV1& candidate) {
                        return candidate.variableId == source.target.stableId;
                    });
                if (variable == world->variables_.end() ||
                    !std::isfinite(source.value) ||
                    source.value < variable->rangeMin ||
                    source.value > variable->rangeMax ||
                    (variable->kind == harkenstudiobuild::VariableKindV1::Step &&
                     (source.value < -16777216.0f ||
                      source.value > 16777216.0f ||
                      source.value != std::floor(source.value)))) {
                    result.status = CompileStatusV1::InvalidActionAuthoring;
                    result.relatedStableId = authored.actionStableId;
                    return result;
                }
                const float value = CanonicalF32(source.value);
                if (source.scope != harkenstudiobuild::ActionScopeV1::Global &&
                    source.scope != harkenstudiobuild::ActionScopeV1::Emitter) {
                    result.status = CompileStatusV1::InvalidActionAuthoring;
                    result.relatedStableId = authored.actionStableId;
                    return result;
                }
                step.scope = source.scope;
                step.valueBits = std::bit_cast<std::uint32_t>(value);
            } else {
                result.status = CompileStatusV1::InvalidActionAuthoring;
                result.relatedStableId = authored.actionStableId;
                return result;
            }
            row.steps.push_back(step);
        }
        row.view.directFeatureMask = 0;
        for (const auto& step : row.steps) {
            if (step.delay.numerator != 0) row.view.directFeatureMask |= 1u;
            if (step.quantizeTiming !=
                    harkenstudiobuild::ActionQuantizeTimingV1::Immediate ||
                step.quantizeReference.stableId != harkenstudio::ObjectId{}) {
                row.view.directFeatureMask |= 2u;
            }
            if (step.cooldown.numerator != 0) {
                row.view.directFeatureMask |= 4u;
            }
            if (step.cooldownScope !=
                    harkenstudiobuild::ActionCooldownScopeV1::Global ||
                step.ifSilent != harkenstudiobuild::ActionIfSilentV1::Immediate) {
                row.view.directFeatureMask |= 8u;
            }
        }
        row.view.transitiveFeatureMask = row.view.directFeatureMask;
        row.view.expandedStepOccurrenceCount =
            static_cast<std::uint32_t>(row.steps.size());
        row.view.expandedFiringCount = 1;
        row.Refresh();
        world->actions_.push_back(std::move(row));
    }
    std::sort(world->actions_.begin(), world->actions_.end(),
              [](const auto& a, const auto& b) {
                  return a.view.actionStableId < b.view.actionStableId;
              });
    if (world->actions_.size() > capacity.maxActionDefinitions) {
        result.status = CompileStatusV1::Capacity;
        return result;
    }
    std::uint64_t totalActionSteps = 0;
    for (std::size_t i = 0; i < world->actions_.size(); ++i) {
        const auto& action = world->actions_[i];
        if ((i != 0 &&
             world->actions_[i - 1].view.actionStableId ==
                 action.view.actionStableId)) {
            result.status = CompileStatusV1::InvalidActionAuthoring;
            result.relatedStableId = action.view.actionStableId;
            return result;
        }
        totalActionSteps += action.steps.size();
        if (action.steps.size() > capacity.maxActionStepsPerDefinition ||
            totalActionSteps > capacity.maxActionStepsTotal) {
            result.status = CompileStatusV1::Capacity;
            result.relatedStableId = action.view.actionStableId;
            return result;
        }
    }
    std::vector<std::uint8_t> actionState(world->actions_.size(), 0);
    std::vector<std::uint32_t> actionDepth(world->actions_.size(), 0);
    bool actionCapacityExceeded = false;
    harkenstudio::ObjectId actionFailureStableId{};
    std::function<bool(std::size_t, std::uint32_t)> visitAction =
        [&](std::size_t index, std::uint32_t traversalDepth) {
        if (actionState[index] == 1) {
            actionFailureStableId = world->actions_[index].view.actionStableId;
            return false;
        }
        if (actionState[index] == 2) return true;
        actionState[index] = 1;
        auto& action = world->actions_[index];
        std::uint64_t expandedSteps = action.steps.size();
        std::uint64_t expandedFirings = 1;
        std::uint32_t depth = 1;
        std::uint32_t transitiveMask = action.view.directFeatureMask;
        for (auto& step : action.steps) {
            if (step.opcode != harkenstudiobuild::ActionOpcodeV1::PostAction) continue;
            const auto child = std::lower_bound(
                world->actions_.begin(), world->actions_.end(),
                step.target.stableId,
                [](const auto& candidate, harkenstudio::ObjectId stableId) {
                    return candidate.view.actionStableId < stableId;
                });
            if (child == world->actions_.end() ||
                child->view.actionStableId != step.target.stableId) {
                actionFailureStableId = action.view.actionStableId;
                return false;
            }
            const std::size_t childIndex = static_cast<std::size_t>(
                std::distance(world->actions_.begin(), child));
            if (traversalDepth >= capacity.maxActionTriggerDepth) {
                actionCapacityExceeded = true;
                actionFailureStableId = action.view.actionStableId;
                return false;
            }
            if (!visitAction(childIndex, traversalDepth + 1u)) return false;
            const auto& childView = world->actions_[childIndex].view;
            step.target.expectedProgramSemanticFingerprint =
                childView.programSemanticFingerprint;
            expandedSteps += childView.expandedStepOccurrenceCount;
            expandedFirings += childView.expandedFiringCount;
            transitiveMask |= childView.transitiveFeatureMask;
            depth = std::max(depth, actionDepth[childIndex] + 1u);
            if (expandedSteps > capacity.maxActionExpandedWork ||
                expandedSteps > UINT32_MAX ||
                expandedFirings > UINT32_MAX ||
                depth > capacity.maxActionTriggerDepth) {
                actionCapacityExceeded = true;
                actionFailureStableId = action.view.actionStableId;
                return false;
            }
        }
        action.view.transitiveFeatureMask = transitiveMask;
        action.view.expandedStepOccurrenceCount =
            static_cast<std::uint32_t>(expandedSteps);
        action.view.expandedFiringCount =
            static_cast<std::uint32_t>(expandedFirings);
        actionDepth[index] = depth;
        action.Refresh();
        action.view.programSemanticFingerprint =
            harkenstudiobuild::ComputeActionProgramFingerprintV1(action.view);
        if (action.view.programSemanticFingerprint == 0) {
            actionFailureStableId = action.view.actionStableId;
            return false;
        }
        actionState[index] = 2;
        return true;
    };
    for (std::size_t i = 0; i < world->actions_.size(); ++i) {
        if (!visitAction(i, 1)) {
            result.status = actionCapacityExceeded
                ? CompileStatusV1::Capacity
                : CompileStatusV1::InvalidActionAuthoring;
            result.relatedStableId = actionFailureStableId;
            return result;
        }
    }

    world->Refresh();
    world->view_.identity.capabilityMask =
        harkenstudiobuild::detail::ComputeCurrentWorldLocalCapabilityMaskV1(world->view_);
    world->view_.identity.runtimeSemanticFingerprint =
        harkenstudiobuild::ComputeCurrentWorldFingerprintV5(world->view_);
    const harkenstudiobuild::WorldInstallStatus validation =
        harkenstudiobuild::ValidateCurrentWorldV1(world->view_);
    if (validation == harkenstudiobuild::WorldInstallStatus::Capacity) {
        result.status = CompileStatusV1::Capacity;
        return result;
    }
    if (validation == harkenstudiobuild::WorldInstallStatus::CapabilityUnavailable &&
        !world->transitions_.empty()) {
        result.status = CompileStatusV1::TransitionCapabilityUnavailable;
        result.relatedStableId = advancedTransitionOwner != harkenstudio::ObjectId{}
            ? advancedTransitionOwner
            : world->transitions_.front().view.ownerContainerStableId;
        return result;
    }
    if (validation != harkenstudiobuild::WorldInstallStatus::Ok &&
        validation != harkenstudiobuild::WorldInstallStatus::CapabilityUnavailable) {
        result.status = CompileStatusV1::InvalidCanonicalWorld;
        return result;
    }
    result.status = CompileStatusV1::Ok;
    result.world = std::move(world);
    return result;
}

} // namespace harkencurrentworldcompiler
