#include <harken_studio_build/BindingTransition.h>
#include <harken_studio_build/AuthoredParameters.h>
#include "harken_studio_build/PackImage.h"

#include "harken_studio_build/WorldCodec.h"
#include "harken_studio_build/BuildLimits.h"
#include "harken_studio_build/WorldValidation.h"
#include "harken_studio_build/EffectEncoding.h"

#include <algorithm>
#include <exception>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <map>
#include <set>
#include "harken_studio_build/VariableExecutionFingerprint.h"
#include <utility>

namespace harkenstudiobuild {

namespace {
bool ValidateV2MediaEntry(const PackTocEntry& e, std::uint64_t fileBytes,
                          std::uint64_t blobRegionStart) {
    if (e.sampleFormat != kPackSampleFormatF32Planar) {
        return false;
    }
    if ((e.flags & ~kPackKnownFlagsMask) != 0) {
        return false;
    }
    if (e.sampleRate == 0 || e.channelCount == 0 ||
        e.channelCount > kMaxChannels || e.frameCount == 0) {
        return false;
    }
    const std::uint64_t channels = e.channelCount;
    if (e.frameCount > UINT64_MAX / (channels * sizeof(float))) {
        return false;
    }
    if (e.blobBytes != e.frameCount * channels * sizeof(float)) {
        return false;
    }
    if (e.blobOffset % kPackBlobAlign != 0) {
        return false;
    }
    if (e.blobOffset < blobRegionStart || e.blobOffset > fileBytes ||
        e.blobBytes > fileBytes - e.blobOffset) {
        return false;
    }
    if ((e.flags & kPackFlagLoopable) != 0) {
        const std::uint64_t loopEnd =
            e.loopEndFrame == 0 ? e.frameCount : e.loopEndFrame;
        if (loopEnd > e.frameCount || e.loopStartFrame >= loopEnd) {
            return false;
        }
    } else if (e.loopStartFrame != 0 || e.loopEndFrame != 0) {
        return false;
    }
    return true;
}
bool KindLegalForPack(std::uint32_t entryKind, std::uint32_t packKind) {
    switch (entryKind) {
    case kPackEntryKindMedia:
    case kPackEntryKindContainer:
    case kPackEntryKindVoiceRecipe:
        return packKind == kPackKindContent;
    case kPackEntryKindAction:
        return false;
    case kPackEntryKindBus:
    case kPackEntryKindBusSend:
    case kPackEntryKindAttenuation:
    case kPackEntryKindBusEffect:
    case kPackEntryKindEffectDynamicRoute:
        return packKind == kPackKindResident;
    case kPackEntryKindBindingDefinition:
    case kPackEntryKindBindingAttachment:
        return true;
    default:
        return false;
    }
}

bool VoiceToneWireValuesValid(const PackVoiceRecipeTone& tone) {
    if (tone.present == 0) {
        return true;
    }
    const float values[] = {
        tone.lowpassFrequencyHz, tone.lowpassQ,
        tone.highpassFrequencyHz, tone.highpassQ,
        tone.lowShelfFrequencyHz, tone.lowShelfQ,
        tone.lowShelfGainDb, tone.highShelfFrequencyHz,
        tone.highShelfQ, tone.highShelfGainDb,
    };
    for (float value : values) {
        if (!std::isfinite(value)) {
            return false;
        }
    }
    return tone.lowpassFrequencyHz > 0.0f &&
           tone.highpassFrequencyHz > 0.0f &&
           tone.lowShelfFrequencyHz > 0.0f &&
           tone.highShelfFrequencyHz > 0.0f &&
           tone.lowpassQ >= 0.1f && tone.lowpassQ <= 10.0f &&
           tone.highpassQ >= 0.1f && tone.highpassQ <= 10.0f &&
           tone.lowShelfQ >= 0.1f && tone.lowShelfQ <= 10.0f &&
           tone.highShelfQ >= 0.1f && tone.highShelfQ <= 10.0f &&
           tone.lowShelfGainDb >= -48.0f &&
           tone.lowShelfGainDb <= 48.0f &&
           tone.highShelfGainDb >= -48.0f &&
           tone.highShelfGainDb <= 48.0f;
}

template <typename T, typename IdFn>
bool HasDuplicateId(const std::vector<T>& entries, IdFn&& idOf) {
    for (std::size_t i = 0; i < entries.size(); ++i) {
        for (std::size_t j = i + 1; j < entries.size(); ++j) {
            if (idOf(entries[i]) == idOf(entries[j])) {
                return true;
            }
        }
    }
    return false;
}

} // namespace

bool ValidateCanonicalBusParentForestV2(
    std::span<const PackBusDefinitionBus> buses) {
    if (buses.empty() || buses.size() > 4096) return false;
    std::map<harkenstudio::ObjectId, std::size_t> indices;
    for (std::size_t i = 0; i < buses.size(); ++i) {
        if (harkenstudio::IsNull(buses[i].busId) ||
            !indices.emplace(buses[i].busId, i).second) return false;
    }
    const auto master = indices.find(harkenstudio::kMasterBusObjectId);
    if (master == indices.end() ||
        !harkenstudio::IsNull(buses[master->second].parentId)) return false;

    std::vector<std::uint8_t> state(buses.size(), 0);
    std::vector<std::size_t> path;
    path.reserve(buses.size());
    state[master->second] = 2;
    for (std::size_t i = 0; i < buses.size(); ++i) {
        path.clear();
        std::size_t current = i;
        while (state[current] == 0) {
            state[current] = 1;
            path.push_back(current);
            const auto parent = indices.find(buses[current].parentId);
            if (parent == indices.end()) return false;
            current = parent->second;
        }
        if (state[current] != 2) return false;
        for (const std::size_t index : path) state[index] = 2;
    }
    return true;
}

bool ValidateCanonicalBusSendGraphV2(
    std::span<const PackBusDefinitionBus> buses,
    std::span<const PackBusDefinitionSend> sends) {
    std::map<harkenstudio::ObjectId, std::size_t> indices;
    for (std::size_t i = 0; i < buses.size(); ++i) {
        indices.emplace(buses[i].busId, i);
    }
    std::vector<std::vector<std::size_t>> edges(buses.size());
    std::vector<std::size_t> indegree(buses.size(), 0);
    const auto addEdge = [&](harkenstudio::ObjectId source,
                             harkenstudio::ObjectId target) {
        const auto from = indices.find(source);
        const auto to = indices.find(target);
        if (from == indices.end() || to == indices.end()) return false;
        edges[from->second].push_back(to->second);
        ++indegree[to->second];
        return true;
    };
    for (const auto& bus : buses) {
        if (!harkenstudio::IsNull(bus.parentId) &&
            !addEdge(bus.busId, bus.parentId)) return false;
    }
    for (const auto& send : sends) {
        if (!addEdge(send.source, send.target)) return false;
    }
    std::vector<std::size_t> ready;
    ready.reserve(buses.size());
    for (std::size_t i = 0; i < buses.size(); ++i) {
        if (indegree[i] == 0) ready.push_back(i);
    }
    for (std::size_t head = 0; head < ready.size(); ++head) {
        for (const auto target : edges[ready[head]]) {
            if (--indegree[target] == 0) ready.push_back(target);
        }
    }
    return ready.size() == buses.size();
}

harkenstudiobuild::PackResult ParsePackV2Image(const std::uint8_t* bytes, std::uint64_t fileBytes,
                          PackLogicImage* out) {
    if (bytes == nullptr || fileBytes < kPackFileHeaderBytes) {
        return harkenstudiobuild::kPackInvalid;
    }
    PackFileHeader header;
    if (!ReadPackFileHeader(bytes, &header)) {
        return harkenstudiobuild::kPackInvalid;
    }
    if (header.formatVersion != kPackFormatVersion ||
        header.headerBytes != kPackFileHeaderBytes ||
        header.fileBytes != fileBytes) {
        return harkenstudiobuild::kPackInvalid;
    }
    if (PackIsCanonicalKind(header.packKind)) {
        return harkenstudiobuild::kPackIncompatible;
    }
    if (header.packKind != kPackKindResident &&
        header.packKind != kPackKindContent) {
        return harkenstudiobuild::kPackInvalid;
    }
    if (header.entryCount == 0 &&
        (header.packKind != kPackKindContent ||
         header.fileBytes != header.headerBytes)) {
        return harkenstudiobuild::kPackInvalid;
    }
    const std::uint64_t dirBytes =
        static_cast<std::uint64_t>(header.entryCount) *
        kPackDirectoryEntryBytes;
    if (header.directoryOffset != header.headerBytes ||
        header.directoryOffset > fileBytes ||
        dirBytes > fileBytes - header.directoryOffset) {
        return harkenstudiobuild::kPackInvalid;
    }
    const std::uint8_t* dir = bytes + header.directoryOffset;
    if (Fnv1a64(dir, static_cast<std::size_t>(dirBytes)) !=
        header.directoryChecksum) {
        return harkenstudiobuild::kPackInvalid;
    }
    const std::uint64_t dirEnd = header.directoryOffset + dirBytes;

    PackLogicImage image;
    image.packKind = header.packKind;
    std::uint64_t sendOrdinal = 0;
    std::uint64_t attachmentOrdinal = 0;
    std::uint64_t busEffectOrdinal = 0;
    std::uint64_t dynamicRouteOrdinal = 0;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    ranges.reserve(static_cast<std::size_t>(header.entryCount) * 2);

    for (std::uint32_t i = 0; i < header.entryCount; ++i) {
        const PackDirectoryEntry entry = ReadPackDirectoryEntry(
            dir + static_cast<std::size_t>(i) * kPackDirectoryEntryBytes);
        if (!PackEntryKindSupported(entry.kind)) return kPackInvalid;
        if (PackIsCanonicalEntryKind(entry.kind)) {
            return harkenstudiobuild::kPackIncompatible;
        }
        if (!VintageLegalForKind(entry.kind, entry.vintage)) {
            return harkenstudiobuild::kPackIncompatible;
        }
        if (!KindLegalForPack(entry.kind, header.packKind)) {
            return harkenstudiobuild::kPackInvalid;
        }
        if (entry.offset < dirEnd || entry.offset > fileBytes ||
            entry.size > fileBytes - entry.offset) {
            return harkenstudiobuild::kPackInvalid;
        }
        if (entry.offset % kPackPayloadAlign != 0) {
            return harkenstudiobuild::kPackInvalid; // the writer aligns every payload
        }
        ranges.emplace_back(entry.offset, entry.size);
        const std::uint8_t* payload = bytes + entry.offset;
        if (Fnv1a64(payload, static_cast<std::size_t>(entry.size)) !=
            entry.payloadChecksum) {
            return harkenstudiobuild::kPackInvalid;
        }
        image.stamps.push_back(
            {entry.kind, entry.id, entry.payloadChecksum});
        switch (entry.kind) {
        case kPackEntryKindMedia: {
            if (entry.size != kPackMediaBytes) {
                return harkenstudiobuild::kPackInvalid;
            }
            const PackTocEntry media = ReadPackMedia(payload);
            if (media.soundId != entry.id ||
                !ValidateV2MediaEntry(media, fileBytes, dirEnd)) {
                return harkenstudiobuild::kPackInvalid;
            }
            if (Fnv1a64(bytes + media.blobOffset,
                        static_cast<std::size_t>(media.blobBytes)) !=
                media.blobChecksum) {
                return harkenstudiobuild::kPackInvalid;
            }
            ranges.emplace_back(media.blobOffset, media.blobBytes);
            image.media.push_back(media);
            break;
        }
        case kPackEntryKindBus: {
            PackBusEntry bus;
            const bool parsed =
                ParsePackBus(payload, entry.size, &bus);
            if (!parsed || bus.busId != entry.id ||
                !std::isfinite(bus.volume) ||
                !std::isfinite(bus.outputBusVolume)) {
                return harkenstudiobuild::kPackInvalid;
            }
            image.buses.push_back(bus);
            break;
        }
        case kPackEntryKindBusSend: {
            PackBusSendEntry send;
            if (!ParsePackBusSend(payload, entry.size, &send) ||
                entry.relationOrdinal != sendOrdinal) {
                return harkenstudiobuild::kPackInvalid;
            }
            if (!std::isfinite(send.levelDb)) {
                return harkenstudiobuild::kPackInvalid;
            }
            ++sendOrdinal;
            image.busSends.push_back(send);
            break;
        }
        case kPackEntryKindBusEffect: {
            PackBusEffectEntry effect;
            if (!ReadPackBusEffect(payload, entry.size, &effect) ||
                entry.relationOrdinal != busEffectOrdinal || effect.busId == harkenstudio::ObjectId{} ||
                effect.bypass > 1 ||
                effect.slot >= kMaxFxSlotsPerBus ||
                effect.faderPointGuard > kMaxFxSlotsPerBus) {
                return harkenstudiobuild::kPackInvalid;
            }
            const PackEffectSchemaRow* effectSchema =
                FindPackEffectSchemaByType(effect.effectType);
            if (effectSchema == nullptr ||
                effect.descriptorVintage !=
                    effectSchema->descriptorVintage ||
                effect.paramCount != effectSchema->paramCount) {
                return harkenstudiobuild::kPackInvalid;
            }
            for (std::uint32_t p = 0; p < effect.paramCount; ++p) {
                if (!std::isfinite(effect.params[p])) {
                    return harkenstudiobuild::kPackInvalid;
                }
            }
            ++busEffectOrdinal;
            image.busEffects.push_back(effect);
            break;
        }
        case kPackEntryKindEffectDynamicRoute: {
            PackEffectDynamicRouteEntry route;
            if (!ParsePackEffectDynamicRoute(payload, entry.size,
                                               &route) ||
                entry.relationOrdinal != dynamicRouteOrdinal ||
                route.targetBusId == harkenstudio::ObjectId{} || route.sourceId == harkenstudio::ObjectId{} ||
                route.expectedEffectType != harkenstudio::kEffectCompressor ||
                route.signalForm != harkenstudio::kDynamicSignalRawAudio ||
                route.sourceKind != harkenstudio::kDynamicSourceBus ||
                route.tap > harkenstudio::kDynamicTapBusPostFader ||
                route.effectSlot >= kMaxFxSlotsPerBus ||
                route.routeSlot >= 4u ||
                !std::isfinite(route.levelDb) ||
                route.levelDb < -96.0f || route.levelDb > 24.0f) {
                return harkenstudiobuild::kPackInvalid;
            }
            ++dynamicRouteOrdinal;
            image.dynamicRoutes.push_back(route);
            break;
        }
        case kPackEntryKindAttenuation: {
            PackLogicImage::Attenuation attenuation;
            attenuation.id = entry.id;
            if (!ReadPackAttenuation(payload, entry.size,
                                        &attenuation.desc)) {
                return harkenstudiobuild::kPackInvalid;
            }
            if (!std::isfinite(attenuation.desc.proximityFrequency) ||
                !(attenuation.desc.proximityFrequency > 0.0f)) {
                return harkenstudiobuild::kPackInvalid;
            }
            image.attenuations.push_back(attenuation);
            break;
        }
        case kPackEntryKindDiscreteVariable:
        case kPackEntryKindMeterWiring:
            return harkenstudiobuild::kPackInvalid;
        case kPackEntryKindBindingDefinition: {
            PackLogicImage::BindingDefinition definition;
            if (!ReadPackBindingDefinition(payload, entry.size,
                                              &definition.id,
                                              &definition.desc) ||
                definition.id != entry.id
                || !IsValidBindingTransitionMs(definition.desc.transitionMs)) {
                return harkenstudiobuild::kPackInvalid;
            }
            if (definition.desc.targetScopeKind >
                harkenstudio::kTargetScopeBusEffectParam) {
                return harkenstudiobuild::kPackInvalid;
            }
            if (header.packKind == kPackKindResident &&
                (definition.desc.targetScopeKind == harkenstudio::kTargetScopeVoice ||
                 definition.desc.targetScopeKind ==
                     harkenstudio::kTargetScopeVoiceEffectParam)) {
                return harkenstudiobuild::kPackInvalid;
            }
            image.bindingDefinitions.push_back(definition);
            break;
        }
        case kPackEntryKindBindingAttachment: {
            PackBindingAttachmentEntry attachment;
            if (!ParsePackBindingAttachment(payload, entry.size,
                                              &attachment) ||
                entry.relationOrdinal != attachmentOrdinal) {
                return harkenstudiobuild::kPackInvalid;
            }
            ++attachmentOrdinal;
            if (attachment.scopeKind ==
                    harkenstudio::kTargetScopeVoiceEffectParam ||
                attachment.scopeKind >
                    harkenstudio::kTargetScopeBusEffectParam) {
                return harkenstudiobuild::kPackInvalid;
            }
            if (header.packKind == kPackKindResident &&
                attachment.scopeKind != harkenstudio::kTargetScopeBus &&
                attachment.scopeKind !=
                    harkenstudio::kTargetScopeBusEffectParam) {
                return harkenstudiobuild::kPackInvalid;
            }
            image.bindingAttachments.push_back(attachment);
            break;
        }
        case kPackEntryKindContainer: {
            PackLogicImage::Container container;
            if (!ReadPackContainerPayload(payload, entry.size, entry.id, container) ||
                container.header.childCount == 0) {
                return harkenstudiobuild::kPackInvalid;
            }
            image.containers.push_back(std::move(container));
            break;
        }
        case kPackEntryKindVoiceRecipe: {
            PackLogicImage::VoiceRecipe recipe;
            if (!ReadPackVoiceRecipePayload(payload, entry.size, entry.id, recipe) ||
                !VoiceToneWireValuesValid(recipe.tone)) {
                return harkenstudiobuild::kPackInvalid;
            }
            std::uint32_t previousSlot = 0;
            for (std::uint32_t n = 0; n < recipe.header.installCount; ++n) {
                const auto& install = recipe.installs[n];
                if (                    install.slot >= kPackVoiceRecipeMaxInstalls ||
                    (n > 0 && install.slot <= previousSlot)) {
                    return harkenstudiobuild::kPackInvalid;
                }
                const PackEffectSchemaRow* effectSchema =
                    FindPackEffectSchemaByType(install.effectType);
                if (effectSchema == nullptr ||
                    install.descriptorVintage !=
                        effectSchema->descriptorVintage ||
                    install.paramCount != effectSchema->paramCount) {
                    return harkenstudiobuild::kPackInvalid;
                }
                for (std::uint32_t p = 0; p < install.paramCount; ++p) {
                    if (!std::isfinite(install.params[p])) {
                        return harkenstudiobuild::kPackInvalid;
                    }
                }
                previousSlot = install.slot;
            }
            harkenstudio::ObjectId previousBinding{};
            for (std::uint32_t n = 0; n < recipe.header.bindingCount; ++n) {
                const auto binding = recipe.bindings[n];
                if (binding == harkenstudio::ObjectId{} ||
                    (n > 0 && binding <= previousBinding)) {
                    return harkenstudiobuild::kPackInvalid;
                }
                previousBinding = binding;
            }
            image.voiceRecipes.push_back(std::move(recipe));
            break;
        }
        default:
            return harkenstudiobuild::kPackInvalid;
        }
    }
    std::sort(ranges.begin(), ranges.end());
    for (std::size_t i = 1; i < ranges.size(); ++i) {
        if (ranges[i].first < ranges[i - 1].first + ranges[i - 1].second) {
            return harkenstudiobuild::kPackInvalid;
        }
    }
    if (HasDuplicateId(image.media,
                       [](const PackTocEntry& e) { return e.soundId; }) ||
        HasDuplicateId(image.buses,
                       [](const PackBusEntry& e) { return e.busId; }) ||
        HasDuplicateId(image.attenuations,
                       [](const PackLogicImage::Attenuation& e) {
                           return e.id;
                       }) ||
        HasDuplicateId(image.bindingDefinitions,
                       [](const PackLogicImage::BindingDefinition& e) {
                           return e.id;
                       }) ||
        HasDuplicateId(image.containers,
                       [](const PackLogicImage::Container& e) {
                           return e.header.containerId;
                       }) ||
        HasDuplicateId(image.voiceRecipes,
                       [](const PackLogicImage::VoiceRecipe& e) {
                           return e.header.playableId;
                       })) {
        return harkenstudiobuild::kPackInvalid;
    }
    for (const PackTocEntry& media : image.media) {
        for (const PackLogicImage::Container& container : image.containers) {
            if (media.soundId == container.header.containerId) {
                return harkenstudiobuild::kPackInvalid;
            }
        }
    }
    for (std::size_t i = 0; i < image.busSends.size(); ++i) {
        for (std::size_t j = i + 1; j < image.busSends.size(); ++j) {
            if (image.busSends[i].fromBusId == image.busSends[j].fromBusId &&
                image.busSends[i].toBusId == image.busSends[j].toBusId) {
                return harkenstudiobuild::kPackInvalid;
            }
        }
    }
    for (std::size_t i = 0; i < image.bindingAttachments.size(); ++i) {
        for (std::size_t j = i + 1; j < image.bindingAttachments.size(); ++j) {
            const PackBindingAttachmentEntry& a = image.bindingAttachments[i];
            const PackBindingAttachmentEntry& b = image.bindingAttachments[j];
            if (a.bindingId == b.bindingId && a.scopeKind == b.scopeKind &&
                a.objectId == b.objectId) {
                return harkenstudiobuild::kPackInvalid;
            }
        }
    }
    for (std::size_t i = 0; i < image.busEffects.size(); ++i) {
        if (i > 0) {
            const PackBusEffectEntry& previous = image.busEffects[i - 1];
            const PackBusEffectEntry& current = image.busEffects[i];
            if (current.busId < previous.busId ||
                (current.busId == previous.busId &&
                 current.slot <= previous.slot)) {
                return harkenstudiobuild::kPackInvalid;
            }
        }
        for (std::size_t j = i + 1; j < image.busEffects.size(); ++j) {
            if (image.busEffects[i].busId == image.busEffects[j].busId &&
                image.busEffects[i].slot == image.busEffects[j].slot) {
                return harkenstudiobuild::kPackInvalid;
            }
        }
    }
    for (std::size_t i = 0; i < image.dynamicRoutes.size(); ++i) {
        if (i > 0) {
            const PackEffectDynamicRouteEntry& previous =
                image.dynamicRoutes[i - 1];
            const PackEffectDynamicRouteEntry& current =
                image.dynamicRoutes[i];
            if (current.targetBusId < previous.targetBusId ||
                (current.targetBusId == previous.targetBusId &&
                 (current.effectSlot < previous.effectSlot ||
                  (current.effectSlot == previous.effectSlot &&
                   current.routeSlot <= previous.routeSlot)))) {
                return harkenstudiobuild::kPackInvalid;
            }
        }
        for (std::size_t j = i + 1; j < image.dynamicRoutes.size(); ++j) {
            const PackEffectDynamicRouteEntry& a =
                image.dynamicRoutes[i];
            const PackEffectDynamicRouteEntry& b =
                image.dynamicRoutes[j];
            if (a.targetBusId == b.targetBusId &&
                a.effectSlot == b.effectSlot &&
                a.routeSlot == b.routeSlot) {
                return harkenstudiobuild::kPackInvalid;
            }
        }
    }
    for (const PackEffectDynamicRouteEntry& route : image.dynamicRoutes) {
        bool declaredTarget = false;
        for (const PackBusEffectEntry& effect : image.busEffects) {
            if (effect.busId == route.targetBusId &&
                effect.slot == route.effectSlot &&
                effect.effectType == route.expectedEffectType) {
                declaredTarget = true;
                break;
            }
        }
        if (!declaredTarget) {
            return harkenstudiobuild::kPackInvalid;
        }
    }
    for (const PackBusSendEntry& send : image.busSends) {
        if (send.fromBusId == send.toBusId) {
            return harkenstudiobuild::kPackInvalid;
        }
    }

    *out = std::move(image);
    return harkenstudiobuild::kPackOk;
}

namespace {

struct PackVariableReadState {
    std::exception_ptr exception;
    template <class Callback>
    bool Attempt(Callback&& callback) noexcept {
        try {
            return callback();
        } catch (...) {
            exception = std::current_exception();
            return false;
        }
    }
};

PackResult ParseVariablePolicyOverlayPayload(
    const std::uint8_t* payload, std::uint64_t size,
    std::vector<PackCanonicalImage::VariablePolicyOverlay>* out) {
    if (payload == nullptr || out == nullptr || size < kPackVariablePolicyHeaderBytes) return kPackInvalid;
    using Policy = PackCanonicalImage::VariablePolicyOverlay;
    struct PolicyRecord {
        Policy& target;
        PackVariablePolicyRow wire{};
        auto& Wire() noexcept { return wire; }
        bool Finish() noexcept {
            target.variableId = wire.variableId;
            target.writers = wire.writers;
            target.executionFingerprint = wire.executionFingerprint;
            return target.variableId != harkenstudio::ObjectId{} && target.executionFingerprint != 0 && target.writers >= 1 && target.writers <= 3;
        }
    };
    struct AnchorVisitor {
        Policy& target;
        PackVariableReadState& state;
        std::uint64_t maximum;
        PackVariableAnchorHeader header{};
        auto& Value() noexcept { return header; }
        auto& Header() noexcept { return header; }
        bool PrepareAnchors(std::uint64_t count) noexcept {
            return count <= maximum && state.Attempt([&] { target.discreteAnchors.resize(static_cast<std::size_t>(count)); return true; });
        }
        auto& Anchors(std::uint64_t index) noexcept { return target.discreteAnchors[static_cast<std::size_t>(index)]; }
        bool Finish() noexcept {
            for (std::size_t i = 1; i < target.discreteAnchors.size(); ++i)
                if (target.discreteAnchors[i] <= target.discreteAnchors[i - 1]) return false;
            return true;
        }
    };
    struct LabelRecord {
        VariableLabelDefinitionV1& target;
        PackVariableLabel wire{};
        auto& Wire() noexcept { return wire; }
        bool Finish() noexcept {
            target.labelId = wire.labelId;
            target.anchor = wire.anchor;
            return target.labelId != harkenstudio::ObjectId{} && target.anchor >= 0 && target.anchor <= 16777216;
        }
    };
    struct LabelVisitor {
        Policy& target;
        PackVariableReadState& state;
        std::uint64_t maximum;
        PackVariableLabelHeader header{};
        auto& Value() noexcept { return header; }
        auto& Header() noexcept { return header; }
        bool PrepareLabels(std::uint64_t count) noexcept {
            return header.labelMappingFingerprint != 0 && count <= maximum && state.Attempt([&] {
                target.labelMappingFingerprint = header.labelMappingFingerprint;
                target.labels.resize(static_cast<std::size_t>(count));
                return true;
            });
        }
        LabelRecord Labels(std::uint64_t index) noexcept { return {target.labels[static_cast<std::size_t>(index)]}; }
        bool Finish() noexcept {
            return state.Attempt([&] {
                std::set<harkenstudio::ObjectId> identities;
                for (std::size_t i = 0; i < target.labels.size(); ++i) {
                    const auto& label = target.labels[i];
                    if (!identities.insert(label.labelId).second || (i > 0 && label.anchor <= target.labels[i - 1].anchor)) return false;
                }
                return ComputeVariableLabelMappingFingerprintV1(target.variableId, target.labels) == target.labelMappingFingerprint;
            });
        }
    };
    struct Visitor {
        std::vector<Policy>& rows;
        PackVariableReadState& state;
        std::uint64_t bytes;
        PackVariablePolicyHeader header{};
        auto& Value() noexcept { return header; }
        auto& Header() noexcept { return header; }
        bool PrepareRows(std::uint64_t count) noexcept {
            return count <= (bytes - kPackVariablePolicyHeaderBytes) / kPackVariablePolicyRowBytes && state.Attempt([&] {
                rows.resize(static_cast<std::size_t>(count)); return true;
            });
        }
        PolicyRecord Rows(std::uint64_t index) noexcept { return {rows[static_cast<std::size_t>(index)]}; }
        bool PrepareAnchorGroups(std::uint64_t count) noexcept {
            if (count != rows.size()) return false;
            for (std::size_t i = 1; i < rows.size(); ++i) if (rows[i].variableId <= rows[i - 1].variableId) return false;
            return true;
        }
        AnchorVisitor AnchorGroups(std::uint64_t index) noexcept { return {rows[static_cast<std::size_t>(index)], state, bytes / kPackVariableAnchorBytes}; }
        bool PrepareLabelGroups(std::uint64_t count) noexcept { return count == rows.size(); }
        LabelVisitor LabelGroups(std::uint64_t index) noexcept { return {rows[static_cast<std::size_t>(index)], state, bytes / kPackVariableLabelBytes}; }
        bool Finish() noexcept { return true; }
    };
    std::vector<Policy> rows;
    PackVariableReadState state;
    Visitor visitor{rows, state, size};
    PackWireBuffer<const std::uint8_t> buffer{payload, size};
    const bool parsed = ReadPackVariablePolicyPayload(buffer, visitor);
    if (state.exception) std::rethrow_exception(state.exception);
    if (!parsed || buffer.offset != size) return kPackInvalid;
    *out = std::move(rows);
    return kPackOk;
}

PackResult ParseVariableRequirementsPayload(
    const std::uint8_t* payload, std::uint64_t size,
    std::vector<PackCanonicalImage::VariableRequirement>* out) {
    if (payload == nullptr || out == nullptr || size < kPackVariableRequirementsHeaderBytes) return kPackInvalid;
    using Requirement = PackCanonicalImage::VariableRequirement;
    struct Record {
        Requirement& target;
        PackVariableRequirement wire{};
        auto& Wire() noexcept { return wire; }
        bool Finish() noexcept {
            target.identityClass = 1;
            target.stableId = wire.stableId;
            target.expectedExecutionFingerprint = wire.expectedExecutionFingerprint;
            target.expectedLabelMappingFingerprint = wire.expectedLabelMappingFingerprint;
            return target.stableId != harkenstudio::ObjectId{} && target.expectedExecutionFingerprint != 0 && target.expectedLabelMappingFingerprint != 0;
        }
    };
    struct Visitor {
        std::vector<Requirement>& rows;
        PackVariableRequirementsHeader header{};
        auto& Value() noexcept { return header; }
        auto& Header() noexcept { return header; }
        bool PrepareRows(std::uint64_t count) noexcept { return count == rows.size(); }
        Record Rows(std::uint64_t index) noexcept { return {rows[static_cast<std::size_t>(index)]}; }
        bool Finish() noexcept {
            for (std::size_t i = 1; i < rows.size(); ++i) if (rows[i].stableId <= rows[i - 1].stableId) return false;
            return true;
        }
    };
    PackVariableRequirementsHeader header;
    if (!ParsePackVariableRequirementsHeader(payload, kPackVariableRequirementsHeaderBytes, &header)) return kPackInvalid;
    std::uint64_t expected = 0;
    std::vector<Requirement> rows;
    Visitor visitor{rows, header};
    if (!MeasurePackVariableRequirementsPayload(visitor, expected) || expected != size) return kPackInvalid;
    rows.resize(header.rowCount);
    PackWireBuffer<const std::uint8_t> buffer{payload, size};
    if (!ReadPackVariableRequirementsPayload(buffer, visitor) || buffer.offset != size) return kPackInvalid;
    *out = std::move(rows);
    return kPackOk;
}





struct PackBusDefinitionSnapshotReader {
    PackBusDefinitionSnapshotHeader& header;
    std::vector<PackBusDefinitionBus>& buses;
    std::vector<PackBusDefinitionSend>& sends;
    std::vector<PackBusDefinitionDownmix>& downmixes;
    std::vector<float>& coefficients;
    auto& Value() noexcept { return header; }
    auto& Header() noexcept { return header; }
    bool PrepareBuses(std::uint64_t count) noexcept { return count == buses.size(); }
    auto& Buses(std::uint64_t i) noexcept { return buses[static_cast<std::size_t>(i)]; }
    bool PrepareSends(std::uint64_t count) noexcept { return count == sends.size(); }
    auto& Sends(std::uint64_t i) noexcept { return sends[static_cast<std::size_t>(i)]; }
    bool PrepareDownmixes(std::uint64_t count) noexcept { return count == downmixes.size(); }
    auto& Downmixes(std::uint64_t i) noexcept { return downmixes[static_cast<std::size_t>(i)]; }
    bool PrepareCoefficients(std::uint64_t count) noexcept { return count == coefficients.size(); }
    auto& Coefficients(std::uint64_t i) noexcept { return coefficients[static_cast<std::size_t>(i)]; }
    bool Finish() noexcept { return true; }
};

harkenstudiobuild::PackResult ParseBusDefinitionSnapshotPayload(
    const std::uint8_t* payload, std::uint64_t size, PackCanonicalImage* image) {
    PackBusDefinitionSnapshotHeader header;
    if (!ReadPackBusDefinitionSnapshotHeader(payload, size, &header) ||
        header.busCount == 0 || header.busCount > 4096 ||
        header.effectCount != 0 || header.effectParameterCount != 0 ||
        header.downmixCount != header.busCount ||
        header.coefficientCount != static_cast<std::uint64_t>(header.busCount) * 64) {
        return kPackInvalid;
    }
    if (!ValidatePackBusDefinitionSnapshotLayout(header, size)) return kPackInvalid;
    std::vector<PackBusDefinitionBus> buses(header.busCount);
    std::vector<PackBusDefinitionSend> sends(header.sendCount);
    std::vector<PackBusDefinitionDownmix> downmixes(header.downmixCount);
    std::vector<float> coefficients(header.coefficientCount);
    PackWireBuffer<const std::uint8_t> input{payload, size};
    PackBusDefinitionSnapshotReader visitor{header, buses, sends, downmixes, coefficients};
    if (!ReadPackBusDefinitionSnapshot(input, visitor) || input.offset != size) return kPackInvalid;
    std::set<harkenstudio::ObjectId> ids;
    for (std::uint32_t i = 0; i < header.busCount; ++i) {
        const auto& row = buses[i];
        if (harkenstudio::IsNull(row.busId) || !ids.insert(row.busId).second ||
            !std::isfinite(row.faderDb) || row.faderDb < -96.0f || row.faderDb > 24.0f ||
            !std::isfinite(row.outputBusVolumeDb) ||
            !std::isfinite(std::pow(10.0f, row.outputBusVolumeDb / 20.0f)) ||
            row.effectFirst != 0 || row.effectCount != 0 ||
            row.downmixCount != 1 || row.downmixFirst != i) {
            return kPackInvalid;
        }
    }
    if (!ValidateCanonicalBusParentForestV2(buses)) return kPackInvalid;
    std::set<std::pair<harkenstudio::ObjectId, harkenstudio::ObjectId>> sendEdges;
    for (std::uint32_t i = 0; i < header.sendCount; ++i) {
        const auto& row = sends[i];
        if (!ids.contains(row.source) || !ids.contains(row.target) ||
            row.source == row.target ||
            (header.publicationCapabilityVintage == 4 &&
             row.target == harkenstudio::kMasterBusObjectId) ||
            (header.publicationCapabilityVintage == 3 ? row.levelDb != 0.0f :
                (!std::isfinite(row.levelDb) || row.levelDb < -96.0f ||
                 row.levelDb > 24.0f ||
                 !std::isfinite(std::pow(10.0f, row.levelDb / 20.0f)))) ||
            !sendEdges.emplace(row.source, row.target).second) {
            return kPackInvalid;
        }
    }
    if (header.publicationCapabilityVintage == 4 &&
        !ValidateCanonicalBusSendGraphV2(buses, sends)) return kPackInvalid;
    for (std::uint32_t i = 0; i < header.downmixCount; ++i) {
        const auto& row = downmixes[i];
        if (row.busId != buses[i].busId || row.coefficientCount != 64 ||
            row.coefficientFirst != i * 64) {
            return kPackInvalid;
        }
    }
    for (const auto value : coefficients) {
        if (!std::isfinite(value)) return kPackInvalid;
    }
    image->hasBusDefinitionSnapshot = true;
    image->busDefinitionSnapshotHeader = header;
    image->busDefinitionBusesV2 = std::move(buses);
    image->busDefinitionSendsV2 = std::move(sends);
    image->busDefinitionDownmixesV2 = std::move(downmixes);
    image->busDefinitionCoefficients = std::move(coefficients);
    return CanonicalVolumeBusFingerprint(*image) == header.publishedBusFingerprint &&
        header.contentGeneration == header.publishedBusFingerprint
        ? kPackOk : kPackInvalid;
}

bool CanonicalKindLegal(std::uint32_t entryKind, std::uint32_t packKind,
                        bool sharedResidentOwner) {
    switch (entryKind) {
    case kPackEntryKindMedia:
        return packKind == kPackKindCanonicalContent;
    case kPackEntryKindBus:
    case kPackEntryKindBusSend:
    case kPackEntryKindAttenuation:
    case kPackEntryKindBusEffect:
    case kPackEntryKindEffectDynamicRoute:
        return packKind == kPackKindInit ||
            (packKind == kPackKindCanonicalContent && sharedResidentOwner);
    case kPackEntryKindBindingDefinition:
    case kPackEntryKindBindingAttachment:
        return PackIsCanonicalKind(packKind);
        return packKind == kPackKindInit;
    case kPackEntryKindCanonicalDefinitionPartition:
    case kPackEntryKindPackRequirementBlock:
    case kPackEntryKindPackDependencyEdge:
        return PackIsCanonicalKind(packKind);
    case kPackEntryKindBusDefinitionSnapshot:
        return packKind == kPackKindInit;
    case kPackEntryKindVariablePolicyOverlay:
        return packKind == kPackKindInit;
    case kPackEntryKindVariableRequirements:
        return packKind == kPackKindCanonicalContent;
    default:
        return false;
    }
}



} // namespace

harkenstudiobuild::PackResult ValidateCanonicalInitDependency(
    std::span<const PackDependencyEdge> edges,
    std::uint64_t expectedInitPackStableId) noexcept {
    if (expectedInitPackStableId == 0) {
        return harkenstudiobuild::kPackInvalid;
    }
    bool found = false;
    for (const auto& edge : edges) {
        if (edge.edgeKind != kPackDependencyEdgeKindInit) {
            continue;
        }
        if (found || edge.required01 != 1 ||
            edge.prerequisitePackStableId != expectedInitPackStableId ||
            edge.expectedPrerequisiteContentRevision == 0 || edge.reserved0 != 0) {
            return harkenstudiobuild::kPackInvalid;
        }
        found = true;
    }
    return found ? harkenstudiobuild::kPackOk : harkenstudiobuild::kPackInvalid;
}

harkenstudiobuild::PackResult ParsePackCanonicalImage(const std::uint8_t* bytes,
                                 std::uint64_t fileBytes,
                                 PackCanonicalImage* out) {
    if (bytes == nullptr || out == nullptr || fileBytes < kPackFileHeaderBytes) {
        return harkenstudiobuild::kPackInvalid;
    }
    PackFileHeader header;
    if (!ReadPackFileHeader(bytes, &header)) {
        return harkenstudiobuild::kPackInvalid;
    }
    if (header.formatVersion != kPackFormatVersion ||
        header.headerBytes != kPackFileHeaderBytes ||
        header.fileBytes != fileBytes) {
        return harkenstudiobuild::kPackInvalid;
    }
    if (!PackIsCanonicalKind(header.packKind)) {
        return harkenstudiobuild::kPackInvalid;
    }
    const std::uint64_t dirBytes =
        static_cast<std::uint64_t>(header.entryCount) *
        kPackDirectoryEntryBytes;
    if (header.directoryOffset != header.headerBytes ||
        header.directoryOffset > fileBytes ||
        dirBytes > fileBytes - header.directoryOffset) {
        return harkenstudiobuild::kPackInvalid;
    }
    const std::uint8_t* dir = bytes + header.directoryOffset;
    if (Fnv1a64(dir, static_cast<std::size_t>(dirBytes)) !=
        header.directoryChecksum) {
        return harkenstudiobuild::kPackInvalid;
    }
    const std::uint64_t dirEnd = header.directoryOffset + dirBytes;

    PackCanonicalImage image;
    image.packKind = header.packKind;

    std::uint64_t attachmentOrdinal = 0;
    std::uint64_t sendOrdinal = 0;
    std::uint64_t busEffectOrdinal = 0;
    std::uint64_t dynamicRouteOrdinal = 0;
    std::uint64_t dependencyOrdinal = 0;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    ranges.reserve(static_cast<std::size_t>(header.entryCount) * 2);

    for (std::uint32_t i = 0; i < header.entryCount; ++i) {
        const PackDirectoryEntry entry = ReadPackDirectoryEntry(
            dir + static_cast<std::size_t>(i) * kPackDirectoryEntryBytes);
        if (!PackEntryKindSupported(entry.kind)) return kPackInvalid;
        if (!VintageLegalForKind(entry.kind, entry.vintage)) {
            return harkenstudiobuild::kPackIncompatible;
        }
        if (entry.offset < dirEnd || entry.offset > fileBytes ||
            entry.size > fileBytes - entry.offset ||
            entry.offset % kPackPayloadAlign != 0) {
            return harkenstudiobuild::kPackInvalid;
        }
        ranges.emplace_back(entry.offset, entry.size);
        const std::uint8_t* payload = bytes + entry.offset;
        if (Fnv1a64(payload, static_cast<std::size_t>(entry.size)) !=
            entry.payloadChecksum) {
            return harkenstudiobuild::kPackInvalid;
        }
        switch (entry.kind) {

        case kPackEntryKindCanonicalDefinitionPartition: {
            if (image.hasPartition) {
                return harkenstudiobuild::kPackInvalid;
            }
            PackCanonicalPartitionHeader partitionHeader;
            if (entry.size < kPackCanonicalPartitionHeaderBytes) {
                return harkenstudiobuild::kPackIncompatible;
            }
            const bool parsed = ReadPackCanonicalPartitionHeader(payload, entry.size, &partitionHeader);
            if (!parsed) {
                return harkenstudiobuild::kPackInvalid;
            }
            if (partitionHeader.bodyVintage !=
                kCanonicalDefinitionPartitionBodyVintage) {
                return harkenstudiobuild::kPackIncompatible;
            }
            CurrentWorldBodyImage body;
            const CurrentWorldBodyDecodeResult decoded = body.DecodeWithResult(
                payload + partitionHeader.headerBytes,
                static_cast<std::size_t>(partitionHeader.bodyBytes),
                kCanonicalDefinitionPartitionBodyVintage);
            if (decoded != CurrentWorldBodyDecodeResult::Ok) {
                return decoded == CurrentWorldBodyDecodeResult::Capacity
                           ? harkenstudiobuild::kPackResidentCapacity
                           : harkenstudiobuild::kPackInvalid;
            }
            const WorldInstallStatus validation =
                detail::ValidateCurrentWorldPartitionLocalV1(*body.ProductionView());
            if (validation == WorldInstallStatus::InvalidFormat ||
                validation == WorldInstallStatus::CapabilityUnavailable) {
                return harkenstudiobuild::kPackIncompatible;
            }
            if (validation == WorldInstallStatus::Capacity) {
                return harkenstudiobuild::kPackResidentCapacity;
            }
            if (validation != WorldInstallStatus::Ok) {
                return harkenstudiobuild::kPackInvalid;
            }
            const ContainerWorldInstallViewV1 decodedView = *body.ProductionView();
            const std::uint64_t decodedOwnedDefinitions =
                static_cast<std::uint64_t>(decodedView.variableCount) +
                decodedView.playablePropertyCount + decodedView.actionCount;
            if (partitionHeader.ownedDefinitionCount !=
                decodedOwnedDefinitions) {
                return harkenstudiobuild::kPackInvalid;
            }
            image.partitionHeader = partitionHeader;
            image.partitionBody.assign(
                payload + partitionHeader.headerBytes,
                payload + partitionHeader.headerBytes +
                    static_cast<std::size_t>(partitionHeader.bodyBytes));
            image.hasPartition = true;
            break;
        }
        case kPackEntryKindPackRequirementBlock: {
            if (image.hasRequirement) {
                return harkenstudiobuild::kPackInvalid;
            }
            PackRequirementBlock requirement;
            if (entry.size != kPackRequirementBlockBytes) {
                return harkenstudiobuild::kPackIncompatible;
            }
            if (!ParsePackRequirementBlock(payload, entry.size, &requirement)) {
                return harkenstudiobuild::kPackInvalid;
            }
            if (requirement.schemaVersion != entry.vintage ||
                requirement.storageModelVersion != entry.vintage) {
                return harkenstudiobuild::kPackIncompatible;
            }
            if (!PackRequirementBlockValid(requirement) ||
                requirement.payloadChecksumCoverage != Fnv1a64(payload, kPackRequirementBlockPayloadChecksumCoverageOffset)) {
                return harkenstudiobuild::kPackInvalid;
            }
            image.requirement = requirement;
            image.packStableId = requirement.packStableId;
            image.hasRequirement = true;
            break;
        }
        case kPackEntryKindPackDependencyEdge: {
            PackDependencyEdge edge;
            if (entry.size != kPackDependencyEdgeBytes) {
                return harkenstudiobuild::kPackIncompatible;
            }
            if (!ReadPackDependencyEdge(payload, entry.size, &edge) ||
                entry.relationOrdinal != dependencyOrdinal) {
                return harkenstudiobuild::kPackInvalid;
            }
            const bool initRequired =
                edge.edgeKind == kPackDependencyEdgeKindInit &&
                edge.required01 == 1;
            if (!initRequired &&
                (edge.expectedBusContentGeneration != 0 ||
                 edge.expectedPublishedBusFingerprint != 0 ||
                 edge.expectedCompileTargetProfileFingerprint != 0)) {
                return harkenstudiobuild::kPackInvalid;
            }
            ++dependencyOrdinal;
            image.dependencyEdges.push_back(edge);
            break;
        }
        case kPackEntryKindBusDefinitionSnapshot: {
            if (image.hasBusDefinitionSnapshot) {
                return harkenstudiobuild::kPackInvalid;
            }
            if (entry.id != harkenstudio::ObjectId{} || entry.relationOrdinal != 1) {
                return harkenstudiobuild::kPackInvalid;
            }
            const harkenstudiobuild::PackResult parsed =
                ParseBusDefinitionSnapshotPayload(payload, entry.size, &image);
            if (parsed != harkenstudiobuild::kPackOk) {
                return parsed;
            }
            break;
        }
        case kPackEntryKindVariablePolicyOverlay: {
            if (image.hasVariablePolicyOverlay) {
                return harkenstudiobuild::kPackInvalid;
            }
            const harkenstudiobuild::PackResult parsed = ParseVariablePolicyOverlayPayload(
                payload, entry.size, &image.variablePolicyOverlays);
            if (parsed != harkenstudiobuild::kPackOk) {
                return parsed;
            }
            image.hasVariablePolicyOverlay = true;
            break;
        }
        case kPackEntryKindVariableRequirements: {
            if (image.hasVariableRequirements) {
                return harkenstudiobuild::kPackInvalid;
            }
            const harkenstudiobuild::PackResult parsed = ParseVariableRequirementsPayload(
                payload, entry.size, &image.variableRequirements);
            if (parsed != harkenstudiobuild::kPackOk) {
                return parsed;
            }
            image.hasVariableRequirements = true;
            break;
        }
        case kPackEntryKindMedia: {
            if (entry.size != kPackMediaBytes) {
                return harkenstudiobuild::kPackInvalid;
            }
            const PackTocEntry media = ReadPackMedia(payload);
            if (media.soundId != entry.id ||
                !ValidateV2MediaEntry(media, fileBytes, dirEnd)) {
                return harkenstudiobuild::kPackInvalid;
            }
            if (Fnv1a64(bytes + media.blobOffset,
                        static_cast<std::size_t>(media.blobBytes)) !=
                media.blobChecksum) {
                return harkenstudiobuild::kPackInvalid;
            }
            ranges.emplace_back(media.blobOffset, media.blobBytes);
            image.media.push_back(media);
            break;
        }
        case kPackEntryKindBusEffect: {
            PackBusEffectEntry effect;
            if (!ReadPackBusEffect(payload, entry.size, &effect) ||
                entry.relationOrdinal != image.busEffects.size() || effect.busId == harkenstudio::ObjectId{} ||
                effect.bypass > 1 || effect.slot >= kMaxFxSlotsPerBus ||
                effect.faderPointGuard > kMaxFxSlotsPerBus) {
                return harkenstudiobuild::kPackInvalid;
            }
            const PackEffectSchemaRow* schema = FindPackEffectSchemaByType(effect.effectType);
            if (schema == nullptr || effect.descriptorVintage != schema->descriptorVintage ||
                effect.paramCount != schema->paramCount) {
                return harkenstudiobuild::kPackInvalid;
            }
            for (std::uint32_t p = 0; p < effect.paramCount; ++p) {
                if (!std::isfinite(effect.params[p])) {
                    return harkenstudiobuild::kPackInvalid;
                }
            }
            image.busEffects.push_back(effect);
            break;
        }
        case kPackEntryKindEffectDynamicRoute: {
            PackEffectDynamicRouteEntry route;
            if (!ParsePackEffectDynamicRoute(payload, entry.size, &route) ||
                entry.relationOrdinal != image.dynamicRoutes.size() || route.targetBusId == harkenstudio::ObjectId{} ||
                route.sourceId == harkenstudio::ObjectId{} || route.expectedEffectType != harkenstudio::kEffectCompressor ||
                route.signalForm != harkenstudio::kDynamicSignalRawAudio ||
                route.sourceKind != harkenstudio::kDynamicSourceBus ||
                route.tap > harkenstudio::kDynamicTapBusPostFader ||
                route.effectSlot >= kMaxFxSlotsPerBus || route.routeSlot >= 4u ||
                !std::isfinite(route.levelDb) || route.levelDb < -96.0f || route.levelDb > 24.0f) {
                return harkenstudiobuild::kPackInvalid;
            }
            image.dynamicRoutes.push_back(route);
            break;
        }
        case kPackEntryKindBus: {
            PackBusEntry bus;
            const bool parsed =
                ParsePackBus(payload, entry.size, &bus);
            if (!parsed || bus.busId != entry.id ||
                !std::isfinite(bus.volume) ||
                !std::isfinite(bus.outputBusVolume)) {
                return harkenstudiobuild::kPackInvalid;
            }
            image.buses.push_back(bus);
            break;
        }
        case kPackEntryKindBusSend: {
            PackBusSendEntry send;
            if (!ParsePackBusSend(payload, entry.size, &send) ||
                entry.relationOrdinal != sendOrdinal || !std::isfinite(send.levelDb)) {
                return harkenstudiobuild::kPackInvalid;
            }
            ++sendOrdinal;
            image.busSends.push_back(send);
            break;
        }
        case kPackEntryKindAttenuation: {
            PackLogicImage::Attenuation attenuation;
            attenuation.id = entry.id;
            if (!ReadPackAttenuation(payload, entry.size,
                                        &attenuation.desc)) {
                return harkenstudiobuild::kPackInvalid;
            }
            image.attenuations.push_back(attenuation);
            break;
        }
        case kPackEntryKindBindingDefinition: {
            PackLogicImage::BindingDefinition definition;
            if (!ReadPackBindingDefinition(payload, entry.size,
                                              &definition.id,
                                              &definition.desc) ||
                definition.id != entry.id ||
                !IsValidBindingTransitionMs(definition.desc.transitionMs)) {
                return harkenstudiobuild::kPackInvalid;
            }
            if (definition.desc.targetScopeKind >
                harkenstudio::kTargetScopeBusEffectParam) {
                return harkenstudiobuild::kPackInvalid;
            }
            if (header.packKind == kPackKindInit &&
                (definition.desc.targetScopeKind == harkenstudio::kTargetScopeVoice ||
                 definition.desc.targetScopeKind ==
                     harkenstudio::kTargetScopeVoiceEffectParam)) {
                return harkenstudiobuild::kPackInvalid;
            }
            image.bindingDefinitions.push_back(definition);
            break;
        }
        case kPackEntryKindBindingAttachment: {
            PackBindingAttachmentEntry attachment;
            if (!ParsePackBindingAttachment(payload, entry.size,
                                              &attachment) ||
                entry.relationOrdinal != attachmentOrdinal) {
                return harkenstudiobuild::kPackInvalid;
            }
            ++attachmentOrdinal;
            if (attachment.scopeKind ==
                    harkenstudio::kTargetScopeVoiceEffectParam ||
                attachment.scopeKind >
                    harkenstudio::kTargetScopeBusEffectParam) {
                return harkenstudiobuild::kPackInvalid;
            }
            if (header.packKind == kPackKindInit &&
                attachment.scopeKind != harkenstudio::kTargetScopeBus &&
                attachment.scopeKind !=
                    harkenstudio::kTargetScopeBusEffectParam) {
                return harkenstudiobuild::kPackInvalid;
            }
            image.bindingAttachments.push_back(attachment);
            break;
        }
        case kPackEntryKindContainer:
        case kPackEntryKindVoiceRecipe:
            return harkenstudiobuild::kPackIncompatible;
        default:
            return harkenstudiobuild::kPackInvalid;
        }
    }

    if (!image.hasRequirement) {
        return harkenstudiobuild::kPackInvalid;
    }
    const std::uint32_t maxBindingScope = harkenstudio::kTargetScopeBusEffectParam;
    for (const auto& definition : image.bindingDefinitions) {
        if (definition.desc.targetScopeKind > maxBindingScope) {
            return harkenstudiobuild::kPackInvalid;
        }
        if (header.packKind == kPackKindInit &&
            (definition.desc.targetScopeKind == harkenstudio::kTargetScopeVoice ||
             definition.desc.targetScopeKind ==
                 harkenstudio::kTargetScopeVoiceEffectParam)) {
            return harkenstudiobuild::kPackInvalid;
        }
    }
    for (const auto& attachment : image.bindingAttachments) {
        if (attachment.scopeKind > maxBindingScope ||
            attachment.scopeKind == harkenstudio::kTargetScopeVoiceEffectParam) {
            return harkenstudiobuild::kPackInvalid;
        }
        if (header.packKind == kPackKindInit &&
            attachment.scopeKind != harkenstudio::kTargetScopeBus &&
            attachment.scopeKind != harkenstudio::kTargetScopeBusEffectParam) {
            return harkenstudiobuild::kPackInvalid;
        }
    }
    const bool sharedResidentOwner =
        header.packKind == kPackKindCanonicalContent &&
        (!image.buses.empty() || !image.busSends.empty() ||
         !image.attenuations.empty() || !image.busEffects.empty() ||
         !image.dynamicRoutes.empty());
    for (std::uint32_t i = 0; i < header.entryCount; ++i) {
        const PackDirectoryEntry entry = ReadPackDirectoryEntry(
            dir + static_cast<std::size_t>(i) * kPackDirectoryEntryBytes);
        if (!CanonicalKindLegal(entry.kind, header.packKind,
                                sharedResidentOwner)) {
            return harkenstudiobuild::kPackInvalid;
        }
    }
    if (header.packKind == kPackKindCanonicalContent &&
        image.hasBusDefinitionSnapshot) {
        return harkenstudiobuild::kPackInvalid;
    }
    if (image.hasPartition &&
        (image.requirement.definitionPartitionRevision == 0 ||
         image.partitionHeader.packStableId != image.packStableId ||
         image.partitionHeader.partitionRevision !=
             image.requirement.definitionPartitionRevision)) {
        return harkenstudiobuild::kPackInvalid;
    }
    if (!image.hasPartition &&
        image.requirement.definitionPartitionRevision != 0) {
        return harkenstudiobuild::kPackInvalid;
    }
    const CanonicalBusTriple requirementT = RequirementBusTriple(image.requirement);
    if (image.hasBusDefinitionSnapshot) {
        const CanonicalBusTriple snapshotT =
            SnapshotBusTriple(image.busDefinitionSnapshotHeader);
        if (!CanonicalBusTripleFullyNonzero(snapshotT) ||
            !CanonicalBusTripleEqual(snapshotT, requirementT)) {
            return harkenstudiobuild::kPackInvalid;
        }
    }
    if (image.hasPartition &&
        !CanonicalBusTripleEqual(PartitionBusTriple(image.partitionHeader),
                                 requirementT)) {
        return harkenstudiobuild::kPackInvalid;
    }
    for (const PackDependencyEdge& edge : image.dependencyEdges) {
        const bool initRequired =
            edge.edgeKind == kPackDependencyEdgeKindInit && edge.required01 == 1;
        if (initRequired &&
            !CanonicalBusTripleEqual(DependencyBusTriple(edge), requirementT)) {
            return harkenstudiobuild::kPackInvalid;
        }
    }
    *out = std::move(image);
    return harkenstudiobuild::kPackOk;
}

harkenstudiobuild::PackResult CheckPackClosure(const PackLogicImage& image,
                          const PackClosureResolver& resolver,
                          PackClosureRef* outRefClass,
                          harkenstudio::ObjectId* outReferrerId,
                          harkenstudio::ObjectId* outMissingId) {
    const auto fail = [&](PackClosureRef refClass, harkenstudio::ObjectId referrer,
                          harkenstudio::ObjectId missing) {
        if (outRefClass != nullptr) {
            *outRefClass = refClass;
        }
        if (outReferrerId != nullptr) {
            *outReferrerId = referrer;
        }
        if (outMissingId != nullptr) {
            *outMissingId = missing;
        }
        return harkenstudiobuild::kPackUnknownId;
    };

    const auto packPlayable = [&](harkenstudio::ObjectId id) {
        for (const PackTocEntry& media : image.media) {
            if (media.soundId == id) {
                return true;
            }
        }
        for (const PackLogicImage::Container& container : image.containers) {
            if (container.header.containerId == id) {
                return true;
            }
        }
        return false;
    };
    const auto playableCloses = [&](harkenstudio::ObjectId id) {
        return packPlayable(id) ||
               (resolver.playableExists != nullptr &&
                resolver.playableExists(resolver.ctx, id));
    };
    const auto busCloses = [&](harkenstudio::ObjectId id) {
        for (const PackBusEntry& bus : image.buses) {
            if (bus.busId == id) {
                return true;
            }
        }
        return resolver.busExists != nullptr &&
               resolver.busExists(resolver.ctx, id);
    };
    const auto bindingCloses = [&](harkenstudio::ObjectId id) {
        for (const PackLogicImage::BindingDefinition& def :
             image.bindingDefinitions) {
            if (def.id == id) {
                return true;
            }
        }
        return resolver.bindingExists != nullptr &&
               resolver.bindingExists(resolver.ctx, id);
    };
    for (const PackBusEntry& bus : image.buses) {
        if (!busCloses(bus.parentBusId)) {
            return fail(PackClosureRef::BusParent, bus.busId,
                        bus.parentBusId);
        }
    }
    for (const PackBusEffectEntry& effect : image.busEffects) {
        if (!busCloses(effect.busId)) {
            return fail(PackClosureRef::BusEffectTarget, effect.busId,
                        effect.busId);
        }
    }
    for (const PackEffectDynamicRouteEntry& route : image.dynamicRoutes) {
        if (!busCloses(route.targetBusId)) {
            return fail(PackClosureRef::DynamicRouteTarget,
                        route.targetBusId, route.targetBusId);
        }
        if (!busCloses(route.sourceId)) {
            return fail(PackClosureRef::DynamicRouteSource,
                        route.targetBusId, route.sourceId);
        }
    }
    for (std::size_t i = 0; i < image.busSends.size(); ++i) {
        const PackBusSendEntry& send = image.busSends[i];
        if (!busCloses(send.fromBusId)) {
            return fail(PackClosureRef::SendBus, send.fromBusId, send.fromBusId);
        }
        if (!busCloses(send.toBusId)) {
            return fail(PackClosureRef::SendBus, send.fromBusId, send.toBusId);
        }
    }
    for (std::size_t i = 0; i < image.bindingAttachments.size(); ++i) {
        const PackBindingAttachmentEntry& attachment =
            image.bindingAttachments[i];
        if (!bindingCloses(attachment.bindingId)) {
            return fail(PackClosureRef::AttachmentBinding, attachment.bindingId,
                        attachment.bindingId);
        }
        if (attachment.scopeKind == harkenstudio::kTargetScopeBus ||
            attachment.scopeKind ==
                harkenstudio::kTargetScopeBusEffectParam) {
            if (!busCloses(attachment.objectId)) {
                return fail(PackClosureRef::AttachmentBus, attachment.bindingId,
                            attachment.objectId);
            }
        } else {
            if (!playableCloses(attachment.objectId)) {
                return fail(PackClosureRef::AttachmentPlayable, attachment.bindingId,
                            attachment.objectId);
            }
        }
    }
    for (const PackLogicImage::Container& container : image.containers) {
        for (const PackContainerChild& child : container.children) {
            if (!playableCloses(child.playableId)) {
                return fail(PackClosureRef::ContainerChild,
                            container.header.containerId, child.playableId);
            }
        }
    }
    for (const PackLogicImage::VoiceRecipe& recipe : image.voiceRecipes) {
        if (!playableCloses(recipe.header.playableId)) {
            return fail(PackClosureRef::RecipePlayable,
                        recipe.header.playableId,
                        recipe.header.playableId);
        }
        for (harkenstudio::ObjectId bindingId : recipe.bindings) {
            if (!bindingCloses(bindingId)) {
                return fail(PackClosureRef::RecipeBinding,
                            recipe.header.playableId, bindingId);
            }
        }
    }
    return harkenstudiobuild::kPackOk;
}

} // namespace harkenstudiobuild
