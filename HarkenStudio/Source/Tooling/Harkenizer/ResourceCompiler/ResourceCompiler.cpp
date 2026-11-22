#include "ResourceCompiler.h"
#include "JsonReader.h"
#include <harken_studio_build/AuthoredParameters.h>
#include "Manifest.h"

#include "harken_studio_build/PackFormat.h"
#include "harken_studio_build/PackFormatV2.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace harkenizer::resourcecompiler {
namespace {

using harken::studio::pack::Diag;
using harken::studio::pack::Fail;
using harken::studio::pack::Manifest;
using harken::studio::pack::ManifestAttachment;
using harken::studio::pack::ManifestAttenuation;
using harken::studio::pack::ManifestBinding;
using harken::studio::pack::ManifestBus;
using harken::studio::pack::ManifestBusEffect;
using harken::studio::pack::ManifestBusSend;
using harken::studio::pack::ManifestContainer;
using harken::studio::pack::ManifestCurve;
using harken::studio::pack::ManifestDynamicRoute;
using harken::studio::pack::ManifestVoiceRecipe;

struct PackEntry {
    std::string path;
    std::shared_ptr<const harken::studio::VorbisEncoder::EncodedMedia> encodedMedia;
    std::uint32_t sampleRate = 0;
    std::uint16_t channelCount = 0;
    std::uint64_t frameCount = 0;
    harken::studio::types::ObjectId soundId{};
    bool loopable = false;
    std::uint64_t loopStartFrame = 0;
    std::uint64_t loopEndFrame = 0; // 0 = whole file
    bool downmixToMono = false;
};

bool PrepareAudioLayout(PackEntry* entry, const float* samples, std::size_t sampleCount,
                        std::uint16_t channels, std::uint32_t sampleRate, Diag* diag) {
    if (!samples || sampleCount == 0 || channels == 0 || sampleRate == 0
        || sampleCount % channels != 0) {
        return Fail(diag, "invalid prepared audio for '%s'", entry->path.c_str());
    }
    if (!entry->downmixToMono && channels != 1 && channels != 2) {
        return Fail(diag, "'%s' has %u channels; only mono and stereo are supported",
                    entry->path.c_str(), channels);
    }
    entry->sampleRate = sampleRate;
    entry->frameCount = sampleCount / channels;
    if (entry->downmixToMono && channels > 2) {
        return Fail(diag,
                    "cannot downmix '%s' to mono: source has %u "
                    "channels; only mono or stereo is supported",
                    entry->path.c_str(), channels);
    }

    entry->channelCount = entry->downmixToMono ? 1 : channels;
    return true;
}

bool ValidateLoopPoints(const PackEntry& entry, std::size_t entryIndex,
                        Diag* diag) {
    if (!entry.loopable) {
        return true;
    }
    const std::uint64_t loopEnd =
        entry.loopEndFrame == 0 ? entry.frameCount : entry.loopEndFrame;
    if (loopEnd > entry.frameCount ||
        entry.loopStartFrame >= loopEnd) {
        return Fail(diag,
                    "sounds[%zu] '%s' has invalid loop points: need "
                    "loopStartFrame < loopEndFrame <= frameCount (%llu), "
                    "got start %llu end %llu",
                    entryIndex, entry.path.c_str(),
                    static_cast<unsigned long long>(entry.frameCount),
                    static_cast<unsigned long long>(
                        entry.loopStartFrame),
                    static_cast<unsigned long long>(entry.loopEndFrame));
    }
    return true;
}

harken::studio::build::AttenuationCurve ToAttenuationCurve(const ManifestCurve& curve,
                                      bool enabled) {
    harken::studio::build::AttenuationCurve out{};
    if (!enabled) {
        return out; // enabled 0, every slot zero — the wire form
    }
    out.enabled = 1;
    out.pointCount = curve.pointCount;
    for (std::uint32_t i = 0; i < curve.pointCount; ++i) {
        out.points[i].x = curve.pointX[i];
        out.points[i].y = curve.pointY[i];
    }
    for (std::uint32_t i = 0; i + 1 < curve.pointCount; ++i) {
        out.handles[i] = curve.handles[i];
    }
    return out;
}

harken::studio::build::BindingCurve ToBindingCurve(const ManifestCurve& curve) {
    harken::studio::build::BindingCurve out{};

    out.pointCount = curve.pointCount;
    for (std::uint32_t i = 0; i < curve.pointCount; ++i) {
        out.points[i].x = curve.pointX[i];
        out.points[i].y = curve.pointY[i];
    }
    for (std::uint32_t i = 0; i + 1 < curve.pointCount; ++i) {
        out.handles[i] = curve.handles[i];
    }
    return out;
}

struct EntryPlacement {
    std::uint32_t kind = 0;
    harken::studio::types::ObjectId id{};
    std::size_t index = 0; // position within its manifest section
    std::uint64_t size = 0;
};

std::vector<EntryPlacement> BuildEntryPlacements(
    const Manifest& manifest, const std::vector<PackEntry>& entries) {
    std::vector<EntryPlacement> placements;
    const auto place = [&placements](std::uint32_t kind,
                                     harken::studio::types::ObjectId id, std::size_t index,
                                     std::uint64_t size) {
        EntryPlacement placement;
        placement.kind = kind;
        placement.id = id;
        placement.index = index;
        placement.size = size;
        placements.push_back(placement);
    };
    for (std::size_t i = 0; i < entries.size(); ++i) {
        place(HK_PACK_ENTRY_KIND_MEDIA, entries[i].soundId, i,
              harken::studio::build::kPackBytes<HkPackMedia>);
    }
    for (std::size_t i = 0; i < manifest.buses.size(); ++i) {
        place(HK_PACK_ENTRY_KIND_BUS, manifest.buses[i].id, i,
              harken::studio::build::kPackBytes<HkPackBus>);
    }
    for (std::size_t i = 0; i < manifest.busSends.size(); ++i) {
        place(HK_PACK_ENTRY_KIND_BUS_SEND, {}, i,
              harken::studio::build::kPackBytes<HkPackBusSend>);
    }
    for (std::size_t i = 0; i < manifest.attenuations.size(); ++i) {
        place(HK_PACK_ENTRY_KIND_ATTENUATION,
              manifest.attenuations[i].id, i,
              harken::studio::build::kPackBytes<HkPackAttenuation>);
    }
    for (std::size_t i = 0; i < manifest.bindings.size(); ++i) {
        place(HK_PACK_ENTRY_KIND_BINDING_DEFINITION,
              manifest.bindings[i].id, i,
              harken::studio::build::kPackBytes<HkPackBindingDefinition>);
    }
    for (std::size_t i = 0; i < manifest.attachments.size(); ++i) {
        place(HK_PACK_ENTRY_KIND_BINDING_ATTACHMENT, {}, i,
              harken::studio::build::kPackBytes<HkPackBindingAttachment>);
    }
    for (std::size_t i = 0; i < manifest.containers.size(); ++i) {
        place(HK_PACK_ENTRY_KIND_CONTAINER, manifest.containers[i].id,
              i,
              harken::studio::build::PackContainerPayloadBytes(
                  static_cast<std::uint32_t>(
                      manifest.containers[i].children.size())));
    }
    for (std::size_t i = 0; i < manifest.voiceRecipes.size(); ++i) {
        const ManifestVoiceRecipe& recipe = manifest.voiceRecipes[i];
        const std::uint32_t installCount =
            static_cast<std::uint32_t>(recipe.effects.size());
        const std::uint32_t bindingCount =
            static_cast<std::uint32_t>(recipe.bindings.size());
        place(HK_PACK_ENTRY_KIND_VOICE_RECIPE, recipe.playable, i,
              harken::studio::build::PackVoiceRecipePayloadBytes(installCount,
                                                  bindingCount));
    }
    for (std::size_t i = 0; i < manifest.busEffects.size(); ++i) {
        place(HK_PACK_ENTRY_KIND_BUS_EFFECT, {}, i,
              harken::studio::build::kPackBytes<HkPackBusEffect>);
    }
    for (std::size_t i = 0; i < manifest.dynamicRoutes.size(); ++i) {
        place(HK_PACK_ENTRY_KIND_EFFECT_DYNAMIC_ROUTE, {}, i,
              harken::studio::build::kPackBytes<HkPackEffectDynamicRoute>);
    }

    return placements;
}

bool SerializeEntryPayload(const Manifest& manifest,
                           const std::vector<PackEntry>& entries,
                           const EntryPlacement& placement, std::uint8_t* dst, Diag* diag,
                           std::uint64_t blobChecksum = 0) {
    switch (placement.kind) {
    case HK_PACK_ENTRY_KIND_MEDIA: {
        const PackEntry& entry = entries[placement.index];
        if (entry.encodedMedia)
            return Fail(diag, "Vorbis Pack writing is not integrated");
        const std::uint64_t blobBytes =
            entry.frameCount * entry.channelCount * sizeof(float);
        harken::studio::build::PackTocEntry toc;
        toc.soundId = entry.soundId;
        toc.sampleRate = entry.sampleRate;
        toc.channelCount = entry.channelCount;
        toc.sampleFormat = HK_PACK_SAMPLE_FORMAT_F32_PLANAR;
        toc.frameCount = entry.frameCount;
        toc.blobOffset = 0;
        toc.blobBytes = blobBytes;
        toc.blobChecksum = blobChecksum;
        toc.flags = entry.loopable ? HK_PACK_MEDIA_FLAG_LOOPABLE : 0;
        toc.loopStartFrame =
            entry.loopable ? entry.loopStartFrame : 0;
        toc.loopEndFrame = entry.loopable ? entry.loopEndFrame : 0;
        harken::studio::build::SerializePackRow<HkPackMedia>(toc, dst);
        break;
    }
    case HK_PACK_ENTRY_KIND_BUS: {
        const ManifestBus& bus = manifest.buses[placement.index];
        harken::studio::build::PackBusEntry wire;
        wire.busId = bus.id;
        wire.parentBusId = bus.parent;
        wire.volume = bus.volume;
        wire.outputBusVolume = bus.outputBusVolume;
        harken::studio::build::SerializePackRow<HkPackBus>(wire, dst);
        break;
    }
    case HK_PACK_ENTRY_KIND_BUS_SEND: {
        const ManifestBusSend& send =
            manifest.busSends[placement.index];
        harken::studio::build::PackBusSendEntry wire;
        wire.fromBusId = send.from;
        wire.toBusId = send.to;
        wire.levelDb = send.levelDb;
        harken::studio::build::SerializePackRow<HkPackBusSend>(wire, dst);
        break;
    }
    case HK_PACK_ENTRY_KIND_ATTENUATION: {
        const ManifestAttenuation& attenuation =
            manifest.attenuations[placement.index];
        harken::studio::build::AttenuationDefinition desc{};

        desc.rolloff = attenuation.rolloff;
        desc.minDistance = attenuation.minDistance;
        desc.maxDistance = attenuation.maxDistance;
        desc.airAbsorptionNearHz = attenuation.airAbsorptionNearHz;
        desc.airAbsorptionFarHz = attenuation.airAbsorptionFarHz;
        desc.proximityRadius = attenuation.proximityRadius;
        desc.proximityMaxBoostDb = attenuation.proximityMaxBoostDb;
        desc.proximityFrequency = attenuation.proximityFrequency;
        desc.coneEnabled = attenuation.coneEnabled ? 1u : 0u;
        desc.coneInnerDeg = attenuation.coneInnerDeg;
        desc.coneOuterDeg = attenuation.coneOuterDeg;
        desc.coneOuterGainDb = attenuation.coneOuterGainDb;
        desc.coneOuterLowpassHz = attenuation.coneOuterLowpassHz;
        desc.volumeCurve = ToAttenuationCurve(
            attenuation.volumeCurve, attenuation.hasVolumeCurve);
        desc.lowpassCurve = ToAttenuationCurve(
            attenuation.lowpassCurve, attenuation.hasLowpassCurve);
        desc.widthCurve = ToAttenuationCurve(
            attenuation.widthCurve, attenuation.hasWidthCurve);
        harken::studio::build::SerializePackRow<HkPackAttenuation>(desc, dst);
        break;
    }
    case HK_PACK_ENTRY_KIND_BINDING_DEFINITION: {
        const ManifestBinding& binding =
            manifest.bindings[placement.index];
        harken::studio::build::BindingDefinition desc{};

        desc.variableId = binding.variable;
        desc.targetScopeKind = binding.scope;
        desc.targetId = binding.target;
        desc.transitionMs = binding.transitionMs;
        desc.curve = ToBindingCurve(binding.curve);
        harken::studio::build::WritePackBindingDefinition(binding.id, desc,
                                                dst);
        break;
    }
    case HK_PACK_ENTRY_KIND_BINDING_ATTACHMENT: {
        const ManifestAttachment& attachment =
            manifest.attachments[placement.index];
        harken::studio::build::PackBindingAttachmentEntry wire;
        wire.bindingId = attachment.binding;
        wire.scopeKind = attachment.scope;
        wire.objectId = attachment.object;
        harken::studio::build::SerializePackRow<HkPackBindingAttachment>(wire, dst);
        break;
    }
    case HK_PACK_ENTRY_KIND_CONTAINER: {
        const ManifestContainer& container =
            manifest.containers[placement.index];
        harken::studio::build::PackContainerEntryHeader header;
        header.containerId = container.id;
        header.selectionOrder = container.order;
        header.avoidRepeatWindow = container.avoidRepeatWindow;
        header.selectionVariableId = container.selectionVariable;
        header.defaultIndex = container.defaultIndex;
        header.childCount = static_cast<std::uint32_t>(
            container.children.size());
        struct Visitor {
            harken::studio::build::PackContainerEntryHeader& header;
            const ManifestContainer& source;
            const auto& Value() const noexcept { return header; }
            const auto& Header() const noexcept { return header; }
            bool PrepareChildren(std::uint64_t count) const noexcept { return count == source.children.size(); }
            harken::studio::build::PackContainerChild Children(std::uint64_t index) const noexcept {
                const auto& child = source.children[static_cast<std::size_t>(index)];
                return {child.playable, child.weight, child.matchValue, child.delayMs, child.delayJitterMs};
            }
            bool Finish() const noexcept { return true; }
        } visitor{header, container};
        harken::studio::build::PackWireBuffer<std::uint8_t> buffer{dst, placement.size};
        if (!harken::studio::build::AppendPackContainerPayload(buffer, visitor))
            return Fail(diag, "container payload size mismatch");
        break;
    }
    case HK_PACK_ENTRY_KIND_VOICE_RECIPE: {
        const ManifestVoiceRecipe& recipe =
            manifest.voiceRecipes[placement.index];
        harken::studio::build::PackVoiceRecipeEntryHeader header;
        header.playableId = recipe.playable;
        header.installCount =
            static_cast<std::uint32_t>(recipe.effects.size());
        header.bindingCount =
            static_cast<std::uint32_t>(recipe.bindings.size());
        harken::studio::build::PackVoiceRecipeTone tone;
        if (recipe.tone.present) {
            tone.present = 1u;
            tone.sectionEnableMask =
                (recipe.tone.lowpassEnabled
                     ? HK_PACK_VOICE_TONE_LOWPASS
                     : 0u) |
                (recipe.tone.highpassEnabled
                     ? HK_PACK_VOICE_TONE_HIGHPASS
                     : 0u) |
                (recipe.tone.lowShelfEnabled
                     ? HK_PACK_VOICE_TONE_LOW_SHELF
                     : 0u) |
                (recipe.tone.highShelfEnabled
                     ? HK_PACK_VOICE_TONE_HIGH_SHELF
                     : 0u);
            tone.lowpassFrequencyHz = recipe.tone.lowpassFrequencyHz;
            tone.lowpassQ = recipe.tone.lowpassQ;
            tone.highpassFrequencyHz = recipe.tone.highpassFrequencyHz;
            tone.highpassQ = recipe.tone.highpassQ;
            tone.lowShelfFrequencyHz = recipe.tone.lowShelfFrequencyHz;
            tone.lowShelfQ = recipe.tone.lowShelfQ;
            tone.lowShelfGainDb = recipe.tone.lowShelfGainDb;
            tone.highShelfFrequencyHz =
                recipe.tone.highShelfFrequencyHz;
            tone.highShelfQ = recipe.tone.highShelfQ;
            tone.highShelfGainDb = recipe.tone.highShelfGainDb;
        }
        harken::studio::build::PackVoiceRecipeSpatial spatial;
        if (recipe.spatial.present) {
            spatial.attenuationId = recipe.spatial.attenuation;
            spatial.distanceScale = recipe.spatial.distanceScale;
        } else {
            spatial.distanceScale = 0.0f;
        }
        harken::studio::build::PackVoiceRecipePlayback playback;
        if (recipe.playback.present) {
            playback.present = 1u;
            playback.loopCount = recipe.playback.loopCount;
        }
        struct InstallVisitor {
            harken::studio::build::PackVoiceRecipeInstallWire row;
            const auto& Wire() const noexcept { return row; }
            bool Finish() const noexcept { return true; }
        };
        struct Visitor {
            const harken::studio::build::PackVoiceRecipeEntryHeader& header;
            const harken::studio::build::PackVoiceRecipeTone& tone;
            const harken::studio::build::PackVoiceRecipeSpatial& spatial;
            const harken::studio::build::PackVoiceRecipePlayback& playback;
            const ManifestVoiceRecipe& source;
            const auto& Value() const noexcept { return header; }
            const auto& Header() const noexcept { return header; }
            const auto& Tone() const noexcept { return tone; }
            const auto& Spatial() const noexcept { return spatial; }
            const auto& Playback() const noexcept { return playback; }
            bool PrepareInstalls(std::uint64_t count) const noexcept { return count == source.effects.size(); }
            InstallVisitor Installs(std::uint64_t index) const noexcept {
                const auto& effect = source.effects[static_cast<std::size_t>(index)];
                InstallVisitor result{};
                auto& row = result.row;
                row.effectId = effect.effectId;
                row.slot = effect.slot;
                row.effectType = effect.type;
                row.descriptorVintage = effect.descriptorVintage;
                row.bypass = effect.bypass ? 1u : 0u;
                row.paramCount = static_cast<std::uint32_t>(effect.parameters.size());
                row.paramBytes = row.paramCount * sizeof(float);
                std::copy(effect.parameters.begin(), effect.parameters.end(), row.params.begin());
                return result;
            }
            bool PrepareBindings(std::uint64_t count) const noexcept { return count == source.bindings.size(); }
            const auto& Bindings(std::uint64_t index) const noexcept { return source.bindings[static_cast<std::size_t>(index)]; }
            bool Finish() const noexcept { return true; }
        } visitor{header, tone, spatial, playback, recipe};
        harken::studio::build::PackWireBuffer<std::uint8_t> buffer{dst, placement.size};
        if (!harken::studio::build::AppendPackVoiceRecipePayload(buffer, visitor))
            return Fail(diag, "voice recipe payload size mismatch");
        break;
    }
    case HK_PACK_ENTRY_KIND_BUS_EFFECT: {
        const ManifestBusEffect& effect =
            manifest.busEffects[placement.index];
        harken::studio::build::PackBusEffectEntry wire;
        wire.effectId = effect.effectId;
        wire.busId = effect.bus;
        wire.slot = effect.slot;
        wire.effectType = effect.type;
        wire.descriptorVintage = effect.descriptorVintage;
        wire.bypass = effect.bypass ? 1u : 0u;
        wire.faderPointGuard = effect.faderPointGuard;
        wire.paramCount =
            static_cast<std::uint32_t>(effect.parameters.size());
        wire.paramBytes = wire.paramCount * sizeof(float);
        for (std::uint32_t p = 0; p < wire.paramCount; ++p) {
            wire.params[p] = effect.parameters[p];
        }
        harken::studio::build::SerializePackRow<HkPackBusEffect>(wire, dst);
        break;
    }
    case HK_PACK_ENTRY_KIND_EFFECT_DYNAMIC_ROUTE: {
        const ManifestDynamicRoute& route =
            manifest.dynamicRoutes[placement.index];
        harken::studio::build::PackEffectDynamicRouteEntry wire;
        wire.targetBusId = route.targetBus;
        wire.effectSlot = route.effectSlot;
        wire.routeSlot = route.routeSlot;
        wire.expectedEffectType = route.expectedEffectType;
        wire.signalForm = route.signalForm;
        wire.sourceKind = route.sourceKind;
        wire.tap = route.tap;
        wire.sourceId = route.sourceBus;
        wire.levelDb = route.levelDb;
        harken::studio::build::SerializePackRow<HkPackEffectDynamicRoute>(wire, dst);
        break;
    }
    default:
        return Fail(diag, "unsupported v2 entry kind %u",
                    placement.kind);
    }
    return true;
}


}

PreparedMediaBlob MediaCache::prepare(const MediaInput& input)
{
    std::lock_guard lock(mutex_);
    const auto key = std::make_tuple(input.contentHash, input.channels, input.downmixToMono);
    if (!input.contentHash.empty()) {
        const auto found = prepared_.find(key);
        if (found != prepared_.end()) return found->second;
    }
    PreparedMediaBlob result;
    const auto frames = input.sampleCount / input.channels;
    const bool downmixed = input.downmixToMono && input.channels == 2;
    if (input.channels == 1 && input.samplesOwner) {
        result.samples = input.samplesOwner;
    } else {
        auto samples = std::make_shared<std::vector<float>>(
            frames * (downmixed ? 1 : input.channels));
        auto* blob = reinterpret_cast<std::uint8_t*>(samples->data());
        if (input.channels == 1) {
            std::memcpy(blob, input.interleavedSamples, samples->size() * sizeof(float));
        } else if (downmixed) {
            for (std::size_t frame = 0; frame < frames; ++frame) {
                const float sample = static_cast<float>(
                    (static_cast<double>(input.interleavedSamples[frame * 2])
                     + static_cast<double>(input.interleavedSamples[frame * 2 + 1])) * 0.5);
                std::memcpy(blob + frame * sizeof(float), &sample, sizeof(float));
            }
        } else {
            for (std::size_t channel = 0; channel < input.channels; ++channel) {
                for (std::size_t frame = 0; frame < frames; ++frame) {
                    std::memcpy(blob + (channel * frames + frame) * sizeof(float),
                                input.interleavedSamples + frame * input.channels + channel,
                                sizeof(float));
                }
            }
        }
        result.samples = std::move(samples);
    }
    const auto bytes = result.samples->size() * sizeof(float);
    result.checksum = harken::studio::build::Fnv1a64(result.samples->data(), bytes);
    const auto candidates = blobs_.equal_range(result.checksum);
    for (auto found = candidates.first; found != candidates.second; ++found) {
        if (found->second->size() == result.samples->size()
            && std::memcmp(found->second->data(), result.samples->data(), bytes) == 0) {
            result.samples = found->second;
            if (!input.contentHash.empty()) prepared_.emplace(key, result);
            return result;
        }
    }
    blobs_.emplace(result.checksum, result.samples);
    if (!input.contentHash.empty()) prepared_.emplace(key, result);
    return result;
}

std::shared_ptr<const std::vector<float>> MediaCache::intern(
    const std::uint8_t* data, std::size_t bytes, std::uint64_t checksum)
{
    if (bytes % sizeof(float) != 0) return {};
    std::lock_guard lock(mutex_);
    const auto candidates = blobs_.equal_range(checksum);
    for (auto found = candidates.first; found != candidates.second; ++found) {
        if (found->second->size() * sizeof(float) == bytes
            && std::memcmp(found->second->data(), data, bytes) == 0)
            return found->second;
    }
    auto samples = std::make_shared<std::vector<float>>(bytes / sizeof(float));
    if (bytes != 0) std::memcpy(samples->data(), data, bytes);
    blobs_.emplace(checksum, samples);
    return samples;
}

void MediaCache::clear()
{
    std::lock_guard lock(mutex_);
    prepared_.clear();
    blobs_.clear();
}

bool compileEntries(const std::string& logicSections, bool resident,
                    const std::vector<MediaInput>& media,
                    std::vector<CompiledEntry>* entries, std::string* error,
                    MediaCache* mediaCache) {
    using namespace harken::studio::pack;
    entries->clear();
    Diag diag;
    const auto refuse = [&]() {
        if (error) *error = diag.message;
        return false;
    };
    harken::studio::json::JsonValue root;
    if (!harken::studio::json::ParseJson(logicSections, &root, &diag.message))
        return refuse();
    if (root.kind != harken::studio::json::JsonValue::Kind::Object) {
        Fail(&diag, "resource description must be an object");
        return refuse();
    }
    Manifest manifest;
    manifest.packKind = resident ? "resident" : "content";
    if (!ParseLogicSection(root, "buses", ParseBusesSection, &manifest, &diag)
        || !ParseLogicSection(root, "busSends", ParseBusSendsSection, &manifest, &diag)
        || !ParseLogicSection(root, "busEffects", ParseBusEffectsSection, &manifest, &diag)
        || !ParseLogicSection(root, "dynamicRoutes", ParseDynamicRoutesSection, &manifest, &diag)
        || !ParseLogicSection(root, "attenuations", ParseAttenuationsSection, &manifest, &diag)
        || !ParseLogicSection(root, "bindings", ParseBindingsSection, &manifest, &diag)
        || !ParseLogicSection(root, "voiceRecipes", ParseVoiceRecipesSection, &manifest, &diag)
        || !ParseLogicSection(root, "attachments", ParseAttachmentsSection, &manifest, &diag)
        || !ParseLogicSection(root, "containers", ParseContainersSection, &manifest, &diag))
        return refuse();

    std::vector<PackEntry> prepared;
    prepared.reserve(media.size());
    std::vector<CompiledEntry> compiled;
    compiled.reserve(media.size());
    MediaCache localMediaCache;
    if (!mediaCache) mediaCache = &localMediaCache;
    std::unordered_set<harken::studio::types::ObjectId> identities;
    for (std::size_t i = 0; i < media.size(); ++i) {
        const auto& input = media[i];
        if (harken::studio::types::IsNull(input.id) || !identities.insert(input.id).second) {
            Fail(&diag, "media[%zu] has a missing or duplicate identity", i);
            return refuse();
        }
        PackEntry entry;
        entry.path = input.sourcePath;
        entry.soundId = input.id;
        entry.downmixToMono = input.downmixToMono;
        entry.loopable = input.loopable;
        entry.loopStartFrame = input.loopStartFrame;
        entry.loopEndFrame = input.loopEndFrame;
        if (input.encodedMedia) {
            entry.encodedMedia = input.encodedMedia;
            entry.sampleRate = input.sampleRate;
            entry.channelCount = input.channels;
            entry.frameCount = input.frameCount;
        } else if (!PrepareAudioLayout(&entry, input.interleavedSamples, input.sampleCount,
                                      input.channels, input.sampleRate, &diag)) return refuse();
        if (!ValidateLoopPoints(entry, i, &diag)) return refuse();
        manifest.entries.push_back({input.id, input.sourcePath});
        prepared.push_back(std::move(entry));

        EntryPlacement placement;
        placement.kind = HK_PACK_ENTRY_KIND_MEDIA;
        placement.id = input.id;
        placement.index = i;
        placement.size = harken::studio::build::kPackBytes<HkPackMedia>;
        CompiledEntry resource;
        resource.kind = placement.kind;
        resource.vintage = harken::studio::build::PackEntryVintage(placement.kind);
        resource.id = placement.id;
        const auto audio = input.encodedMedia ? PreparedMediaBlob{} : mediaCache->prepare(input);
        resource.media = audio.samples;
        resource.payload.resize(static_cast<std::size_t>(placement.size));
        if (!SerializeEntryPayload(manifest, prepared, placement, resource.payload.data(), &diag,
                                   audio.checksum)) return refuse();
        compiled.push_back(std::move(resource));
    }
    if (!ValidateLogicSections(manifest, &diag)) return refuse();

    const auto placements = BuildEntryPlacements(manifest, prepared);
    compiled.reserve(placements.size());
    for (const auto& placement : placements) {
        if (placement.kind == HK_PACK_ENTRY_KIND_MEDIA) continue;
        CompiledEntry entry;
        entry.kind = placement.kind;
        entry.vintage = harken::studio::build::PackEntryVintage(placement.kind);
        entry.id = placement.id;
        entry.relationOrdinal = harken::studio::types::IsNull(placement.id) ? placement.index : 0;
        entry.payload.resize(static_cast<std::size_t>(placement.size));
        if (!SerializeEntryPayload(manifest, prepared, placement, entry.payload.data(), &diag)) return refuse();
        compiled.push_back(std::move(entry));
    }
    *entries = std::move(compiled);
    return true;
}

}
