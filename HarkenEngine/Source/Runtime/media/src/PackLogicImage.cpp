#include <harken/core/BindingTransition.h>
#include "harken/media/PackLogicImage.h"

#include "harken/core/CurrentWorldCodec.h"
#include "harken/core/EngineConfig.h"
#include "harken/core/Utf8Path.h"
#include "harken/core/detail/CurrentWorldValidation.h"
#include "harken/dsp/GainRamp.h"
#include "harken/media/generated/harken_pack_effect_schema.g.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include "harken/core/VariableExecutionFingerprint.h"
#include <utility>

namespace harken {

namespace {

// v2 media entries must satisfy every v1 container invariant. The checks
// are replicated from the v1 reader rather than shared with it on
// purpose: the v1 parse branch stays untouched line for line, and the
// two branches version independently from here on.
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

// Whether an entry kind may travel in a pack of this kind. The
// classification rule is mechanical (section 19.1): content cargo uses the
// ordinary symmetric unregister paths. Engine infrastructure is resident-only;
// Bus/Send/Bus-Effect/Dynamic-Route rows may leave through the dedicated
// reversible Resident transaction, while attenuation stays one-way. Legacy
// Variable and meter entry numbers remain recognizable only to reject them.
// Binding definitions and attachments travel in both. Resident packs
// restrict them to Bus-hosted surfaces; Voice Effect Parameter bindings
// travel through content-pack Recipes instead of attachments. These rules
// are enforced after payload parse.
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
    std::map<VnObjectId, std::size_t> indices;
    for (std::size_t i = 0; i < buses.size(); ++i) {
        if (buses[i].busId == VnObjectId{} ||
            !indices.emplace(buses[i].busId, i).second) return false;
    }
    const auto master = indices.find(VN_BUS_MASTER);
    if (master == indices.end() ||
        buses[master->second].parentId != VnObjectId{}) return false;

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
    std::map<VnObjectId, std::size_t> indices;
    for (std::size_t i = 0; i < buses.size(); ++i) {
        indices.emplace(buses[i].busId, i);
    }
    std::vector<std::vector<std::size_t>> edges(buses.size());
    std::vector<std::size_t> indegree(buses.size(), 0);
    const auto addEdge = [&](VnObjectId source, VnObjectId target) {
        const auto from = indices.find(source);
        const auto to = indices.find(target);
        if (from == indices.end() || to == indices.end()) return false;
        edges[from->second].push_back(to->second);
        ++indegree[to->second];
        return true;
    };
    for (const auto& bus : buses) {
        if (bus.parentId != VnObjectId{} &&
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

VnResult ParsePackV2Image(const std::uint8_t* bytes, std::uint64_t fileBytes,
                          PackLogicImage* out) {
    if (bytes == nullptr || fileBytes < kPackV2HeaderBytes) {
        return VN_BAD_PACK;
    }
    PackV2Header header;
    if (!ParsePackV2Header(bytes, &header)) {
        return VN_BAD_PACK;
    }
    if (!SupportedPackFormatVersion(header.formatVersion) ||
        header.headerBytes != kPackV2HeaderBytes ||
        header.fileBytes != fileBytes) {
        return VN_BAD_PACK;
    }
    if (PackV2IsCanonicalKind(header.packKind)) {
        return VN_PACK_INIT_LIFECYCLE;
    }
    if (header.packKind != kPackKindResident &&
        header.packKind != kPackKindContent) {
        return VN_BAD_PACK;
    }
    if (header.entryCount == 0 &&
        (header.packKind != kPackKindContent ||
         header.fileBytes != header.headerBytes)) {
        return VN_BAD_PACK;
    }
    const std::uint64_t dirBytes =
        static_cast<std::uint64_t>(header.entryCount) *
        kPackV2DirectoryEntryBytes;
    // Canonical layout, enforced rather than merely emitted: the
    // directory sits immediately after the header (one writer shape, no
    // reader heuristics), so a gap between them is malformed.
    if (header.directoryOffset != header.headerBytes ||
        header.directoryOffset > fileBytes ||
        dirBytes > fileBytes - header.directoryOffset) {
        return VN_BAD_PACK;
    }
    const std::uint8_t* dir = bytes + header.directoryOffset;
    if (Fnv1a64(dir, static_cast<std::size_t>(dirBytes)) !=
        header.directoryChecksum) {
        return VN_BAD_PACK;
    }
    const std::uint64_t dirEnd = header.directoryOffset + dirBytes;

    PackLogicImage image;
    image.packKind = header.packKind;

    // Relation kinds carry ordinal directory ids (their identity lives in
    // the payload); these count appearances to pin the ordinals.
    std::uint64_t sendOrdinal = 0;
    std::uint64_t attachmentOrdinal = 0;
    std::uint64_t busEffectOrdinal = 0;
    std::uint64_t dynamicRouteOrdinal = 0;

    // Every payload and media blob byte range, for the no-overlap rule
    // checked after the loop: one file byte belongs to at most one
    // entry's payload or blob (aliased ranges are a malformed writer,
    // never forward compatibility).
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    ranges.reserve(static_cast<std::size_t>(header.entryCount) * 2);

    for (std::uint32_t i = 0; i < header.entryCount; ++i) {
        const PackV2DirectoryEntry entry = ParsePackV2DirectoryEntry(
            dir + static_cast<std::size_t>(i) * kPackV2DirectoryEntryBytes);
        if (PackV2IsCanonicalEntryKind(entry.kind)) {
            return VN_PACK_INCOMPATIBLE;
        }
        if (!VintageLegalForKind(entry.kind, entry.vintage)) {
            return VN_PACK_INCOMPATIBLE;
        }
        if (!KindLegalForPack(entry.kind, header.packKind)) {
            return VN_BAD_PACK;
        }
        if (entry.offset < dirEnd || entry.offset > fileBytes ||
            entry.size > fileBytes - entry.offset) {
            return VN_BAD_PACK;
        }
        if (entry.offset % kPackV2PayloadAlign != 0) {
            return VN_BAD_PACK; // the writer aligns every payload
        }
        ranges.emplace_back(entry.offset, entry.size);
        const std::uint8_t* payload = bytes + entry.offset;
        if (Fnv1a64(payload, static_cast<std::size_t>(entry.size)) !=
            entry.payloadChecksum) {
            return VN_BAD_PACK;
        }
        image.stamps.push_back(
            {entry.kind, entry.id, entry.payloadChecksum});
        switch (entry.kind) {
        case kPackEntryKindMedia: {
            if (entry.size != kPackV2MediaPayloadBytes) {
                return VN_BAD_PACK;
            }
            const PackTocEntry media = ParsePackTocEntry(payload);
            if (media.soundId != entry.id ||
                !ValidateV2MediaEntry(media, fileBytes, dirEnd)) {
                return VN_BAD_PACK;
            }
            if (Fnv1a64(bytes + media.blobOffset,
                        static_cast<std::size_t>(media.blobBytes)) !=
                media.blobChecksum) {
                return VN_BAD_PACK;
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
                return VN_BAD_PACK;
            }
            image.buses.push_back(bus);
            break;
        }
        case kPackEntryKindBusSend: {
            PackBusSendEntry send;
            if (!ParsePackV2BusSend(payload, entry.size, &send) ||
                entry.relationOrdinal != sendOrdinal) {
                return VN_BAD_PACK;
            }
            // Pack levels bypass the live setters, so reject non-finite dB
            // and linear conversion overflow before publishing the image.
            if (!TryVolumeGainFromDb(send.levelDb)) {
                return VN_BAD_PACK;
            }
            ++sendOrdinal;
            image.busSends.push_back(send);
            break;
        }
        case kPackEntryKindBusEffect: {
            PackBusEffectEntry effect;
            if (!ParsePackV2BusEffect(payload, entry.size, &effect) ||
                entry.relationOrdinal != busEffectOrdinal || effect.busId == VnObjectId{} ||
                effect.bypass > 1 ||
                effect.slot >= kMaxFxSlotsPerBus ||
                effect.faderPointGuard > kMaxFxSlotsPerBus) {
                return VN_BAD_PACK;
            }
            const PackEffectSchemaRow* effectSchema =
                FindPackEffectSchemaByType(effect.effectType);
            if (effectSchema == nullptr ||
                effect.descriptorVintage !=
                    effectSchema->descriptorVintage ||
                effect.paramCount != effectSchema->paramCount) {
                return VN_BAD_PACK;
            }
            for (std::uint32_t p = 0; p < effect.paramCount; ++p) {
                if (!std::isfinite(effect.params[p])) {
                    return VN_BAD_PACK;
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
                route.targetBusId == VnObjectId{} || route.sourceId == VnObjectId{} ||
                route.expectedEffectType != VN_EFFECT_COMPRESSOR ||
                route.signalForm != VN_DYNAMIC_SIGNAL_RAW_AUDIO ||
                route.sourceKind != VN_DYNAMIC_SOURCE_BUS ||
                route.tap > VN_DYNAMIC_TAP_BUS_POST_FADER ||
                route.effectSlot >= kMaxFxSlotsPerBus ||
                route.routeSlot >= 4u ||
                !std::isfinite(route.levelDb) ||
                route.levelDb < -96.0f || route.levelDb > 24.0f) {
                return VN_BAD_PACK;
            }
            ++dynamicRouteOrdinal;
            image.dynamicRoutes.push_back(route);
            break;
        }
        case kPackEntryKindAttenuation: {
            PackLogicImage::Attenuation attenuation;
            attenuation.id = entry.id;
            if (!ParsePackAttenuation(payload, entry.size, header.formatVersion,
                                        &attenuation.desc)) {
                return VN_BAD_PACK;
            }
            if (!std::isfinite(attenuation.desc.proximityFrequency) ||
                !(attenuation.desc.proximityFrequency > 0.0f)) {
                return VN_BAD_PACK;
            }
            image.attenuations.push_back(attenuation);
            break;
        }
        case kPackEntryKindDiscreteVariable:
        case kPackEntryKindMeterWiring:
            return VN_BAD_PACK;
        case kPackEntryKindBindingDefinition: {
            PackLogicImage::BindingDefinition definition;
            if (!ParsePackV2BindingDefinition(payload, entry.size,
                                              &definition.id,
                                              &definition.desc) ||
                definition.id != entry.id
                || !IsValidBindingTransitionMs(definition.desc.transitionMs)) {
                return VN_BAD_PACK;
            }
            // Binding definitions support all four released scope kinds.
            // Attachments separately admit Voice, Bus, and Bus Effect
            // Parameter; Voice Effect Parameter travels through Recipes.
            if (definition.desc.targetScopeKind >
                VN_TARGET_SCOPE_BUS_EFFECT_PARAM) {
                return VN_BAD_PACK;
            }
            // Resident packs carry the mix-level binding surface only:
            // voice and voice-effect-parameter definitions reference the
            // playable world, which is content-pack cargo.
            if (header.packKind == kPackKindResident &&
                (definition.desc.targetScopeKind == VN_TARGET_SCOPE_VOICE ||
                 definition.desc.targetScopeKind ==
                     VN_TARGET_SCOPE_VOICE_EFFECT_PARAM)) {
                return VN_BAD_PACK;
            }
            image.bindingDefinitions.push_back(definition);
            break;
        }
        case kPackEntryKindBindingAttachment: {
            PackBindingAttachmentEntry attachment;
            if (!ParsePackV2BindingAttachment(payload, entry.size,
                                              &attachment) ||
                entry.relationOrdinal != attachmentOrdinal) {
                return VN_BAD_PACK;
            }
            ++attachmentOrdinal;
            if (attachment.scopeKind ==
                    VN_TARGET_SCOPE_VOICE_EFFECT_PARAM ||
                attachment.scopeKind >
                    VN_TARGET_SCOPE_BUS_EFFECT_PARAM) {
                return VN_BAD_PACK;
            }
            if (header.packKind == kPackKindResident &&
                attachment.scopeKind != VN_TARGET_SCOPE_BUS &&
                attachment.scopeKind !=
                    VN_TARGET_SCOPE_BUS_EFFECT_PARAM) {
                return VN_BAD_PACK;
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
                return VN_BAD_PACK;
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
                recipe.header.playableId == VnObjectId{} ||
                recipe.header.playableId != entry.id) {
                return VN_BAD_PACK;
            }
            const std::uint8_t* installBase =
                payload + kPackV2VoiceRecipeHeaderBytes;
            if (!ParsePackV3VoiceRecipeTone(installBase, &recipe.tone) ||
                !VoiceToneWireValuesValid(recipe.tone)) {
                return VN_BAD_PACK;
            }
            installBase += kPackV3VoiceRecipeToneBytes;
            if (!ParsePackV5VoiceRecipeSpatial(installBase,
                                               &recipe.spatial)) {
                return VN_BAD_PACK;
            }
            installBase += kPackV4VoiceRecipeSpatialBytes;
            if (!ParsePackV5VoiceRecipePlayback(installBase,
                                                &recipe.playback)) {
                return VN_BAD_PACK;
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
                    return VN_BAD_PACK;
                }
                const PackEffectSchemaRow* effectSchema =
                    FindPackEffectSchemaByType(install.effectType);
                if (effectSchema == nullptr ||
                    install.descriptorVintage !=
                        effectSchema->descriptorVintage ||
                    install.paramCount != effectSchema->paramCount) {
                    return VN_BAD_PACK;
                }
                for (std::uint32_t p = 0; p < install.paramCount; ++p) {
                    if (!std::isfinite(install.params[p])) {
                        return VN_BAD_PACK;
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
            VnBindingId previousBinding{};
            for (std::uint32_t n = 0; n < recipe.header.bindingCount; ++n) {
                const VnBindingId binding = ParsePackV2VoiceRecipeBinding(
                    bindingBase + static_cast<std::size_t>(n) *
                                      kPackV2VoiceRecipeBindingBytes);
                if (binding == VnObjectId{} ||
                    (n > 0 && binding <= previousBinding)) {
                    return VN_BAD_PACK;
                }
                previousBinding = binding;
                recipe.bindings.push_back(binding);
            }
            image.voiceRecipes.push_back(std::move(recipe));
            break;
        }
        default:
            return VN_BAD_PACK;
        }
    }

    // No two payload/blob ranges may alias (sorted sweep; sizes are all
    // nonzero, so a shared start byte is an overlap too).
    std::sort(ranges.begin(), ranges.end());
    for (std::size_t i = 1; i < ranges.size(); ++i) {
        if (ranges[i].first < ranges[i - 1].first + ranges[i - 1].second) {
            return VN_BAD_PACK;
        }
    }

    // Per-kind id uniqueness. A directory that repeats an id is malformed
    // (the compiler refuses to build one) — the v1 duplicate rule, kept so
    // conflict codes at load keep meaning "conflicts with existing state".
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
        return VN_BAD_PACK;
    }
    // Media and containers share the flat playable-ID space, so their ids
    // are unique across BOTH sets, not just within each.
    for (const PackTocEntry& media : image.media) {
        for (const PackLogicImage::Container& container : image.containers) {
            if (media.soundId == container.header.containerId) {
                return VN_BAD_PACK;
            }
        }
    }
    // Relation uniqueness: one send edge per (from, to); one attachment
    // per (binding, scope, object). Duplicates would refuse VN_IN_USE at
    // apply, so a pack carrying them can never load — malformed.
    for (std::size_t i = 0; i < image.busSends.size(); ++i) {
        for (std::size_t j = i + 1; j < image.busSends.size(); ++j) {
            if (image.busSends[i].fromBusId == image.busSends[j].fromBusId &&
                image.busSends[i].toBusId == image.busSends[j].toBusId) {
                return VN_BAD_PACK;
            }
        }
    }
    for (std::size_t i = 0; i < image.bindingAttachments.size(); ++i) {
        for (std::size_t j = i + 1; j < image.bindingAttachments.size(); ++j) {
            const PackBindingAttachmentEntry& a = image.bindingAttachments[i];
            const PackBindingAttachmentEntry& b = image.bindingAttachments[j];
            if (a.bindingId == b.bindingId && a.scopeKind == b.scopeKind &&
                a.objectId == b.objectId) {
                return VN_BAD_PACK;
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
                return VN_BAD_PACK;
            }
        }
        for (std::size_t j = i + 1; j < image.busEffects.size(); ++j) {
            if (image.busEffects[i].busId == image.busEffects[j].busId &&
                image.busEffects[i].slot == image.busEffects[j].slot) {
                return VN_BAD_PACK;
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
                return VN_BAD_PACK;
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
                return VN_BAD_PACK;
            }
        }
    }
    // Every Pack route targets a Bus Effect declared by this exact Pack
    // revision. A live external Compressor is intentionally insufficient:
    // ownership and replacement must remain attributable to one Pack.
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
            return VN_BAD_PACK;
        }
    }
    // A send edge never connects a bus to itself (the live rule, checked
    // here so staging's conflict codes stay about existing state).
    for (const PackBusSendEntry& send : image.busSends) {
        if (send.fromBusId == send.toBusId) {
            return VN_BAD_PACK;
        }
    }

    *out = std::move(image);
    return VN_OK;
}

namespace {

VnResult ParseVariablePolicyOverlayPayload(
    const std::uint8_t* payload, std::uint64_t size,
    std::vector<PackCanonicalImage::VariablePolicyOverlay>* out) {
    if (payload == nullptr || out == nullptr || size < 16) {
        return VN_BAD_PACK;
    }
    const std::uint32_t rowCount = LoadLE32(payload);
    const std::uint32_t format = LoadLE32(payload + 4);
    if (format > 2 || LoadLE64(payload + 8) != 0) {
        return VN_BAD_PACK;
    }
    std::uint64_t rowsBytes = 0;
    std::uint64_t needed = 16;
    if (!CheckedMulU64(rowCount, 32, &rowsBytes) ||
        !CheckedAddU64(needed, rowsBytes, &needed) || needed > size) {
        return VN_BAD_PACK;
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
        if (item.variableId == VnObjectId{} || item.executionFingerprint == 0 ||
            item.writers < 1 || item.writers > 3 || row[17] != 0 ||
            LoadLE16(row + 18) != 0 || LoadLE32(row + 20) != 0) {
            return VN_BAD_PACK;
        }
        if (i > 0 && item.variableId <= rows.back().variableId) {
            return VN_BAD_PACK;
        }
        rows.push_back(item);
    }
    std::size_t cursor = static_cast<std::size_t>(needed);
    for (std::uint32_t i = 0; i < rowCount; ++i) {
        if (size - cursor < 4) {
            return VN_BAD_PACK;
        }
        const std::uint32_t count = LoadLE32(payload + cursor);
        cursor += 4;
        std::uint64_t anchorsBytes = 0;
        if (!CheckedMulU64(count, 4, &anchorsBytes) ||
            anchorsBytes > size - cursor) {
            return VN_BAD_PACK;
        }
        rows[i].discreteAnchors.reserve(count);
        for (std::uint32_t j = 0; j < count; ++j) {
            const std::int32_t anchor =
                static_cast<std::int32_t>(LoadLE32(payload + cursor));
            cursor += 4;
            if (j > 0 && anchor <= rows[i].discreteAnchors.back()) {
                return VN_BAD_PACK;
            }
            rows[i].discreteAnchors.push_back(anchor);
        }
    }
    if (format != 0) {
        for (auto& row : rows) {
            const std::uint32_t headerBytes = format == 1 ? 24 : 16;
            if (size - cursor < headerBytes) return VN_BAD_PACK;
            row.labelMappingFingerprint = LoadLE64(payload + cursor);
            if (format == 1) {
                row.catalogFingerprint = LoadLE64(payload + cursor + 8);
                if (row.catalogFingerprint == 0) return VN_BAD_PACK;
            }
            const std::uint32_t count = LoadLE32(payload + cursor + headerBytes - 8);
            if (LoadLE32(payload + cursor + headerBytes - 4) != 0
                || row.labelMappingFingerprint == 0)
                return VN_BAD_PACK;
            cursor += headerBytes;
            if (count > (size - cursor) / 24) return VN_BAD_PACK;
            std::set<VnObjectId> identities;
            std::set<std::string> names;
            row.labels.reserve(count);
            for (std::uint32_t j = 0; j < count; ++j) {
                if (size - cursor < 24) return VN_BAD_PACK;
                VariableLabelDefinitionV1 label;
                label.labelId = LoadObjectId(payload + cursor);
                label.anchor = static_cast<std::int32_t>(LoadLE32(payload + cursor + 16));
                const std::uint32_t length = LoadLE32(payload + cursor + 20);
                cursor += 24;
                if (format == 1) {
                    const std::uint64_t paddedLength = (static_cast<std::uint64_t>(length) + 3u) & ~std::uint64_t{3};
                    if (paddedLength > size - cursor) return VN_BAD_PACK;
                    label.name.assign(reinterpret_cast<const char*>(payload + cursor), length);
                    for (std::uint64_t k = length; k < paddedLength; ++k)
                        if (payload[cursor + k] != 0) return VN_BAD_PACK;
                    cursor += static_cast<std::size_t>(paddedLength);
                } else if (length != 0) {
                    return VN_BAD_PACK;
                }
                if (label.labelId == VnObjectId{} || !identities.insert(label.labelId).second
                    || label.anchor < 0 || label.anchor > 16777216
                    || (j > 0 && label.anchor <= row.labels.back().anchor)
                    || label.name.find('\0') != std::string::npos
                    || (!label.name.empty() && !names.insert(label.name).second))
                    return VN_BAD_PACK;
                row.labels.push_back(std::move(label));
            }
            if (ComputeVariableLabelMappingFingerprintV1(row.variableId, row.labels)
                != row.labelMappingFingerprint) return VN_BAD_PACK;
        }
    }
    if (cursor != size) {
        return VN_BAD_PACK;
    }
    *out = std::move(rows);
    return VN_OK;
}

VnResult ParseVariableRequirementsPayload(
    const std::uint8_t* payload, std::uint64_t size,
    std::vector<PackCanonicalImage::VariableRequirement>* out) {
    if (payload == nullptr || out == nullptr || size < 16) {
        return VN_BAD_PACK;
    }
    const std::uint32_t rowCount = LoadLE32(payload);
    const std::uint32_t format = LoadLE32(payload + 4);
    if (format > 1 || LoadLE64(payload + 8) != 0) {
        return VN_BAD_PACK;
    }
    std::uint64_t rowsBytes = 0;
    std::uint64_t needed = 16;
    const std::uint32_t rowBytes = format == 1 ? 40 : 32;
    if (!CheckedMulU64(rowCount, rowBytes, &rowsBytes) ||
        !CheckedAddU64(needed, rowsBytes, &needed) || needed != size) {
        return VN_BAD_PACK;
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
            if (item.expectedLabelMappingFingerprint == 0) return VN_BAD_PACK;
        }
        if (item.identityClass != 1 || row[1] != 0 ||
            LoadLE16(row + 2) != 0 || LoadLE32(row + 4) != 0 ||
            item.stableId == VnObjectId{} || item.expectedExecutionFingerprint == 0) {
            return VN_BAD_PACK;
        }
        if (i > 0 &&
            (item.identityClass < rows.back().identityClass ||
             (item.identityClass == rows.back().identityClass &&
              item.stableId <= rows.back().stableId))) {
            return VN_BAD_PACK;
        }
        rows.push_back(item);
    }
    *out = std::move(rows);
    return VN_OK;
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

VnResult ParseBusDefinitionSnapshotPayload(const std::uint8_t* payload,
                                           std::uint64_t size,
                                           PackCanonicalImage* image) {
    PackBusDefinitionSnapshotHeaderV1 header;
    if (!ParsePackBusDefinitionSnapshotHeaderV1(payload, size, &header)) {
        return VN_BAD_PACK;
    }
    if (header.effectCount != 0 || header.effectParameterCount != 0) {
        return VN_BAD_PACK;
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
        return VN_BAD_PACK;
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
                return VN_BAD_PACK;
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
            return VN_BAD_PACK;
        }
        cursor = span.second;
    }
    if (!ZeroPadding(entry, cursor, size)) {
        return VN_BAD_PACK;
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
            return VN_BAD_PACK;
        }
        std::uint8_t zeroFader[4] = {};
        StoreLEF32(zeroFader, 0.0f);
        std::uint8_t actualFader[4] = {};
        StoreLEF32(actualFader, row.faderDb);
        if (row.effectCount != 0 ||
            std::memcmp(actualFader, zeroFader, 4) != 0) {
            return VN_BAD_PACK;
        }
        if (row.downmixCount > 1) {
            return VN_BAD_PACK;
        }
        if (row.downmixCount == 0 && row.downmixFirst != 0) {
            return VN_BAD_PACK;
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
            !TryVolumeGainFromDb(row.levelDb)) {
            return VN_BAD_PACK;
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
            return VN_BAD_PACK;
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
            return VN_BAD_PACK;
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
            return VN_BAD_PACK;
        }
        std::uint64_t coeffEndIndex = 0;
        if (!CheckedAddU64(row.coefficientFirst, row.coefficientCount, &coeffEndIndex) ||
            coeffEndIndex > header.coefficientCount) {
            return VN_BAD_PACK;
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
            return VN_BAD_PACK;
        }
        coefficients.push_back(value);
    }

    std::vector<std::uint32_t> claimCount(header.downmixCount, 0);
    for (const auto& bus : buses) {
        if (bus.downmixCount == 0) {
            continue;
        }
        if (bus.downmixFirst >= header.downmixCount) {
            return VN_BAD_PACK;
        }
        const PackBusDefinitionDownmixV1& row = downmixes[bus.downmixFirst];
        if (row.busId != bus.busId) {
            return VN_BAD_PACK;
        }
        ++claimCount[bus.downmixFirst];
    }
    for (std::uint32_t i = 0; i < header.downmixCount; ++i) {
        if (claimCount[i] != 1) {
            return VN_BAD_PACK;
        }
        bool found = false;
        for (const auto& bus : buses) {
            if (bus.busId == downmixes[i].busId) {
                found = true;
                break;
            }
        }
        if (!found) {
            return VN_BAD_PACK;
        }
        for (std::uint32_t j = i + 1; j < header.downmixCount; ++j) {
            if (downmixes[i].busId == downmixes[j].busId) {
                return VN_BAD_PACK;
            }
            const std::uint32_t a0 = downmixes[i].coefficientFirst;
            const std::uint32_t a1 = a0 + downmixes[i].coefficientCount;
            const std::uint32_t b0 = downmixes[j].coefficientFirst;
            const std::uint32_t b1 = b0 + downmixes[j].coefficientCount;
            if (!(a1 <= b0 || b1 <= a0)) {
                return VN_BAD_PACK;
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
    return VN_OK;
}

VnResult ParseBusDefinitionSnapshotPayloadV2(
    const std::uint8_t* payload, std::uint64_t size, PackCanonicalImage* image) {
    PackBusDefinitionSnapshotHeaderV1 header;
    if (!ParsePackBusDefinitionSnapshotHeaderV2(payload, size, &header) ||
        header.busCount == 0 || header.busCount > 4096 ||
        header.effectCount != 0 || header.effectParameterCount != 0 ||
        header.downmixCount != header.busCount ||
        header.coefficientCount != static_cast<std::uint64_t>(header.busCount) * 64) {
        return VN_BAD_PACK;
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
        return VN_BAD_PACK;
    }
    std::vector<PackBusDefinitionBusV2> buses;
    std::set<VnObjectId> ids;
    buses.reserve(header.busCount);
    for (std::uint32_t i = 0; i < header.busCount; ++i) {
        PackBusDefinitionBusV2 row;
        if (!ParsePackBusDefinitionBusV2(payload + header.busOffset +
                static_cast<std::size_t>(i) * header.busRowBytes, &row) ||
            row.busId == VnObjectId{} || !ids.insert(row.busId).second ||
            !std::isfinite(row.faderDb) || row.faderDb < -96.0f || row.faderDb > 24.0f ||
            !std::isfinite(row.makeUpGainDb) ||
            !std::isfinite(row.outputBusVolumeDb) ||
            !std::isfinite(std::pow(10.0f, (row.faderDb + row.makeUpGainDb) / 20.0f)) ||
            !std::isfinite(std::pow(10.0f, row.outputBusVolumeDb / 20.0f)) ||
            row.effectFirst != 0 || row.effectCount != 0 ||
            row.downmixCount != 1 || row.downmixFirst != i) {
            return VN_BAD_PACK;
        }
        buses.push_back(row);
    }
    if (!ValidateCanonicalBusParentForestV2(buses)) return VN_BAD_PACK;
    std::vector<PackBusDefinitionSendV2> sends;
    std::set<std::pair<VnObjectId, VnObjectId>> sendEdges;
    sends.reserve(header.sendCount);
    for (std::uint32_t i = 0; i < header.sendCount; ++i) {
        PackBusDefinitionSendV2 row;
        if (!ParsePackBusDefinitionSendV2(payload + header.sendOffset +
                static_cast<std::size_t>(i) * header.sendRowBytes, &row) ||
            !ids.contains(row.source) || !ids.contains(row.target) ||
            row.source == row.target ||
            (header.publicationCapabilityVintage == 4 &&
             row.target == VN_BUS_MASTER) ||
            (header.publicationCapabilityVintage == 3 ? row.levelDb != 0.0f :
                (!std::isfinite(row.levelDb) || row.levelDb < -96.0f ||
                 row.levelDb > 24.0f ||
                 !std::isfinite(std::pow(10.0f, row.levelDb / 20.0f)))) ||
            !sendEdges.emplace(row.source, row.target).second) {
            return VN_BAD_PACK;
        }
        sends.push_back(row);
    }
    if (header.publicationCapabilityVintage == 4 &&
        !ValidateCanonicalBusSendGraphV2(buses, sends)) return VN_BAD_PACK;
    std::vector<PackBusDefinitionDownmixV2> downmixes;
    downmixes.reserve(header.downmixCount);
    for (std::uint32_t i = 0; i < header.downmixCount; ++i) {
        PackBusDefinitionDownmixV2 row;
        if (!ParsePackBusDefinitionDownmixV2(payload + header.downmixOffset +
                static_cast<std::size_t>(i) * header.downmixRowBytes, &row) ||
            row.busId != buses[i].busId || row.coefficientCount != 64 ||
            row.coefficientFirst != i * 64) {
            return VN_BAD_PACK;
        }
        downmixes.push_back(row);
    }
    std::vector<float> coefficients;
    coefficients.reserve(header.coefficientCount);
    for (std::uint32_t i = 0; i < header.coefficientCount; ++i) {
        const float value = LoadLEF32(payload + header.coefficientOffset +
            static_cast<std::size_t>(i) * header.coefficientRowBytes);
        if (!std::isfinite(value)) return VN_BAD_PACK;
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
        ? VN_OK : VN_BAD_PACK;
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

bool InitSelfHashFields(const PackCanonicalImage& image,
                        std::vector<std::uint64_t>* fields, std::uint64_t* expected) {
    if (!image.hasCatalog || image.packKind != kPackKindInit) return true;
    for (std::size_t i = 0; i < image.catalogRows.size(); ++i) {
        const auto& row = image.catalogRows[i];
        if (row.ownerPackStableId != image.packStableId) continue;
        if (!fields->empty() && row.expectedContentHash != *expected) return false;
        *expected = row.expectedContentHash;
        fields->push_back(image.catalogPayloadOffset + kInitCatalogHeaderBytes +
            static_cast<std::uint64_t>(i) * kInitCatalogRowBytes + kInitCatalogContentHashOffset);
    }
    return true;
}

std::uint64_t HashWithZeroFields(const std::uint8_t* bytes, std::size_t count,
    std::uint64_t offset, const std::vector<std::uint64_t>& fields,
    std::size_t* fieldCursor, std::uint64_t hash) noexcept {
    static constexpr std::uint8_t zeros[8]{};
    std::size_t cursor = 0;
    while (*fieldCursor < fields.size()) {
        const auto field = fields[*fieldCursor];
        if (field + 8 <= offset) { ++*fieldCursor; continue; }
        if (field >= offset + count) break;
        const auto begin = std::max(field, offset + cursor);
        const auto end = std::min(field + 8, offset + count);
        const auto preceding = static_cast<std::size_t>(begin - offset) - cursor;
        hash = Fnv1a64(bytes + cursor, preceding, hash);
        hash = Fnv1a64(zeros, static_cast<std::size_t>(end - begin), hash);
        cursor = static_cast<std::size_t>(end - offset);
        if (end == field + 8) ++*fieldCursor;
    }
    return Fnv1a64(bytes + cursor, count - cursor, hash);
}

bool InitSelfHashMatches(const std::uint8_t* bytes, std::uint64_t fileBytes,
                         const PackCanonicalImage& image) {
    std::vector<std::uint64_t> fields;
    std::uint64_t expected = 0;
    if (!InitSelfHashFields(image, &fields, &expected)) return false;
    if (fields.empty()) return true;
    std::size_t cursor = 0;
    return HashWithZeroFields(bytes, static_cast<std::size_t>(fileBytes), 0,
        fields, &cursor, kFnv1a64OffsetBasis) == expected;
}

VnResult ParseGameplayCatalogPayload(const std::uint8_t* payload,
                                     std::uint64_t size,
                                     GameplayCatalogImage* out) {
    if (size > kGameplayCatalogMaxBytes) {
        return VN_PACK_CATALOG_CAPACITY;
    }
    if (payload == nullptr || out == nullptr ||
        size < kGameplayCatalogHeaderBytes) {
        return VN_BAD_PACK;
    }
    PackGameplayCatalogHeader header;
    LoadPackGameplayCatalogHeader(payload, &header);
    if (header.activeCount > kGameplayCatalogMaxEntries ||
        header.retiredCount > kGameplayCatalogMaxEntries) {
        return VN_PACK_CATALOG_CAPACITY;
    }
    std::uint64_t expectedBytes = 0;
    if (!GameplayCatalogBytes(header.activeCount, header.retiredCount,
                              header.namePoolBytes, &expectedBytes) ||
        expectedBytes > kGameplayCatalogMaxBytes) {
        return VN_PACK_CATALOG_CAPACITY;
    }
    if (expectedBytes != size) {
        return VN_BAD_PACK;
    }
    if (header.schemaVersion != 1 ||
        header.layoutVintage != kPackGameplayCatalogVintage) {
        return VN_PACK_INCOMPATIBLE;
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
        return VN_BAD_PACK;
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
            return VN_BAD_PACK;
        }
        for (std::uint32_t j = 0; j < i; ++j) {
            if (image.active[j].actionStableId == row.actionStableId) {
                return VN_BAD_PACK;
            }
            const int nameCmp = CompareGameplayCatalogNameBytes(
                image.namePool.data() + image.active[j].nameOffset,
                image.active[j].nameLength,
                image.namePool.data() + row.nameOffset, row.nameLength);
            if (nameCmp == 0) {
                return VN_BAD_PACK;
            }
        }
        if (i > 0) {
            const PackGameplayCatalogActiveRow& prev = image.active.back();
            const int nameCmp = CompareGameplayCatalogNameBytes(
                image.namePool.data() + prev.nameOffset, prev.nameLength,
                image.namePool.data() + row.nameOffset, row.nameLength);
            if (nameCmp > 0 ||
                (nameCmp == 0 && prev.actionStableId >= row.actionStableId)) {
                return VN_BAD_PACK;
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
            return VN_BAD_PACK;
        }
        for (const auto& active : image.active) {
            if (CompareGameplayCatalogNameBytes(
                    image.namePool.data() + active.nameOffset, active.nameLength,
                    image.namePool.data() + row.nameOffset, row.nameLength) ==
                0) {
                return VN_BAD_PACK;
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
                return VN_BAD_PACK;
            }
        }
        cursor += row.nameLength;
        image.retired.push_back(row);
    }
    const std::uint32_t padded = (cursor + 7u) & ~7u;
    if (padded != header.namePoolBytes) {
        return VN_BAD_PACK;
    }
    for (std::uint32_t i = cursor; i < header.namePoolBytes; ++i) {
        if (image.namePool[i] != 0) {
            return VN_BAD_PACK;
        }
    }
    *out = std::move(image);
    return VN_OK;
}

} // namespace

VnResult ParseGameplayCatalog(const std::uint8_t* bytes, std::uint64_t size,
                              GameplayCatalogImage* out) {
    return ParseGameplayCatalogPayload(bytes, size, out);
}

VnResult ValidateCanonicalInitDependency(
    std::span<const PackDependencyEdge> edges,
    std::uint64_t expectedInitPackStableId) noexcept {
    if (expectedInitPackStableId == 0) {
        return VN_BAD_PACK;
    }
    bool found = false;
    for (const auto& edge : edges) {
        if (edge.edgeKind != kPackDependencyEdgeKindInit) {
            continue;
        }
        if (found || edge.required01 != 1 ||
            edge.prerequisitePackStableId != expectedInitPackStableId ||
            edge.expectedPrerequisiteContentRevision == 0 || edge.reserved0 != 0) {
            return VN_BAD_PACK;
        }
        found = true;
    }
    return found ? VN_OK : VN_BAD_PACK;
}

namespace {

struct SpanCanonicalSource {
    struct ArtifactHashState {
        const std::uint8_t* bytes;
        std::size_t byteCount;
        std::uint64_t* output;
        std::size_t cursor = 0;
        std::uint64_t hash = kFnv1a64OffsetBasis;

        ~ArtifactHashState() noexcept {
            if (output != nullptr && bytes != nullptr) {
                // Finish before the acquisition owner releases its byte span,
                // including when parser allocation unwinds with an exception.
                *output = Fnv1a64(bytes + cursor, byteCount - cursor, hash);
            }
        }
    };
    ArtifactHashState artifact;
    std::uint32_t pairedMediaEntry = UINT32_MAX;
    std::uint64_t pairedMediaChecksum = 0;
    std::uint64_t ByteCount() const noexcept { return artifact.byteCount; }
    const std::uint8_t* Data(std::uint64_t offset, std::uint64_t size) const noexcept {
        if (artifact.bytes == nullptr || offset > artifact.byteCount ||
            size > artifact.byteCount - offset) return nullptr;
        return artifact.bytes + offset;
    }
    bool VerifyMedia(const PackTocEntry& media, std::uint32_t i,
                     const std::uint8_t* dir, const PackV2Header& header,
                     std::uint64_t dirEnd) {
        const auto* bytes = artifact.bytes;
        const auto fileBytes = ByteCount();
        std::uint64_t checksum = 0;
        if (artifact.output != nullptr && artifact.cursor < artifact.byteCount) {
            const auto remaining = artifact.byteCount - artifact.cursor;
            const auto wholeBytes = static_cast<std::size_t>(
                std::min<std::uint64_t>(media.blobBytes, remaining));
            // Both chains retain their own byte order. Directory order
            // need not match the physical order of the media blobs.
            const auto hashes = Fnv1a64Pair(
                bytes + artifact.cursor, wholeBytes,
                bytes + media.blobOffset,
                static_cast<std::size_t>(media.blobBytes), artifact.hash);
            artifact.hash = hashes[0];
            artifact.cursor += wholeBytes;
            checksum = hashes[1];
        } else if (pairedMediaEntry == i) {
            checksum = pairedMediaChecksum;
            pairedMediaEntry = UINT32_MAX;
        } else {
            bool paired = false;
            if (header.entryCount - i > 1) {
                const auto next = ParsePackV2DirectoryEntry(
                    dir + static_cast<std::size_t>(i + 1) *
                              kPackV2DirectoryEntryBytes);
                // Only bounded metadata is inspected ahead. Its original
                // vintage, payload and blob checks still run in order.
                if (next.kind == kPackEntryKindMedia &&
                    next.size == kPackV2MediaPayloadBytes &&
                    next.offset >= dirEnd && next.offset <= fileBytes &&
                    next.size <= fileBytes - next.offset &&
                    next.offset % kPackV2PayloadAlign == 0) {
                    const auto nextMedia = ParsePackTocEntry(bytes + next.offset);
                    if (nextMedia.soundId == next.id &&
                        ValidateV2MediaEntry(nextMedia, fileBytes, dirEnd)) {
                        const auto hashes = Fnv1a64Pair(
                            bytes + media.blobOffset,
                            static_cast<std::size_t>(media.blobBytes),
                            bytes + nextMedia.blobOffset,
                            static_cast<std::size_t>(nextMedia.blobBytes));
                        checksum = hashes[0];
                        pairedMediaChecksum = hashes[1];
                        pairedMediaEntry = i + 1;
                        paired = true;
                    }
                }
            }
            if (!paired)
                checksum = Fnv1a64(bytes + media.blobOffset,
                                  static_cast<std::size_t>(media.blobBytes));
        }
        return checksum == media.blobChecksum;
    }
    VnResult Finish(const PackCanonicalImage& image,
        std::vector<std::pair<std::uint64_t, std::uint64_t>>&) const {
        return InitSelfHashMatches(artifact.bytes, ByteCount(), image)
            ? VN_OK : VN_PACK_HASH_MISMATCH;
    }
};

#if defined(_MSVC_STL_VERSION)
constexpr std::size_t kCanonicalFileBufferBytes = 16 * 1024;
// Keep complete buffer fills so invalidation does not discard read-ahead tails.
constexpr std::size_t kCanonicalReadWindowBytes =
    (63 * 1024 - kCanonicalFileBufferBytes) / kCanonicalFileBufferBytes *
    kCanonicalFileBufferBytes;
#else
constexpr std::size_t kCanonicalReadWindowBytes = 63 * 1024;
#endif

struct FileCanonicalSource {
    std::ifstream& file;
    std::uint64_t fileBytes;
    PCMRegistry& registry;
    std::span<const PCMRegistry::PreparedPackSamples* const> equalCandidates;
    std::vector<PCMRegistry::PreparedPackSamples>* output;
    const PackCanonicalReadHooksForTests* hooks;
    std::uint64_t hash = kFnv1a64OffsetBasis;
    bool retryWithBytes = false;
    std::map<std::pair<std::uint64_t, std::uint64_t>, std::vector<std::uint8_t>> metadata;

    std::uint64_t ByteCount() const noexcept { return fileBytes; }
    const std::uint8_t* Data(std::uint64_t offset, std::uint64_t size) {
        if (offset > fileBytes || size > fileBytes - offset || size > SIZE_MAX ||
            size > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max()))
            return nullptr;
        const auto key = std::make_pair(offset, size);
        const auto found = metadata.find(key);
        if (found != metadata.end()) return found->second.data();
        auto& bytes = metadata[key];
        bytes.resize(static_cast<std::size_t>(size == 0 ? 1 : size));
        file.clear();
        file.seekg(static_cast<std::streamoff>(offset));
        if (!file) return nullptr;
        file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
        return file.gcount() == static_cast<std::streamsize>(size) ? bytes.data() : nullptr;
    }
    bool VerifyMedia(const PackTocEntry&, std::uint32_t, const std::uint8_t*,
                     const PackV2Header&, std::uint64_t) const noexcept {
        // The common parser must finish all metadata checks before physical
        // bytes are consumed into unpublished sample owners in Finish.
        return true;
    }
    VnResult Finish(const PackCanonicalImage& image,
                   std::vector<std::pair<std::uint64_t, std::uint64_t>>& ranges) {
        std::sort(ranges.begin(), ranges.end());
        std::uint64_t previousEnd = 0;
        for (const auto& range : ranges) {
            if (range.second == 0) continue;
            if (range.first < previousEnd) {
                // Preserve the byte-span parser's accepted layout domain.
                retryWithBytes = true;
                return VN_BACKPRESSURE;
            }
            previousEnd = range.first + range.second;
        }
        std::vector<std::uint64_t> zeroFields;
        std::uint64_t expectedSelfHash = 0;
        if (!InitSelfHashFields(image, &zeroFields, &expectedSelfHash))
            return VN_PACK_HASH_MISMATCH;
        std::vector<PCMRegistry::PackSampleBuilder> builders(image.media.size());
        std::vector<PCMRegistry::PreparedPackSamples> completed(image.media.size());
        std::vector<std::size_t> physicalOrder;
        physicalOrder.reserve(image.media.size());
        std::vector<std::uint64_t> checksums(image.media.size(), kFnv1a64OffsetBasis);
        std::vector<VnResult> sampleStatus(image.media.size(), VN_OK);
        for (std::size_t i = 0; i < image.media.size(); ++i) {
            physicalOrder.push_back(i);
        }
        std::sort(physicalOrder.begin(), physicalOrder.end(), [&](auto a, auto b) {
            return image.media[a].blobOffset < image.media[b].blobOffset;
        });
        // Physical input, file buffering and the builder's 1 KiB prefix
        // workspace share the same 64 KiB budget, independent of media size.
        std::array<float, kCanonicalReadWindowBytes / sizeof(float)> window;
        auto cached = metadata.begin();
        std::size_t currentMedia = 0, zeroCursor = 0;
        auto selfHash = kFnv1a64OffsetBasis;
        file.clear();
        file.seekg(0);
        if (!file) return VN_BAD_PACK;
        for (std::uint64_t offset = 0; offset < fileBytes;) {
            const auto count = static_cast<std::size_t>(
                std::min<std::uint64_t>(sizeof(window), fileBytes - offset));
            if (hooks != nullptr && hooks->beforeSequentialRead != nullptr)
                hooks->beforeSequentialRead(hooks->userData, offset);
#if defined(_MSVC_STL_VERSION)
            // An absolute seek can reuse the CRT input buffer. SEEK_END
            // discards it before returning to the requested position, so
            // metadata comparisons never use bytes from an earlier read.
            file.clear();
            file.seekg(0, std::ios::end);
            file.seekg(static_cast<std::streamoff>(offset));
            if (!file) return VN_BAD_PACK;
#endif
            file.read(reinterpret_cast<char*>(window.data()), static_cast<std::streamsize>(count));
            if (file.gcount() != static_cast<std::streamsize>(count)) return VN_BAD_PACK;
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(window.data());
            const auto end = offset + count;
            while (cached != metadata.end()) {
                const auto start = cached->first.first;
                const auto cachedEnd = start + cached->first.second;
                if (cachedEnd <= offset) { ++cached; continue; }
                if (start >= end) break;
                const auto first = std::max(start, offset);
                const auto last = std::min(cachedEnd, end);
                if (std::memcmp(bytes + (first - offset), cached->second.data() + (first - start),
                                static_cast<std::size_t>(last - first)) != 0)
                    return VN_BAD_PACK;
                if (cachedEnd > end) break;
                ++cached;
            }
            if (!zeroFields.empty())
                selfHash = HashWithZeroFields(bytes, count, offset, zeroFields, &zeroCursor, selfHash);
            std::size_t cursor = 0;
            while (cursor < count) {
                const auto position = offset + cursor;
                while (currentMedia < physicalOrder.size()) {
                    const auto& media = image.media[physicalOrder[currentMedia]];
                    if (media.blobOffset + media.blobBytes > position) break;
                    ++currentMedia;
                }
                if (currentMedia == physicalOrder.size()) {
                    hash = Fnv1a64(bytes + cursor, count - cursor, hash);
                    break;
                }
                const auto index = physicalOrder[currentMedia];
                const auto& media = image.media[index];
                if (position < media.blobOffset) {
                    const auto step = static_cast<std::size_t>(std::min<std::uint64_t>(
                        count - cursor, media.blobOffset - position));
                    hash = Fnv1a64(bytes + cursor, step, hash);
                    cursor += step;
                    continue;
                }
                const auto step = static_cast<std::size_t>(std::min<std::uint64_t>(
                    count - cursor, media.blobOffset + media.blobBytes - position));
                if (cursor % alignof(float) != 0 || step % sizeof(float) != 0) return VN_BAD_PACK;
                if (position == media.blobOffset) {
                    VnPCMDesc shape{};
                    shape.structSize = sizeof(shape);
                    shape.sampleRate = media.sampleRate;
                    shape.channels = media.channelCount;
                    shape.frames = media.frameCount;
                    shape.layout = VN_PCM_LAYOUT_PLANAR;
                    shape.policy = VN_PCM_POLICY_COPY;
                    const PCMRegistry::PreparedPackSamples* candidate = nullptr;
                    for (std::size_t priorOrder = 0; priorOrder < currentMedia; ++priorOrder) {
                        const auto prior = physicalOrder[priorOrder];
                        if (image.media[prior].soundId != media.soundId) continue;
                        if (!completed[prior].IsPrepared() && sampleStatus[prior] == VN_OK)
                            sampleStatus[prior] = builders[prior].Complete(&completed[prior]);
                        if (completed[prior].IsPrepared()) { candidate = &completed[prior]; break; }
                    }
                    if (candidate == nullptr) {
                        for (const auto* prior : equalCandidates) {
                            if (prior != nullptr && prior->IsPrepared() && prior->SoundId() == media.soundId) {
                                candidate = prior;
                                break;
                            }
                        }
                    }
                    const auto prepared = registry.BeginPackSamples(media.soundId, &shape,
                        &builders[index], CanonicalPcmLoop(media), candidate);
                    if (prepared != VN_OK) {
                        retryWithBytes = prepared == VN_BACKPRESSURE;
                        return prepared;
                    }
                }
                const auto hashes = Fnv1a64Pair(bytes + cursor, step, bytes + cursor, step,
                                                hash, checksums[index]);
                hash = hashes[0];
                checksums[index] = hashes[1];
                if (sampleStatus[index] == VN_OK) {
                    const auto appended = builders[index].Append(window.data() + cursor / sizeof(float),
                                                                  step / sizeof(float));
                    if (appended != VN_OK && appended != VN_INVALID_ARG) {
                        retryWithBytes = builders[index].ReservationUnavailable();
                        return appended;
                    }
                    sampleStatus[index] = appended;
                }
                cursor += step;
            }
            offset = end;
        }
        if (file.get() != std::char_traits<char>::eof() || file.bad()) return VN_BAD_PACK;
        for (std::size_t i = 0; i < image.media.size(); ++i) {
            if (checksums[i] != image.media[i].blobChecksum) return VN_BAD_PACK;
        }
        if (!zeroFields.empty() && selfHash != expectedSelfHash) return VN_PACK_HASH_MISMATCH;
        // Retain the existing independent revision chains across two final
        // owners. This never keeps another complete PCM representation.
        for (std::size_t order = 0; order < physicalOrder.size(); ++order) {
            const auto index = physicalOrder[order];
            if (sampleStatus[index] != VN_OK || completed[index].IsPrepared()) continue;
            if (order + 1 < physicalOrder.size()) {
                const auto following = physicalOrder[order + 1];
                VnResult results[2]{};
                if (builders[index].TryCompletePair(builders[following],
                        &completed[index], &completed[following], results)) {
                    sampleStatus[index] = results[0];
                    sampleStatus[following] = results[1];
                    ++order;
                    continue;
                }
            }
            sampleStatus[index] = builders[index].Complete(&completed[index]);
        }
        for (std::size_t i = 0; i < image.media.size(); ++i) {
            if (sampleStatus[i] != VN_OK) return sampleStatus[i];
        }
        *output = std::move(completed);
        return VN_OK;
    }
};

template <class Source>
VnResult ParsePackCanonicalCore(Source& source, PackCanonicalImage* out) {

    const auto fileBytes = source.ByteCount();
    const auto* bytes = source.Data(0, kPackV2HeaderBytes);
    if (bytes == nullptr || out == nullptr || fileBytes < kPackV2HeaderBytes) {
        return VN_BAD_PACK;
    }
    PackV2Header header;
    if (!ParsePackV2Header(bytes, &header)) {
        return VN_BAD_PACK;
    }
    if (!SupportedPackFormatVersion(header.formatVersion) ||
        header.headerBytes != kPackV2HeaderBytes ||
        header.fileBytes != fileBytes) {
        return VN_BAD_PACK;
    }
    if (!PackV2IsCanonicalKind(header.packKind)) {
        return VN_BAD_PACK;
    }
    const std::uint64_t dirBytes =
        static_cast<std::uint64_t>(header.entryCount) *
        kPackV2DirectoryEntryBytes;
    if (header.directoryOffset != header.headerBytes ||
        header.directoryOffset > fileBytes ||
        dirBytes > fileBytes - header.directoryOffset) {
        return VN_BAD_PACK;
    }
    const std::uint8_t* dir = source.Data(header.directoryOffset, dirBytes);
    if (dir == nullptr) return VN_BAD_PACK;
    if (Fnv1a64(dir, static_cast<std::size_t>(dirBytes)) !=
        header.directoryChecksum) {
        return VN_BAD_PACK;
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
                return VN_PACK_INCOMPATIBLE;
            }
            canonicalVintage = entry.vintage;
        } else if (entry.kind == kPackEntryKindBusDefinitionSnapshot) {
            if (entry.vintage != kPackBusDefinitionSnapshotVintage &&
                entry.vintage != kPackBusDefinitionSnapshotVintage2) {
                return VN_PACK_INCOMPATIBLE;
            }
        } else if (PackV2IsCanonicalEntryKind(entry.kind)) {
            if (entry.vintage != kPackCanonicalEntryVintage) {
                return VN_PACK_INCOMPATIBLE;
            }
        } else if (!VintageLegalForKind(entry.kind, entry.vintage)) {
            return VN_PACK_INCOMPATIBLE;
        }
        if (entry.offset < dirEnd || entry.offset > fileBytes ||
            entry.size > fileBytes - entry.offset ||
            entry.offset % kPackV2PayloadAlign != 0) {
            return VN_BAD_PACK;
        }
        ranges.emplace_back(entry.offset, entry.size);
        const std::uint8_t* payload = source.Data(entry.offset, entry.size);
        if (payload == nullptr) return VN_BAD_PACK;
        const bool catalogKind = entry.kind == kPackEntryKindInitRoutingCatalog;
        if (!catalogKind &&
            Fnv1a64(payload, static_cast<std::size_t>(entry.size)) !=
                entry.payloadChecksum) {
            return VN_BAD_PACK;
        }
        switch (entry.kind) {
        case kPackEntryKindInitRoutingCatalog: {
            if (image.hasCatalog) {
                return VN_BAD_PACK;
            }
            PackInitCatalogHeader catalogHeader;
            if (!ParsePackInitCatalogHeader(payload, entry.size, &catalogHeader) ||
                catalogHeader.catalogEntryCount > kInitCatalogMaxEntries ||
                catalogHeader.startupPackCount > kInitCatalogMaxStartupPacks) {
                return VN_BAD_PACK;
            }
            std::uint64_t expectedBytes = 0;
            if (!InitCatalogBytes(catalogHeader.catalogEntryCount,
                                  catalogHeader.startupPackCount,
                                  &expectedBytes) ||
                expectedBytes != catalogHeader.catalogBytes ||
                entry.size != catalogHeader.catalogBytes) {
                return VN_BAD_PACK;
            }
            image.catalogRows.reserve(catalogHeader.catalogEntryCount);
            for (std::uint32_t r = 0; r < catalogHeader.catalogEntryCount; ++r) {
                PackInitCatalogRow row;
                if (!ParsePackInitCatalogRow(
                        payload + kInitCatalogHeaderBytes +
                            static_cast<std::size_t>(r) * kInitCatalogRowBytes,
                        &row)) {
                    return VN_BAD_PACK;
                }
                if (r > 0 &&
                    row.stableId <= image.catalogRows.back().stableId) {
                    return VN_BAD_PACK;
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
                    return VN_BAD_PACK;
                }
                for (std::uint64_t seen : image.startupPackIds) {
                    if (seen == packId) {
                        return VN_BAD_PACK;
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
                    return VN_BAD_PACK;
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
                return VN_BAD_PACK;
            image.hasVariableNameCatalog = true;
            break;
        }
        case kPackEntryKindGameplayCatalog: {
            if (header.packKind != kPackKindInit || image.hasGameplayCatalog) {
                return VN_BAD_PACK;
            }
            const VnResult parsed = ParseGameplayCatalogPayload(
                payload, entry.size, &image.gameplayCatalog);
            if (parsed != VN_OK) {
                return parsed;
            }
            image.hasGameplayCatalog = true;
            break;
        }
        case kPackEntryKindCanonicalDefinitionPartition: {
            if (image.hasPartition) {
                return VN_BAD_PACK;
            }
            PackCanonicalPartitionHeader partitionHeader;
            const bool versionTwo = entry.vintage == kPackCanonicalRequirementVintage;
            if (entry.size < (versionTwo ? kCanonicalDefinitionPartitionHeaderV2Bytes
                                        : kCanonicalDefinitionPartitionHeaderBytes)) {
                return VN_PACK_INCOMPATIBLE;
            }
            const bool parsed = versionTwo
                ? ParsePackCanonicalPartitionHeaderV2(payload, entry.size, &partitionHeader)
                : ParsePackCanonicalPartitionHeader(payload, entry.size, &partitionHeader);
            if (!parsed) {
                return VN_BAD_PACK;
            }
            if (!IsReadableCurrentWorldBodyVintage(partitionHeader.bodyVintage)) {
                return VN_PACK_INCOMPATIBLE;
            }
            CurrentWorldBodyImage body;
            const CurrentWorldBodyDecodeResult decoded = body.DecodeWithResult(
                payload + partitionHeader.headerBytes,
                static_cast<std::size_t>(partitionHeader.bodyBytes),
                partitionHeader.bodyVintage,
                CurrentWorldDecodeMode::StrictArtifact);
            if (decoded != CurrentWorldBodyDecodeResult::Ok) {
                return decoded == CurrentWorldBodyDecodeResult::Capacity
                           ? VN_PACK_RESIDENT_CAPACITY
                           : VN_BAD_PACK;
            }
            const WorldInstallStatus validation =
                detail::ValidateCurrentWorldPartitionLocalV1(*body.ProductionView());
            if (validation == WorldInstallStatus::InvalidFormat ||
                validation == WorldInstallStatus::CapabilityUnavailable) {
                return VN_PACK_INCOMPATIBLE;
            }
            if (validation == WorldInstallStatus::Capacity) {
                return VN_PACK_RESIDENT_CAPACITY;
            }
            if (validation != WorldInstallStatus::Ok) {
                return VN_BAD_PACK;
            }
            const ContainerWorldInstallViewV1 decodedView = *body.ProductionView();
            const std::uint64_t decodedOwnedDefinitions =
                static_cast<std::uint64_t>(decodedView.variableCount) +
                decodedView.playablePropertyCount + decodedView.actionCount;
            if (partitionHeader.ownedDefinitionCount !=
                decodedOwnedDefinitions) {
                return VN_BAD_PACK;
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
                return VN_BAD_PACK;
            }
            PackRequirementBlock requirement;
            const bool versionTwo = entry.vintage == kPackCanonicalRequirementVintage;
            if (entry.size != (versionTwo ? kPackRequirementBlockV2Bytes : kPackRequirementBlockBytes)) {
                return VN_PACK_INCOMPATIBLE;
            }
            if (LoadLE32(payload) != entry.vintage || LoadLE32(payload + 4) != entry.vintage) {
                return VN_PACK_INCOMPATIBLE;
            }
            const bool parsed = versionTwo
                ? ParsePackRequirementBlockV2(payload, entry.size, &requirement)
                : ParsePackRequirementBlock(payload, entry.size, &requirement);
            if (!parsed ||
                requirement.payloadChecksumCoverage != Fnv1a64(payload, 104)) {
                return VN_BAD_PACK;
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
                return VN_PACK_INCOMPATIBLE;
            }
            const bool parsed = versionTwo
                ? ParsePackDependencyEdgeV2(payload, entry.size, &edge)
                : ParsePackDependencyEdge(payload, entry.size, &edge);
            if (!parsed ||
                entry.relationOrdinal != dependencyOrdinal) {
                return VN_BAD_PACK;
            }
            const bool initRequired =
                edge.edgeKind == kPackDependencyEdgeKindInit &&
                edge.required01 == 1;
            if (!initRequired &&
                (edge.expectedBusContentGeneration != 0 ||
                 edge.expectedPublishedBusFingerprint != 0 ||
                 edge.expectedCompileTargetProfileFingerprint != 0)) {
                return VN_BAD_PACK;
            }
            ++dependencyOrdinal;
            image.dependencyEdges.push_back(edge);
            break;
        }
        case kPackEntryKindBusDefinitionSnapshot: {
            if (image.hasBusDefinitionSnapshot) {
                return VN_BAD_PACK;
            }
            if (entry.id != VnObjectId{} || entry.relationOrdinal != 1) {
                return VN_BAD_PACK;
            }
            const VnResult parsed =
                entry.vintage == kPackBusDefinitionSnapshotVintage2
                    ? ParseBusDefinitionSnapshotPayloadV2(payload, entry.size, &image)
                    : ParseBusDefinitionSnapshotPayload(payload, entry.size, &image);
            if (parsed != VN_OK) {
                return parsed;
            }
            break;
        }
        case kPackEntryKindVariablePolicyOverlay: {
            if (image.hasVariablePolicyOverlay) {
                return VN_BAD_PACK;
            }
            const VnResult parsed = ParseVariablePolicyOverlayPayload(
                payload, entry.size, &image.variablePolicyOverlays);
            if (parsed != VN_OK) {
                return parsed;
            }
            image.hasVariablePolicyOverlay = true;
            break;
        }
        case kPackEntryKindVariableRequirements: {
            if (image.hasVariableRequirements) {
                return VN_BAD_PACK;
            }
            const VnResult parsed = ParseVariableRequirementsPayload(
                payload, entry.size, &image.variableRequirements);
            if (parsed != VN_OK) {
                return parsed;
            }
            image.hasVariableRequirements = true;
            break;
        }
        case kPackEntryKindMedia: {
            if (entry.size != kPackV2MediaPayloadBytes) {
                return VN_BAD_PACK;
            }
            const PackTocEntry media = ParsePackTocEntry(payload);
            if (media.soundId != entry.id ||
                !ValidateV2MediaEntry(media, fileBytes, dirEnd)) {
                return VN_BAD_PACK;
            }
            if (!source.VerifyMedia(media, i, dir, header, dirEnd)) return VN_BAD_PACK;
            ranges.emplace_back(media.blobOffset, media.blobBytes);
            image.media.push_back(media);
            break;
        }
        case kPackEntryKindBusEffect: {
            PackBusEffectEntry effect;
            if (!ParsePackV2BusEffect(payload, entry.size, &effect) ||
                entry.relationOrdinal != image.busEffects.size() || effect.busId == VnObjectId{} ||
                effect.bypass > 1 || effect.slot >= kMaxFxSlotsPerBus ||
                effect.faderPointGuard > kMaxFxSlotsPerBus) {
                return VN_BAD_PACK;
            }
            const PackEffectSchemaRow* schema = FindPackEffectSchemaByType(effect.effectType);
            if (schema == nullptr || effect.descriptorVintage != schema->descriptorVintage ||
                effect.paramCount != schema->paramCount) {
                return VN_BAD_PACK;
            }
            for (std::uint32_t p = 0; p < effect.paramCount; ++p) {
                if (!std::isfinite(effect.params[p])) {
                    return VN_BAD_PACK;
                }
            }
            image.busEffects.push_back(effect);
            break;
        }
        case kPackEntryKindEffectDynamicRoute: {
            PackEffectDynamicRouteEntry route;
            if (!ParsePackV2EffectDynamicRoute(payload, entry.size, &route) ||
                entry.relationOrdinal != image.dynamicRoutes.size() || route.targetBusId == VnObjectId{} ||
                route.sourceId == VnObjectId{} || route.expectedEffectType != VN_EFFECT_COMPRESSOR ||
                route.signalForm != VN_DYNAMIC_SIGNAL_RAW_AUDIO ||
                route.sourceKind != VN_DYNAMIC_SOURCE_BUS ||
                route.tap > VN_DYNAMIC_TAP_BUS_POST_FADER ||
                route.effectSlot >= kMaxFxSlotsPerBus || route.routeSlot >= 4u ||
                !std::isfinite(route.levelDb) || route.levelDb < -96.0f || route.levelDb > 24.0f) {
                return VN_BAD_PACK;
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
                return VN_BAD_PACK;
            }
            image.buses.push_back(bus);
            break;
        }
        case kPackEntryKindBusSend: {
            PackBusSendEntry send;
            if (!ParsePackV2BusSend(payload, entry.size, &send) ||
                entry.relationOrdinal != sendOrdinal ||
                !TryVolumeGainFromDb(send.levelDb)) {
                return VN_BAD_PACK;
            }
            ++sendOrdinal;
            image.busSends.push_back(send);
            break;
        }
        case kPackEntryKindAttenuation: {
            PackLogicImage::Attenuation attenuation;
            attenuation.id = entry.id;
            if (!ParsePackAttenuation(payload, entry.size, header.formatVersion,
                                        &attenuation.desc)) {
                return VN_BAD_PACK;
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
                return VN_BAD_PACK;
            }
            // Align with ParsePackV2Image: definitions admit the four
            // released scopes. v2 Init is the mix-level surface, so voice
            // and voice-effect-parameter stay on Content. v1 Init still
            // admits the production Voice/Bus mix.
            if (definition.desc.targetScopeKind >
                VN_TARGET_SCOPE_BUS_EFFECT_PARAM) {
                return VN_BAD_PACK;
            }
            if (header.packKind == kPackKindInit &&
                canonicalVintage == kPackCanonicalRequirementVintage &&
                (definition.desc.targetScopeKind == VN_TARGET_SCOPE_VOICE ||
                 definition.desc.targetScopeKind ==
                     VN_TARGET_SCOPE_VOICE_EFFECT_PARAM)) {
                return VN_BAD_PACK;
            }
            image.bindingDefinitions.push_back(definition);
            break;
        }
        case kPackEntryKindBindingAttachment: {
            PackBindingAttachmentEntry attachment;
            if (!ParsePackV2BindingAttachment(payload, entry.size,
                                              &attachment) ||
                entry.relationOrdinal != attachmentOrdinal) {
                return VN_BAD_PACK;
            }
            ++attachmentOrdinal;
            if (attachment.scopeKind ==
                    VN_TARGET_SCOPE_VOICE_EFFECT_PARAM ||
                attachment.scopeKind >
                    VN_TARGET_SCOPE_BUS_EFFECT_PARAM) {
                return VN_BAD_PACK;
            }
            if (header.packKind == kPackKindInit &&
                canonicalVintage == kPackCanonicalRequirementVintage &&
                attachment.scopeKind != VN_TARGET_SCOPE_BUS &&
                attachment.scopeKind !=
                    VN_TARGET_SCOPE_BUS_EFFECT_PARAM) {
                return VN_BAD_PACK;
            }
            image.bindingAttachments.push_back(attachment);
            break;
        }
        case kPackEntryKindContainer:
        case kPackEntryKindVoiceRecipe:
            return VN_PACK_INCOMPATIBLE;
        default:
            return VN_BAD_PACK;
        }
    }

    if (!image.hasRequirement) {
        return VN_BAD_PACK;
    }
    // v1 layouts keep Voice/Bus/BusEffect. v2 admits the same released
    // definition scopes so production Init can carry mix-level Bus
    // bindings; Voice Effect Parameter still cannot attach. v2 Init
    // keeps voice cargo on Content.
    const std::uint32_t maxBindingScope = VN_TARGET_SCOPE_BUS_EFFECT_PARAM;
    for (const auto& definition : image.bindingDefinitions) {
        if (definition.desc.targetScopeKind > maxBindingScope) {
            return VN_BAD_PACK;
        }
        if (header.packKind == kPackKindInit &&
            canonicalVintage == kPackCanonicalRequirementVintage &&
            (definition.desc.targetScopeKind == VN_TARGET_SCOPE_VOICE ||
             definition.desc.targetScopeKind ==
                 VN_TARGET_SCOPE_VOICE_EFFECT_PARAM)) {
            return VN_BAD_PACK;
        }
    }
    for (const auto& attachment : image.bindingAttachments) {
        if (attachment.scopeKind > maxBindingScope ||
            attachment.scopeKind == VN_TARGET_SCOPE_VOICE_EFFECT_PARAM) {
            return VN_BAD_PACK;
        }
        if (header.packKind == kPackKindInit &&
            canonicalVintage == kPackCanonicalRequirementVintage &&
            attachment.scopeKind != VN_TARGET_SCOPE_BUS &&
            attachment.scopeKind != VN_TARGET_SCOPE_BUS_EFFECT_PARAM) {
            return VN_BAD_PACK;
        }
    }
    if (canonicalVintage == kPackCanonicalEntryVintage && image.hasBusDefinitionSnapshot) {
        return VN_BAD_PACK;
    }
    if (image.hasVariableNameCatalog &&
        (image.variableNameCatalog.initPackStableId != image.packStableId ||
         image.variableNameCatalog.initContentRevision != image.requirement.packContentRevision))
        return VN_BAD_PACK;
    if (image.hasGameplayCatalog &&
        (image.gameplayCatalog.header.initPackStableId != image.packStableId ||
         image.gameplayCatalog.header.initContentRevision !=
             image.requirement.packContentRevision)) {
        return VN_BAD_PACK;
    }
    if (header.packKind == kPackKindCanonicalContent && image.hasCatalog) {
        return VN_BAD_PACK;
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
            return VN_BAD_PACK;
        }
    }
    // Additive 10a: LoadPack still accepts Init without kind-24. Activate
    // requires the snapshot separately. Content still cannot carry one.
    if (header.packKind == kPackKindCanonicalContent &&
        image.hasBusDefinitionSnapshot) {
        return VN_BAD_PACK;
    }
    if (image.hasPartition &&
        (image.requirement.definitionPartitionRevision == 0 ||
         image.partitionHeader.packStableId != image.packStableId ||
         image.partitionHeader.partitionRevision !=
             image.requirement.definitionPartitionRevision)) {
        return VN_BAD_PACK;
    }
    if (!image.hasPartition &&
        image.requirement.definitionPartitionRevision != 0) {
        return VN_BAD_PACK;
    }
    const CanonicalBusTriple requirementT = RequirementBusTriple(image.requirement);
    if (image.hasBusDefinitionSnapshot) {
        const CanonicalBusTriple snapshotT =
            SnapshotBusTriple(image.busDefinitionSnapshotHeader);
        if (!CanonicalBusTripleFullyNonzero(snapshotT) ||
            !CanonicalBusTripleEqual(snapshotT, requirementT)) {
            return VN_BAD_PACK;
        }
    }
    if (image.hasPartition &&
        !CanonicalBusTripleEqual(PartitionBusTriple(image.partitionHeader),
                                 requirementT)) {
        return VN_BAD_PACK;
    }
    for (const PackDependencyEdge& edge : image.dependencyEdges) {
        const bool initRequired =
            edge.edgeKind == kPackDependencyEdgeKindInit && edge.required01 == 1;
        if (initRequired &&
            !CanonicalBusTripleEqual(DependencyBusTriple(edge), requirementT)) {
            return VN_BAD_PACK;
        }
    }
    if (image.hasCatalog) {
        const auto* catalog = source.Data(image.catalogPayloadOffset, image.catalogHeader.catalogBytes);
        if (catalog == nullptr) return VN_BAD_PACK;
        std::vector<std::uint8_t> catalogCopy(catalog, catalog + image.catalogHeader.catalogBytes);
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
            return VN_BAD_PACK;
        }
        if (Fnv1a64(catalogCopy.data(), catalogCopy.size()) !=
            image.catalogDirectoryChecksum) {
            return VN_BAD_PACK;
        }
    }
    const auto finished = source.Finish(image, ranges);
    if (finished != VN_OK) return finished;
    *out = std::move(image);
    return VN_OK;
}

} // namespace

VnResult ParsePackCanonicalImage(const std::uint8_t* bytes, std::uint64_t fileBytes,
                                 PackCanonicalImage* out, std::uint64_t* artifactHash) {
    SpanCanonicalSource source{{bytes, static_cast<std::size_t>(fileBytes), artifactHash}};
    return ParsePackCanonicalCore(source, out);
}

VnResult ReadPackCanonicalFile(const char* utf8Path, PCMRegistry& registry,
    PackCanonicalImage* out, std::vector<PCMRegistry::PreparedPackSamples>* samples,
    std::uint64_t* byteCount, std::uint64_t* artifactHash, std::uint64_t maxBytes,
    std::span<const PCMRegistry::PreparedPackSamples* const> equalCandidates,
    bool* retryWithBytes, const PackCanonicalReadHooksForTests* hooks) {
    if (retryWithBytes != nullptr) *retryWithBytes = false;
    if (out == nullptr || samples == nullptr || byteCount == nullptr || artifactHash == nullptr)
        return VN_INVALID_ARG;
    std::filesystem::path path;
    if (!TryMakeUtf8Path(utf8Path, &path)) return VN_BAD_PACK;
#if defined(_MSVC_STL_VERSION)
    std::array<char, kCanonicalFileBufferBytes> fileBuffer;
#endif
    std::ifstream file;
    // Metadata must be reread by the bounded pass, not served from a stale
    // stream buffer populated while inspecting the directory.
#if !defined(_MSVC_STL_VERSION)
    if (file.rdbuf()->pubsetbuf(nullptr, 0) == nullptr) {
        if (retryWithBytes != nullptr) *retryWithBytes = true;
        return VN_BACKPRESSURE;
    }
#endif
    file.open(path, std::ios::binary);
    if (!file) return VN_BAD_PACK;
#if defined(_MSVC_STL_VERSION)
    // Reuse the existing larger-fill strategy before the first file read.
    // The buffer outlives the stream and is included in the input budget.
    if (file.rdbuf()->pubsetbuf(fileBuffer.data(), fileBuffer.size()) == nullptr) {
        if (retryWithBytes != nullptr) *retryWithBytes = true;
        return VN_BACKPRESSURE;
    }
#endif
    file.seekg(0, std::ios::end);
    const auto end = file.tellg();
    if (end <= 0 || static_cast<std::uint64_t>(end) > SIZE_MAX) return VN_BAD_PACK;
    const auto size = static_cast<std::uint64_t>(end);
    if (size > maxBytes) return VN_PACK_RESIDENT_CAPACITY;
    FileCanonicalSource source{file, size, registry, equalCandidates, samples, hooks};
    const auto result = ParsePackCanonicalCore(source, out);
    if (retryWithBytes != nullptr) *retryWithBytes = source.retryWithBytes;
    if (result == VN_OK) {
        *byteCount = size;
        *artifactHash = source.hash;
    }
    return result;
}

VnResult CheckPackClosure(const PackLogicImage& image,
                          const PackClosureResolver& resolver,
                          PackClosureRef* outRefClass,
                          VnObjectId* outReferrerId,
                          VnObjectId* outMissingId) {
    const auto fail = [&](PackClosureRef refClass, VnObjectId referrer,
                          VnObjectId missing) {
        if (outRefClass != nullptr) {
            *outRefClass = refClass;
        }
        if (outReferrerId != nullptr) {
            *outReferrerId = referrer;
        }
        if (outMissingId != nullptr) {
            *outMissingId = missing;
        }
        return VN_UNKNOWN_ID;
    };

    const auto packPlayable = [&](VnObjectId id) {
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
    const auto playableCloses = [&](VnObjectId id) {
        return packPlayable(id) ||
               (resolver.playableExists != nullptr &&
                resolver.playableExists(resolver.ctx, id));
    };
    const auto busCloses = [&](VnBusId id) {
        for (const PackBusEntry& bus : image.buses) {
            if (bus.busId == id) {
                return true;
            }
        }
        return resolver.busExists != nullptr &&
               resolver.busExists(resolver.ctx, id);
    };
    const auto bindingCloses = [&](VnBindingId id) {
        for (const PackLogicImage::BindingDefinition& def :
             image.bindingDefinitions) {
            if (def.id == id) {
                return true;
            }
        }
        return resolver.bindingExists != nullptr &&
               resolver.bindingExists(resolver.ctx, id);
    };
    // A bus's parent is a logic reference like any other: it must resolve
    // against {this pack + loaded state}, or the apply-side CreateBus —
    // which cannot refuse after staging — would meet an unknown parent.
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
        if (attachment.scopeKind == VN_TARGET_SCOPE_BUS ||
            attachment.scopeKind ==
                VN_TARGET_SCOPE_BUS_EFFECT_PARAM) {
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
        for (VnBindingId bindingId : recipe.bindings) {
            if (!bindingCloses(bindingId)) {
                return fail(PackClosureRef::RecipeBinding,
                            recipe.header.playableId, bindingId);
            }
        }
    }
    return VN_OK;
}

} // namespace harken
