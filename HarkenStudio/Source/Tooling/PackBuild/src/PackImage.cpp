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

namespace harken::studio::build {

namespace {
bool ValidateV2MediaEntry(const PackTocEntry& e, std::uint64_t fileBytes,
                          std::uint64_t blobRegionStart) {
    const bool vorbis = e.sampleFormat == HK_PACK_SAMPLE_FORMAT_VORBIS_RESIDENT;
    if (!vorbis && e.sampleFormat != HK_PACK_SAMPLE_FORMAT_F32_PLANAR) {
        return false;
    }
    if ((e.flags & ~HK_PACK_MEDIA_KNOWN_FLAGS) != 0) {
        return false;
    }
    if (e.sampleRate == 0 || e.channelCount == 0 ||
        e.channelCount > kMaxChannels || e.frameCount == 0) {
        return false;
    }
    const std::uint64_t channels = e.channelCount;
    if (vorbis) {
        if ((channels != 1 && channels != 2 && channels != 4 && channels != 6 && channels != 8) ||
            e.blobBytes < sizeof(HkPackVorbisHeader)) return false;
    } else if (e.frameCount > UINT64_MAX / (channels * sizeof(float)) ||
               e.blobBytes != e.frameCount * channels * sizeof(float)) {
        return false;
    }
    if (e.blobOffset % HK_PACK_BLOB_ALIGN != 0) {
        return false;
    }
    if (e.blobOffset < blobRegionStart || e.blobOffset > fileBytes ||
        e.blobBytes > fileBytes - e.blobOffset) {
        return false;
    }
    if ((e.flags & HK_PACK_MEDIA_FLAG_LOOPABLE) != 0) {
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
    case HK_PACK_ENTRY_KIND_MEDIA:
    case HK_PACK_ENTRY_KIND_CONTAINER:
    case HK_PACK_ENTRY_KIND_VOICE_RECIPE:
        return packKind == HK_PACK_KIND_CONTENT;
    case HK_PACK_ENTRY_KIND_RETIRED_ACTION:
        return false;
    case HK_PACK_ENTRY_KIND_BUS:
    case HK_PACK_ENTRY_KIND_BUS_SEND:
    case HK_PACK_ENTRY_KIND_ATTENUATION:
    case HK_PACK_ENTRY_KIND_BUS_EFFECT:
    case HK_PACK_ENTRY_KIND_EFFECT_DYNAMIC_ROUTE:
        return packKind == HK_PACK_KIND_RESIDENT;
    case HK_PACK_ENTRY_KIND_BINDING_DEFINITION:
    case HK_PACK_ENTRY_KIND_BINDING_ATTACHMENT:
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
    std::map<harken::studio::types::ObjectId, std::size_t> indices;
    for (std::size_t i = 0; i < buses.size(); ++i) {
        if (harken::studio::types::IsNull(buses[i].busId) ||
            !indices.emplace(buses[i].busId, i).second) return false;
    }
    const auto master = indices.find(harken::studio::types::kMasterBusObjectId);
    if (master == indices.end() ||
        !harken::studio::types::IsNull(buses[master->second].parentId)) return false;

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
    std::map<harken::studio::types::ObjectId, std::size_t> indices;
    for (std::size_t i = 0; i < buses.size(); ++i) {
        indices.emplace(buses[i].busId, i);
    }
    std::vector<std::vector<std::size_t>> edges(buses.size());
    std::vector<std::size_t> indegree(buses.size(), 0);
    const auto addEdge = [&](harken::studio::types::ObjectId source,
                             harken::studio::types::ObjectId target) {
        const auto from = indices.find(source);
        const auto to = indices.find(target);
        if (from == indices.end() || to == indices.end()) return false;
        edges[from->second].push_back(to->second);
        ++indegree[to->second];
        return true;
    };
    for (const auto& bus : buses) {
        if (!harken::studio::types::IsNull(bus.parentId) &&
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

harken::studio::build::PackResult ParsePackV2Image(const std::uint8_t* bytes, std::uint64_t fileBytes,
                          PackLogicImage* out) {
    if (bytes == nullptr || fileBytes < kPackBytes<HkPackFileHeader>) {
        return harken::studio::build::kPackInvalid;
    }
    PackFileHeader header;
    if (!ReadPackFileHeader(bytes, &header)) {
        return harken::studio::build::kPackInvalid;
    }
    if (header.formatVersion != HK_PACK_FORMAT_VERSION ||
        header.headerBytes != kPackBytes<HkPackFileHeader> ||
        header.fileBytes != fileBytes) {
        return harken::studio::build::kPackInvalid;
    }
    if (PackIsCanonicalKind(header.packKind)) {
        return harken::studio::build::kPackIncompatible;
    }
    if (header.packKind != HK_PACK_KIND_RESIDENT &&
        header.packKind != HK_PACK_KIND_CONTENT) {
        return harken::studio::build::kPackInvalid;
    }
    if (header.entryCount == 0 &&
        (header.packKind != HK_PACK_KIND_CONTENT ||
         header.fileBytes != header.headerBytes)) {
        return harken::studio::build::kPackInvalid;
    }
    const std::uint64_t dirBytes =
        static_cast<std::uint64_t>(header.entryCount) *
        kPackBytes<HkPackDirectoryEntry>;
    if (header.directoryOffset != header.headerBytes ||
        header.directoryOffset > fileBytes ||
        dirBytes > fileBytes - header.directoryOffset) {
        return harken::studio::build::kPackInvalid;
    }
    const std::uint8_t* dir = bytes + header.directoryOffset;
    if (Fnv1a64(dir, static_cast<std::size_t>(dirBytes)) !=
        header.directoryChecksum) {
        return harken::studio::build::kPackInvalid;
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
            dir + static_cast<std::size_t>(i) * kPackBytes<HkPackDirectoryEntry>);
        if (!PackEntryKindSupported(entry.kind)) return kPackInvalid;
        if (PackIsCanonicalEntryKind(entry.kind)) {
            return harken::studio::build::kPackIncompatible;
        }
        if (!VintageLegalForKind(entry.kind, entry.vintage)) {
            return harken::studio::build::kPackIncompatible;
        }
        if (!KindLegalForPack(entry.kind, header.packKind)) {
            return harken::studio::build::kPackInvalid;
        }
        if (entry.offset < dirEnd || entry.offset > fileBytes ||
            entry.size > fileBytes - entry.offset) {
            return harken::studio::build::kPackInvalid;
        }
        if (entry.offset % HK_PACK_PAYLOAD_ALIGN != 0) {
            return harken::studio::build::kPackInvalid; // the writer aligns every payload
        }
        ranges.emplace_back(entry.offset, entry.size);
        const std::uint8_t* payload = bytes + entry.offset;
        if (Fnv1a64(payload, static_cast<std::size_t>(entry.size)) !=
            entry.payloadChecksum) {
            return harken::studio::build::kPackInvalid;
        }
        image.stamps.push_back(
            {entry.kind, entry.id, entry.payloadChecksum});
        switch (entry.kind) {
        case HK_PACK_ENTRY_KIND_MEDIA: {
            if (entry.size != kPackBytes<HkPackMedia>) {
                return harken::studio::build::kPackInvalid;
            }
            const PackTocEntry media = ReadPackMedia(payload);
            if (media.soundId != entry.id ||
                !ValidateV2MediaEntry(media, fileBytes, dirEnd)) {
                return harken::studio::build::kPackInvalid;
            }
            if (Fnv1a64(bytes + media.blobOffset,
                        static_cast<std::size_t>(media.blobBytes)) !=
                media.blobChecksum) {
                return harken::studio::build::kPackInvalid;
            }
            ranges.emplace_back(media.blobOffset, media.blobBytes);
            image.media.push_back(media);
            break;
        }
        case HK_PACK_ENTRY_KIND_BUS: {
            PackBusEntry bus;
            const bool parsed =
                ParsePackRow<HkPackBus>(payload, entry.size, &bus);
            if (!parsed || bus.busId != entry.id ||
                !std::isfinite(bus.volume) ||
                !std::isfinite(bus.outputBusVolume)) {
                return harken::studio::build::kPackInvalid;
            }
            image.buses.push_back(bus);
            break;
        }
        case HK_PACK_ENTRY_KIND_BUS_SEND: {
            PackBusSendEntry send;
            if (!ParsePackRow<HkPackBusSend>(payload, entry.size, &send) ||
                entry.relationOrdinal != sendOrdinal) {
                return harken::studio::build::kPackInvalid;
            }
            if (!std::isfinite(send.levelDb)) {
                return harken::studio::build::kPackInvalid;
            }
            ++sendOrdinal;
            image.busSends.push_back(send);
            break;
        }
        case HK_PACK_ENTRY_KIND_BUS_EFFECT: {
            PackBusEffectEntry effect;
            if (!ReadPackBusEffect(payload, entry.size, &effect) ||
                entry.relationOrdinal != busEffectOrdinal || effect.busId == harken::studio::types::ObjectId{} ||
                effect.bypass > 1 ||
                effect.slot >= kMaxFxSlotsPerBus ||
                effect.faderPointGuard > kMaxFxSlotsPerBus) {
                return harken::studio::build::kPackInvalid;
            }
            const PackEffectSchemaRow* effectSchema =
                FindPackEffectSchemaByType(effect.effectType);
            if (effectSchema == nullptr ||
                effect.descriptorVintage !=
                    effectSchema->descriptorVintage ||
                effect.paramCount != effectSchema->paramCount) {
                return harken::studio::build::kPackInvalid;
            }
            for (std::uint32_t p = 0; p < effect.paramCount; ++p) {
                if (!std::isfinite(effect.params[p])) {
                    return harken::studio::build::kPackInvalid;
                }
            }
            ++busEffectOrdinal;
            image.busEffects.push_back(effect);
            break;
        }
        case HK_PACK_ENTRY_KIND_EFFECT_DYNAMIC_ROUTE: {
            PackEffectDynamicRouteEntry route;
            if (!ParsePackRow<HkPackEffectDynamicRoute>(payload, entry.size,
                                               &route) ||
                entry.relationOrdinal != dynamicRouteOrdinal ||
                route.targetBusId == harken::studio::types::ObjectId{} || route.sourceId == harken::studio::types::ObjectId{} ||
                route.expectedEffectType != harken::studio::types::kEffectCompressor ||
                route.signalForm != harken::studio::types::kDynamicSignalRawAudio ||
                route.sourceKind != harken::studio::types::kDynamicSourceBus ||
                route.tap > harken::studio::types::kDynamicTapBusPostFader ||
                route.effectSlot >= kMaxFxSlotsPerBus ||
                route.routeSlot >= 4u ||
                !std::isfinite(route.levelDb) ||
                route.levelDb < -96.0f || route.levelDb > 24.0f) {
                return harken::studio::build::kPackInvalid;
            }
            ++dynamicRouteOrdinal;
            image.dynamicRoutes.push_back(route);
            break;
        }
        case HK_PACK_ENTRY_KIND_ATTENUATION: {
            PackLogicImage::Attenuation attenuation;
            attenuation.id = entry.id;
            if (!ReadPackAttenuation(payload, entry.size,
                                        &attenuation.desc)) {
                return harken::studio::build::kPackInvalid;
            }
            if (!std::isfinite(attenuation.desc.proximityFrequency) ||
                !(attenuation.desc.proximityFrequency > 0.0f)) {
                return harken::studio::build::kPackInvalid;
            }
            image.attenuations.push_back(attenuation);
            break;
        }
        case HK_PACK_ENTRY_KIND_RETIRED_DISCRETE_VARIABLE:
        case HK_PACK_ENTRY_KIND_RETIRED_METER_WIRING:
            return harken::studio::build::kPackInvalid;
        case HK_PACK_ENTRY_KIND_BINDING_DEFINITION: {
            PackLogicImage::BindingDefinition definition;
            if (!ReadPackBindingDefinition(payload, entry.size,
                                              &definition.id,
                                              &definition.desc) ||
                definition.id != entry.id
                || !IsValidBindingTransitionMs(definition.desc.transitionMs)) {
                return harken::studio::build::kPackInvalid;
            }
            if (definition.desc.targetScopeKind >
                harken::studio::types::kTargetScopeBusEffectParam) {
                return harken::studio::build::kPackInvalid;
            }
            if (header.packKind == HK_PACK_KIND_RESIDENT &&
                (definition.desc.targetScopeKind == harken::studio::types::kTargetScopeVoice ||
                 definition.desc.targetScopeKind ==
                     harken::studio::types::kTargetScopeVoiceEffectParam)) {
                return harken::studio::build::kPackInvalid;
            }
            image.bindingDefinitions.push_back(definition);
            break;
        }
        case HK_PACK_ENTRY_KIND_BINDING_ATTACHMENT: {
            PackBindingAttachmentEntry attachment;
            if (!ParsePackRow<HkPackBindingAttachment>(payload, entry.size,
                                              &attachment) ||
                entry.relationOrdinal != attachmentOrdinal) {
                return harken::studio::build::kPackInvalid;
            }
            ++attachmentOrdinal;
            if (attachment.scopeKind ==
                    harken::studio::types::kTargetScopeVoiceEffectParam ||
                attachment.scopeKind >
                    harken::studio::types::kTargetScopeBusEffectParam) {
                return harken::studio::build::kPackInvalid;
            }
            if (header.packKind == HK_PACK_KIND_RESIDENT &&
                attachment.scopeKind != harken::studio::types::kTargetScopeBus &&
                attachment.scopeKind !=
                    harken::studio::types::kTargetScopeBusEffectParam) {
                return harken::studio::build::kPackInvalid;
            }
            image.bindingAttachments.push_back(attachment);
            break;
        }
        case HK_PACK_ENTRY_KIND_CONTAINER: {
            PackLogicImage::Container container;
            if (!ReadPackContainerPayload(payload, entry.size, entry.id, container) ||
                container.header.childCount == 0) {
                return harken::studio::build::kPackInvalid;
            }
            image.containers.push_back(std::move(container));
            break;
        }
        case HK_PACK_ENTRY_KIND_VOICE_RECIPE: {
            PackLogicImage::VoiceRecipe recipe;
            if (!ReadPackVoiceRecipePayload(payload, entry.size, entry.id, recipe) ||
                !VoiceToneWireValuesValid(recipe.tone)) {
                return harken::studio::build::kPackInvalid;
            }
            std::uint32_t previousSlot = 0;
            for (std::uint32_t n = 0; n < recipe.header.installCount; ++n) {
                const auto& install = recipe.installs[n];
                if (                    install.slot >= HK_PACK_VOICE_RECIPE_MAX_INSTALLS ||
                    (n > 0 && install.slot <= previousSlot)) {
                    return harken::studio::build::kPackInvalid;
                }
                const PackEffectSchemaRow* effectSchema =
                    FindPackEffectSchemaByType(install.effectType);
                if (effectSchema == nullptr ||
                    install.descriptorVintage !=
                        effectSchema->descriptorVintage ||
                    install.paramCount != effectSchema->paramCount) {
                    return harken::studio::build::kPackInvalid;
                }
                for (std::uint32_t p = 0; p < install.paramCount; ++p) {
                    if (!std::isfinite(install.params[p])) {
                        return harken::studio::build::kPackInvalid;
                    }
                }
                previousSlot = install.slot;
            }
            harken::studio::types::ObjectId previousBinding{};
            for (std::uint32_t n = 0; n < recipe.header.bindingCount; ++n) {
                const auto binding = recipe.bindings[n];
                if (binding == harken::studio::types::ObjectId{} ||
                    (n > 0 && binding <= previousBinding)) {
                    return harken::studio::build::kPackInvalid;
                }
                previousBinding = binding;
            }
            image.voiceRecipes.push_back(std::move(recipe));
            break;
        }
        default:
            return harken::studio::build::kPackInvalid;
        }
    }
    std::sort(ranges.begin(), ranges.end());
    for (std::size_t i = 1; i < ranges.size(); ++i) {
        if (ranges[i].first < ranges[i - 1].first + ranges[i - 1].second) {
            return harken::studio::build::kPackInvalid;
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
        return harken::studio::build::kPackInvalid;
    }
    for (const PackTocEntry& media : image.media) {
        for (const PackLogicImage::Container& container : image.containers) {
            if (media.soundId == container.header.containerId) {
                return harken::studio::build::kPackInvalid;
            }
        }
    }
    for (std::size_t i = 0; i < image.busSends.size(); ++i) {
        for (std::size_t j = i + 1; j < image.busSends.size(); ++j) {
            if (image.busSends[i].fromBusId == image.busSends[j].fromBusId &&
                image.busSends[i].toBusId == image.busSends[j].toBusId) {
                return harken::studio::build::kPackInvalid;
            }
        }
    }
    for (std::size_t i = 0; i < image.bindingAttachments.size(); ++i) {
        for (std::size_t j = i + 1; j < image.bindingAttachments.size(); ++j) {
            const PackBindingAttachmentEntry& a = image.bindingAttachments[i];
            const PackBindingAttachmentEntry& b = image.bindingAttachments[j];
            if (a.bindingId == b.bindingId && a.scopeKind == b.scopeKind &&
                a.objectId == b.objectId) {
                return harken::studio::build::kPackInvalid;
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
                return harken::studio::build::kPackInvalid;
            }
        }
        for (std::size_t j = i + 1; j < image.busEffects.size(); ++j) {
            if (image.busEffects[i].busId == image.busEffects[j].busId &&
                image.busEffects[i].slot == image.busEffects[j].slot) {
                return harken::studio::build::kPackInvalid;
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
                return harken::studio::build::kPackInvalid;
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
                return harken::studio::build::kPackInvalid;
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
            return harken::studio::build::kPackInvalid;
        }
    }
    for (const PackBusSendEntry& send : image.busSends) {
        if (send.fromBusId == send.toBusId) {
            return harken::studio::build::kPackInvalid;
        }
    }

    *out = std::move(image);
    return harken::studio::build::kPackOk;
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
    if (payload == nullptr || out == nullptr || size < kPackBytes<HkPackVariablePolicyHeader>) return kPackInvalid;
    using Policy = PackCanonicalImage::VariablePolicyOverlay;
    struct PolicyRecord {
        Policy& target;
        PackVariablePolicyRow wire{};
        auto& Wire() noexcept { return wire; }
        bool Finish() noexcept {
            target.variableId = wire.variableId;
            target.writers = wire.writers;
            target.executionFingerprint = wire.executionFingerprint;
            return target.variableId != harken::studio::types::ObjectId{} && target.executionFingerprint != 0 && target.writers >= 1 && target.writers <= 3;
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
            return target.labelId != harken::studio::types::ObjectId{} && target.anchor >= 0 && target.anchor <= 16777216;
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
                std::set<harken::studio::types::ObjectId> identities;
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
            return count <= (bytes - kPackBytes<HkPackVariablePolicyHeader>) / kPackBytes<HkPackVariablePolicyRow> && state.Attempt([&] {
                rows.resize(static_cast<std::size_t>(count)); return true;
            });
        }
        PolicyRecord Rows(std::uint64_t index) noexcept { return {rows[static_cast<std::size_t>(index)]}; }
        bool PrepareAnchorGroups(std::uint64_t count) noexcept {
            if (count != rows.size()) return false;
            for (std::size_t i = 1; i < rows.size(); ++i) if (rows[i].variableId <= rows[i - 1].variableId) return false;
            return true;
        }
        AnchorVisitor AnchorGroups(std::uint64_t index) noexcept { return {rows[static_cast<std::size_t>(index)], state, bytes / kPackBytes<HkPackVariableAnchor>}; }
        bool PrepareLabelGroups(std::uint64_t count) noexcept { return count == rows.size(); }
        LabelVisitor LabelGroups(std::uint64_t index) noexcept { return {rows[static_cast<std::size_t>(index)], state, bytes / kPackBytes<HkPackVariableLabel>}; }
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
    if (payload == nullptr || out == nullptr || size < kPackBytes<HkPackVariableRequirementsHeader>) return kPackInvalid;
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
            return target.stableId != harken::studio::types::ObjectId{} && target.expectedExecutionFingerprint != 0 && target.expectedLabelMappingFingerprint != 0;
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
    if (!ParsePackRow<HkPackVariableRequirementsHeader>(payload, kPackBytes<HkPackVariableRequirementsHeader>, &header)) return kPackInvalid;
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





struct PackEffectPresetReader {
    PackEffectPreset& preset;
    auto& Value() noexcept { return preset.header; }
    auto& Header() noexcept { return preset.header; }
    bool PrepareParameters(std::uint64_t count) noexcept { return count == preset.parameters.size(); }
    auto& Parameters(std::uint64_t i) noexcept { return preset.parameters[static_cast<std::size_t>(i)]; }
    bool Finish() noexcept { return true; }
};

PackResult ParseEffectPresetPayload(const std::uint8_t* payload, std::uint64_t size,
    PackEffectPreset& preset) {
    if (size < kPackBytes<HkPackEffectPresetHeader> ||
        !ParsePackRow<HkPackEffectPresetHeader>(payload, kPackBytes<HkPackEffectPresetHeader>, &preset.header))
        return kPackInvalid;
    const auto& header = preset.header;
    const auto* schema = FindPackEffectSchemaByType(header.effectType);
    if (header.presetId == harken::studio::types::ObjectId{} || !schema ||
        header.descriptorVintage != schema->descriptorVintage ||
        header.paramCount != schema->paramCount ||
        size != kPackBytes<HkPackEffectPresetHeader> +
            static_cast<std::uint64_t>(header.paramCount) * kPackBytes<HkPackEffectPresetParameter>)
        return kPackInvalid;
    preset.parameters.resize(header.paramCount);
    PackWireBuffer<const std::uint8_t> input{payload, size};
    PackEffectPresetReader visitor{preset};
    if (!ReadPackEffectPreset(input, visitor) || input.offset != size) return kPackInvalid;
    for (std::uint32_t i = 0; i < header.paramCount; ++i) {
        const auto& parameter = preset.parameters[i];
        float value = 0.0f;
        std::memcpy(&value, &parameter.valueBits, sizeof(value));
        if (parameter.paramId != i || !PackEffectParamInAuthoredRange(header.effectType, i, value))
            return kPackInvalid;
    }
    return kPackOk;
}

struct PackBusDefinitionSnapshotReader {
    PackBusDefinitionSnapshotHeader& header;
    std::vector<PackBusDefinitionBus>& buses;
    std::vector<PackBusDefinitionSend>& sends;
    std::vector<PackBusDefinitionEffect>& effects;
    std::vector<PackBusDefinitionEffectParameter>& effectParameters;
    std::vector<PackBusDefinitionDownmix>& downmixes;
    std::vector<float>& coefficients;
    auto& Value() noexcept { return header; }
    auto& Header() noexcept { return header; }
    bool PrepareBuses(std::uint64_t count) noexcept { return count == buses.size(); }
    auto& Buses(std::uint64_t i) noexcept { return buses[static_cast<std::size_t>(i)]; }
    bool PrepareSends(std::uint64_t count) noexcept { return count == sends.size(); }
    auto& Sends(std::uint64_t i) noexcept { return sends[static_cast<std::size_t>(i)]; }
    bool PrepareEffects(std::uint64_t count) noexcept { return count == effects.size(); }
    auto& Effects(std::uint64_t i) noexcept { return effects[static_cast<std::size_t>(i)]; }
    bool PrepareEffectParameters(std::uint64_t count) noexcept { return count == effectParameters.size(); }
    auto& EffectParameters(std::uint64_t i) noexcept { return effectParameters[static_cast<std::size_t>(i)]; }
    bool PrepareDownmixes(std::uint64_t count) noexcept { return count == downmixes.size(); }
    auto& Downmixes(std::uint64_t i) noexcept { return downmixes[static_cast<std::size_t>(i)]; }
    bool PrepareCoefficients(std::uint64_t count) noexcept { return count == coefficients.size(); }
    auto& Coefficients(std::uint64_t i) noexcept { return coefficients[static_cast<std::size_t>(i)]; }
    bool Finish() noexcept { return true; }
};

harken::studio::build::PackResult ParseBusDefinitionSnapshotPayload(
    const std::uint8_t* payload, std::uint64_t size, PackCanonicalImage* image) {
    PackBusDefinitionSnapshotHeader header;
    if (!ReadPackBusDefinitionSnapshotHeader(payload, size, &header) ||
        header.busCount == 0 || header.busCount > 4096 ||
        (header.publicationCapabilityVintage != 5 &&
         (header.effectCount != 0 || header.effectParameterCount != 0)) ||
        header.effectCount > static_cast<std::uint64_t>(header.busCount) * kMaxFxSlotsPerBus ||
        header.downmixCount != header.busCount ||
        header.coefficientCount != static_cast<std::uint64_t>(header.busCount) * 64) {
        return kPackInvalid;
    }
    if (!ValidatePackBusDefinitionSnapshotLayout(header, size)) return kPackInvalid;
    std::vector<PackBusDefinitionBus> buses(header.busCount);
    std::vector<PackBusDefinitionSend> sends(header.sendCount);
    std::vector<PackBusDefinitionEffect> effects(header.effectCount);
    std::vector<PackBusDefinitionEffectParameter> effectParameters(header.effectParameterCount);
    std::vector<PackBusDefinitionDownmix> downmixes(header.downmixCount);
    std::vector<float> coefficients(header.coefficientCount);
    PackWireBuffer<const std::uint8_t> input{payload, size};
    PackBusDefinitionSnapshotReader visitor{header, buses, sends, effects, effectParameters, downmixes, coefficients};
    if (!ReadPackBusDefinitionSnapshot(input, visitor) || input.offset != size) return kPackInvalid;
    std::set<harken::studio::types::ObjectId> ids;
    for (std::uint32_t i = 0; i < header.busCount; ++i) {
        const auto& row = buses[i];
        if (harken::studio::types::IsNull(row.busId) || !ids.insert(row.busId).second ||
            !std::isfinite(row.faderDb) || row.faderDb < -96.0f || row.faderDb > 24.0f ||
            !std::isfinite(row.outputBusVolumeDb) ||
            !std::isfinite(std::pow(10.0f, row.outputBusVolumeDb / 20.0f)) ||
            (header.publicationCapabilityVintage != 5 &&
             (row.effectFirst != 0 || row.effectCount != 0)) ||
            (header.publicationCapabilityVintage == 5 && row.faderAfterSlot != 8) ||
            row.effectFirst > effects.size() || row.effectCount > effects.size() - row.effectFirst ||
            row.downmixCount != 1 || row.downmixFirst != i) {
            return kPackInvalid;
        }
    }
    if (!ValidateCanonicalBusParentForestV2(buses)) return kPackInvalid;
    std::set<harken::studio::types::ObjectId> effectIds;
    std::size_t nextEffect = 0;
    std::size_t nextParameter = 0;
    for (const auto& bus : buses) {
        if (bus.busId == harken::studio::types::kMasterBusObjectId) {
            if (bus.effectFirst != 0 || bus.effectCount != 0) return kPackInvalid;
            continue;
        }
        if (bus.effectFirst != nextEffect) return kPackInvalid;
        for (std::uint32_t offset = 0; offset < bus.effectCount; ++offset) {
            const auto& effect = effects[nextEffect++];
            const auto* schema = FindPackEffectSchemaByType(effect.effectType);
            if (effect.busId != bus.busId || harken::studio::types::IsNull(effect.effectId) ||
                !effectIds.insert(effect.effectId).second || effect.slot >= kMaxFxSlotsPerBus ||
                (offset != 0 && effects[nextEffect - 2].slot >= effect.slot) ||
                (effect.flags & ~1u) != 0 || !schema ||
                effect.descriptorVintage != schema->descriptorVintage ||
                effect.parameterCount != schema->paramCount ||
                effect.parameterFirst != nextParameter ||
                effect.parameterCount > effectParameters.size() - nextParameter ||
                (header.runtimeCapabilityMask & (UINT64_C(1) << 19)) == 0 ||
                (header.runtimeCapabilityMask & (UINT64_C(1) << (32 + effect.effectType))) == 0)
                return kPackInvalid;
            for (std::uint32_t index = 0; index < effect.parameterCount; ++index) {
                const auto& parameter = effectParameters[nextParameter++];
                if (parameter.index != index || !PackEffectParamInAuthoredRange(
                        effect.effectType, index, parameter.value)) return kPackInvalid;
            }
        }
    }
    if (nextEffect != effects.size() || nextParameter != effectParameters.size()) return kPackInvalid;
    std::set<std::pair<harken::studio::types::ObjectId, harken::studio::types::ObjectId>> sendEdges;
    for (std::uint32_t i = 0; i < header.sendCount; ++i) {
        const auto& row = sends[i];
        if (!ids.contains(row.source) || !ids.contains(row.target) ||
            row.source == row.target ||
            (header.publicationCapabilityVintage >= 4 &&
             row.target == harken::studio::types::kMasterBusObjectId) ||
            (header.publicationCapabilityVintage == 3 ? row.levelDb != 0.0f :
                (!std::isfinite(row.levelDb) || row.levelDb < -96.0f ||
                 row.levelDb > 24.0f ||
                 !std::isfinite(std::pow(10.0f, row.levelDb / 20.0f)))) ||
            !sendEdges.emplace(row.source, row.target).second) {
            return kPackInvalid;
        }
    }
    if (header.publicationCapabilityVintage >= 4 &&
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
    image->busDefinitionEffects = std::move(effects);
    image->busDefinitionEffectParameters = std::move(effectParameters);
    image->busDefinitionDownmixesV2 = std::move(downmixes);
    image->busDefinitionCoefficients = std::move(coefficients);
    return CanonicalVolumeBusFingerprint(*image, image->busDefinitionEffects,
        image->busDefinitionEffectParameters) == header.publishedBusFingerprint &&
        header.contentGeneration == header.publishedBusFingerprint
        ? kPackOk : kPackInvalid;
}

bool CanonicalKindLegal(std::uint32_t entryKind, std::uint32_t packKind,
                        bool sharedResidentOwner) {
    switch (entryKind) {
    case HK_PACK_ENTRY_KIND_MEDIA:
        return packKind == HK_PACK_KIND_CANONICAL_CONTENT;
    case HK_PACK_ENTRY_KIND_BUS:
    case HK_PACK_ENTRY_KIND_BUS_SEND:
    case HK_PACK_ENTRY_KIND_ATTENUATION:
    case HK_PACK_ENTRY_KIND_BUS_EFFECT:
    case HK_PACK_ENTRY_KIND_EFFECT_DYNAMIC_ROUTE:
        return packKind == HK_PACK_KIND_INIT ||
            (packKind == HK_PACK_KIND_CANONICAL_CONTENT && sharedResidentOwner);
    case HK_PACK_ENTRY_KIND_BINDING_DEFINITION:
    case HK_PACK_ENTRY_KIND_BINDING_ATTACHMENT:
        return PackIsCanonicalKind(packKind);
    case HK_PACK_ENTRY_KIND_CANONICAL_DEFINITION_PARTITION:
    case HK_PACK_ENTRY_KIND_PACK_REQUIREMENT_BLOCK:
    case HK_PACK_ENTRY_KIND_PACK_DEPENDENCY_EDGE:
        return PackIsCanonicalKind(packKind);
    case HK_PACK_ENTRY_KIND_EFFECT_PRESET:
        return packKind == HK_PACK_KIND_INIT;
    case HK_PACK_ENTRY_KIND_BUS_DEFINITION_SNAPSHOT:
        return packKind == HK_PACK_KIND_INIT;
    case HK_PACK_ENTRY_KIND_VARIABLE_POLICY_OVERLAY:
        return packKind == HK_PACK_KIND_INIT;
    case HK_PACK_ENTRY_KIND_VARIABLE_REQUIREMENTS:
        return packKind == HK_PACK_KIND_CANONICAL_CONTENT;
    default:
        return false;
    }
}



} // namespace

harken::studio::build::PackResult ValidateCanonicalInitDependency(
    std::span<const PackDependencyEdge> edges,
    std::uint64_t expectedInitPackStableId) noexcept {
    if (expectedInitPackStableId == 0) {
        return harken::studio::build::kPackInvalid;
    }
    bool found = false;
    for (const auto& edge : edges) {
        if (edge.edgeKind != HK_PACK_DEPENDENCY_EDGE_KIND_INIT) {
            continue;
        }
        if (found || edge.required01 != 1 ||
            edge.prerequisitePackStableId != expectedInitPackStableId ||
            edge.expectedPrerequisiteContentRevision == 0 || edge.reserved0 != 0) {
            return harken::studio::build::kPackInvalid;
        }
        found = true;
    }
    return found ? harken::studio::build::kPackOk : harken::studio::build::kPackInvalid;
}

harken::studio::build::PackResult ParsePackCanonicalImage(const std::uint8_t* bytes,
                                 std::uint64_t fileBytes,
                                 PackCanonicalImage* out) {
    if (bytes == nullptr || out == nullptr || fileBytes < kPackBytes<HkPackFileHeader>) {
        return harken::studio::build::kPackInvalid;
    }
    PackFileHeader header;
    if (!ReadPackFileHeader(bytes, &header)) {
        return harken::studio::build::kPackInvalid;
    }
    if (header.formatVersion != HK_PACK_FORMAT_VERSION ||
        header.headerBytes != kPackBytes<HkPackFileHeader> ||
        header.fileBytes != fileBytes) {
        return harken::studio::build::kPackInvalid;
    }
    if (!PackIsCanonicalKind(header.packKind)) {
        return harken::studio::build::kPackInvalid;
    }
    const std::uint64_t dirBytes =
        static_cast<std::uint64_t>(header.entryCount) *
        kPackBytes<HkPackDirectoryEntry>;
    if (header.directoryOffset != header.headerBytes ||
        header.directoryOffset > fileBytes ||
        dirBytes > fileBytes - header.directoryOffset) {
        return harken::studio::build::kPackInvalid;
    }
    const std::uint8_t* dir = bytes + header.directoryOffset;
    if (Fnv1a64(dir, static_cast<std::size_t>(dirBytes)) !=
        header.directoryChecksum) {
        return harken::studio::build::kPackInvalid;
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
            dir + static_cast<std::size_t>(i) * kPackBytes<HkPackDirectoryEntry>);
        if (!PackEntryKindSupported(entry.kind)) return kPackInvalid;
        if (!VintageLegalForKind(entry.kind, entry.vintage)) {
            return harken::studio::build::kPackIncompatible;
        }
        if (entry.offset < dirEnd || entry.offset > fileBytes ||
            entry.size > fileBytes - entry.offset ||
            entry.offset % HK_PACK_PAYLOAD_ALIGN != 0) {
            return harken::studio::build::kPackInvalid;
        }
        ranges.emplace_back(entry.offset, entry.size);
        const std::uint8_t* payload = bytes + entry.offset;
        if (Fnv1a64(payload, static_cast<std::size_t>(entry.size)) !=
            entry.payloadChecksum) {
            return harken::studio::build::kPackInvalid;
        }
        switch (entry.kind) {

        case HK_PACK_ENTRY_KIND_CANONICAL_DEFINITION_PARTITION: {
            if (image.hasPartition) {
                return harken::studio::build::kPackInvalid;
            }
            PackCanonicalPartitionHeader partitionHeader;
            if (entry.size < kPackBytes<HkPackCanonicalPartitionHeader>) {
                return harken::studio::build::kPackIncompatible;
            }
            const bool parsed = ReadPackCanonicalPartitionHeader(payload, entry.size, &partitionHeader);
            if (!parsed) {
                return harken::studio::build::kPackInvalid;
            }
            if (partitionHeader.bodyVintage !=
                kCanonicalDefinitionPartitionBodyVintage) {
                return harken::studio::build::kPackIncompatible;
            }
            CurrentWorldBodyImage body;
            const CurrentWorldBodyDecodeResult decoded = body.DecodeWithResult(
                payload + partitionHeader.headerBytes,
                static_cast<std::size_t>(partitionHeader.bodyBytes),
                kCanonicalDefinitionPartitionBodyVintage);
            if (decoded != CurrentWorldBodyDecodeResult::Ok) {
                return decoded == CurrentWorldBodyDecodeResult::Capacity
                           ? harken::studio::build::kPackResidentCapacity
                           : harken::studio::build::kPackInvalid;
            }
            const WorldInstallStatus validation =
                detail::ValidateCurrentWorldPartitionLocalV1(*body.ProductionView());
            if (validation == WorldInstallStatus::InvalidFormat ||
                validation == WorldInstallStatus::CapabilityUnavailable) {
                return harken::studio::build::kPackIncompatible;
            }
            if (validation == WorldInstallStatus::Capacity) {
                return harken::studio::build::kPackResidentCapacity;
            }
            if (validation != WorldInstallStatus::Ok) {
                return harken::studio::build::kPackInvalid;
            }
            const ContainerWorldInstallViewV1 decodedView = *body.ProductionView();
            const std::uint64_t decodedOwnedDefinitions =
                static_cast<std::uint64_t>(decodedView.variableCount) +
                decodedView.playablePropertyCount + decodedView.actionCount;
            if (partitionHeader.ownedDefinitionCount !=
                decodedOwnedDefinitions) {
                return harken::studio::build::kPackInvalid;
            }
            image.partitionHeader = partitionHeader;
            image.partitionBody.assign(
                payload + partitionHeader.headerBytes,
                payload + partitionHeader.headerBytes +
                    static_cast<std::size_t>(partitionHeader.bodyBytes));
            image.hasPartition = true;
            break;
        }
        case HK_PACK_ENTRY_KIND_PACK_REQUIREMENT_BLOCK: {
            if (image.hasRequirement) {
                return harken::studio::build::kPackInvalid;
            }
            PackRequirementBlock requirement;
            if (entry.size != kPackBytes<HkPackRequirementBlock>) {
                return harken::studio::build::kPackIncompatible;
            }
            if (!ParsePackRow<HkPackRequirementBlock>(payload, entry.size, &requirement)) {
                return harken::studio::build::kPackInvalid;
            }
            if (requirement.schemaVersion != entry.vintage ||
                requirement.storageModelVersion != entry.vintage) {
                return harken::studio::build::kPackIncompatible;
            }
            if (!PackRequirementBlockValid(requirement) ||
                requirement.payloadChecksumCoverage != Fnv1a64(payload, offsetof(HkPackRequirementBlock, payloadChecksumCoverage))) {
                return harken::studio::build::kPackInvalid;
            }
            image.requirement = requirement;
            image.packStableId = requirement.packStableId;
            image.hasRequirement = true;
            break;
        }
        case HK_PACK_ENTRY_KIND_PACK_DEPENDENCY_EDGE: {
            PackDependencyEdge edge;
            if (entry.size != kPackBytes<HkPackDependencyEdge>) {
                return harken::studio::build::kPackIncompatible;
            }
            if (!ReadPackDependencyEdge(payload, entry.size, &edge) ||
                entry.relationOrdinal != dependencyOrdinal) {
                return harken::studio::build::kPackInvalid;
            }
            const bool initRequired =
                edge.edgeKind == HK_PACK_DEPENDENCY_EDGE_KIND_INIT &&
                edge.required01 == 1;
            if (!initRequired &&
                (edge.expectedBusContentGeneration != 0 ||
                 edge.expectedPublishedBusFingerprint != 0 ||
                 edge.expectedCompileTargetProfileFingerprint != 0)) {
                return harken::studio::build::kPackInvalid;
            }
            ++dependencyOrdinal;
            image.dependencyEdges.push_back(edge);
            break;
        }
        case HK_PACK_ENTRY_KIND_BUS_DEFINITION_SNAPSHOT: {
            if (image.hasBusDefinitionSnapshot) {
                return harken::studio::build::kPackInvalid;
            }
            if (entry.id != harken::studio::types::ObjectId{} || entry.relationOrdinal != 1) {
                return harken::studio::build::kPackInvalid;
            }
            const harken::studio::build::PackResult parsed =
                ParseBusDefinitionSnapshotPayload(payload, entry.size, &image);
            if (parsed != harken::studio::build::kPackOk) {
                return parsed;
            }
            break;
        }
        case HK_PACK_ENTRY_KIND_VARIABLE_POLICY_OVERLAY: {
            if (image.hasVariablePolicyOverlay) {
                return harken::studio::build::kPackInvalid;
            }
            const harken::studio::build::PackResult parsed = ParseVariablePolicyOverlayPayload(
                payload, entry.size, &image.variablePolicyOverlays);
            if (parsed != harken::studio::build::kPackOk) {
                return parsed;
            }
            image.hasVariablePolicyOverlay = true;
            break;
        }
        case HK_PACK_ENTRY_KIND_VARIABLE_REQUIREMENTS: {
            if (image.hasVariableRequirements) {
                return harken::studio::build::kPackInvalid;
            }
            const harken::studio::build::PackResult parsed = ParseVariableRequirementsPayload(
                payload, entry.size, &image.variableRequirements);
            if (parsed != harken::studio::build::kPackOk) {
                return parsed;
            }
            image.hasVariableRequirements = true;
            break;
        }
        case HK_PACK_ENTRY_KIND_MEDIA: {
            if (entry.size != kPackBytes<HkPackMedia>) {
                return harken::studio::build::kPackInvalid;
            }
            const PackTocEntry media = ReadPackMedia(payload);
            if (media.soundId != entry.id ||
                !ValidateV2MediaEntry(media, fileBytes, dirEnd)) {
                return harken::studio::build::kPackInvalid;
            }
            if (Fnv1a64(bytes + media.blobOffset,
                        static_cast<std::size_t>(media.blobBytes)) !=
                media.blobChecksum) {
                return harken::studio::build::kPackInvalid;
            }
            ranges.emplace_back(media.blobOffset, media.blobBytes);
            image.media.push_back(media);
            break;
        }
        case HK_PACK_ENTRY_KIND_BUS_EFFECT: {
            PackBusEffectEntry effect;
            if (!ReadPackBusEffect(payload, entry.size, &effect) ||
                entry.relationOrdinal != image.busEffects.size() || effect.busId == harken::studio::types::ObjectId{} ||
                effect.bypass > 1 || effect.slot >= kMaxFxSlotsPerBus ||
                effect.faderPointGuard > kMaxFxSlotsPerBus) {
                return harken::studio::build::kPackInvalid;
            }
            const PackEffectSchemaRow* schema = FindPackEffectSchemaByType(effect.effectType);
            if (schema == nullptr || effect.descriptorVintage != schema->descriptorVintage ||
                effect.paramCount != schema->paramCount) {
                return harken::studio::build::kPackInvalid;
            }
            for (std::uint32_t p = 0; p < effect.paramCount; ++p) {
                if (!std::isfinite(effect.params[p])) {
                    return harken::studio::build::kPackInvalid;
                }
            }
            image.busEffects.push_back(effect);
            break;
        }
        case HK_PACK_ENTRY_KIND_EFFECT_DYNAMIC_ROUTE: {
            PackEffectDynamicRouteEntry route;
            if (!ParsePackRow<HkPackEffectDynamicRoute>(payload, entry.size, &route) ||
                entry.relationOrdinal != image.dynamicRoutes.size() || route.targetBusId == harken::studio::types::ObjectId{} ||
                route.sourceId == harken::studio::types::ObjectId{} || route.expectedEffectType != harken::studio::types::kEffectCompressor ||
                route.signalForm != harken::studio::types::kDynamicSignalRawAudio ||
                route.sourceKind != harken::studio::types::kDynamicSourceBus ||
                route.tap > harken::studio::types::kDynamicTapBusPostFader ||
                route.effectSlot >= kMaxFxSlotsPerBus || route.routeSlot >= 4u ||
                !std::isfinite(route.levelDb) || route.levelDb < -96.0f || route.levelDb > 24.0f) {
                return harken::studio::build::kPackInvalid;
            }
            image.dynamicRoutes.push_back(route);
            break;
        }
        case HK_PACK_ENTRY_KIND_BUS: {
            PackBusEntry bus;
            const bool parsed =
                ParsePackRow<HkPackBus>(payload, entry.size, &bus);
            if (!parsed || bus.busId != entry.id ||
                !std::isfinite(bus.volume) ||
                !std::isfinite(bus.outputBusVolume)) {
                return harken::studio::build::kPackInvalid;
            }
            image.buses.push_back(bus);
            break;
        }
        case HK_PACK_ENTRY_KIND_BUS_SEND: {
            PackBusSendEntry send;
            if (!ParsePackRow<HkPackBusSend>(payload, entry.size, &send) ||
                entry.relationOrdinal != sendOrdinal || !std::isfinite(send.levelDb)) {
                return harken::studio::build::kPackInvalid;
            }
            ++sendOrdinal;
            image.busSends.push_back(send);
            break;
        }
        case HK_PACK_ENTRY_KIND_EFFECT_PRESET: {
            PackEffectPreset preset;
            if (header.packKind != HK_PACK_KIND_INIT ||
                ParseEffectPresetPayload(payload, entry.size, preset) != kPackOk ||
                preset.header.presetId != entry.id || entry.relationOrdinal != 0 ||
                std::any_of(image.effectPresets.begin(), image.effectPresets.end(),
                    [&](const auto& existing) { return existing.header.presetId == entry.id; }))
                return kPackInvalid;
            image.effectPresets.push_back(std::move(preset));
            break;
        }
        case HK_PACK_ENTRY_KIND_ATTENUATION: {
            PackLogicImage::Attenuation attenuation;
            attenuation.id = entry.id;
            if (!ReadPackAttenuation(payload, entry.size,
                                        &attenuation.desc)) {
                return harken::studio::build::kPackInvalid;
            }
            image.attenuations.push_back(attenuation);
            break;
        }
        case HK_PACK_ENTRY_KIND_BINDING_DEFINITION: {
            PackLogicImage::BindingDefinition definition;
            if (!ReadPackBindingDefinition(payload, entry.size,
                                              &definition.id,
                                              &definition.desc) ||
                definition.id != entry.id ||
                !IsValidBindingTransitionMs(definition.desc.transitionMs)) {
                return harken::studio::build::kPackInvalid;
            }
            if (definition.desc.targetScopeKind >
                harken::studio::types::kTargetScopeBusEffectParam) {
                return harken::studio::build::kPackInvalid;
            }
            if (header.packKind == HK_PACK_KIND_INIT &&
                (definition.desc.targetScopeKind == harken::studio::types::kTargetScopeVoice ||
                 definition.desc.targetScopeKind ==
                     harken::studio::types::kTargetScopeVoiceEffectParam)) {
                return harken::studio::build::kPackInvalid;
            }
            image.bindingDefinitions.push_back(definition);
            break;
        }
        case HK_PACK_ENTRY_KIND_BINDING_ATTACHMENT: {
            PackBindingAttachmentEntry attachment;
            if (!ParsePackRow<HkPackBindingAttachment>(payload, entry.size,
                                              &attachment) ||
                entry.relationOrdinal != attachmentOrdinal) {
                return harken::studio::build::kPackInvalid;
            }
            ++attachmentOrdinal;
            if (attachment.scopeKind ==
                    harken::studio::types::kTargetScopeVoiceEffectParam ||
                attachment.scopeKind >
                    harken::studio::types::kTargetScopeBusEffectParam) {
                return harken::studio::build::kPackInvalid;
            }
            if (header.packKind == HK_PACK_KIND_INIT &&
                attachment.scopeKind != harken::studio::types::kTargetScopeBus &&
                attachment.scopeKind !=
                    harken::studio::types::kTargetScopeBusEffectParam) {
                return harken::studio::build::kPackInvalid;
            }
            image.bindingAttachments.push_back(attachment);
            break;
        }
        case HK_PACK_ENTRY_KIND_CONTAINER:
        case HK_PACK_ENTRY_KIND_VOICE_RECIPE:
            return harken::studio::build::kPackIncompatible;
        default:
            return harken::studio::build::kPackInvalid;
        }
    }

    if (!image.hasRequirement) {
        return harken::studio::build::kPackInvalid;
    }
    const std::uint32_t maxBindingScope = harken::studio::types::kTargetScopeBusEffectParam;
    for (const auto& definition : image.bindingDefinitions) {
        if (definition.desc.targetScopeKind > maxBindingScope) {
            return harken::studio::build::kPackInvalid;
        }
        if (header.packKind == HK_PACK_KIND_INIT &&
            (definition.desc.targetScopeKind == harken::studio::types::kTargetScopeVoice ||
             definition.desc.targetScopeKind ==
                 harken::studio::types::kTargetScopeVoiceEffectParam)) {
            return harken::studio::build::kPackInvalid;
        }
    }
    for (const auto& attachment : image.bindingAttachments) {
        if (attachment.scopeKind > maxBindingScope ||
            attachment.scopeKind == harken::studio::types::kTargetScopeVoiceEffectParam) {
            return harken::studio::build::kPackInvalid;
        }
        if (header.packKind == HK_PACK_KIND_INIT &&
            attachment.scopeKind != harken::studio::types::kTargetScopeBus &&
            attachment.scopeKind != harken::studio::types::kTargetScopeBusEffectParam) {
            return harken::studio::build::kPackInvalid;
        }
    }
    const bool sharedResidentOwner =
        header.packKind == HK_PACK_KIND_CANONICAL_CONTENT &&
        (!image.buses.empty() || !image.busSends.empty() ||
         !image.attenuations.empty() || !image.busEffects.empty() ||
         !image.dynamicRoutes.empty());
    for (std::uint32_t i = 0; i < header.entryCount; ++i) {
        const PackDirectoryEntry entry = ReadPackDirectoryEntry(
            dir + static_cast<std::size_t>(i) * kPackBytes<HkPackDirectoryEntry>);
        if (!CanonicalKindLegal(entry.kind, header.packKind,
                                sharedResidentOwner)) {
            return harken::studio::build::kPackInvalid;
        }
    }
    if (header.packKind == HK_PACK_KIND_CANONICAL_CONTENT &&
        image.hasBusDefinitionSnapshot) {
        return harken::studio::build::kPackInvalid;
    }
    if (image.hasPartition &&
        (image.requirement.definitionPartitionRevision == 0 ||
         image.partitionHeader.packStableId != image.packStableId ||
         image.partitionHeader.partitionRevision !=
             image.requirement.definitionPartitionRevision)) {
        return harken::studio::build::kPackInvalid;
    }
    if (!image.hasPartition &&
        image.requirement.definitionPartitionRevision != 0) {
        return harken::studio::build::kPackInvalid;
    }
    const CanonicalBusTriple requirementT = RequirementBusTriple(image.requirement);
    if (image.hasBusDefinitionSnapshot) {
        const CanonicalBusTriple snapshotT =
            SnapshotBusTriple(image.busDefinitionSnapshotHeader);
        if (!CanonicalBusTripleFullyNonzero(snapshotT) ||
            !CanonicalBusTripleEqual(snapshotT, requirementT)) {
            return harken::studio::build::kPackInvalid;
        }
    }
    if (image.hasPartition &&
        !CanonicalBusTripleEqual(PartitionBusTriple(image.partitionHeader),
                                 requirementT)) {
        return harken::studio::build::kPackInvalid;
    }
    for (const PackDependencyEdge& edge : image.dependencyEdges) {
        const bool initRequired =
            edge.edgeKind == HK_PACK_DEPENDENCY_EDGE_KIND_INIT && edge.required01 == 1;
        if (initRequired &&
            !CanonicalBusTripleEqual(DependencyBusTriple(edge), requirementT)) {
            return harken::studio::build::kPackInvalid;
        }
    }
    *out = std::move(image);
    return harken::studio::build::kPackOk;
}

harken::studio::build::PackResult CheckPackClosure(const PackLogicImage& image,
                          const PackClosureResolver& resolver,
                          PackClosureRef* outRefClass,
                          harken::studio::types::ObjectId* outReferrerId,
                          harken::studio::types::ObjectId* outMissingId) {
    const auto fail = [&](PackClosureRef refClass, harken::studio::types::ObjectId referrer,
                          harken::studio::types::ObjectId missing) {
        if (outRefClass != nullptr) {
            *outRefClass = refClass;
        }
        if (outReferrerId != nullptr) {
            *outReferrerId = referrer;
        }
        if (outMissingId != nullptr) {
            *outMissingId = missing;
        }
        return harken::studio::build::kPackUnknownId;
    };

    const auto packPlayable = [&](harken::studio::types::ObjectId id) {
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
    const auto playableCloses = [&](harken::studio::types::ObjectId id) {
        return packPlayable(id) ||
               (resolver.playableExists != nullptr &&
                resolver.playableExists(resolver.ctx, id));
    };
    const auto busCloses = [&](harken::studio::types::ObjectId id) {
        for (const PackBusEntry& bus : image.buses) {
            if (bus.busId == id) {
                return true;
            }
        }
        return resolver.busExists != nullptr &&
               resolver.busExists(resolver.ctx, id);
    };
    const auto bindingCloses = [&](harken::studio::types::ObjectId id) {
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
        if (attachment.scopeKind == harken::studio::types::kTargetScopeBus ||
            attachment.scopeKind ==
                harken::studio::types::kTargetScopeBusEffectParam) {
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
        for (harken::studio::types::ObjectId bindingId : recipe.bindings) {
            if (!bindingCloses(bindingId)) {
                return fail(PackClosureRef::RecipeBinding,
                            recipe.header.playableId, bindingId);
            }
        }
    }
    return harken::studio::build::kPackOk;
}

} // namespace harken::studio::build
