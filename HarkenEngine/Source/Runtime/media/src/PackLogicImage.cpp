#include <harken/core/BindingTransition.h>
#include "harken/media/PackLogicImage.h"

#include "harken/core/CurrentWorldCodec.h"
#include "harken/core/EngineConfig.h"
#include "harken/core/Utf8Path.h"
#include "harken/core/detail/CurrentWorldValidation.h"
#include "harken/dsp/GainRamp.h"
#include "harken/media/generated/harken_pack_effect_schema.g.h"
#include "harken/core/MediaContentRevision.h"
#include "harken/core/generated/harken_playable_property_runtime.g.h"
#include "harken/media/PackMediaRange.h"
#include "harken/media/StreamByteSource.h"

#include <algorithm>
#include <exception>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <filesystem>
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
    std::span<const PackBusDefinitionBus> buses, const detail::PackDecodeBudget& budget) {
    detail::PackDecodeCharge scratch(budget);
    if (buses.empty() || buses.size() > 4096) return false;
    scratch.AddRows<std::pair<const VnObjectId, std::size_t>>(buses.size());
    std::map<VnObjectId, std::size_t> indices;
    for (std::size_t i = 0; i < buses.size(); ++i) {
        if (buses[i].busId == VnObjectId{} ||
            !indices.emplace(buses[i].busId, i).second) return false;
    }
    const auto master = indices.find(VN_BUS_MASTER);
    if (master == indices.end() ||
        buses[master->second].parentId != VnObjectId{}) return false;

    std::vector<std::uint8_t> state;
    detail::DecodeResize(state, buses.size(), scratch);
    std::vector<std::size_t> path;
    detail::DecodeReserve(path, buses.size(), scratch);
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

bool ValidateCanonicalBusParentForestV2(std::span<const PackBusDefinitionBus> buses) {
    return ValidateCanonicalBusParentForestV2(buses, {});
}

bool ValidateCanonicalBusSendGraphV2(
    std::span<const PackBusDefinitionBus> buses,
    std::span<const PackBusDefinitionSend> sends, const detail::PackDecodeBudget& budget) {
    detail::PackDecodeCharge scratch(budget);
    scratch.AddRows<std::pair<const VnObjectId, std::size_t>>(buses.size());
    std::map<VnObjectId, std::size_t> indices;
    for (std::size_t i = 0; i < buses.size(); ++i) {
        indices.emplace(buses[i].busId, i);
    }
    std::vector<std::vector<std::size_t>> edges;
    detail::DecodeResize(edges, buses.size(), scratch);
    std::vector<std::size_t> indegree;
    detail::DecodeResize(indegree, buses.size(), scratch);
    std::vector<std::size_t> edgeSlots;
    detail::DecodeResize(edgeSlots, buses.size(), scratch);
    const auto addEdge = [&](VnObjectId source, VnObjectId target) {
        const auto from = indices.find(source);
        const auto to = indices.find(target);
        if (from == indices.end() || to == indices.end()) return false;
        detail::DecodeAppend(edges[from->second], to->second, scratch, edgeSlots[from->second]);
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
    detail::DecodeReserve(ready, buses.size(), scratch);
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
    if (bytes == nullptr || fileBytes < kPackFileHeaderBytes) {
        return VN_BAD_PACK;
    }
    PackFileHeader header;
    if (!ReadPackFileHeader(bytes, &header)) {
        return VN_BAD_PACK;
    }
    if (!SupportedPackFormatVersion(header.formatVersion) ||
        header.headerBytes != kPackFileHeaderBytes ||
        header.fileBytes != fileBytes) {
        return VN_BAD_PACK;
    }
    if (PackIsCanonicalKind(header.packKind)) {
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
        kPackDirectoryEntryBytes;
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
        const PackDirectoryEntry entry = ReadPackDirectoryEntry(
            dir + static_cast<std::size_t>(i) * kPackDirectoryEntryBytes);
        if (!PackEntryKindSupported(entry.kind)) return VN_BAD_PACK;
        if (PackIsCanonicalEntryKind(entry.kind)) {
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
        if (entry.offset % kPackPayloadAlign != 0) {
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
            if (entry.size != kPackMediaBytes) {
                return VN_BAD_PACK;
            }
            const PackTocEntry media = ReadPackMedia(payload);
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
                ParsePackBus(payload, entry.size, &bus);
            if (!parsed || bus.busId != entry.id ||
                !std::isfinite(bus.volume) ||
                !std::isfinite(bus.outputBusVolume)) {
                return VN_BAD_PACK;
            }
            image.buses.push_back(bus);
            break;
        }
        case kPackEntryKindBusSend: {
            PackBusSendEntry send;
            if (!ParsePackBusSend(payload, entry.size, &send) ||
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
            if (!ReadPackBusEffect(payload, entry.size, &effect) ||
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
            if (!ParsePackEffectDynamicRoute(payload, entry.size,
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
            if (!ReadPackAttenuation(payload, entry.size, &attenuation.desc)) {
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
            if (!ReadPackBindingDefinition(payload, entry.size,
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
            if (!ParsePackBindingAttachment(payload, entry.size,
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
            if (!ReadPackContainerPayload(payload, entry.size, entry.id, container) ||
                container.header.childCount == 0) {
                return VN_BAD_PACK;
            }
            image.containers.push_back(std::move(container));
            break;
        }
        case kPackEntryKindVoiceRecipe: {
            PackLogicImage::VoiceRecipe recipe;
            if (!ReadPackVoiceRecipePayload(payload, entry.size, entry.id, recipe) ||
                !VoiceToneWireValuesValid(recipe.tone)) {
                return VN_BAD_PACK;
            }
            std::uint32_t previousSlot = 0;
            for (std::uint32_t n = 0; n < recipe.header.installCount; ++n) {
                const auto& install = recipe.installs[n];
                if (                    install.slot >= kPackVoiceRecipeMaxInstalls ||
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
            }
            VnBindingId previousBinding{};
            for (std::uint32_t n = 0; n < recipe.header.bindingCount; ++n) {
                const auto binding = recipe.bindings[n];
                if (binding == VnObjectId{} ||
                    (n > 0 && binding <= previousBinding)) {
                    return VN_BAD_PACK;
                }
                previousBinding = binding;
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

VnResult ParseVariablePolicyOverlayPayload(
    const std::uint8_t* payload, std::uint64_t size,
    std::vector<PackCanonicalImage::VariablePolicyOverlay>* out, detail::PackDecodeCharge& staging) {
    if (payload == nullptr || out == nullptr || size < kPackVariablePolicyHeaderBytes) return VN_BAD_PACK;
    using Policy = PackCanonicalImage::VariablePolicyOverlay;
    struct PolicyRecord {
        Policy& target;
        PackVariablePolicyRow wire{};
        auto& Wire() noexcept { return wire; }
        bool Finish() noexcept {
            target.variableId = wire.variableId;
            target.writers = wire.writers;
            target.executionFingerprint = wire.executionFingerprint;
            return target.variableId != VnObjectId{} && target.executionFingerprint != 0 && target.writers >= 1 && target.writers <= 3;
        }
    };
    struct AnchorVisitor {
        Policy& target;
        PackVariableReadState& state;
        detail::PackDecodeCharge& staging;
        std::uint64_t maximum;
        PackVariableAnchorHeader header{};
        auto& Value() noexcept { return header; }
        auto& Header() noexcept { return header; }
        bool PrepareAnchors(std::uint64_t count) noexcept {
            return count <= maximum && state.Attempt([&] { detail::DecodeResize(target.discreteAnchors, static_cast<std::size_t>(count), staging); return true; });
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
            return target.labelId != VnObjectId{} && target.anchor >= 0 && target.anchor <= 16777216;
        }
    };
    struct LabelVisitor {
        Policy& target;
        PackVariableReadState& state;
        detail::PackDecodeCharge& staging;
        std::uint64_t maximum;
        PackVariableLabelHeader header{};
        auto& Value() noexcept { return header; }
        auto& Header() noexcept { return header; }
        bool PrepareLabels(std::uint64_t count) noexcept {
            return header.labelMappingFingerprint != 0 && count <= maximum && state.Attempt([&] {
                target.labelMappingFingerprint = header.labelMappingFingerprint;
                detail::DecodeResize(target.labels, static_cast<std::size_t>(count), staging);
                return true;
            });
        }
        LabelRecord Labels(std::uint64_t index) noexcept { return {target.labels[static_cast<std::size_t>(index)]}; }
        bool Finish() noexcept {
            return state.Attempt([&] {
                detail::PackDecodeCharge scratch(staging.Budget());
                scratch.AddRows<VnObjectId>(target.labels.size());
                std::set<VnObjectId> identities;
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
        detail::PackDecodeCharge& staging;
        std::uint64_t bytes;
        PackVariablePolicyHeader header{};
        auto& Value() noexcept { return header; }
        auto& Header() noexcept { return header; }
        bool PrepareRows(std::uint64_t count) noexcept {
            return count <= (bytes - kPackVariablePolicyHeaderBytes) / kPackVariablePolicyRowBytes && state.Attempt([&] {
                detail::DecodeResize(rows, static_cast<std::size_t>(count), staging); return true;
            });
        }
        PolicyRecord Rows(std::uint64_t index) noexcept { return {rows[static_cast<std::size_t>(index)]}; }
        bool PrepareAnchorGroups(std::uint64_t count) noexcept {
            if (count != rows.size()) return false;
            for (std::size_t i = 1; i < rows.size(); ++i) if (rows[i].variableId <= rows[i - 1].variableId) return false;
            return true;
        }
        AnchorVisitor AnchorGroups(std::uint64_t index) noexcept { return {rows[static_cast<std::size_t>(index)], state, staging, bytes / kPackVariableAnchorBytes}; }
        bool PrepareLabelGroups(std::uint64_t count) noexcept { return count == rows.size(); }
        LabelVisitor LabelGroups(std::uint64_t index) noexcept { return {rows[static_cast<std::size_t>(index)], state, staging, bytes / kPackVariableLabelBytes}; }
        bool Finish() noexcept { return true; }
    };
    std::vector<Policy> rows;
    PackVariableReadState state;
    Visitor visitor{rows, state, staging, size};
    PackWireBuffer<const std::uint8_t> buffer{payload, size};
    const bool parsed = ReadPackVariablePolicyPayload(buffer, visitor);
    if (state.exception) std::rethrow_exception(state.exception);
    if (!parsed || buffer.offset != size) return VN_BAD_PACK;
    *out = std::move(rows);
    return VN_OK;
}

VnResult ParseVariableRequirementsPayload(
    const std::uint8_t* payload, std::uint64_t size,
    std::vector<PackCanonicalImage::VariableRequirement>* out, detail::PackDecodeCharge& staging) {
    if (payload == nullptr || out == nullptr || size < kPackVariableRequirementsHeaderBytes) return VN_BAD_PACK;
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
            return target.stableId != VnObjectId{} && target.expectedExecutionFingerprint != 0 && target.expectedLabelMappingFingerprint != 0;
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
    if (!ParsePackVariableRequirementsHeader(payload, kPackVariableRequirementsHeaderBytes, &header)) return VN_BAD_PACK;
    std::uint64_t expected = 0;
    std::vector<Requirement> rows;
    Visitor visitor{rows, header};
    if (!MeasurePackVariableRequirementsPayload(visitor, expected) || expected != size) return VN_BAD_PACK;
    detail::DecodeResize(rows, header.rowCount, staging);
    PackWireBuffer<const std::uint8_t> buffer{payload, size};
    if (!ReadPackVariableRequirementsPayload(buffer, visitor) || buffer.offset != size) return VN_BAD_PACK;
    *out = std::move(rows);
    return VN_OK;
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

VnResult ParseBusDefinitionSnapshotPayload(
    const std::uint8_t* payload, std::uint64_t size, PackCanonicalImage* image) {
    auto& staging = image->staging;
    detail::PackDecodeCharge scratch(staging.Budget());
    PackBusDefinitionSnapshotHeader header;
    if (!ReadPackBusDefinitionSnapshotHeader(payload, size, &header) ||
        header.busCount == 0 || header.busCount > 4096 ||
        header.effectCount != 0 || header.effectParameterCount != 0 ||
        header.downmixCount != header.busCount ||
        header.coefficientCount != static_cast<std::uint64_t>(header.busCount) * 64) {
        return VN_BAD_PACK;
    }
    if (!ValidatePackBusDefinitionSnapshotLayout(header, size)) return VN_BAD_PACK;
    std::vector<PackBusDefinitionBus> buses;
    std::vector<PackBusDefinitionSend> sends;
    std::vector<PackBusDefinitionDownmix> downmixes;
    std::vector<float> coefficients;
    detail::DecodeResize(buses, header.busCount, staging);
    detail::DecodeResize(sends, header.sendCount, staging);
    detail::DecodeResize(downmixes, header.downmixCount, staging);
    detail::DecodeResize(coefficients, static_cast<std::size_t>(header.coefficientCount), staging);
    PackWireBuffer<const std::uint8_t> input{payload, size};
    PackBusDefinitionSnapshotReader visitor{header, buses, sends, downmixes, coefficients};
    if (!ReadPackBusDefinitionSnapshot(input, visitor) || input.offset != size) return VN_BAD_PACK;
    scratch.AddRows<VnObjectId>(header.busCount);
    std::set<VnObjectId> ids;
    for (std::uint32_t i = 0; i < header.busCount; ++i) {
        const auto& row = buses[i];
        if (row.busId == VnObjectId{} || !ids.insert(row.busId).second ||
            !std::isfinite(row.faderDb) || row.faderDb < -96.0f || row.faderDb > 24.0f ||
            !std::isfinite(row.outputBusVolumeDb) ||
            !std::isfinite(std::pow(10.0f, row.outputBusVolumeDb / 20.0f)) ||
            row.effectFirst != 0 || row.effectCount != 0 ||
            row.downmixCount != 1 || row.downmixFirst != i) {
            return VN_BAD_PACK;
        }
    }
    if (!ValidateCanonicalBusParentForestV2(buses, staging.Budget())) return VN_BAD_PACK;
    scratch.AddRows<std::pair<VnObjectId, VnObjectId>>(header.sendCount);
    std::set<std::pair<VnObjectId, VnObjectId>> sendEdges;
    for (std::uint32_t i = 0; i < header.sendCount; ++i) {
        const auto& row = sends[i];
        if (!ids.contains(row.source) || !ids.contains(row.target) ||
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
    }
    if (header.publicationCapabilityVintage == 4 &&
        !ValidateCanonicalBusSendGraphV2(buses, sends, staging.Budget())) return VN_BAD_PACK;
    for (std::uint32_t i = 0; i < header.downmixCount; ++i) {
        const auto& row = downmixes[i];
        if (row.busId != buses[i].busId || row.coefficientCount != 64 ||
            row.coefficientFirst != i * 64) {
            return VN_BAD_PACK;
        }
    }
    for (const auto value : coefficients) {
        if (!std::isfinite(value)) return VN_BAD_PACK;
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
        return PackIsCanonicalKind(packKind);
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
                     const std::uint8_t* dir, const PackFileHeader& header,
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
                const auto next = ReadPackDirectoryEntry(
                    dir + static_cast<std::size_t>(i + 1) *
                              kPackDirectoryEntryBytes);
                // Only bounded metadata is inspected ahead. Its original
                // vintage, payload and blob checks still run in order.
                if (next.kind == kPackEntryKindMedia &&
                    next.size == kPackMediaBytes &&
                    next.offset >= dirEnd && next.offset <= fileBytes &&
                    next.size <= fileBytes - next.offset &&
                    next.offset % kPackPayloadAlign == 0) {
                    const auto nextMedia = ReadPackMedia(bytes + next.offset);
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
    VnResult Finish(PackCanonicalImage&,
        std::vector<std::pair<std::uint64_t, std::uint64_t>>&) const {
        return VN_OK;
    }
};

constexpr std::size_t kCanonicalReadWindowBytes = 63 * 1024;

PackMediaDemand ResolvePackMediaDemand(const PackMediaDemandResolver* demand,
    const PackCanonicalImage& image, VnSoundId soundId) {
    if (demand != nullptr && demand->resolve != nullptr)
        return demand->resolve(demand->ctx, image, soundId);
    bool streamed = false;
    return FindSoundStreaming(image.soundStreaming, soundId, &streamed) && streamed
        ? PackMediaDemand::Stream : PackMediaDemand::Resident;
}

struct FileCanonicalSource {
    std::shared_ptr<const StreamByteSource> file;
    std::uint64_t fileBytes;
    std::uint64_t maxResidentBytes;
    PCMRegistry& registry;
    std::span<const PCMRegistry::PreparedPackSamples* const> equalCandidates;
    std::vector<PCMRegistry::PreparedPackSamples>* output;
    const PackCanonicalReadHooksForTests* hooks;
    const PackMediaDemandResolver* demand = nullptr;
    bool metadataOnly = false;
    std::filesystem::path path;
    std::uint64_t hash = kFnv1a64OffsetBasis;
    bool retryWithBytes = false;
    std::uint64_t cachedBytes = 0;
    detail::PackDecodeCharge scratch;
    detail::PackDecodeCharge encoded;
    detail::PackDecodeCharge* outputStaging = nullptr;
    std::map<std::pair<std::uint64_t, std::uint64_t>, std::vector<std::uint8_t>> metadata;

    std::uint64_t ByteCount() const noexcept { return fileBytes; }
    const std::uint8_t* Data(std::uint64_t offset, std::uint64_t size) {
        if (offset > fileBytes || size > fileBytes - offset || size > SIZE_MAX)
            return nullptr;
        const auto key = std::make_pair(offset, size);
        const auto found = metadata.find(key);
        if (found != metadata.end()) return found->second.data();
        const auto requested = size == 0 ? 1 : size;
        if (cachedBytes > fileBytes || requested > fileBytes - cachedBytes) {
            retryWithBytes = true;
            return nullptr;
        }
        scratch.AddRows<decltype(metadata)::value_type>(1);
        auto& bytes = metadata[key];
        bytes.resize(static_cast<std::size_t>(requested));
        cachedBytes += requested;
        return ReadStreamBytes(*file, offset, bytes.data(), static_cast<std::size_t>(size), {}) ==
                StreamReadStatus::Ok ? bytes.data() : nullptr;
    }
    bool VerifyMedia(const PackTocEntry&, std::uint32_t, const std::uint8_t*,
                     const PackFileHeader&, std::uint64_t) const noexcept {
        // The common parser must finish all metadata checks before physical
        // bytes are consumed into unpublished sample owners in Finish.
        return true;
    }
    // Second bounded pass over one streamed blob: proves each chunk equals the
    // bytes checked by the sequential pass and computes the PCM revision in
    // frame/channel order.
    VnResult PrepareStreamRange(const PackTocEntry& media, std::vector<std::uint64_t>&& checksums,
                                std::vector<float>& staging,
                                PCMRegistry::PreparedPackSamples* out,
                                detail::PackDecodeCharge& completedStaging) {
        auto range = std::make_shared<PackMediaRange>();
        range->source = file;
        range->blobOffset = media.blobOffset;
        range->sampleRate = media.sampleRate;
        range->channelCount = media.channelCount;
        range->frameCount = media.frameCount;
        range->packFlags = media.flags;
        range->loop = CanonicalPcmLoop(media);
        range->chunkFrames = kPackMediaRangeChunkFrames;
        range->chunkChecksums = std::move(checksums);
        const MediaPcmContentRevisionInputV1 input{range->sampleRate, range->channelCount,
            range->frameCount, nullptr, 1, range->loop.startFrame, range->loop.endFrame};
        auto revision = detail::BeginMediaPcmRevisionV1(input);
        VnResult status = VN_OK;
        for (std::uint32_t chunk = 0; chunk < range->chunkChecksums.size(); ++chunk) {
            if (ReadPlanarMediaChunk(*file, range->blobOffset, range->frameCount,
                    range->channelCount, range->chunkFrames, chunk, staging.data(), {}) !=
                    StreamReadStatus::Ok ||
                Fnv1a64(staging.data(), static_cast<std::size_t>(range->ChunkBytes())) !=
                    range->chunkChecksums[chunk])
                return VN_BAD_PACK;
            if (status != VN_OK) continue;
            const std::uint64_t first = static_cast<std::uint64_t>(chunk) * range->chunkFrames;
            const auto valid = std::min<std::uint64_t>(range->chunkFrames, range->frameCount - first);
            for (std::uint64_t frame = 0; frame < valid && status == VN_OK; ++frame) {
                for (std::uint32_t ch = 0; ch < range->channelCount; ++ch) {
                    std::uint32_t bits = 0;
                    if (!detail::ReadMediaPcmRevisionBitsV1(
                            staging.data() + static_cast<std::size_t>(ch) * range->chunkFrames,
                            frame, &bits)) {
                        status = VN_INVALID_ARG;
                        break;
                    }
                    revision.AddU32(bits);
                }
            }
        }
        if (status != VN_OK) return status;
        range->contentRevision = detail::FinishMediaPcmRevisionV1(revision, input);
        const PCMRegistry::PreparedPackSamples* equal = nullptr;
        for (const auto* prior : equalCandidates) {
            if (prior == nullptr || !prior->IsPrepared() || prior->SoundId() != media.soundId) continue;
            const auto match = MatchMediaContent(prior->View(), prior->Range(), {}, range.get());
            if (match == MediaContentMatch::Failed) return VN_BAD_PACK;
            if (match == MediaContentMatch::Equal) equal = prior;
            break;
        }
        if (equal == nullptr)
            completedStaging.Add(sizeof(PackMediaRange) +
                detail::DecodeBytes(range->chunkChecksums.size(), sizeof(std::uint64_t)));
        return registry.PreparePackRangeSamples(media.soundId, std::move(range), out, equal);
    }
    VnResult Finish(PackCanonicalImage& image,
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
        if (metadataOnly) return VN_OK;
        detail::PackDecodeCharge scanScratch(scratch.Budget());
        std::vector<std::uint8_t> streamed;
        detail::DecodeResize(streamed, image.media.size(), scanScratch);
        std::uint64_t residentBytes = fileBytes;
        std::uint32_t maxStreamChannels = 0;
        for (std::size_t i = 0; i < image.media.size(); ++i) {
            streamed[i] = ResolvePackMediaDemand(demand, image, image.media[i].soundId) ==
                PackMediaDemand::Stream;
            if (streamed[i] != 0) {
                residentBytes -= image.media[i].blobBytes;
                maxStreamChannels = std::max<std::uint32_t>(maxStreamChannels, image.media[i].channelCount);
            }
        }
        if (residentBytes > maxResidentBytes) return VN_PACK_RESIDENT_CAPACITY;
        encoded.Add(residentBytes);
        std::vector<PCMRegistry::PackSampleBuilder> builders;
        detail::DecodeResize(builders, image.media.size(), scanScratch);
        std::vector<PlanarChunkChecksumBuilder> chunkSums;
        detail::DecodeResize(chunkSums, image.media.size(), scanScratch);
        detail::PackDecodeCharge completedStaging(scratch.Budget());
        std::vector<PCMRegistry::PreparedPackSamples> completed;
        detail::DecodeResize(completed, image.media.size(), completedStaging);
        std::vector<std::size_t> physicalOrder;
        detail::DecodeReserve(physicalOrder, image.media.size(), scanScratch);
        std::vector<std::uint64_t> checksums;
        detail::DecodeResize(checksums, image.media.size(), scanScratch);
        std::fill(checksums.begin(), checksums.end(), kFnv1a64OffsetBasis);
        std::vector<VnResult> sampleStatus;
        detail::DecodeResize(sampleStatus, image.media.size(), scanScratch);
        std::fill(sampleStatus.begin(), sampleStatus.end(), VN_OK);
        for (std::size_t i = 0; i < image.media.size(); ++i) {
            physicalOrder.push_back(i);
        }
        std::sort(physicalOrder.begin(), physicalOrder.end(), [&](auto a, auto b) {
            return image.media[a].blobOffset < image.media[b].blobOffset;
        });
        // Physical input and the builder's 1 KiB prefix workspace share the
        // same 64 KiB budget, independent of media size.
        std::array<float, kCanonicalReadWindowBytes / sizeof(float)> window;
        auto cached = metadata.begin();
        std::size_t currentMedia = 0;
        for (std::uint64_t offset = 0; offset < fileBytes;) {
            const auto count = static_cast<std::size_t>(
                std::min<std::uint64_t>(sizeof(window), fileBytes - offset));
            if (hooks != nullptr && hooks->beforeSequentialRead != nullptr)
                hooks->beforeSequentialRead(hooks->userData, offset);
            if (ReadStreamBytes(*file, offset, window.data(), count, {}) != StreamReadStatus::Ok)
                return VN_BAD_PACK;
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
                if (position == media.blobOffset && streamed[index] != 0) {
                    if (!chunkSums[index].Begin(media.frameCount, media.channelCount,
                                                kPackMediaRangeChunkFrames))
                        return VN_BAD_PACK;
                } else if (position == media.blobOffset) {
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
                        if (image.media[prior].soundId != media.soundId || streamed[prior] != 0) continue;
                        if (!completed[prior].IsPrepared() && sampleStatus[prior] == VN_OK)
                            sampleStatus[prior] = builders[prior].Complete(&completed[prior]);
                        if (completed[prior].IsPrepared()) { candidate = &completed[prior]; break; }
                    }
                    if (candidate == nullptr) {
                        for (const auto* prior : equalCandidates) {
                            if (prior != nullptr && prior->IsPrepared() && prior->SoundId() == media.soundId &&
                                prior->View().Valid()) {
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
                if (streamed[index] != 0) {
                    chunkSums[index].Add(bytes + cursor, step);
                } else if (sampleStatus[index] == VN_OK) {
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
        // The artifact is exactly fileBytes; growth during validation refuses.
        std::error_code sizeError;
        if (std::filesystem::file_size(path, sizeError) != fileBytes || sizeError) return VN_BAD_PACK;
        for (std::size_t i = 0; i < image.media.size(); ++i) {
            if (checksums[i] != image.media[i].blobChecksum) return VN_BAD_PACK;
            if (streamed[i] != 0 && !chunkSums[i].Complete()) return VN_BAD_PACK;
        }
        // Retain the existing independent revision chains across two final
        // owners. This never keeps another complete PCM representation.
        for (std::size_t order = 0; order < physicalOrder.size(); ++order) {
            const auto index = physicalOrder[order];
            if (streamed[index] != 0 || sampleStatus[index] != VN_OK || completed[index].IsPrepared())
                continue;
            if (order + 1 < physicalOrder.size() && streamed[physicalOrder[order + 1]] == 0) {
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
        if (maxStreamChannels != 0) {
            std::vector<float> staging;
            detail::DecodeResize(staging,
                static_cast<std::size_t>(kPackMediaRangeChunkFrames) * maxStreamChannels, scanScratch);
            for (const auto index : physicalOrder) {
                if (streamed[index] == 0) continue;
                const auto prepared = PrepareStreamRange(image.media[index], chunkSums[index].Take(),
                    staging, &completed[index], completedStaging);
                if (prepared == VN_BAD_PACK) return prepared;
                sampleStatus[index] = prepared;
            }
        }
        for (std::size_t i = 0; i < image.media.size(); ++i) {
            if (sampleStatus[i] != VN_OK) return sampleStatus[i];
        }
        *output = std::move(completed);
        if (outputStaging != nullptr) outputStaging->Merge(std::move(completedStaging));
        return VN_OK;
    }
};

template <class Source>
VnResult ParsePackCanonicalCore(Source& source, PackCanonicalImage* out,
    const detail::PackDecodeBudget& budget) {
    detail::PackDecodeCharge scratch(budget);

    const auto fileBytes = source.ByteCount();
    const auto* bytes = source.Data(0, kPackFileHeaderBytes);
    if (bytes == nullptr || out == nullptr || fileBytes < kPackFileHeaderBytes) {
        return VN_BAD_PACK;
    }
    PackFileHeader header;
    if (!ReadPackFileHeader(bytes, &header)) {
        return VN_BAD_PACK;
    }
    if (!SupportedPackFormatVersion(header.formatVersion) ||
        header.headerBytes != kPackFileHeaderBytes ||
        header.fileBytes != fileBytes) {
        return VN_BAD_PACK;
    }
    if (!PackIsCanonicalKind(header.packKind)) {
        return VN_BAD_PACK;
    }
    const std::uint64_t dirBytes =
        static_cast<std::uint64_t>(header.entryCount) *
        kPackDirectoryEntryBytes;
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
    image.staging = detail::PackDecodeCharge(budget);
    std::array<std::size_t, 9> requestedSlots{};
    image.packKind = header.packKind;

    std::uint64_t attachmentOrdinal = 0;
    std::uint64_t sendOrdinal = 0;
    std::uint64_t busEffectOrdinal = 0;
    std::uint64_t dynamicRouteOrdinal = 0;
    std::uint64_t dependencyOrdinal = 0;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    detail::DecodeReserve(ranges, static_cast<std::size_t>(header.entryCount) * 2, scratch);

    for (std::uint32_t i = 0; i < header.entryCount; ++i) {
        const PackDirectoryEntry entry = ReadPackDirectoryEntry(
            dir + static_cast<std::size_t>(i) * kPackDirectoryEntryBytes);
        if (!PackEntryKindSupported(entry.kind)) return VN_BAD_PACK;
        if (!VintageLegalForKind(entry.kind, entry.vintage)) {
            return VN_PACK_INCOMPATIBLE;
        }
        if (entry.offset < dirEnd || entry.offset > fileBytes ||
            entry.size > fileBytes - entry.offset ||
            entry.offset % kPackPayloadAlign != 0) {
            return VN_BAD_PACK;
        }
        ranges.emplace_back(entry.offset, entry.size);
        const std::uint8_t* payload = source.Data(entry.offset, entry.size);
        if (payload == nullptr) return VN_BAD_PACK;
        if (Fnv1a64(payload, static_cast<std::size_t>(entry.size)) !=
            entry.payloadChecksum) {
            return VN_BAD_PACK;
        }
        switch (entry.kind) {

        case kPackEntryKindCanonicalDefinitionPartition: {
            if (image.hasPartition) {
                return VN_BAD_PACK;
            }
            PackCanonicalPartitionHeader partitionHeader;
            if (entry.size < kPackCanonicalPartitionHeaderBytes) {
                return VN_PACK_INCOMPATIBLE;
            }
            const bool parsed = ReadPackCanonicalPartitionHeader(payload, entry.size, &partitionHeader);
            if (!parsed) {
                return VN_BAD_PACK;
            }
            if (!IsReadableCurrentWorldBodyVintage(partitionHeader.bodyVintage)) {
                return VN_PACK_INCOMPATIBLE;
            }
            CurrentWorldBodyImage body;
            const CurrentWorldBodyDecodeResult decoded = body.DecodeWithBudget(
                payload + partitionHeader.headerBytes,
                static_cast<std::size_t>(partitionHeader.bodyBytes),
                partitionHeader.bodyVintage,
                CurrentWorldDecodeMode::StrictArtifact, budget);
            if (decoded != CurrentWorldBodyDecodeResult::Ok) {
                return decoded == CurrentWorldBodyDecodeResult::Capacity
                           ? VN_PACK_RESIDENT_CAPACITY
                           : VN_BAD_PACK;
            }
            const WorldInstallStatus validation =
                detail::ValidateCurrentWorldPartitionLocalWithBudget(*body.ProductionView(), budget);
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
            detail::DecodeReserve(image.partitionBody, static_cast<std::size_t>(partitionHeader.bodyBytes), image.staging);
            image.partitionBody.assign(
                payload + partitionHeader.headerBytes,
                payload + partitionHeader.headerBytes +
                    static_cast<std::size_t>(partitionHeader.bodyBytes));
            detail::DecodeReserve(image.soundStreaming, decodedView.playablePropertyCount, image.staging);
            CollectSoundStreaming(decodedView, &image.soundStreaming);
            image.hasPartition = true;
            break;
        }
        case kPackEntryKindPackRequirementBlock: {
            if (image.hasRequirement) {
                return VN_BAD_PACK;
            }
            PackRequirementBlock requirement;
            if (entry.size != kPackRequirementBlockBytes) {
                return VN_PACK_INCOMPATIBLE;
            }
            if (!ParsePackRequirementBlock(payload, entry.size, &requirement)) {
                return VN_BAD_PACK;
            }
            if (requirement.schemaVersion != entry.vintage ||
                requirement.storageModelVersion != entry.vintage) {
                return VN_PACK_INCOMPATIBLE;
            }
            if (!PackRequirementBlockValid(requirement) ||
                requirement.payloadChecksumCoverage != Fnv1a64(payload, kPackRequirementBlockPayloadChecksumCoverageOffset)) {
                return VN_BAD_PACK;
            }
            image.requirement = requirement;
            image.packStableId = requirement.packStableId;
            image.hasRequirement = true;
            break;
        }
        case kPackEntryKindPackDependencyEdge: {
            PackDependencyEdge edge;
            if (entry.size != kPackDependencyEdgeBytes) {
                return VN_PACK_INCOMPATIBLE;
            }
            if (!ReadPackDependencyEdge(payload, entry.size, &edge) ||
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
            detail::DecodeAppend(image.dependencyEdges, edge, image.staging, requestedSlots[0]);
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
                ParseBusDefinitionSnapshotPayload(payload, entry.size, &image);
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
                payload, entry.size, &image.variablePolicyOverlays, image.staging);
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
                payload, entry.size, &image.variableRequirements, image.staging);
            if (parsed != VN_OK) {
                return parsed;
            }
            image.hasVariableRequirements = true;
            break;
        }
        case kPackEntryKindMedia: {
            if (entry.size != kPackMediaBytes) {
                return VN_BAD_PACK;
            }
            const PackTocEntry media = ReadPackMedia(payload);
            if (media.soundId != entry.id ||
                !ValidateV2MediaEntry(media, fileBytes, dirEnd)) {
                return VN_BAD_PACK;
            }
            if (!source.VerifyMedia(media, i, dir, header, dirEnd)) return VN_BAD_PACK;
            ranges.emplace_back(media.blobOffset, media.blobBytes);
            detail::DecodeAppend(image.media, media, image.staging, requestedSlots[1]);
            break;
        }
        case kPackEntryKindBusEffect: {
            PackBusEffectEntry effect;
            if (!ReadPackBusEffect(payload, entry.size, &effect) ||
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
            detail::DecodeAppend(image.busEffects, effect, image.staging, requestedSlots[2]);
            break;
        }
        case kPackEntryKindEffectDynamicRoute: {
            PackEffectDynamicRouteEntry route;
            if (!ParsePackEffectDynamicRoute(payload, entry.size, &route) ||
                entry.relationOrdinal != image.dynamicRoutes.size() || route.targetBusId == VnObjectId{} ||
                route.sourceId == VnObjectId{} || route.expectedEffectType != VN_EFFECT_COMPRESSOR ||
                route.signalForm != VN_DYNAMIC_SIGNAL_RAW_AUDIO ||
                route.sourceKind != VN_DYNAMIC_SOURCE_BUS ||
                route.tap > VN_DYNAMIC_TAP_BUS_POST_FADER ||
                route.effectSlot >= kMaxFxSlotsPerBus || route.routeSlot >= 4u ||
                !std::isfinite(route.levelDb) || route.levelDb < -96.0f || route.levelDb > 24.0f) {
                return VN_BAD_PACK;
            }
            detail::DecodeAppend(image.dynamicRoutes, route, image.staging, requestedSlots[3]);
            break;
        }
        case kPackEntryKindBus: {
            PackBusEntry bus;
            const bool parsed =
                ParsePackBus(payload, entry.size, &bus);
            if (!parsed || bus.busId != entry.id ||
                !std::isfinite(bus.volume) ||
                !std::isfinite(bus.outputBusVolume)) {
                return VN_BAD_PACK;
            }
            detail::DecodeAppend(image.buses, bus, image.staging, requestedSlots[4]);
            break;
        }
        case kPackEntryKindBusSend: {
            PackBusSendEntry send;
            if (!ParsePackBusSend(payload, entry.size, &send) ||
                entry.relationOrdinal != sendOrdinal ||
                !TryVolumeGainFromDb(send.levelDb)) {
                return VN_BAD_PACK;
            }
            ++sendOrdinal;
            detail::DecodeAppend(image.busSends, send, image.staging, requestedSlots[5]);
            break;
        }
        case kPackEntryKindAttenuation: {
            PackLogicImage::Attenuation attenuation;
            attenuation.id = entry.id;
            if (!ReadPackAttenuation(payload, entry.size, &attenuation.desc)) {
                return VN_BAD_PACK;
            }
            detail::DecodeAppend(image.attenuations, attenuation, image.staging, requestedSlots[6]);
            break;
        }
        case kPackEntryKindBindingDefinition: {
            PackLogicImage::BindingDefinition definition;
            if (!ReadPackBindingDefinition(payload, entry.size,
                                              &definition.id,
                                              &definition.desc) ||
                definition.id != entry.id ||
                !IsValidBindingTransitionMs(definition.desc.transitionMs)) {
                return VN_BAD_PACK;
            }
            if (definition.desc.targetScopeKind >
                VN_TARGET_SCOPE_BUS_EFFECT_PARAM) {
                return VN_BAD_PACK;
            }
            if (header.packKind == kPackKindInit &&
                (definition.desc.targetScopeKind == VN_TARGET_SCOPE_VOICE ||
                 definition.desc.targetScopeKind ==
                     VN_TARGET_SCOPE_VOICE_EFFECT_PARAM)) {
                return VN_BAD_PACK;
            }
            detail::DecodeAppend(image.bindingDefinitions, definition, image.staging, requestedSlots[7]);
            break;
        }
        case kPackEntryKindBindingAttachment: {
            PackBindingAttachmentEntry attachment;
            if (!ParsePackBindingAttachment(payload, entry.size,
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
                attachment.scopeKind != VN_TARGET_SCOPE_BUS &&
                attachment.scopeKind !=
                    VN_TARGET_SCOPE_BUS_EFFECT_PARAM) {
                return VN_BAD_PACK;
            }
            detail::DecodeAppend(image.bindingAttachments, attachment, image.staging, requestedSlots[8]);
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
    const std::uint32_t maxBindingScope = VN_TARGET_SCOPE_BUS_EFFECT_PARAM;
    for (const auto& definition : image.bindingDefinitions) {
        if (definition.desc.targetScopeKind > maxBindingScope) {
            return VN_BAD_PACK;
        }
        if (header.packKind == kPackKindInit &&
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
            attachment.scopeKind != VN_TARGET_SCOPE_BUS &&
            attachment.scopeKind != VN_TARGET_SCOPE_BUS_EFFECT_PARAM) {
            return VN_BAD_PACK;
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
    const auto finished = source.Finish(image, ranges);
    if (finished != VN_OK) return finished;
    *out = std::move(image);
    return VN_OK;
}

} // namespace

VnResult ParsePackCanonicalImage(const std::uint8_t* bytes, std::uint64_t fileBytes,
                                 PackCanonicalImage* out, std::uint64_t* artifactHash) {
    SpanCanonicalSource source{{bytes, static_cast<std::size_t>(fileBytes), artifactHash}};
    return ParsePackCanonicalCore(source, out, {});
}

PackCanonicalImage CopyCanonicalImageWithBudget(const PackCanonicalImage& source,
    const detail::PackDecodeBudget& budget) {
    if (source.staging.Budget() == budget) return source;
    detail::PackDecodeCharge admitted(budget);
    admitted.Add(source.staging.Bytes());
    auto copy = source;
    copy.staging = std::move(admitted);
    return copy;
}

VnResult ParsePackCanonicalImageWithBudget(const std::uint8_t* bytes, std::uint64_t fileBytes,
    PackCanonicalImage* out, std::uint64_t* artifactHash, const detail::PackDecodeBudget& budget) {
    try {
        SpanCanonicalSource source{{bytes, static_cast<std::size_t>(fileBytes), artifactHash}};
        return ParsePackCanonicalCore(source, out, budget);
    } catch (const std::bad_alloc&) { return VN_PACK_RESIDENT_CAPACITY; }
}

VnResult ReadPackCanonicalFile(const char* utf8Path, PCMRegistry& registry,
    PackCanonicalImage* out, std::vector<PCMRegistry::PreparedPackSamples>* samples,
    std::uint64_t* byteCount, std::uint64_t* artifactHash, std::uint64_t maxBytes,
    std::span<const PCMRegistry::PreparedPackSamples* const> equalCandidates,
    bool* retryWithBytes, const PackCanonicalReadHooksForTests* hooks,
    const detail::PackDecodeBudget& budget, detail::PackDecodeCharge* sampleStaging,
    const PackMediaDemandResolver* demand, bool metadataOnly) try {
    if (budget != nullptr && sampleStaging == nullptr) return VN_INVALID_ARG;
    if (retryWithBytes != nullptr) *retryWithBytes = false;
    if (out == nullptr || samples == nullptr || byteCount == nullptr || artifactHash == nullptr)
        return VN_INVALID_ARG;
    std::filesystem::path path;
    if (!TryMakeUtf8Path(utf8Path, &path)) return VN_BAD_PACK;
    auto file = OpenStreamByteSource(utf8Path);
    if (file == nullptr) return VN_BAD_PACK;
    const auto size = file->ByteCount();
    if (size == 0 || size > SIZE_MAX) return VN_BAD_PACK;
    FileCanonicalSource source{std::move(file), size, maxBytes, registry, equalCandidates,
                               samples, hooks, demand, metadataOnly};
    source.path = std::move(path);
    source.outputStaging = sampleStaging;
    source.scratch = detail::PackDecodeCharge(budget);
    source.encoded = detail::PackDecodeCharge(budget);
    const auto result = ParsePackCanonicalCore(source, out, budget);
    if (retryWithBytes != nullptr) *retryWithBytes = source.retryWithBytes;
    if (result == VN_OK) {
        *byteCount = size;
        *artifactHash = source.hash;
    }
    return result;
} catch (const std::bad_alloc&) {
    return VN_PACK_RESIDENT_CAPACITY;
}

void CollectSoundStreaming(const ContainerWorldInstallViewV1& view,
                           std::vector<PackSoundStreamingRow>* out) {
    using PropertyId = playableproperties::PlayablePropertyIdV1;
    constexpr auto slot = static_cast<std::size_t>(PropertyId::StreamingMode) - 1;
    for (std::uint32_t i = 0; i < view.playablePropertyCount; ++i) {
        const auto& row = view.playableProperties[i];
        if (row.contentPropertyKind != playableproperties::ContentPropertyKindV1::Sound) continue;
        const bool present = (row.payload.presentMask & (UINT64_C(1) << slot)) != 0;
        out->push_back({row.stableId,
            present && static_cast<std::uint32_t>(row.payload.values[slot].bits) != 0});
    }
    std::sort(out->begin(), out->end(),
              [](const auto& a, const auto& b) { return a.soundId < b.soundId; });
}

bool FindSoundStreaming(const std::vector<PackSoundStreamingRow>& rows,
                        VnSoundId soundId, bool* streamed) noexcept {
    const auto found = std::lower_bound(rows.begin(), rows.end(), soundId,
        [](const auto& row, VnSoundId id) { return row.soundId < id; });
    if (found == rows.end() || found->soundId != soundId) return false;
    if (streamed != nullptr) *streamed = found->streamed;
    return true;
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
