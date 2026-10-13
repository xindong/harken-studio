#include <harken_studio_build/BindingTransition.h>
#include <harken_studio_build/AuthoredParameters.h>
#include "harken_studio_build/PackImage.h"

#include "harken_studio_build/WorldCodec.h"
#include "harken_studio_build/BuildLimits.h"
#include "harken_studio_build/WorldValidation.h"
#include "harken_studio_build/EffectEncoding.h"

#include <algorithm>
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

bool VintageLegalForKind(std::uint32_t entryKind,
                         std::uint32_t vintage) {
    switch (entryKind) {
    case kPackEntryKindBusEffect:
    case kPackEntryKindEffectDynamicRoute:
        return vintage == kPackV2DynamicSchemaVintage;
    case kPackEntryKindVoiceRecipe:
        return vintage == kPackV2PlaybackRecipeSchemaVintage;
    case kPackEntryKindAttenuation:
        return vintage == kPackV2AuthoredToneSchemaVintage;
    case kPackEntryKindBus:
        return vintage == kPackV2SchemaVintage ||
               vintage == kPackV2BusMixSchemaVintage;
    default:
        return vintage == kPackV2SchemaVintage;
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
    std::span<const PackBusDefinitionBusV2> buses) {
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
    std::span<const PackBusDefinitionBusV2> buses,
    std::span<const PackBusDefinitionSendV2> sends) {
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
    if (bytes == nullptr || fileBytes < kPackV2HeaderBytes) {
        return harkenstudiobuild::kPackInvalid;
    }
    PackV2Header header;
    if (!ParsePackV2Header(bytes, &header)) {
        return harkenstudiobuild::kPackInvalid;
    }
    if (header.formatVersion != kPackFormatVersion2 ||
        header.headerBytes != kPackV2HeaderBytes ||
        header.fileBytes != fileBytes) {
        return harkenstudiobuild::kPackInvalid;
    }
    if (PackV2IsCanonicalKind(header.packKind)) {
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
        kPackV2DirectoryEntryBytes;
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
        const PackV2DirectoryEntry entry = ParsePackV2DirectoryEntry(
            dir + static_cast<std::size_t>(i) * kPackV2DirectoryEntryBytes);
        if (PackV2IsCanonicalEntryKind(entry.kind)) {
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
        if (entry.offset % kPackV2PayloadAlign != 0) {
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
            if (entry.size != kPackV2MediaPayloadBytes) {
                return harkenstudiobuild::kPackInvalid;
            }
            const PackTocEntry media = ParsePackTocEntry(payload);
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
                entry.vintage == kPackV2SchemaVintage
                    ? ParsePackV2Bus(payload, entry.size, &bus)
                    : ParsePackV6Bus(payload, entry.size, &bus);
            if (!parsed || bus.busId != entry.id ||
                !std::isfinite(bus.volume) ||
                !std::isfinite(bus.outputBusVolume) ||
                !std::isfinite(bus.makeUpGain) ||
                !std::isfinite(bus.volume +
                               bus.makeUpGain)) {
                return harkenstudiobuild::kPackInvalid;
            }
            image.buses.push_back(bus);
            break;
        }
        case kPackEntryKindBusSend: {
            PackBusSendEntry send;
            if (!ParsePackV2BusSend(payload, entry.size, &send) ||
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
            if (!ParsePackV2BusEffect(payload, entry.size, &effect) ||
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
            if (!ParsePackV2EffectDynamicRoute(payload, entry.size,
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
            if (!ParsePackV3Attenuation(payload, entry.size,
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
            if (!ParsePackV2BindingDefinition(payload, entry.size,
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
            if (!ParsePackV2BindingAttachment(payload, entry.size,
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
            if (!ParsePackV2ContainerHeader(payload, entry.size,
                                            &container.header) ||
                container.header.containerId != entry.id ||
                container.header.childCount == 0) {
                return harkenstudiobuild::kPackInvalid;
            }
            container.children.reserve(container.header.childCount);
            for (std::uint32_t c = 0; c < container.header.childCount; ++c) {
                container.children.push_back(ParsePackV2ContainerChild(
                    payload + kPackV2ContainerHeaderBytes +
                    static_cast<std::size_t>(c) * kPackV2ContainerChildBytes));
            }
            image.containers.push_back(std::move(container));
            break;
        }
        case kPackEntryKindVoiceRecipe: {
            PackLogicImage::VoiceRecipe recipe;
            if (!ParsePackV5VoiceRecipeHeader(payload, entry.size,
                                              &recipe.header) ||
                recipe.header.playableId == harkenstudio::ObjectId{} ||
                recipe.header.playableId != entry.id) {
                return harkenstudiobuild::kPackInvalid;
            }
            const std::uint8_t* installBase =
                payload + kPackV2VoiceRecipeHeaderBytes;
            if (!ParsePackV3VoiceRecipeTone(installBase, &recipe.tone) ||
                !VoiceToneWireValuesValid(recipe.tone)) {
                return harkenstudiobuild::kPackInvalid;
            }
            installBase += kPackV3VoiceRecipeToneBytes;
            if (!ParsePackV5VoiceRecipeSpatial(installBase,
                                               &recipe.spatial)) {
                return harkenstudiobuild::kPackInvalid;
            }
            installBase += kPackV4VoiceRecipeSpatialBytes;
            if (!ParsePackV5VoiceRecipePlayback(installBase,
                                                &recipe.playback)) {
                return harkenstudiobuild::kPackInvalid;
            }
            installBase += kPackV5VoiceRecipePlaybackBytes;
            recipe.installs.reserve(recipe.header.installCount);
            std::uint32_t previousSlot = 0;
            for (std::uint32_t n = 0; n < recipe.header.installCount; ++n) {
                PackVoiceRecipeInstall install;
                if (!ParsePackV2VoiceRecipeInstall(
                        installBase + static_cast<std::size_t>(n) *
                                          kPackV2VoiceRecipeInstallBytes,
                        &install) ||
                    install.slot >= kPackV2VoiceRecipeMaxInstalls ||
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
                recipe.installs.push_back(install);
            }
            const std::uint8_t* bindingBase =
                installBase +
                static_cast<std::size_t>(recipe.header.installCount) *
                    kPackV2VoiceRecipeInstallBytes;
            recipe.bindings.reserve(recipe.header.bindingCount);
            harkenstudio::ObjectId previousBinding{};
            for (std::uint32_t n = 0; n < recipe.header.bindingCount; ++n) {
                const harkenstudio::ObjectId binding = ParsePackV2VoiceRecipeBinding(
                    bindingBase + static_cast<std::size_t>(n) *
                                      kPackV2VoiceRecipeBindingBytes);
                if (binding == harkenstudio::ObjectId{} ||
                    (n > 0 && binding <= previousBinding)) {
                    return harkenstudiobuild::kPackInvalid;
                }
                previousBinding = binding;
                recipe.bindings.push_back(binding);
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

harkenstudiobuild::PackResult ParseVariablePolicyOverlayPayload(
    const std::uint8_t* payload, std::uint64_t size,
    std::vector<PackCanonicalImage::VariablePolicyOverlay>* out) {
    if (payload == nullptr || out == nullptr || size < 16) {
        return harkenstudiobuild::kPackInvalid;
    }
    const std::uint32_t rowCount = LoadLE32(payload);
    const std::uint32_t format = LoadLE32(payload + 4);
    if (format > 1 || LoadLE64(payload + 8) != 0) {
        return harkenstudiobuild::kPackInvalid;
    }
    std::uint64_t rowsBytes = 0;
    std::uint64_t needed = 16;
    if (!CheckedMulU64(rowCount, 32, &rowsBytes) ||
        !CheckedAddU64(needed, rowsBytes, &needed) || needed > size) {
        return harkenstudiobuild::kPackInvalid;
    }
    std::vector<PackCanonicalImage::VariablePolicyOverlay> rows;
    rows.reserve(rowCount);
    for (std::uint32_t i = 0; i < rowCount; ++i) {
        const std::uint8_t* row =
            payload + 16 + static_cast<std::size_t>(i) * 32;
        PackCanonicalImage::VariablePolicyOverlay item;
        item.variableId = LoadObjectId(row);
        item.writers = row[16];
        item.executionFingerprint = LoadLE64(row + 24);
        if (item.variableId == harkenstudio::ObjectId{} || item.executionFingerprint == 0 ||
            item.writers < 1 || item.writers > 3 || row[17] != 0 ||
            LoadLE16(row + 18) != 0 || LoadLE32(row + 20) != 0) {
            return harkenstudiobuild::kPackInvalid;
        }
        if (i > 0 && item.variableId <= rows.back().variableId) {
            return harkenstudiobuild::kPackInvalid;
        }
        rows.push_back(item);
    }
    std::size_t cursor = static_cast<std::size_t>(needed);
    for (std::uint32_t i = 0; i < rowCount; ++i) {
        if (size - cursor < 4) {
            return harkenstudiobuild::kPackInvalid;
        }
        const std::uint32_t count = LoadLE32(payload + cursor);
        cursor += 4;
        std::uint64_t anchorsBytes = 0;
        if (!CheckedMulU64(count, 4, &anchorsBytes) ||
            anchorsBytes > size - cursor) {
            return harkenstudiobuild::kPackInvalid;
        }
        rows[i].discreteAnchors.reserve(count);
        for (std::uint32_t j = 0; j < count; ++j) {
            const std::int32_t anchor =
                static_cast<std::int32_t>(LoadLE32(payload + cursor));
            cursor += 4;
            if (j > 0 && anchor <= rows[i].discreteAnchors.back()) {
                return harkenstudiobuild::kPackInvalid;
            }
            rows[i].discreteAnchors.push_back(anchor);
        }
    }
    if (format == 1) {
        for (auto& row : rows) {
            if (size - cursor < 24) return harkenstudiobuild::kPackInvalid;
            row.labelMappingFingerprint = LoadLE64(payload + cursor);
            row.catalogFingerprint = LoadLE64(payload + cursor + 8);
            const std::uint32_t count = LoadLE32(payload + cursor + 16);
            if (LoadLE32(payload + cursor + 20) != 0
                || row.labelMappingFingerprint == 0 || row.catalogFingerprint == 0)
                return harkenstudiobuild::kPackInvalid;
            cursor += 24;
            if (count > (size - cursor) / 24) return harkenstudiobuild::kPackInvalid;
            std::set<harkenstudio::ObjectId> identities;
            std::set<std::string> names;
            row.labels.reserve(count);
            for (std::uint32_t j = 0; j < count; ++j) {
                if (size - cursor < 24) return harkenstudiobuild::kPackInvalid;
                VariableLabelDefinitionV1 label;
                label.labelId = LoadObjectId(payload + cursor);
                label.anchor = static_cast<std::int32_t>(LoadLE32(payload + cursor + 16));
                const std::uint32_t length = LoadLE32(payload + cursor + 20);
                cursor += 24;
                const std::uint64_t paddedLength = (static_cast<std::uint64_t>(length) + 3u) & ~std::uint64_t{3};
                if (paddedLength > size - cursor) return harkenstudiobuild::kPackInvalid;
                label.name.assign(reinterpret_cast<const char*>(payload + cursor), length);
                for (std::uint64_t k = length; k < paddedLength; ++k)
                    if (payload[cursor + k] != 0) return harkenstudiobuild::kPackInvalid;
                cursor += static_cast<std::size_t>(paddedLength);
                if (label.labelId == harkenstudio::ObjectId{} || !identities.insert(label.labelId).second
                    || label.anchor < 0 || label.anchor > 16777216
                    || (j > 0 && label.anchor <= row.labels.back().anchor)
                    || label.name.find('\0') != std::string::npos
                    || (!label.name.empty() && !names.insert(label.name).second))
                    return harkenstudiobuild::kPackInvalid;
                row.labels.push_back(std::move(label));
            }
            if (ComputeVariableLabelMappingFingerprintV1(row.variableId, row.labels)
                != row.labelMappingFingerprint) return harkenstudiobuild::kPackInvalid;
        }
    }
    if (cursor != size) {
        return harkenstudiobuild::kPackInvalid;
    }
    *out = std::move(rows);
    return harkenstudiobuild::kPackOk;
}

harkenstudiobuild::PackResult ParseVariableRequirementsPayload(
    const std::uint8_t* payload, std::uint64_t size,
    std::vector<PackCanonicalImage::VariableRequirement>* out) {
    if (payload == nullptr || out == nullptr || size < 16) {
        return harkenstudiobuild::kPackInvalid;
    }
    const std::uint32_t rowCount = LoadLE32(payload);
    const std::uint32_t format = LoadLE32(payload + 4);
    if (format > 1 || LoadLE64(payload + 8) != 0) {
        return harkenstudiobuild::kPackInvalid;
    }
    std::uint64_t rowsBytes = 0;
    std::uint64_t needed = 16;
    const std::uint32_t rowBytes = format == 1 ? 40 : 32;
    if (!CheckedMulU64(rowCount, rowBytes, &rowsBytes) ||
        !CheckedAddU64(needed, rowsBytes, &needed) || needed != size) {
        return harkenstudiobuild::kPackInvalid;
    }
    std::vector<PackCanonicalImage::VariableRequirement> rows;
    rows.reserve(rowCount);
    for (std::uint32_t i = 0; i < rowCount; ++i) {
        const std::uint8_t* row =
            payload + 16 + static_cast<std::size_t>(i) * rowBytes;
        PackCanonicalImage::VariableRequirement item;
        item.identityClass = row[0];
        item.stableId = LoadObjectId(row + 8);
        item.expectedExecutionFingerprint = LoadLE64(row + 24);
        if (format == 1) {
            item.expectedLabelMappingFingerprint = LoadLE64(row + 32);
            if (item.expectedLabelMappingFingerprint == 0) return harkenstudiobuild::kPackInvalid;
        }
        if (item.identityClass != 1 || row[1] != 0 ||
            LoadLE16(row + 2) != 0 || LoadLE32(row + 4) != 0 ||
            item.stableId == harkenstudio::ObjectId{} || item.expectedExecutionFingerprint == 0) {
            return harkenstudiobuild::kPackInvalid;
        }
        if (i > 0 &&
            (item.identityClass < rows.back().identityClass ||
             (item.identityClass == rows.back().identityClass &&
              item.stableId <= rows.back().stableId))) {
            return harkenstudiobuild::kPackInvalid;
        }
        rows.push_back(item);
    }
    *out = std::move(rows);
    return harkenstudiobuild::kPackOk;
}

bool EntrySectionFits(std::uint64_t offset, std::uint32_t count, std::uint32_t rowBytes,
                      std::uint64_t entryBytes, std::uint64_t* outEnd,
                      std::uint64_t headerBytes = kPackBusDefinitionSnapshotHeaderBytes) {
    std::uint64_t sectionBytes = 0;
    std::uint64_t end = 0;
    if (offset < headerBytes || offset % 8 != 0 ||
        !CheckedMulU64(count, rowBytes, &sectionBytes) ||
        !CheckedAddU64(offset, sectionBytes, &end) || end > entryBytes) {
        return false;
    }
    *outEnd = end;
    return true;
}

bool ZeroPadding(const std::uint8_t* payload, std::uint64_t from, std::uint64_t to) {
    for (std::uint64_t i = from; i < to; ++i) {
        if (payload[i] != 0) {
            return false;
        }
    }
    return true;
}

harkenstudiobuild::PackResult ParseBusDefinitionSnapshotPayload(const std::uint8_t* payload,
                                           std::uint64_t size,
                                           PackCanonicalImage* image) {
    PackBusDefinitionSnapshotHeaderV1 header;
    if (!ParsePackBusDefinitionSnapshotHeaderV1(payload, size, &header)) {
        return harkenstudiobuild::kPackInvalid;
    }
    if (header.effectCount != 0 || header.effectParameterCount != 0) {
        return harkenstudiobuild::kPackInvalid;
    }
    const std::uint8_t* entry = payload;
    std::uint64_t busEnd = 0;
    std::uint64_t sendEnd = 0;
    std::uint64_t effectEnd = 0;
    std::uint64_t parameterEnd = 0;
    std::uint64_t downmixEnd = 0;
    std::uint64_t coeffEnd = 0;
    if (!EntrySectionFits(header.busOffset, header.busCount,
                          kPackBusDefinitionBusRowBytes, size, &busEnd) ||
        !EntrySectionFits(header.sendOffset, header.sendCount,
                          kPackBusDefinitionSendRowBytes, size, &sendEnd) ||
        !EntrySectionFits(header.effectOffset, header.effectCount,
                          kPackBusDefinitionEffectRowBytes, size, &effectEnd) ||
        !EntrySectionFits(header.effectParameterOffset,
                          header.effectParameterCount,
                          kPackBusDefinitionEffectParameterRowBytes, size,
                          &parameterEnd) ||
        !EntrySectionFits(header.downmixOffset, header.downmixCount,
                          kPackBusDefinitionDownmixRowBytes, size,
                          &downmixEnd) ||
        !EntrySectionFits(header.coefficientOffset, header.coefficientCount,
                          kPackBusDefinitionCoefficientRowBytes, size,
                          &coeffEnd)) {
        return harkenstudiobuild::kPackInvalid;
    }
    const std::uint64_t ends[] = {busEnd, sendEnd, effectEnd, parameterEnd,
                                  downmixEnd, coeffEnd};
    const std::uint64_t starts[] = {
        header.busOffset, header.sendOffset, header.effectOffset,
        header.effectParameterOffset, header.downmixOffset,
        header.coefficientOffset};
    for (std::size_t i = 0; i < 6; ++i) {
        if (ends[i] == starts[i]) {
            continue;
        }
        for (std::size_t j = i + 1; j < 6; ++j) {
            if (ends[j] == starts[j]) {
                continue;
            }
            if (!(ends[i] <= starts[j] || ends[j] <= starts[i])) {
                return harkenstudiobuild::kPackInvalid;
            }
        }
    }
    std::array<std::pair<std::uint64_t, std::uint64_t>, 6> ordered = {{
        {header.busOffset, busEnd},
        {header.sendOffset, sendEnd},
        {header.effectOffset, effectEnd},
        {header.effectParameterOffset, parameterEnd},
        {header.downmixOffset, downmixEnd},
        {header.coefficientOffset, coeffEnd},
    }};
    std::sort(ordered.begin(), ordered.end());
    std::uint64_t cursor = kPackBusDefinitionSnapshotHeaderBytes;
    for (const auto& span : ordered) {
        if (span.second == span.first) {
            continue;
        }
        if (span.first < cursor || !ZeroPadding(entry, cursor, span.first)) {
            return harkenstudiobuild::kPackInvalid;
        }
        cursor = span.second;
    }
    if (!ZeroPadding(entry, cursor, size)) {
        return harkenstudiobuild::kPackInvalid;
    }

    std::vector<PackBusDefinitionBusV1> buses;
    buses.reserve(header.busCount);
    for (std::uint32_t i = 0; i < header.busCount; ++i) {
        PackBusDefinitionBusV1 row;
        if (!ParsePackBusDefinitionBusV1(
                entry + header.busOffset +
                    static_cast<std::size_t>(i) * kPackBusDefinitionBusRowBytes,
                &row) ||
            row.busId == 0) {
            return harkenstudiobuild::kPackInvalid;
        }
        std::uint8_t zeroFader[4] = {};
        StoreLEF32(zeroFader, 0.0f);
        std::uint8_t actualFader[4] = {};
        StoreLEF32(actualFader, row.faderDb);
        if (row.effectCount != 0 ||
            std::memcmp(actualFader, zeroFader, 4) != 0) {
            return harkenstudiobuild::kPackInvalid;
        }
        if (row.downmixCount > 1) {
            return harkenstudiobuild::kPackInvalid;
        }
        if (row.downmixCount == 0 && row.downmixFirst != 0) {
            return harkenstudiobuild::kPackInvalid;
        }
        buses.push_back(row);
    }

    std::vector<PackBusDefinitionSendV1> sends;
    sends.reserve(header.sendCount);
    for (std::uint32_t i = 0; i < header.sendCount; ++i) {
        PackBusDefinitionSendV1 row;
        if (!ParsePackBusDefinitionSendV1(
                entry + header.sendOffset +
                    static_cast<std::size_t>(i) * kPackBusDefinitionSendRowBytes,
                &row) ||
            !std::isfinite(row.levelDb)) {
            return harkenstudiobuild::kPackInvalid;
        }
        sends.push_back(row);
    }

    std::vector<PackBusDefinitionEffectV1> effects;
    effects.reserve(header.effectCount);
    for (std::uint32_t i = 0; i < header.effectCount; ++i) {
        PackBusDefinitionEffectV1 row;
        if (!ParsePackBusDefinitionEffectV1(
                entry + header.effectOffset +
                    static_cast<std::size_t>(i) *
                        kPackBusDefinitionEffectRowBytes,
                &row)) {
            return harkenstudiobuild::kPackInvalid;
        }
        effects.push_back(row);
    }

    std::vector<PackBusDefinitionEffectParameterV1> parameters;
    parameters.reserve(header.effectParameterCount);
    for (std::uint32_t i = 0; i < header.effectParameterCount; ++i) {
        PackBusDefinitionEffectParameterV1 row;
        if (!ParsePackBusDefinitionEffectParameterV1(
                entry + header.effectParameterOffset +
                    static_cast<std::size_t>(i) *
                        kPackBusDefinitionEffectParameterRowBytes,
                &row)) {
            return harkenstudiobuild::kPackInvalid;
        }
        parameters.push_back(row);
    }

    std::vector<PackBusDefinitionDownmixV1> downmixes;
    downmixes.reserve(header.downmixCount);
    for (std::uint32_t i = 0; i < header.downmixCount; ++i) {
        PackBusDefinitionDownmixV1 row;
        if (!ParsePackBusDefinitionDownmixV1(
                entry + header.downmixOffset +
                    static_cast<std::size_t>(i) *
                        kPackBusDefinitionDownmixRowBytes,
                &row) ||
            row.coefficientCount != kPackBusDefinitionDownmixCoefficientCount) {
            return harkenstudiobuild::kPackInvalid;
        }
        std::uint64_t coeffEndIndex = 0;
        if (!CheckedAddU64(row.coefficientFirst, row.coefficientCount, &coeffEndIndex) ||
            coeffEndIndex > header.coefficientCount) {
            return harkenstudiobuild::kPackInvalid;
        }
        downmixes.push_back(row);
    }

    std::vector<float> coefficients;
    coefficients.reserve(header.coefficientCount);
    for (std::uint32_t i = 0; i < header.coefficientCount; ++i) {
        const float value = LoadLEF32(
            entry + header.coefficientOffset +
            static_cast<std::size_t>(i) * kPackBusDefinitionCoefficientRowBytes);
        if (!std::isfinite(value)) {
            return harkenstudiobuild::kPackInvalid;
        }
        coefficients.push_back(value);
    }

    std::vector<std::uint32_t> claimCount(header.downmixCount, 0);
    for (const auto& bus : buses) {
        if (bus.downmixCount == 0) {
            continue;
        }
        if (bus.downmixFirst >= header.downmixCount) {
            return harkenstudiobuild::kPackInvalid;
        }
        const PackBusDefinitionDownmixV1& row = downmixes[bus.downmixFirst];
        if (row.busId != bus.busId) {
            return harkenstudiobuild::kPackInvalid;
        }
        ++claimCount[bus.downmixFirst];
    }
    for (std::uint32_t i = 0; i < header.downmixCount; ++i) {
        if (claimCount[i] != 1) {
            return harkenstudiobuild::kPackInvalid;
        }
        bool found = false;
        for (const auto& bus : buses) {
            if (bus.busId == downmixes[i].busId) {
                found = true;
                break;
            }
        }
        if (!found) {
            return harkenstudiobuild::kPackInvalid;
        }
        for (std::uint32_t j = i + 1; j < header.downmixCount; ++j) {
            if (downmixes[i].busId == downmixes[j].busId) {
                return harkenstudiobuild::kPackInvalid;
            }
            const std::uint32_t a0 = downmixes[i].coefficientFirst;
            const std::uint32_t a1 = a0 + downmixes[i].coefficientCount;
            const std::uint32_t b0 = downmixes[j].coefficientFirst;
            const std::uint32_t b1 = b0 + downmixes[j].coefficientCount;
            if (!(a1 <= b0 || b1 <= a0)) {
                return harkenstudiobuild::kPackInvalid;
            }
        }
    }

    image->hasBusDefinitionSnapshot = true;
    image->busDefinitionSnapshotHeader = header;
    image->busDefinitionBuses = std::move(buses);
    image->busDefinitionSends = std::move(sends);
    image->busDefinitionEffects = std::move(effects);
    image->busDefinitionEffectParameters = std::move(parameters);
    image->busDefinitionDownmixes = std::move(downmixes);
    image->busDefinitionCoefficients = std::move(coefficients);
    return harkenstudiobuild::kPackOk;
}

harkenstudiobuild::PackResult ParseBusDefinitionSnapshotPayloadV2(
    const std::uint8_t* payload, std::uint64_t size, PackCanonicalImage* image) {
    PackBusDefinitionSnapshotHeaderV1 header;
    if (!ParsePackBusDefinitionSnapshotHeaderV2(payload, size, &header) ||
        header.busCount == 0 || header.busCount > 4096 ||
        header.effectCount != 0 || header.effectParameterCount != 0 ||
        header.downmixCount != header.busCount ||
        header.coefficientCount != static_cast<std::uint64_t>(header.busCount) * 64) {
        return kPackInvalid;
    }
    std::uint64_t busEnd = 0, sendEnd = 0, downmixEnd = 0, coeffEnd = 0;
    if (!EntrySectionFits(header.busOffset, header.busCount, header.busRowBytes,
                          size, &busEnd, header.headerBytes) ||
        !EntrySectionFits(header.sendOffset, header.sendCount, header.sendRowBytes,
                          size, &sendEnd, header.headerBytes) ||
        !EntrySectionFits(header.downmixOffset, header.downmixCount,
                          header.downmixRowBytes, size, &downmixEnd, header.headerBytes) ||
        !EntrySectionFits(header.coefficientOffset, header.coefficientCount,
                          header.coefficientRowBytes, size, &coeffEnd, header.headerBytes) ||
        header.busOffset != header.headerBytes || header.sendOffset != busEnd ||
        header.effectOffset != sendEnd || header.effectParameterOffset != sendEnd ||
        header.downmixOffset != sendEnd || header.coefficientOffset != downmixEnd ||
        coeffEnd != size) {
        return kPackInvalid;
    }
    std::vector<PackBusDefinitionBusV2> buses;
    std::set<harkenstudio::ObjectId> ids;
    buses.reserve(header.busCount);
    for (std::uint32_t i = 0; i < header.busCount; ++i) {
        PackBusDefinitionBusV2 row;
        if (!ParsePackBusDefinitionBusV2(payload + header.busOffset +
                static_cast<std::size_t>(i) * header.busRowBytes, &row) ||
            harkenstudio::IsNull(row.busId) || !ids.insert(row.busId).second ||
            !std::isfinite(row.faderDb) || row.faderDb < -96.0f || row.faderDb > 24.0f ||
            !std::isfinite(row.makeUpGainDb) ||
            !std::isfinite(row.outputBusVolumeDb) ||
            !std::isfinite(std::pow(10.0f, (row.faderDb + row.makeUpGainDb) / 20.0f)) ||
            !std::isfinite(std::pow(10.0f, row.outputBusVolumeDb / 20.0f)) ||
            row.effectFirst != 0 || row.effectCount != 0 ||
            row.downmixCount != 1 || row.downmixFirst != i) {
            return kPackInvalid;
        }
        buses.push_back(row);
    }
    if (!ValidateCanonicalBusParentForestV2(buses)) return kPackInvalid;
    std::vector<PackBusDefinitionSendV2> sends;
    std::set<std::pair<harkenstudio::ObjectId, harkenstudio::ObjectId>> sendEdges;
    sends.reserve(header.sendCount);
    for (std::uint32_t i = 0; i < header.sendCount; ++i) {
        PackBusDefinitionSendV2 row;
        if (!ParsePackBusDefinitionSendV2(payload + header.sendOffset +
                static_cast<std::size_t>(i) * header.sendRowBytes, &row) ||
            !ids.contains(row.source) || !ids.contains(row.target) ||
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
        sends.push_back(row);
    }
    if (header.publicationCapabilityVintage == 4 &&
        !ValidateCanonicalBusSendGraphV2(buses, sends)) return kPackInvalid;
    std::vector<PackBusDefinitionDownmixV2> downmixes;
    downmixes.reserve(header.downmixCount);
    for (std::uint32_t i = 0; i < header.downmixCount; ++i) {
        PackBusDefinitionDownmixV2 row;
        if (!ParsePackBusDefinitionDownmixV2(payload + header.downmixOffset +
                static_cast<std::size_t>(i) * header.downmixRowBytes, &row) ||
            row.busId != buses[i].busId || row.coefficientCount != 64 ||
            row.coefficientFirst != i * 64) {
            return kPackInvalid;
        }
        downmixes.push_back(row);
    }
    std::vector<float> coefficients;
    coefficients.reserve(header.coefficientCount);
    for (std::uint32_t i = 0; i < header.coefficientCount; ++i) {
        const float value = LoadLEF32(payload + header.coefficientOffset +
            static_cast<std::size_t>(i) * header.coefficientRowBytes);
        if (!std::isfinite(value)) return kPackInvalid;
        coefficients.push_back(value);
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
        return PackV2IsCanonicalKind(packKind);
    case kPackEntryKindInitRoutingCatalog:
    case kPackEntryKindGameplayCatalog:
    case kPackEntryKindVariableNameCatalog:
        return packKind == kPackKindInit;
    case kPackEntryKindCanonicalDefinitionPartition:
    case kPackEntryKindPackRequirementBlock:
    case kPackEntryKindPackDependencyEdge:
        return PackV2IsCanonicalKind(packKind);
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

bool InitSelfHashMatches(const std::uint8_t* bytes, std::uint64_t fileBytes,
                         const PackCanonicalImage& image) {
    if (!image.hasCatalog || image.packKind != kPackKindInit) {
        return true;
    }
    std::vector<std::uint8_t> copy(bytes, bytes + static_cast<std::size_t>(fileBytes));
    bool sawInitOwned = false;
    std::uint64_t expected = 0;
    for (std::size_t i = 0; i < image.catalogRows.size(); ++i) {
        const PackInitCatalogRow& row = image.catalogRows[i];
        if (row.ownerPackStableId != image.packStableId) {
            continue;
        }
        if (!sawInitOwned) {
            expected = row.expectedContentHash;
            sawInitOwned = true;
        } else if (row.expectedContentHash != expected) {
            return false;
        }
        const std::uint64_t rowOffset =
            image.catalogPayloadOffset + kInitCatalogHeaderBytes +
            static_cast<std::uint64_t>(i) * kInitCatalogRowBytes;
        StoreLE64(copy.data() + static_cast<std::size_t>(rowOffset) +
                      kInitCatalogContentHashOffset, 0);
    }
    if (!sawInitOwned) {
        return true;
    }
    return Fnv1a64(copy.data(), static_cast<std::size_t>(fileBytes)) == expected;
}

harkenstudiobuild::PackResult ParseGameplayCatalogPayload(const std::uint8_t* payload,
                                     std::uint64_t size,
                                     GameplayCatalogImage* out) {
    if (size > kGameplayCatalogMaxBytes) {
        return harkenstudiobuild::kPackCatalogCapacity;
    }
    if (payload == nullptr || out == nullptr ||
        size < kGameplayCatalogHeaderBytes) {
        return harkenstudiobuild::kPackInvalid;
    }
    PackGameplayCatalogHeader header;
    LoadPackGameplayCatalogHeader(payload, &header);
    if (header.activeCount > kGameplayCatalogMaxEntries ||
        header.retiredCount > kGameplayCatalogMaxEntries) {
        return harkenstudiobuild::kPackCatalogCapacity;
    }
    std::uint64_t expectedBytes = 0;
    if (!GameplayCatalogBytes(header.activeCount, header.retiredCount,
                              header.namePoolBytes, &expectedBytes) ||
        expectedBytes > kGameplayCatalogMaxBytes) {
        return harkenstudiobuild::kPackCatalogCapacity;
    }
    if (expectedBytes != size) {
        return harkenstudiobuild::kPackInvalid;
    }
    if (header.schemaVersion != 1 ||
        header.layoutVintage != kPackGameplayCatalogVintage) {
        return harkenstudiobuild::kPackIncompatible;
    }
    if (header.reserved0 != 0 || header.reserved1 != 0 ||
        (header.flags & ~kGameplayCatalogFlagHasRetired) != 0 ||
        ((header.flags & kGameplayCatalogFlagHasRetired) != 0) !=
            (header.retiredCount > 0) ||
        (header.namePoolBytes & 7u) != 0 ||
        Fnv1a64(payload, 88) != header.headerChecksum ||
        GameplayCatalogDigest(payload, static_cast<std::size_t>(size)) !=
            header.digest ||
        GameplayCatalogPublicationPairing(header) !=
            header.publicationPairingFingerprint) {
        return harkenstudiobuild::kPackInvalid;
    }

    const std::uint8_t* activeBase = payload + kGameplayCatalogHeaderBytes;
    const std::uint8_t* retiredBase =
        activeBase + static_cast<std::size_t>(header.activeCount) *
                         kGameplayCatalogActiveRowBytes;
    const std::uint8_t* pool =
        retiredBase + static_cast<std::size_t>(header.retiredCount) *
                          kGameplayCatalogRetiredRowBytes;

    GameplayCatalogImage image;
    image.header = header;
    image.active.reserve(header.activeCount);
    image.retired.reserve(header.retiredCount);
    image.namePool.assign(pool, pool + header.namePoolBytes);

    std::uint32_t cursor = 0;
    for (std::uint32_t i = 0; i < header.activeCount; ++i) {
        PackGameplayCatalogActiveRow row;
        if (!ParsePackGameplayCatalogActiveRow(
                activeBase + static_cast<std::size_t>(i) *
                                 kGameplayCatalogActiveRowBytes,
                &row) ||
            row.nameOffset != cursor ||
            row.nameOffset > header.namePoolBytes ||
            row.nameLength > header.namePoolBytes - row.nameOffset) {
            return harkenstudiobuild::kPackInvalid;
        }
        for (std::uint32_t j = 0; j < i; ++j) {
            if (image.active[j].actionStableId == row.actionStableId) {
                return harkenstudiobuild::kPackInvalid;
            }
            const int nameCmp = CompareGameplayCatalogNameBytes(
                image.namePool.data() + image.active[j].nameOffset,
                image.active[j].nameLength,
                image.namePool.data() + row.nameOffset, row.nameLength);
            if (nameCmp == 0) {
                return harkenstudiobuild::kPackInvalid;
            }
        }
        if (i > 0) {
            const PackGameplayCatalogActiveRow& prev = image.active.back();
            const int nameCmp = CompareGameplayCatalogNameBytes(
                image.namePool.data() + prev.nameOffset, prev.nameLength,
                image.namePool.data() + row.nameOffset, row.nameLength);
            if (nameCmp > 0 ||
                (nameCmp == 0 && prev.actionStableId >= row.actionStableId)) {
                return harkenstudiobuild::kPackInvalid;
            }
        }
        cursor += row.nameLength;
        image.active.push_back(row);
    }
    for (std::uint32_t i = 0; i < header.retiredCount; ++i) {
        PackGameplayCatalogRetiredRow row;
        if (!ParsePackGameplayCatalogRetiredRow(
                retiredBase + static_cast<std::size_t>(i) *
                                  kGameplayCatalogRetiredRowBytes,
                &row) ||
            row.nameOffset != cursor ||
            row.nameOffset > header.namePoolBytes ||
            row.nameLength > header.namePoolBytes - row.nameOffset) {
            return harkenstudiobuild::kPackInvalid;
        }
        for (const auto& active : image.active) {
            if (CompareGameplayCatalogNameBytes(
                    image.namePool.data() + active.nameOffset, active.nameLength,
                    image.namePool.data() + row.nameOffset, row.nameLength) ==
                0) {
                return harkenstudiobuild::kPackInvalid;
            }
        }
        if (i > 0) {
            const PackGameplayCatalogRetiredRow& prev = image.retired.back();
            const int nameCmp = CompareGameplayCatalogNameBytes(
                image.namePool.data() + prev.nameOffset, prev.nameLength,
                image.namePool.data() + row.nameOffset, row.nameLength);
            if (nameCmp > 0 ||
                (nameCmp == 0 &&
                 prev.lastActionStableId >= row.lastActionStableId)) {
                return harkenstudiobuild::kPackInvalid;
            }
        }
        cursor += row.nameLength;
        image.retired.push_back(row);
    }
    const std::uint32_t padded = (cursor + 7u) & ~7u;
    if (padded != header.namePoolBytes) {
        return harkenstudiobuild::kPackInvalid;
    }
    for (std::uint32_t i = cursor; i < header.namePoolBytes; ++i) {
        if (image.namePool[i] != 0) {
            return harkenstudiobuild::kPackInvalid;
        }
    }
    *out = std::move(image);
    return harkenstudiobuild::kPackOk;
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
    if (bytes == nullptr || out == nullptr || fileBytes < kPackV2HeaderBytes) {
        return harkenstudiobuild::kPackInvalid;
    }
    PackV2Header header;
    if (!ParsePackV2Header(bytes, &header)) {
        return harkenstudiobuild::kPackInvalid;
    }
    if (header.formatVersion != kPackFormatVersion2 ||
        header.headerBytes != kPackV2HeaderBytes ||
        header.fileBytes != fileBytes) {
        return harkenstudiobuild::kPackInvalid;
    }
    if (!PackV2IsCanonicalKind(header.packKind)) {
        return harkenstudiobuild::kPackInvalid;
    }
    const std::uint64_t dirBytes =
        static_cast<std::uint64_t>(header.entryCount) *
        kPackV2DirectoryEntryBytes;
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
    image.catalogPayloadOffset = 0;

    std::uint64_t attachmentOrdinal = 0;
    std::uint64_t sendOrdinal = 0;
    std::uint64_t busEffectOrdinal = 0;
    std::uint64_t dynamicRouteOrdinal = 0;
    std::uint64_t dependencyOrdinal = 0;
    std::uint32_t canonicalVintage = 0;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    ranges.reserve(static_cast<std::size_t>(header.entryCount) * 2);

    for (std::uint32_t i = 0; i < header.entryCount; ++i) {
        const PackV2DirectoryEntry entry = ParsePackV2DirectoryEntry(
            dir + static_cast<std::size_t>(i) * kPackV2DirectoryEntryBytes);
        if (entry.kind == kPackEntryKindCanonicalDefinitionPartition ||
            entry.kind == kPackEntryKindPackRequirementBlock ||
            entry.kind == kPackEntryKindPackDependencyEdge) {
            if ((entry.vintage != kPackCanonicalRequirementVintage &&
                 entry.vintage != kPackCanonicalEntryVintage) ||
                (canonicalVintage != 0 && canonicalVintage != entry.vintage)) {
                return harkenstudiobuild::kPackIncompatible;
            }
            canonicalVintage = entry.vintage;
        } else if (entry.kind == kPackEntryKindBusDefinitionSnapshot) {
            if (entry.vintage != kPackBusDefinitionSnapshotVintage &&
                entry.vintage != kPackBusDefinitionSnapshotVintage2) {
                return harkenstudiobuild::kPackIncompatible;
            }
        } else if (PackV2IsCanonicalEntryKind(entry.kind)) {
            if (entry.vintage != kPackCanonicalEntryVintage) {
                return harkenstudiobuild::kPackIncompatible;
            }
        } else if (!VintageLegalForKind(entry.kind, entry.vintage)) {
            return harkenstudiobuild::kPackIncompatible;
        }
        if (entry.offset < dirEnd || entry.offset > fileBytes ||
            entry.size > fileBytes - entry.offset ||
            entry.offset % kPackV2PayloadAlign != 0) {
            return harkenstudiobuild::kPackInvalid;
        }
        ranges.emplace_back(entry.offset, entry.size);
        const std::uint8_t* payload = bytes + entry.offset;
        const bool catalogKind = entry.kind == kPackEntryKindInitRoutingCatalog;
        if (!catalogKind &&
            Fnv1a64(payload, static_cast<std::size_t>(entry.size)) !=
                entry.payloadChecksum) {
            return harkenstudiobuild::kPackInvalid;
        }
        switch (entry.kind) {
        case kPackEntryKindInitRoutingCatalog: {
            if (image.hasCatalog) {
                return harkenstudiobuild::kPackInvalid;
            }
            PackInitCatalogHeader catalogHeader;
            if (!ParsePackInitCatalogHeader(payload, entry.size, &catalogHeader) ||
                catalogHeader.catalogEntryCount > kInitCatalogMaxEntries ||
                catalogHeader.startupPackCount > kInitCatalogMaxStartupPacks) {
                return harkenstudiobuild::kPackInvalid;
            }
            std::uint64_t expectedBytes = 0;
            if (!InitCatalogBytes(catalogHeader.catalogEntryCount,
                                  catalogHeader.startupPackCount,
                                  &expectedBytes) ||
                expectedBytes != catalogHeader.catalogBytes ||
                entry.size != catalogHeader.catalogBytes) {
                return harkenstudiobuild::kPackInvalid;
            }
            image.catalogRows.reserve(catalogHeader.catalogEntryCount);
            for (std::uint32_t r = 0; r < catalogHeader.catalogEntryCount; ++r) {
                PackInitCatalogRow row;
                if (!ParsePackInitCatalogRow(
                        payload + kInitCatalogHeaderBytes +
                            static_cast<std::size_t>(r) * kInitCatalogRowBytes,
                        &row)) {
                    return harkenstudiobuild::kPackInvalid;
                }
                if (r > 0 &&
                    row.stableId <= image.catalogRows.back().stableId) {
                    return harkenstudiobuild::kPackInvalid;
                }
                image.catalogRows.push_back(row);
            }
            image.startupPackIds.reserve(catalogHeader.startupPackCount);
            const std::uint8_t* startup =
                payload + kInitCatalogHeaderBytes +
                static_cast<std::size_t>(catalogHeader.catalogEntryCount) *
                    kInitCatalogRowBytes;
            for (std::uint32_t s = 0; s < catalogHeader.startupPackCount; ++s) {
                const std::uint64_t packId = LoadLE64(startup + s * 8);
                if (packId == 0) {
                    return harkenstudiobuild::kPackInvalid;
                }
                for (std::uint64_t seen : image.startupPackIds) {
                    if (seen == packId) {
                        return harkenstudiobuild::kPackInvalid;
                    }
                }
                bool ownerSeen = false;
                for (const PackInitCatalogRow& row : image.catalogRows) {
                    if (row.ownerPackStableId == packId) {
                        ownerSeen = true;
                        break;
                    }
                }
                if (!ownerSeen) {
                    return harkenstudiobuild::kPackInvalid;
                }
                image.startupPackIds.push_back(packId);
            }
            image.catalogHeader = catalogHeader;
            image.hasCatalog = true;
            image.catalogPayloadOffset = entry.offset;
            image.catalogDirectoryChecksum = entry.payloadChecksum;
            break;
        }
        case kPackEntryKindVariableNameCatalog: {
            if (header.packKind != kPackKindInit || image.hasVariableNameCatalog ||
                entry.id != decltype(entry.id){} || entry.relationOrdinal != 0 ||
                !DecodeVariableNameCatalog(payload, entry.size, &image.variableNameCatalog))
                return harkenstudiobuild::kPackInvalid;
            image.hasVariableNameCatalog = true;
            break;
        }
        case kPackEntryKindGameplayCatalog: {
            if (header.packKind != kPackKindInit || image.hasGameplayCatalog) {
                return harkenstudiobuild::kPackInvalid;
            }
            const harkenstudiobuild::PackResult parsed = ParseGameplayCatalogPayload(
                payload, entry.size, &image.gameplayCatalog);
            if (parsed != harkenstudiobuild::kPackOk) {
                return parsed;
            }
            image.hasGameplayCatalog = true;
            break;
        }
        case kPackEntryKindCanonicalDefinitionPartition: {
            if (image.hasPartition) {
                return harkenstudiobuild::kPackInvalid;
            }
            PackCanonicalPartitionHeader partitionHeader;
            const bool versionTwo = entry.vintage == kPackCanonicalRequirementVintage;
            if (entry.size < (versionTwo ? kCanonicalDefinitionPartitionHeaderV2Bytes
                                        : kCanonicalDefinitionPartitionHeaderBytes)) {
                return harkenstudiobuild::kPackIncompatible;
            }
            const bool parsed = versionTwo
                ? ParsePackCanonicalPartitionHeaderV2(payload, entry.size, &partitionHeader)
                : ParsePackCanonicalPartitionHeader(payload, entry.size, &partitionHeader);
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
            const bool versionTwo = entry.vintage == kPackCanonicalRequirementVintage;
            if (entry.size != (versionTwo ? kPackRequirementBlockV2Bytes : kPackRequirementBlockBytes)) {
                return harkenstudiobuild::kPackIncompatible;
            }
            if (LoadLE32(payload) != entry.vintage || LoadLE32(payload + 4) != entry.vintage) {
                return harkenstudiobuild::kPackIncompatible;
            }
            const bool parsed = versionTwo
                ? ParsePackRequirementBlockV2(payload, entry.size, &requirement)
                : ParsePackRequirementBlock(payload, entry.size, &requirement);
            if (!parsed ||
                requirement.payloadChecksumCoverage != Fnv1a64(payload, 104)) {
                return harkenstudiobuild::kPackInvalid;
            }
            image.requirement = requirement;
            image.packStableId = requirement.packStableId;
            image.hasRequirement = true;
            break;
        }
        case kPackEntryKindPackDependencyEdge: {
            PackDependencyEdge edge;
            const bool versionTwo = entry.vintage == kPackCanonicalRequirementVintage;
            if (entry.size != (versionTwo ? kPackDependencyEdgeV2Bytes : kPackDependencyEdgeBytes)) {
                return harkenstudiobuild::kPackIncompatible;
            }
            const bool parsed = versionTwo
                ? ParsePackDependencyEdgeV2(payload, entry.size, &edge)
                : ParsePackDependencyEdge(payload, entry.size, &edge);
            if (!parsed ||
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
                entry.vintage == kPackBusDefinitionSnapshotVintage2
                    ? ParseBusDefinitionSnapshotPayloadV2(payload, entry.size, &image)
                    : ParseBusDefinitionSnapshotPayload(payload, entry.size, &image);
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
            if (entry.size != kPackV2MediaPayloadBytes) {
                return harkenstudiobuild::kPackInvalid;
            }
            const PackTocEntry media = ParsePackTocEntry(payload);
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
            if (!ParsePackV2BusEffect(payload, entry.size, &effect) ||
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
            if (!ParsePackV2EffectDynamicRoute(payload, entry.size, &route) ||
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
                entry.vintage == kPackV2SchemaVintage
                    ? ParsePackV2Bus(payload, entry.size, &bus)
                    : ParsePackV6Bus(payload, entry.size, &bus);
            if (!parsed || bus.busId != entry.id ||
                !std::isfinite(bus.volume) ||
                !std::isfinite(bus.outputBusVolume) ||
                !std::isfinite(bus.makeUpGain) ||
                !std::isfinite(bus.volume +
                               bus.makeUpGain)) {
                return harkenstudiobuild::kPackInvalid;
            }
            image.buses.push_back(bus);
            break;
        }
        case kPackEntryKindBusSend: {
            PackBusSendEntry send;
            if (!ParsePackV2BusSend(payload, entry.size, &send) ||
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
            if (!ParsePackV3Attenuation(payload, entry.size,
                                        &attenuation.desc)) {
                return harkenstudiobuild::kPackInvalid;
            }
            image.attenuations.push_back(attenuation);
            break;
        }
        case kPackEntryKindBindingDefinition: {
            PackLogicImage::BindingDefinition definition;
            if (!ParsePackV2BindingDefinition(payload, entry.size,
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
                canonicalVintage == kPackCanonicalRequirementVintage &&
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
            if (!ParsePackV2BindingAttachment(payload, entry.size,
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
                canonicalVintage == kPackCanonicalRequirementVintage &&
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
            canonicalVintage == kPackCanonicalRequirementVintage &&
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
            canonicalVintage == kPackCanonicalRequirementVintage &&
            attachment.scopeKind != harkenstudio::kTargetScopeBus &&
            attachment.scopeKind != harkenstudio::kTargetScopeBusEffectParam) {
            return harkenstudiobuild::kPackInvalid;
        }
    }
    if (canonicalVintage == kPackCanonicalEntryVintage && image.hasBusDefinitionSnapshot) {
        return harkenstudiobuild::kPackInvalid;
    }
    if (image.hasVariableNameCatalog &&
        (image.variableNameCatalog.initPackStableId != image.packStableId ||
         image.variableNameCatalog.initContentRevision != image.requirement.packContentRevision))
        return harkenstudiobuild::kPackInvalid;
    if (image.hasGameplayCatalog &&
        (image.gameplayCatalog.header.initPackStableId != image.packStableId ||
         image.gameplayCatalog.header.initContentRevision !=
             image.requirement.packContentRevision)) {
        return harkenstudiobuild::kPackInvalid;
    }
    if (header.packKind == kPackKindCanonicalContent && image.hasCatalog) {
        return harkenstudiobuild::kPackInvalid;
    }
    const bool sharedResidentOwner =
        header.packKind == kPackKindCanonicalContent &&
        (!image.buses.empty() || !image.busSends.empty() ||
         !image.attenuations.empty() || !image.busEffects.empty() ||
         !image.dynamicRoutes.empty());
    for (std::uint32_t i = 0; i < header.entryCount; ++i) {
        const PackV2DirectoryEntry entry = ParsePackV2DirectoryEntry(
            dir + static_cast<std::size_t>(i) * kPackV2DirectoryEntryBytes);
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
    if (image.hasCatalog) {
        std::vector<std::uint8_t> catalogCopy(
            bytes + image.catalogPayloadOffset,
            bytes + image.catalogPayloadOffset +
                static_cast<std::size_t>(image.catalogHeader.catalogBytes));
        for (std::size_t i = 0; i < image.catalogRows.size(); ++i) {
            if (image.catalogRows[i].ownerPackStableId != image.packStableId)
                continue;
            StoreLE64(catalogCopy.data() + kInitCatalogHeaderBytes +
                          i * kInitCatalogRowBytes + kInitCatalogContentHashOffset,
                      0);
        }
        if (Fnv1a64(catalogCopy.data() + kInitCatalogHeaderBytes,
                    static_cast<std::size_t>(image.catalogHeader.catalogBytes -
                                             kInitCatalogHeaderBytes)) !=
            image.catalogHeader.checksum) {
            return harkenstudiobuild::kPackInvalid;
        }
        if (Fnv1a64(catalogCopy.data(), catalogCopy.size()) !=
            image.catalogDirectoryChecksum) {
            return harkenstudiobuild::kPackInvalid;
        }
    }
    if (!InitSelfHashMatches(bytes, fileBytes, image)) {
        return harkenstudiobuild::kPackHashMismatch;
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
        for (const PackContainerChildV2& child : container.children) {
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
