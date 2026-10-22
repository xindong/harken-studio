#include <harken/Harken.h>

#include "harken/core/CurrentWorldCodec.h"
#include "harken/core/PackCompanionNames.h"
#include "harken/core/PlayablePropertyCut.h"
#include "harken/param/VariableService.h"

#include <algorithm>
#include <cmath>

namespace {
using namespace harken;

VnResult Decode(const VnPackByteSpan& span, PackCanonicalImage& image,
                CurrentWorldBodyImage& body) {
    const auto parsed = ParsePackCanonicalImage(span.bytes, span.byteCount, &image);
    if (parsed != VN_OK) return parsed;
    if (!image.hasPartition || image.requirement.storageModelVersion != 2) return VN_PACK_INCOMPATIBLE;
    const auto decoded = body.DecodeWithResult(image.partitionBody.data(), image.partitionBody.size(),
        kCanonicalDefinitionPartitionBodyVintage, CurrentWorldDecodeMode::StrictArtifact);
    if (decoded == CurrentWorldBodyDecodeResult::Capacity) return VN_PACK_RESIDENT_CAPACITY;
    return decoded == CurrentWorldBodyDecodeResult::Ok && body.ProductionView() != nullptr
        ? VN_OK : VN_BAD_PACK;
}

bool MainBusGain(const PackCanonicalImage& init, VnObjectId id, float& db) {
    if (id == VnObjectId{}) id = VN_BUS_MASTER;
    for (std::uint32_t visited = 0; visited < init.busDefinitionSnapshotHeader.busCount; ++visited) {
        VnObjectId parent{};
        if (init.busDefinitionSnapshotHeader.schemaVersion == 2) {
            const auto found = std::find_if(init.busDefinitionBusesV2.begin(), init.busDefinitionBusesV2.end(),
                [&](const auto& row) { return row.busId == id; });
            if (found == init.busDefinitionBusesV2.end()) return false;
            db += found->faderDb + found->makeUpGainDb + found->outputBusVolumeDb;
            parent = found->parentId;
        } else {
            const auto found = std::find_if(init.busDefinitionBuses.begin(), init.busDefinitionBuses.end(),
                [&](const auto& row) { return CanonicalSnapshotBusIdV1(row.busId) == id; });
            if (found == init.busDefinitionBuses.end()) return false;
            parent = CanonicalSnapshotBusIdV1(found->parentId);
        }
        if (id == VN_BUS_MASTER) return parent == VnObjectId{} && std::isfinite(db);
        if (parent == VnObjectId{}) return false;
        id = parent;
    }
    return false;
}

bool HasPitchBinding(const PackCanonicalImage& pack,
    const ResolvedPlayablePropertyCutViewV1& cut) {
    for (const auto& attachment : pack.bindingAttachments) {
        if (attachment.scopeKind != VN_TARGET_SCOPE_VOICE) continue;
        bool inPath = false;
        for (std::uint32_t i = 0; i < cut.pathCount; ++i)
            inPath |= cut.path[i].stableId == attachment.objectId;
        if (!inPath) continue;
        for (const auto& binding : pack.bindingDefinitions)
            if (binding.id == attachment.bindingId && binding.desc.targetId == VN_TARGET_VOICE_PITCH)
                return true;
    }
    return false;
}

void PreviewFacts(const ContainerWorldInstallViewV1& view, const PackCanonicalImage& pack,
    const PackCanonicalImage& init, VnObjectId actionId,
    std::uint32_t& kind, std::vector<VnPackOccurrenceFactV1>& facts) {
    const ActionProgramViewV1* action = nullptr;
    for (std::uint32_t i = 0; i < view.actionCount; ++i)
        if (view.actions[i].actionStableId == actionId) action = &view.actions[i];
    if (action == nullptr || action->stepCount != 1) return;
    const auto& step = action->steps[0];
    if (step.opcode != ActionOpcodeV1::Play) return;
    PlayableRefV1 root{};
    root.stableId = step.target.stableId;
    root.expectedProgramSemanticFingerprint = step.target.expectedProgramSemanticFingerprint;
    const ContainerProgramViewV3* container = nullptr;
    if (step.target.kind == ActionTargetKindV1::Sound) root.kind = PlayableKindV1::Sound;
    else if (step.target.kind == ActionTargetKindV1::Container) {
        root.kind = PlayableKindV1::Container;
        for (std::uint32_t i = 0; i < view.containerCount; ++i)
            if (view.containers[i].containerStableId == root.stableId) container = &view.containers[i];
        if (container == nullptr || container->mode != ContainerModeV1::Random || container->childCount == 0) return;
        for (std::uint32_t i = 0; i < container->childCount; ++i)
            if (container->children[i].playable.kind != PlayableKindV1::Sound) return;
    } else return;
    const auto property = std::find_if(view.playableProperties, view.playableProperties + view.playablePropertyCount,
        [&](const auto& row) { return row.stableId == root.stableId &&
            row.programSemanticFingerprint == root.expectedProgramSemanticFingerprint; });
    if (property == view.playableProperties + view.playablePropertyCount) return;
    root.contentRevision = property->contentRevision;
    root.routeStableId = property->routeStableId;
    root.compatibilityFingerprint = property->compatibilityFingerprint;

    PlayableWorldViewV1 world{};
    // Local fold context only; no installed-world identity leaves inspection.
    world.generation = 1;
    world.identity = view.identity;
    world.properties = view.playableProperties;
    world.propertyCount = view.playablePropertyCount;
    world.occurrences = view.playableOccurrences;
    world.occurrenceCount = view.playableOccurrenceCount;
    world.containers = view.containers;
    world.containerCount = view.containerCount;
    ResolvedPlayablePropertyCutStorageV1 storage;
    ResolvedPlayablePropertyCutConfigV1 capacity{1, 1, 1, 1, 1};
    for (std::uint32_t i = 0; i < view.playablePropertyCount; ++i) {
        const auto& property = view.playableProperties[i];
        const auto ancestors = std::max(property.structuralAncestorCount, property.organizationLayerAncestorCount);
        if (ancestors > UINT32_MAX - 3u) return;
        capacity.maxPathRows = std::max(capacity.maxPathRows, ancestors + 3u);
        const auto& payload = property.payload;
        capacity.maxSends = std::max(capacity.maxSends, payload.sendCount);
        capacity.maxEffectNodes = std::max(capacity.maxEffectNodes, payload.effectNodeCount);
        capacity.maxEffectEdges = std::max(capacity.maxEffectEdges, payload.effectEdgeCount);
        std::uint64_t params = 0;
        for (std::uint32_t j = 0; j < payload.effectNodeCount; ++j) params += payload.effectNodes[j].paramCount;
        if (params > UINT32_MAX) return;
        capacity.maxEffectParams = std::max(capacity.maxEffectParams, static_cast<std::uint32_t>(params));
    }
    if (!storage.Reset(capacity)) throw std::bad_alloc();
    const auto count = container == nullptr ? 1u : container->childCount;
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto ordinal = container == nullptr ? UINT32_MAX : container->children[i].stableChildOrdinal;
        if (storage.Resolve(world, root, container == nullptr ? nullptr : &ordinal,
                container == nullptr ? 0 : 1) != ResolvedPlayablePropertyCutStatusV1::Ok) {
            facts.clear();
            return;
        }
        const auto& cut = storage.View();
        const auto media = std::find_if(pack.media.begin(), pack.media.end(),
            [&](const auto& row) { return row.soundId == cut.leaf.stableId; });
        if (media == pack.media.end()) { facts.clear(); return; }
        VnPackOccurrenceFactV1 row{};
        row.rootPlayableId = root.stableId;
        row.soundId = cut.leaf.stableId;
        row.rootPlayableKind = static_cast<std::uint32_t>(root.kind);
        row.childOrdinal = ordinal;
        row.rootProgramFingerprint = root.expectedProgramSemanticFingerprint;
        row.soundProgramFingerprint = cut.leaf.expectedProgramSemanticFingerprint;
        for (std::uint32_t j = 0; j < view.playableOccurrenceCount; ++j) {
            const auto& occurrence = view.playableOccurrences[j];
            if (occurrence.parentStableId == root.stableId && occurrence.stableOccurrenceOrdinal == ordinal)
                row.occurrenceFingerprint = occurrence.occurrenceSemanticFingerprint;
        }
        row.mediaChecksum = media->blobChecksum;
        row.mediaFrameCount = media->frameCount;
        row.sampleRate = media->sampleRate;
        row.channelCount = media->channelCount;
        float gain = cut.volumeDb + cut.makeUpGainDb + cut.outputBusVolumeDb;
        if (MainBusGain(init, cut.mainOutputBusRef, gain)) {
            const float linear = std::pow(10.0f, gain / 20.0f);
            if (std::isfinite(linear)) {
                row.flags |= VN_PACK_OCCURRENCE_FIXED_GAIN;
                row.fixedGainDb = gain;
                row.fixedGainLinear = linear;
            }
        }
        const bool simpleTiming = step.delay.numerator == 0 &&
            step.quantizeTiming == ActionQuantizeTimingV1::Immediate &&
            (container == nullptr || container->children[i].timingHint.kind == ChildTimingHintKindV2::Unknown) &&
            !HasPitchBinding(pack, cut) && !HasPitchBinding(init, cut);
        if (simpleTiming && cut.loopCount != 0 && media->sampleRate != 0 && std::isfinite(cut.pitchCents)) {
            const auto loopEnd = media->loopEndFrame == 0 ? media->frameCount : media->loopEndFrame;
            const auto loopFrames = loopEnd - media->loopStartFrame;
            const double frames = static_cast<double>(media->frameCount) +
                static_cast<double>(loopFrames) * (cut.loopCount - 1u);
            const double duration = frames / media->sampleRate / std::exp2(cut.pitchCents / 1200.0);
            if (std::isfinite(duration)) {
                row.flags |= VN_PACK_OCCURRENCE_DURATION;
                row.durationSeconds = duration;
            }
        }
        facts.push_back(row);
    }
    kind = container == nullptr ? VN_PACK_PREVIEW_SOUND : VN_PACK_PREVIEW_RANDOM_SOUNDS;
}
} // namespace

VnResult Vn_InspectCanonicalPackV1(const VnPackInspectDescV1* desc,
                                  VnPackInspectReportV1* report) {
    if (desc == nullptr || report == nullptr || desc->structSize != sizeof(*desc) ||
        report->structSize != sizeof(*report) || desc->flags != 0 ||
        desc->names == nullptr || desc->maxInputBytes == 0 ||
        (desc->variableCapacity != 0 && desc->variables == nullptr) ||
        (desc->labelCapacity != 0 && desc->labels == nullptr) ||
        (desc->occurrenceCapacity != 0 && desc->occurrences == nullptr) ||
        (desc->mediaCapacity != 0 && desc->media == nullptr)) return VN_INVALID_ARG;
    try {
        const auto valid = [](const VnPackByteSpan& span) {
            return span.structSize == sizeof(span) && span.reserved0 == 0 &&
                span.bytes != nullptr && span.byteCount != 0 && span.byteCount <= SIZE_MAX;
        };
        if (!valid(desc->pack)) return VN_INVALID_ARG;
        auto budget = desc->maxInputBytes;
        PackCompanionNames names;
        const auto captured = CapturePackCompanionNames(*desc->names, budget, &names);
        if (captured != VN_OK) return captured;
        if (desc->pack.byteCount > budget) return VN_PACK_RESIDENT_CAPACITY;
        budget -= desc->pack.byteCount;
        PackCanonicalImage pack, initImage;
        CurrentWorldBodyImage body, initBody;
        auto result = Decode(desc->pack, pack, body);
        if (result != VN_OK) return result;
        const bool isInit = pack.packKind == kPackKindInit;
        if (!isInit && pack.packKind != kPackKindCanonicalContent) return VN_BAD_PACK;
        if (!isInit) {
            if (!valid(desc->init)) return VN_INVALID_ARG;
            if (desc->init.byteCount > budget) return VN_PACK_RESIDENT_CAPACITY;
            result = Decode(desc->init, initImage, initBody);
            if (result != VN_OK) return result;
            if (initImage.packKind != kPackKindInit ||
                initBody.ProductionView()->identity.projectId != body.ProductionView()->identity.projectId)
                return VN_BAD_PACK;
        }
        const auto& init = isInit ? pack : initImage;
        if (!init.hasBusDefinitionSnapshot) return VN_BAD_PACK;
        if (!isInit) {
            bool initEdge = false;
            for (const auto& edge : pack.dependencyEdges) {
                if (!edge.required01) continue;
                if (edge.edgeKind != kPackDependencyEdgeKindInit ||
                    edge.prerequisitePackStableId != init.packStableId) return VN_CONTENT_NOT_LOADED;
                initEdge = true;
                const auto& bus = init.busDefinitionSnapshotHeader;
                if ((edge.expectedPrerequisiteContentRevision != 0 &&
                     edge.expectedPrerequisiteContentRevision != init.requirement.packContentRevision) ||
                    (edge.expectedBusContentGeneration != 0 && edge.expectedBusContentGeneration != bus.contentGeneration) ||
                    (edge.expectedPublishedBusFingerprint != 0 && edge.expectedPublishedBusFingerprint != bus.publishedBusFingerprint) ||
                    (edge.expectedCompileTargetProfileFingerprint != 0 &&
                     edge.expectedCompileTargetProfileFingerprint != bus.compileTargetProfileFingerprint)) return VN_PACK_STALE_REVISION;
            }
            if (!initEdge) return VN_BAD_PACK;
        }
        const auto& view = *body.ProductionView();
        result = ValidatePackCompanionNames(names, pack.packKind, pack.packStableId,
            init.packStableId, view, pack.variablePolicyOverlays);
        if (result != VN_OK) return result;
        std::vector<VnPackVariableFactV1> variables;
        std::vector<VnPackLabelFactV1> labels;
        std::vector<VnPackOccurrenceFactV1> occurrences;
        std::vector<VnPackMediaFactV1> media;
        for (const auto& row : pack.media)
            media.push_back({row.soundId, row.blobChecksum, row.frameCount, row.sampleRate, row.channelCount});
        for (std::uint32_t i = 0; i < view.variableCount; ++i) {
            const auto& variable = view.variables[i];
            VnPackVariableFactV1 row{};
            row.variableId = variable.variableId;
            row.kind = static_cast<std::uint32_t>(variable.kind);
            row.rangeMin = variable.rangeMin;
            row.rangeMax = variable.rangeMax;
            row.defaultValue = variable.defaultValue;
            for (const auto& overlay : pack.variablePolicyOverlays) {
                if (overlay.variableId != row.variableId) continue;
                row.writers = overlay.writers;
                row.executionFingerprint = overlay.executionFingerprint;
                row.labelMappingFingerprint = overlay.labelMappingFingerprint;
                for (const auto& label : overlay.labels)
                    labels.push_back({row.variableId, label.labelId, label.anchor, 0});
            }
            variables.push_back(row);
        }
        VnPackInspectReportV1 output{};
        output.structSize = sizeof(output);
        output.packKind = pack.packKind;
        output.projectId = view.identity.projectId;
        output.rootActionId = names.rootActionId;
        output.packStableId = pack.packStableId;
        output.initPackStableId = init.packStableId;
        output.contentRevision = pack.requirement.packContentRevision;
        output.contentHash = Fnv1a64(desc->pack.bytes, static_cast<std::size_t>(desc->pack.byteCount));
        output.initContentHash = isInit ? output.contentHash :
            Fnv1a64(desc->init.bytes, static_cast<std::size_t>(desc->init.byteCount));
        output.executionFingerprint = view.identity.runtimeSemanticFingerprint;
        if (!isInit) PreviewFacts(view, pack, init, names.rootActionId, output.previewKind, occurrences);
        output.variableCount = static_cast<std::uint32_t>(variables.size());
        output.labelCount = static_cast<std::uint32_t>(labels.size());
        output.occurrenceCount = static_cast<std::uint32_t>(occurrences.size());
        output.mediaCount = static_cast<std::uint32_t>(media.size());
        *report = output;
        if (desc->variableCapacity == 0 && desc->labelCapacity == 0 && desc->occurrenceCapacity == 0 &&
            desc->mediaCapacity == 0 && desc->variables == nullptr && desc->labels == nullptr &&
            desc->occurrences == nullptr && desc->media == nullptr) return VN_OK;
        if (desc->variableCapacity < variables.size() || desc->labelCapacity < labels.size() ||
            desc->occurrenceCapacity < occurrences.size() || desc->mediaCapacity < media.size())
            return VN_PACK_RESIDENT_CAPACITY;
        if (!variables.empty()) std::copy(variables.begin(), variables.end(), desc->variables);
        if (!labels.empty()) std::copy(labels.begin(), labels.end(), desc->labels);
        if (!occurrences.empty()) std::copy(occurrences.begin(), occurrences.end(), desc->occurrences);
        if (!media.empty()) std::copy(media.begin(), media.end(), desc->media);
        return VN_OK;
    } catch (const std::bad_alloc&) { return VN_PACK_RESIDENT_CAPACITY; }
    catch (...) { return VN_BAD_PACK; }
}
