#include "ResourceCompiler.h"
#include "JsonReader.h"
#include <harken_studio_build/AuthoredParameters.h>
#include "Cache.h"
#include "Manifest.h"
#include "Report.h"
#include "MediaDecode.h"

#include "harken_studio_build/PackFormat.h"
#include "harken_studio_build/PackFormatV2.h"
#include "harken_studio_build/StreamFormat.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace harkenizer::resourcecompiler {
namespace {


using harkenstudiojson::packc::CacheRecord;
using harkenstudiojson::packc::Diag;
using harkenstudiojson::packc::Fail;
using harkenstudiojson::packc::Manifest;
using harkenstudiojson::packc::ManifestAttachment;
using harkenstudiojson::packc::ManifestAttenuation;
using harkenstudiojson::packc::ManifestBinding;
using harkenstudiojson::packc::ManifestBus;
using harkenstudiojson::packc::ManifestBusEffect;
using harkenstudiojson::packc::ManifestBusSend;
using harkenstudiojson::packc::ManifestContainer;
using harkenstudiojson::packc::ManifestContainerChild;
using harkenstudiojson::packc::ManifestCurve;
using harkenstudiojson::packc::ManifestEntry;
using harkenstudiojson::packc::ManifestDynamicRoute;
using harkenstudiojson::packc::ManifestVoiceRecipe;
using harkenstudiojson::packc::ManifestVoiceRecipeEffect;
using harkenstudiojson::packc::PreviousPack;
using harkenstudiojson::packc::Report;
using harkenstudiojson::packc::ReportEntry;
using harkenstudiojson::packc::ReportLogicCount;
using harkenstudiojson::packc::ReportPack;
using harkenstudiojson::packc::ReportWarning;

struct PackEntry {
    std::string path;         // resolved path the file was read from
    std::string sourceText;   // path as written (report/cache identity)
    std::vector<float> planar; // channel planes concatenated
    std::uint32_t sampleRate = 0;
    std::uint16_t channelCount = 0;
    std::uint64_t frameCount = 0;
    harkenstudio::ObjectId soundId{};
    bool explicitId = false;
    bool loopable = false;
    std::uint64_t loopStartFrame = 0;
    std::uint64_t loopEndFrame = 0; // 0 = whole file
    std::string codec;
    std::string downmix;
    std::uint16_t sourceChannelCount = 0;
    float sourcePeakAbs = 0.0f;
    float peakAbs = 0.0f;
    bool downmixed = false;
    bool skippedByCache = false;
    bool hashed = false; // raw source hash below is meaningful
    std::uint64_t sourceBytes = 0;
    std::uint64_t sourceHash = 0;
};

PackEntry MakePackEntry(const ManifestEntry& source) {
    PackEntry entry;
    entry.path = source.resolvedPath;
    entry.sourceText = source.path;
    entry.soundId = source.soundId;
    entry.explicitId = source.explicitId;
    entry.loopable = source.loopable;
    entry.loopStartFrame = source.loopStartFrame;
    entry.loopEndFrame = source.loopEndFrame;
    entry.codec = source.codec;
    entry.downmix = source.downmix;
    return entry;
}

// Default chunk framing for .stream output when the manifest does not
// choose one. 4096 frames per chunk keeps the I/O granule comfortably
// sized (one mono chunk = 16 KiB, one 5.1-ish 6-channel chunk = 96 KiB)
// while a chunk still spans well under 100 ms at common rates, so a loop
// seek never stages more than a fraction of a second of material.
constexpr std::uint32_t kDefaultChunkFrames = 4096;

bool ReadTextFile(const std::string& path, std::string* out) {
    std::ifstream in(harkenstudiojson::Utf8Path(path), std::ios::binary);
    if (!in) {
        return false;
    }
    out->assign(std::istreambuf_iterator<char>(in),
                std::istreambuf_iterator<char>());
    return in.good() || in.eof();
}

bool WriteWholeFile(const std::string& path, const void* data,
                    std::size_t bytes) {
    std::ofstream out(harkenstudiojson::Utf8Path(path), std::ios::binary | std::ios::trunc);
    if (!out ||
        !out.write(static_cast<const char*>(data),
                   static_cast<std::streamsize>(bytes))) {
        return false;
    }
    out.close();
    return static_cast<bool>(out);
}

const char* CacheDownmixPolicy(const std::string& downmix) {
    return downmix.empty() ? harkenstudiojson::packc::kCacheDownmixPreserve
                           : downmix.c_str();
}

std::string CacheIdentityKey(const std::string& sourcePath,
                             const std::string& downmixPolicy,
                             std::uint32_t algorithmVersion) {
    std::string key;
    key.reserve(sourcePath.size() + downmixPolicy.size() + 16);
    key.append(sourcePath);
    key.push_back('\0');
    key.append(downmixPolicy);
    key.push_back('\0');
    key.append(std::to_string(algorithmVersion));
    return key;
}

bool PrepareAudioLayout(PackEntry* entry, const float* samples, std::size_t sampleCount,
                        std::uint16_t channels, std::uint32_t sampleRate, Diag* diag) {
    if (!samples || sampleCount == 0 || channels == 0 || sampleRate == 0
        || sampleCount % channels != 0) {
        return Fail(diag, "invalid prepared audio for '%s'", entry->path.c_str());
    }
    if (entry->downmix.empty() && channels != 1 && channels != 2) {
        return Fail(diag, "'%s' has %u channels; only mono and stereo are supported",
                    entry->path.c_str(), channels);
    }
    entry->sampleRate = sampleRate;
    entry->sourceChannelCount = channels;
    entry->frameCount = sampleCount / channels;
    if (!entry->downmix.empty() && channels > 2) {
        return Fail(diag,
                    "cannot downmix '%s' to mono: source has %u "
                    "channels; only mono or stereo is supported",
                    entry->path.c_str(), channels);
    }

    entry->downmixed = !entry->downmix.empty() && channels == 2;
    entry->channelCount = entry->downmixed ? 1 : channels;
    return true;
}

bool PreparePlanar(PackEntry* entry, const float* samples, std::size_t sampleCount,
                   std::uint16_t channels, std::uint32_t sampleRate, Diag* diag) {
    if (!PrepareAudioLayout(entry, samples, sampleCount, channels, sampleRate, diag))
        return false;
    const std::size_t frames = sampleCount / channels;
    if (entry->downmixed) {
        entry->planar.resize(frames);
        float sourcePeak = 0.0f;
        float peak = 0.0f;
        for (std::size_t n = 0; n < frames; ++n) {
            const float left = samples[n * 2];
            const float right = samples[n * 2 + 1];
            sourcePeak = std::max(
                sourcePeak,
                std::max(std::fabs(left), std::fabs(right)));
            const float sample = static_cast<float>(
                (static_cast<double>(left) +
                 static_cast<double>(right)) *
                0.5);
            entry->planar[n] = sample;
            const float magnitude = std::fabs(sample);
            if (magnitude > peak) {
                peak = magnitude;
            }
        }
        entry->sourcePeakAbs = sourcePeak;
        entry->peakAbs = peak;
        return true;
    }

    entry->planar.resize(sampleCount);
    float peak = 0.0f;
    for (std::uint32_t ch = 0; ch < channels; ++ch) {
        float* plane = entry->planar.data() +
                       static_cast<std::size_t>(ch) * frames;
        for (std::size_t n = 0; n < frames; ++n) {
            const float sample = samples[n * channels + ch];
            plane[n] = sample;
            const float magnitude = std::fabs(sample);
            if (magnitude > peak) {
                peak = magnitude;
            }
        }
    }
    entry->peakAbs = peak;
    return true;
}

bool DecodeToPlanar(PackEntry* entry, Diag* diag, Result* result) {
    harkenstudiojson::LoadedWav wav;
    if (!harkenstudiojson::LoadWav(entry->path, &wav,
                            !entry->downmix.empty(), result->diagnosticLog)) {
        return Fail(diag, "cannot open or decode '%s'",
                    entry->path.c_str());
    }
    return PreparePlanar(entry, wav.samples.data(), wav.samples.size(),
                         static_cast<std::uint16_t>(wav.channels), wav.sampleRate, diag);
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

std::uint64_t AlignUp64(std::uint64_t value) {
    const std::uint64_t align = harkenstudiobuild::kPackBlobAlign;
    return (value + align - 1) / align * align;
}

bool BuildPackBytes(const std::vector<PackEntry>& entries,
                    std::vector<std::uint8_t>* out, Diag* diag) {
    const std::uint32_t entryCount =
        static_cast<std::uint32_t>(entries.size());
    const std::uint64_t tocOffset = harkenstudiobuild::kPackHeaderBytes;
    const std::uint64_t tocBytes =
        static_cast<std::uint64_t>(entryCount) *
        harkenstudiobuild::kPackTocEntryBytes;

    // Blob placement: packed in manifest order, each aligned. fileBytes
    // ends exactly at the last blob's end — no trailing padding.
    std::vector<std::uint64_t> blobOffsets(entries.size());
    std::uint64_t cursor = AlignUp64(tocOffset + tocBytes);
    std::uint64_t fileBytes = tocOffset + tocBytes;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        blobOffsets[i] = cursor;
        const std::uint64_t blobBytes =
            static_cast<std::uint64_t>(entries[i].planar.size()) *
            sizeof(float);
        fileBytes = cursor + blobBytes;
        cursor = AlignUp64(fileBytes);
    }
    if (fileBytes > static_cast<std::uint64_t>(SIZE_MAX)) {
        return Fail(diag, "pack would exceed addressable size");
    }

    // Zero-initialized image: alignment gaps stay zeroed, which is part
    // of the byte-stable output contract.
    out->assign(static_cast<std::size_t>(fileBytes), 0);

    std::vector<std::uint8_t> tocImage(
        static_cast<std::size_t>(tocBytes), 0);
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const PackEntry& entry = entries[i];
        const std::uint64_t blobBytes =
            static_cast<std::uint64_t>(entry.planar.size()) *
            sizeof(float);
        std::memcpy(out->data() + blobOffsets[i], entry.planar.data(),
                    static_cast<std::size_t>(blobBytes));

        harkenstudiobuild::PackTocEntry toc;
        toc.soundId = entry.soundId;
        toc.sampleRate = entry.sampleRate;
        toc.channelCount = entry.channelCount;
        toc.sampleFormat = harkenstudiobuild::kPackSampleFormatF32Planar;
        toc.frameCount = entry.frameCount;
        toc.blobOffset = blobOffsets[i];
        toc.blobBytes = blobBytes;
        toc.blobChecksum =
            harkenstudiobuild::Fnv1a64(out->data() + blobOffsets[i],
                           static_cast<std::size_t>(blobBytes));
        toc.flags = entry.loopable ? harkenstudiobuild::kPackFlagLoopable : 0;
        toc.loopStartFrame = entry.loopable ? entry.loopStartFrame : 0;
        toc.loopEndFrame = entry.loopable ? entry.loopEndFrame : 0;
        harkenstudiobuild::SerializePackTocEntry(
            toc, tocImage.data() + i * harkenstudiobuild::kPackTocEntryBytes);
    }
    std::memcpy(out->data() + tocOffset, tocImage.data(),
                tocImage.size());

    harkenstudiobuild::PackHeader header;
    header.formatVersion = harkenstudiobuild::kPackFormatVersion;
    header.headerBytes = harkenstudiobuild::kPackHeaderBytes;
    header.fileBytes = fileBytes;
    header.entryCount = entryCount;
    header.tocOffset = static_cast<std::uint32_t>(tocOffset);
    header.tocChecksum = harkenstudiobuild::Fnv1a64(tocImage.data(), tocImage.size());
    harkenstudiobuild::SerializePackHeader(header, out->data());
    return true;
}

// --- v2 container emission ---

std::uint64_t AlignUpTo(std::uint64_t value, std::uint64_t align) {
    return (value + align - 1) / align * align;
}

// Converters from the manifest working forms to the descriptor structs
// the PackFormatV2.h serializers take. The manifest parser already
// proved every field, so these are pure translations.

harkenstudiobuild::AttenuationCurve ToAttenuationCurve(const ManifestCurve& curve,
                                      bool enabled) {
    harkenstudiobuild::AttenuationCurve out{};
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

harkenstudiobuild::BindingCurve ToBindingCurve(const ManifestCurve& curve) {
    harkenstudiobuild::BindingCurve out{};

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

// Lays out and serializes the formatVersion 2 container
// (PackFormatV2.h): header, directory in canonical order — ascending
// entry kind, manifest order within a kind; the relation kinds (bus
// send, binding attachment) carry their 0-based within-kind ordinal as
// the directory id — then the payload region (each payload aligned to
// kPackV2PayloadAlign), then the media blob region (kPackBlobAlign,
// manifest order). fileBytes ends at the last blob, or at the last
// payload when the pack carries no media. The image is
// zero-initialized so every alignment gap stays zeroed, and nothing
// time- or machine-derived enters the bytes: identical manifest and
// sources produce a byte-identical container.
struct EntryPlacement {
    std::uint32_t kind = 0;
    harkenstudio::ObjectId id{};
    std::size_t index = 0; // position within its manifest section
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
};

std::vector<EntryPlacement> BuildEntryPlacements(
    const Manifest& manifest, const std::vector<PackEntry>& entries) {
    std::vector<EntryPlacement> placements;
    const auto place = [&placements](std::uint32_t kind,
                                     harkenstudio::ObjectId id, std::size_t index,
                                     std::uint64_t size) {
        EntryPlacement placement;
        placement.kind = kind;
        placement.id = id;
        placement.index = index;
        placement.size = size;
        placements.push_back(placement);
    };
    for (std::size_t i = 0; i < entries.size(); ++i) {
        place(harkenstudiobuild::kPackEntryKindMedia, entries[i].soundId, i,
              harkenstudiobuild::kPackV2MediaPayloadBytes);
    }
    for (std::size_t i = 0; i < manifest.buses.size(); ++i) {
        place(harkenstudiobuild::kPackEntryKindBus, manifest.buses[i].id, i,
              harkenstudiobuild::kPackV6BusPayloadBytes);
    }
    for (std::size_t i = 0; i < manifest.busSends.size(); ++i) {
        place(harkenstudiobuild::kPackEntryKindBusSend, {}, i,
              harkenstudiobuild::kPackV2BusSendPayloadBytes);
    }
    for (std::size_t i = 0; i < manifest.attenuations.size(); ++i) {
        place(harkenstudiobuild::kPackEntryKindAttenuation,
              manifest.attenuations[i].id, i,
              harkenstudiobuild::kPackV3AttenuationPayloadBytes);
    }
    for (std::size_t i = 0; i < manifest.bindings.size(); ++i) {
        place(harkenstudiobuild::kPackEntryKindBindingDefinition,
              manifest.bindings[i].id, i,
              harkenstudiobuild::kPackV2BindingDefinitionPayloadBytes);
    }
    for (std::size_t i = 0; i < manifest.attachments.size(); ++i) {
        place(harkenstudiobuild::kPackEntryKindBindingAttachment, {}, i,
              harkenstudiobuild::kPackV2BindingAttachmentPayloadBytes);
    }
    for (std::size_t i = 0; i < manifest.containers.size(); ++i) {
        place(harkenstudiobuild::kPackEntryKindContainer, manifest.containers[i].id,
              i,
              harkenstudiobuild::PackV2ContainerPayloadBytes(
                  static_cast<std::uint32_t>(
                      manifest.containers[i].children.size())));
    }
    for (std::size_t i = 0; i < manifest.voiceRecipes.size(); ++i) {
        const ManifestVoiceRecipe& recipe = manifest.voiceRecipes[i];
        const std::uint32_t installCount =
            static_cast<std::uint32_t>(recipe.effects.size());
        const std::uint32_t bindingCount =
            static_cast<std::uint32_t>(recipe.bindings.size());
        place(harkenstudiobuild::kPackEntryKindVoiceRecipe, recipe.playable, i,
              harkenstudiobuild::PackV5VoiceRecipePayloadBytes(installCount,
                                                  bindingCount));
    }
    for (std::size_t i = 0; i < manifest.busEffects.size(); ++i) {
        place(harkenstudiobuild::kPackEntryKindBusEffect, {}, i,
              harkenstudiobuild::kPackV2BusEffectPayloadBytes);
    }
    for (std::size_t i = 0; i < manifest.dynamicRoutes.size(); ++i) {
        place(harkenstudiobuild::kPackEntryKindEffectDynamicRoute, {}, i,
              harkenstudiobuild::kPackV2EffectDynamicRoutePayloadBytes);
    }

    return placements;
}

bool SerializeEntryPayload(const Manifest& manifest,
                           const std::vector<PackEntry>& entries,
                           const EntryPlacement& placement, std::uint64_t blobOffset,
                           const std::uint8_t* blob, std::uint8_t* dst, Diag* diag,
                           const std::uint64_t* blobChecksum = nullptr) {
    switch (placement.kind) {
    case harkenstudiobuild::kPackEntryKindMedia: {
        const PackEntry& entry = entries[placement.index];
        const std::uint64_t blobBytes =
            entry.frameCount * entry.channelCount * sizeof(float);
        harkenstudiobuild::PackTocEntry toc;
        toc.soundId = entry.soundId;
        toc.sampleRate = entry.sampleRate;
        toc.channelCount = entry.channelCount;
        toc.sampleFormat = harkenstudiobuild::kPackSampleFormatF32Planar;
        toc.frameCount = entry.frameCount;
        toc.blobOffset = blobOffset;
        toc.blobBytes = blobBytes;
        toc.blobChecksum = blobChecksum ? *blobChecksum : harkenstudiobuild::Fnv1a64(
            blob, static_cast<std::size_t>(blobBytes));
        toc.flags = entry.loopable ? harkenstudiobuild::kPackFlagLoopable : 0;
        toc.loopStartFrame =
            entry.loopable ? entry.loopStartFrame : 0;
        toc.loopEndFrame = entry.loopable ? entry.loopEndFrame : 0;
        harkenstudiobuild::SerializePackTocEntry(toc, dst);
        break;
    }
    case harkenstudiobuild::kPackEntryKindBus: {
        const ManifestBus& bus = manifest.buses[placement.index];
        harkenstudiobuild::PackBusEntry wire;
        wire.busId = bus.id;
        wire.parentBusId = bus.parent;
        wire.volume = bus.volume;
        wire.outputBusVolume = bus.outputBusVolume;
        wire.makeUpGain = bus.makeUpGain;
        harkenstudiobuild::SerializePackV6Bus(wire, dst);
        break;
    }
    case harkenstudiobuild::kPackEntryKindBusSend: {
        const ManifestBusSend& send =
            manifest.busSends[placement.index];
        harkenstudiobuild::PackBusSendEntry wire;
        wire.fromBusId = send.from;
        wire.toBusId = send.to;
        wire.levelDb = send.levelDb;
        harkenstudiobuild::SerializePackV2BusSend(wire, dst);
        break;
    }
    case harkenstudiobuild::kPackEntryKindAttenuation: {
        const ManifestAttenuation& attenuation =
            manifest.attenuations[placement.index];
        harkenstudiobuild::AttenuationDefinition desc{};

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
        harkenstudiobuild::SerializePackV3Attenuation(desc, dst);
        break;
    }
    case harkenstudiobuild::kPackEntryKindBindingDefinition: {
        const ManifestBinding& binding =
            manifest.bindings[placement.index];
        harkenstudiobuild::BindingDefinition desc{};

        desc.variableId = binding.variable;
        desc.targetScopeKind = binding.scope;
        desc.targetId = binding.target;
        desc.transitionMs = binding.transitionMs;
        desc.curve = ToBindingCurve(binding.curve);
        harkenstudiobuild::SerializePackV2BindingDefinition(binding.id, desc,
                                                dst);
        break;
    }
    case harkenstudiobuild::kPackEntryKindBindingAttachment: {
        const ManifestAttachment& attachment =
            manifest.attachments[placement.index];
        harkenstudiobuild::PackBindingAttachmentEntry wire;
        wire.bindingId = attachment.binding;
        wire.scopeKind = attachment.scope;
        wire.objectId = attachment.object;
        harkenstudiobuild::SerializePackV2BindingAttachment(wire, dst);
        break;
    }
    case harkenstudiobuild::kPackEntryKindContainer: {
        const ManifestContainer& container =
            manifest.containers[placement.index];
        harkenstudiobuild::PackContainerEntryHeader header;
        header.containerId = container.id;
        header.selectionOrder = container.order;
        header.avoidRepeatWindow = container.avoidRepeatWindow;
        header.selectionVariableId = container.selectionVariable;
        header.defaultIndex = container.defaultIndex;
        header.childCount = static_cast<std::uint32_t>(
            container.children.size());
        harkenstudiobuild::SerializePackV2ContainerHeader(header, dst);
        for (std::size_t c = 0; c < container.children.size();
             ++c) {
            const ManifestContainerChild& child =
                container.children[c];
            harkenstudiobuild::PackContainerChildV2 wire{};
            wire.playableId = child.playable;
            wire.weight = child.weight;
            wire.matchValue = child.matchValue;
            wire.delayMs = child.delayMs;
            wire.delayJitterMs = child.delayJitterMs;
            harkenstudiobuild::SerializePackV2ContainerChild(
                wire,
                dst + harkenstudiobuild::kPackV2ContainerHeaderBytes +
                    c * harkenstudiobuild::kPackV2ContainerChildBytes);
        }
        break;
    }
    case harkenstudiobuild::kPackEntryKindVoiceRecipe: {
        const ManifestVoiceRecipe& recipe =
            manifest.voiceRecipes[placement.index];
        harkenstudiobuild::PackVoiceRecipeEntryHeader header;
        header.playableId = recipe.playable;
        header.installCount =
            static_cast<std::uint32_t>(recipe.effects.size());
        header.bindingCount =
            static_cast<std::uint32_t>(recipe.bindings.size());
        harkenstudiobuild::SerializePackV2VoiceRecipeHeader(header, dst);
        std::uint64_t cursor = harkenstudiobuild::kPackV2VoiceRecipeHeaderBytes;
        harkenstudiobuild::PackVoiceRecipeTone tone;
        if (recipe.tone.present) {
            tone.present = 1u;
            tone.sectionEnableMask =
                (recipe.tone.lowpassEnabled
                     ? harkenstudiobuild::kPackVoiceToneLowpassEnabled
                     : 0u) |
                (recipe.tone.highpassEnabled
                     ? harkenstudiobuild::kPackVoiceToneHighpassEnabled
                     : 0u) |
                (recipe.tone.lowShelfEnabled
                     ? harkenstudiobuild::kPackVoiceToneLowShelfEnabled
                     : 0u) |
                (recipe.tone.highShelfEnabled
                     ? harkenstudiobuild::kPackVoiceToneHighShelfEnabled
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
        harkenstudiobuild::SerializePackV3VoiceRecipeTone(tone, dst + cursor);
        cursor += harkenstudiobuild::kPackV3VoiceRecipeToneBytes;
        harkenstudiobuild::PackVoiceRecipeSpatial spatial;
        if (recipe.spatial.present) {
            spatial.attenuationId = recipe.spatial.attenuation;
            spatial.distanceScale = recipe.spatial.distanceScale;
        } else {
            spatial.distanceScale = 0.0f;
        }
        harkenstudiobuild::SerializePackV4VoiceRecipeSpatial(spatial, dst + cursor);
        cursor += harkenstudiobuild::kPackV4VoiceRecipeSpatialBytes;
        harkenstudiobuild::PackVoiceRecipePlayback playback;
        if (recipe.playback.present) {
            playback.present = 1u;
            playback.loopCount = recipe.playback.loopCount;
        }
        harkenstudiobuild::SerializePackV5VoiceRecipePlayback(playback,
                                                  dst + cursor);
        cursor += harkenstudiobuild::kPackV5VoiceRecipePlaybackBytes;
        for (const ManifestVoiceRecipeEffect& effect : recipe.effects) {
            harkenstudiobuild::PackVoiceRecipeInstall install;
            install.effectId = effect.effectId;
            install.slot = effect.slot;
            install.effectType = effect.type;
            install.descriptorVintage = effect.descriptorVintage;
            install.bypass = effect.bypass ? 1u : 0u;
            install.paramCount =
                static_cast<std::uint32_t>(effect.parameters.size());
            for (std::uint32_t p = 0; p < install.paramCount; ++p) {
                install.params[p] = effect.parameters[p];
            }
            harkenstudiobuild::SerializePackV2VoiceRecipeInstall(
                install, dst + cursor);
            cursor += harkenstudiobuild::kPackV2VoiceRecipeInstallBytes;
        }
        for (harkenstudio::ObjectId binding : recipe.bindings) {
            harkenstudiobuild::SerializePackV2VoiceRecipeBinding(binding,
                                                     dst + cursor);
            cursor += harkenstudiobuild::kPackV2VoiceRecipeBindingBytes;
        }
        break;
    }
    case harkenstudiobuild::kPackEntryKindBusEffect: {
        const ManifestBusEffect& effect =
            manifest.busEffects[placement.index];
        harkenstudiobuild::PackBusEffectEntry wire;
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
        harkenstudiobuild::SerializePackV2BusEffect(wire, dst);
        break;
    }
    case harkenstudiobuild::kPackEntryKindEffectDynamicRoute: {
        const ManifestDynamicRoute& route =
            manifest.dynamicRoutes[placement.index];
        harkenstudiobuild::PackEffectDynamicRouteEntry wire;
        wire.targetBusId = route.targetBus;
        wire.effectSlot = route.effectSlot;
        wire.routeSlot = route.routeSlot;
        wire.expectedEffectType = route.expectedEffectType;
        wire.signalForm = route.signalForm;
        wire.sourceKind = route.sourceKind;
        wire.tap = route.tap;
        wire.sourceId = route.sourceBus;
        wire.levelDb = route.levelDb;
        harkenstudiobuild::SerializePackV2EffectDynamicRoute(wire, dst);
        break;
    }
    default:
        return Fail(diag, "unsupported v2 entry kind %u",
                    placement.kind);
    }
    return true;
}

std::uint32_t EntryVintage(std::uint32_t kind) {
    const bool dynamicDeploymentEntry =
        kind == harkenstudiobuild::kPackEntryKindBusEffect ||
        kind == harkenstudiobuild::kPackEntryKindEffectDynamicRoute;
    if (dynamicDeploymentEntry) {
        return harkenstudiobuild::kPackV2DynamicSchemaVintage;
    } else if (kind == harkenstudiobuild::kPackEntryKindBus) {
        return harkenstudiobuild::kPackV2BusMixSchemaVintage;
    } else if (kind == harkenstudiobuild::kPackEntryKindVoiceRecipe) {
        return harkenstudiobuild::kPackV2PlaybackRecipeSchemaVintage;
    } else if (kind == harkenstudiobuild::kPackEntryKindAttenuation) {
        return harkenstudiobuild::kPackV2AuthoredToneSchemaVintage;
    } else {
        return harkenstudiobuild::kPackV2SchemaVintage;
    }
}

bool BuildPackBytesV2(const Manifest& manifest,
                      const std::vector<PackEntry>& entries,
                      std::vector<std::uint8_t>* out, Diag* diag) {
    auto placements = BuildEntryPlacements(manifest, entries);

    const std::uint32_t entryCount =
        static_cast<std::uint32_t>(placements.size());
    const std::uint64_t directoryBytes =
        static_cast<std::uint64_t>(entryCount) *
        harkenstudiobuild::kPackV2DirectoryEntryBytes;
    std::uint64_t cursor = harkenstudiobuild::kPackV2HeaderBytes + directoryBytes;
    for (EntryPlacement& placement : placements) {
        placement.offset =
            AlignUpTo(cursor, harkenstudiobuild::kPackV2PayloadAlign);
        cursor = placement.offset + placement.size;
    }
    std::vector<std::uint64_t> blobOffsets(entries.size());
    std::uint64_t fileBytes = cursor;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        blobOffsets[i] = AlignUpTo(fileBytes, harkenstudiobuild::kPackBlobAlign);
        fileBytes = blobOffsets[i] +
                    static_cast<std::uint64_t>(
                        entries[i].planar.size()) *
                        sizeof(float);
    }
    if (fileBytes > static_cast<std::uint64_t>(SIZE_MAX)) {
        return Fail(diag, "pack would exceed addressable size");
    }
    out->assign(static_cast<std::size_t>(fileBytes), 0);

    // Blobs land before the media payloads serialize so the recorded
    // blob checksums cover the final image bytes (the v1 discipline).
    for (std::size_t i = 0; i < entries.size(); ++i) {
        std::memcpy(out->data() + blobOffsets[i],
                    entries[i].planar.data(),
                    entries[i].planar.size() * sizeof(float));
    }

    for (const EntryPlacement& placement : placements) {
        const auto blobOffset = placement.kind == harkenstudiobuild::kPackEntryKindMedia
            ? blobOffsets[placement.index] : 0;
        if (!SerializeEntryPayload(manifest, entries, placement, blobOffset,
                                   out->data() + blobOffset,
                                   out->data() + placement.offset, diag)) return false;
    }

    std::vector<std::uint8_t> directoryImage(
        static_cast<std::size_t>(directoryBytes), 0);
    for (std::size_t i = 0; i < placements.size(); ++i) {
        const EntryPlacement& placement = placements[i];
        harkenstudiobuild::PackV2DirectoryEntry entry;
        entry.kind = placement.kind;
        entry.vintage = EntryVintage(placement.kind);
        entry.id = placement.id;
        entry.relationOrdinal = harkenstudio::IsNull(placement.id) ? placement.index : 0;
        entry.offset = placement.offset;
        entry.size = placement.size;
        entry.payloadChecksum =
            harkenstudiobuild::Fnv1a64(out->data() + placement.offset,
                           static_cast<std::size_t>(placement.size));
        harkenstudiobuild::SerializePackV2DirectoryEntry(
            entry, directoryImage.data() +
                       i * harkenstudiobuild::kPackV2DirectoryEntryBytes);
    }
    if (!directoryImage.empty()) {
        std::memcpy(out->data() + harkenstudiobuild::kPackV2HeaderBytes,
                    directoryImage.data(), directoryImage.size());
    }

    harkenstudiobuild::PackV2Header header;
    header.formatVersion = harkenstudiobuild::kPackFormatVersion2;
    header.headerBytes = harkenstudiobuild::kPackV2HeaderBytes;
    header.fileBytes = fileBytes;
    header.packKind = manifest.packKind == "resident"
                          ? harkenstudiobuild::kPackKindResident
                          : harkenstudiobuild::kPackKindContent;
    header.entryCount = entryCount;
    header.directoryOffset = harkenstudiobuild::kPackV2HeaderBytes;
    header.directoryChecksum =
        harkenstudiobuild::Fnv1a64(directoryImage.data(), directoryImage.size());
    harkenstudiobuild::SerializePackV2Header(header, out->data());
    return true;
}

// Lays out the single-sound streaming container: header at offset 0, one
// TOC entry right after it, the chunk-checksum table after the TOC, and
// the chunk-framed payload at the next payload-alignment boundary. Every
// chunk occupies exactly chunkBytes; the final chunk's tail is zero-padded
// and those zeros participate in its checksum, so the whole image is
// byte-stable for identical input.
bool BuildStreamBytes(const PackEntry& entry, std::uint32_t chunkFrames,
                      std::vector<std::uint8_t>* out, Diag* diag) {
    const std::uint64_t chunkBytes =
        harkenstudiobuild::StreamChunkBytes(chunkFrames, entry.channelCount);
    const std::uint32_t chunkCount =
        harkenstudiobuild::StreamChunkCount(entry.frameCount, chunkFrames);
    if (chunkCount == 0) {
        return Fail(diag,
                    "'%s' decodes to zero frames — nothing to stream",
                    entry.path.c_str());
    }
    const std::uint64_t checksumOffset =
        static_cast<std::uint64_t>(harkenstudiobuild::kStreamHeaderBytes) +
        harkenstudiobuild::kStreamTocEntryBytes;
    const std::uint64_t checksumBytes =
        static_cast<std::uint64_t>(chunkCount) *
        harkenstudiobuild::kStreamChunkChecksumBytes;
    const std::uint64_t align = harkenstudiobuild::kStreamPayloadAlign;
    const std::uint64_t payloadOffset =
        (checksumOffset + checksumBytes + align - 1) / align * align;
    const std::uint64_t payloadBytes =
        static_cast<std::uint64_t>(chunkCount) * chunkBytes;
    const std::uint64_t fileBytes = payloadOffset + payloadBytes;
    if (fileBytes > static_cast<std::uint64_t>(SIZE_MAX)) {
        return Fail(diag, "stream would exceed addressable size");
    }

    // Zero-initialized image: the pre-payload alignment gap and the final
    // chunk's padding stay zeroed, part of the byte-stable contract.
    out->assign(static_cast<std::size_t>(fileBytes), 0);

    // Payload chunks are planar WITHIN each chunk — channel 0's
    // chunkFrames floats, then channel 1's, and so on — sliced out of the
    // whole-file channel planes the decoder produced. Each chunk's
    // checksum covers the full fixed span including padding zeros.
    const std::size_t frames = static_cast<std::size_t>(entry.frameCount);
    for (std::uint32_t c = 0; c < chunkCount; ++c) {
        const std::uint32_t valid = harkenstudiobuild::StreamChunkValidFrames(
            c, chunkFrames, entry.frameCount);
        std::uint8_t* chunk =
            out->data() +
            static_cast<std::size_t>(harkenstudiobuild::StreamChunkFileOffset(
                payloadOffset, c, chunkBytes));
        for (std::uint32_t ch = 0; ch < entry.channelCount; ++ch) {
            const float* src = entry.planar.data() +
                               static_cast<std::size_t>(ch) * frames +
                               static_cast<std::size_t>(c) * chunkFrames;
            std::memcpy(chunk + static_cast<std::size_t>(ch) *
                                    chunkFrames * sizeof(float),
                        src,
                        static_cast<std::size_t>(valid) * sizeof(float));
        }
        harkenstudiobuild::StoreLE64(
            out->data() + static_cast<std::size_t>(checksumOffset) +
                static_cast<std::size_t>(c) *
                    harkenstudiobuild::kStreamChunkChecksumBytes,
            harkenstudiobuild::Fnv1a64(chunk, static_cast<std::size_t>(chunkBytes)));
    }

    harkenstudiobuild::StreamTocEntry toc;
    toc.soundId = entry.soundId;
    toc.sampleRate = entry.sampleRate;
    toc.channelCount = entry.channelCount;
    toc.sampleFormat = harkenstudiobuild::kStreamSampleFormatF32Planar;
    toc.frameCount = entry.frameCount;
    toc.payloadOffset = payloadOffset;
    toc.payloadBytes = payloadBytes;
    toc.chunkChecksumOffset = checksumOffset;
    toc.chunkCount = chunkCount;
    toc.flags = entry.loopable ? harkenstudiobuild::kStreamFlagLoopable : 0;
    toc.loopStartFrame = entry.loopable ? entry.loopStartFrame : 0;
    toc.loopEndFrame = entry.loopable ? entry.loopEndFrame : 0;
    harkenstudiobuild::SerializeStreamTocEntry(
        toc, out->data() + harkenstudiobuild::kStreamHeaderBytes);

    // tocChecksum chains the TOC entry bytes then the chunk-checksum
    // table bytes, in that order — the reader recomputes the same chain
    // at registration, covering both regions with one value.
    std::uint64_t tocChecksum =
        harkenstudiobuild::Fnv1a64(out->data() + harkenstudiobuild::kStreamHeaderBytes,
                       harkenstudiobuild::kStreamTocEntryBytes);
    tocChecksum =
        harkenstudiobuild::Fnv1a64(out->data() + static_cast<std::size_t>(checksumOffset),
                       static_cast<std::size_t>(checksumBytes), tocChecksum);

    harkenstudiobuild::StreamHeader header;
    header.formatVersion = harkenstudiobuild::kStreamFormatVersion;
    header.headerBytes = harkenstudiobuild::kStreamHeaderBytes;
    header.fileBytes = fileBytes;
    header.entryCount = harkenstudiobuild::kStreamEntryCount;
    header.tocOffset = harkenstudiobuild::kStreamHeaderBytes;
    header.chunkFrames = chunkFrames;
    header.tocChecksum = tocChecksum;
    harkenstudiobuild::SerializeStreamHeader(header, out->data());
    return true;
}


void AddPeakWarnings(const PackEntry& entry, ReportPack* reportPack) {
    const auto makePeakWarning = [&](const char* kind) {
        ReportWarning warning;
        warning.kind = kind;
        warning.hasId = true;
        warning.id = entry.soundId;
        warning.source = entry.sourceText;
        warning.hasPeakAbs = true;
        warning.peakAbs = entry.peakAbs;
        return warning;
    };
    if (entry.peakAbs <= harkenstudiojson::packc::kSilenceFloorPeak) {
        reportPack->warnings.push_back(makePeakWarning("silent-entry"));
    }
    if (entry.peakAbs > harkenstudiojson::packc::kClipPeak) {
        reportPack->warnings.push_back(makePeakWarning("clipped-source"));
    }
    if (entry.downmixed && entry.sourcePeakAbs > 0.0f &&
        static_cast<double>(entry.peakAbs) <=
            static_cast<double>(entry.sourcePeakAbs) *
                harkenstudiojson::packc::kDownmixCancellationPeakRatio) {
        ReportWarning warning = makePeakWarning("downmix-cancellation");
        warning.hasSourcePeakAbs = true;
        warning.sourcePeakAbs = entry.sourcePeakAbs;
        reportPack->warnings.push_back(std::move(warning));
    }
}

float MeasurePeak(const std::vector<float>& planar) {
    float peak = 0.0f;
    for (const float sample : planar) {
        const float magnitude = std::fabs(sample);
        if (magnitude > peak) {
            peak = magnitude;
        }
    }
    return peak;
}

// Builds one .pack from one manifest, filling its report section and —
// when caching is on — refreshing the cache file. Returns false with
// the refusal in diag; the caller owns report emission either way.
bool BuildOnePack(const Manifest& manifest, const Request& request,
                  const std::string& outputText,
                  const std::string& outputResolved, Report* report,
                  Diag* diag, Result* result) {
    ReportPack reportPack;
    reportPack.manifest = manifest.manifestPath;
    reportPack.manifestVersion = manifest.version;
    reportPack.output = outputText;
    reportPack.container = "pack";

    // A pack kind selects the v2 container. The report then names the
    // kind and counts the logic entries per section — present sections
    // only, fixed alphabetical (byte-order) keys.
    const bool v2Container = !manifest.packKind.empty();
    if (v2Container) {
        reportPack.packKind = manifest.packKind;
        const auto addCount = [&reportPack](const char* section,
                                            std::size_t count) {
            if (count > 0) {
                ReportLogicCount row;
                row.section = section;
                row.count = count;
                reportPack.logicEntries.push_back(std::move(row));
            }
        };
        addCount("attachments", manifest.attachments.size());
        addCount("attenuations", manifest.attenuations.size());
        addCount("bindings", manifest.bindings.size());
        addCount("busEffects", manifest.busEffects.size());
        addCount("busSends", manifest.busSends.size());
        addCount("buses", manifest.buses.size());
        addCount("containers", manifest.containers.size());
        addCount("dynamicRoutes", manifest.dynamicRoutes.size());
        addCount("voiceRecipes", manifest.voiceRecipes.size());
    }

    // Cache policy: v2 caches by default (the canonical authoring input
    // earns the toolchain behavior); v1 caches only when --cache-path
    // opts in, so the original command line's file footprint gains only
    // the report. --no-cache wins everywhere.
    bool cacheEnabled = false;
    std::string cachePath;
    if (manifest.version == 3) {
        cacheEnabled = manifest.cacheEnabled && !request.disableCache;
        if (!request.cachePath.empty()) {
            cachePath = request.cachePath;
        } else if (!manifest.cachePath.empty()) {
            cachePath = harkenstudiojson::packc::ResolveAgainst(
                harkenstudiojson::packc::DirectoryOf(manifest.manifestPath),
                manifest.cachePath);
        } else {
            cachePath = outputResolved + ".cache.json";
        }
    } else {
        cacheEnabled =
            !request.cachePath.empty() && !request.disableCache;
        cachePath = request.cachePath;
    }

    std::vector<CacheRecord> oldRecords;
    PreviousPack previous;
    if (cacheEnabled) {
        std::string ignoreReason;
        const bool loaded = harkenstudiojson::packc::LoadCacheFile(
            cachePath, &oldRecords, &ignoreReason);
        if (!loaded) {
            oldRecords.clear();
            if (!ignoreReason.empty()) {
                ReportWarning warning;
                warning.kind = "cache-ignored";
                warning.source = manifest.manifestPath;
                warning.reason = ignoreReason;
                report->warnings.push_back(std::move(warning));
                result->diagnosticLog +=
                    "packc: note: " + ignoreReason + " — building clean\n";
            }
        } else {
            harkenstudiojson::packc::LoadPreviousPack(outputResolved, &previous,
                                               &ignoreReason);
            if (!previous.valid) {
                ReportWarning warning;
                warning.kind = "cache-ignored";
                warning.source = manifest.manifestPath;
                warning.reason = ignoreReason;
                report->warnings.push_back(std::move(warning));
            }
        }
    }
    std::unordered_map<std::string, const CacheRecord*> recordByIdentity;
    for (const CacheRecord& record : oldRecords) {
        recordByIdentity[CacheIdentityKey(
            record.sourcePath, record.downmixPolicy,
            record.downmixAlgorithmVersion)] = &record;
    }

    std::vector<PackEntry> entries;
    entries.reserve(manifest.entries.size());
    for (std::size_t i = 0; i < manifest.entries.size(); ++i) {
        const ManifestEntry& source = manifest.entries[i];
        PackEntry entry = MakePackEntry(source);

        if (cacheEnabled) {
            std::vector<std::uint8_t> raw;
            if (!harkenstudiojson::packc::ReadBinaryFile(source.resolvedPath,
                                                  &raw)) {
                return Fail(diag, "cannot read source '%s'",
                            source.resolvedPath.c_str());
            }
            entry.hashed = true;
            entry.sourceBytes = raw.size();
            entry.sourceHash = harkenstudiobuild::Fnv1a64(raw.data(), raw.size());

            // A hit requires an explicit id (identity must never derive
            // through the cache), a full parameter match, and a
            // checksum-verified blob in the previous output.
            const std::string downmixPolicy =
                CacheDownmixPolicy(source.downmix);
            const auto found = recordByIdentity.find(CacheIdentityKey(
                source.path, downmixPolicy,
                harkenstudiojson::packc::kDownmixAlgorithmVersion));
            if (source.explicitId &&
                found != recordByIdentity.end()) {
                const CacheRecord& record = *found->second;
                const std::uint64_t expectedBlobBytes =
                    record.frameCount * record.channelCount *
                    sizeof(float);
                if (record.sourceBytes == entry.sourceBytes &&
                    record.sourceHash == entry.sourceHash &&
                    record.codec == source.codec &&
                    record.downmixPolicy == downmixPolicy &&
                    record.downmixAlgorithmVersion ==
                        harkenstudiojson::packc::kDownmixAlgorithmVersion &&
                    (source.downmix.empty() ||
                     record.sourceChannelCount <= 2) &&
                    record.blobBytes == expectedBlobBytes) {
                    const std::uint8_t* blob =
                        harkenstudiojson::packc::FindVerifiedBlob(
                            previous, source.soundId, record);
                    if (blob != nullptr) {
                        entry.sampleRate = record.sampleRate;
                        entry.channelCount = record.channelCount;
                        entry.sourceChannelCount =
                            record.sourceChannelCount;
                        entry.frameCount = record.frameCount;
                        entry.planar.resize(static_cast<std::size_t>(
                            record.blobBytes / sizeof(float)));
                        std::memcpy(entry.planar.data(), blob,
                                    static_cast<std::size_t>(
                                        record.blobBytes));
                        // The output peak is remeasured over the lifted
                        // floats. The source peak is retained beside the
                        // exact source hash so cancellation warnings stay
                        // stable on cache hits without decoding again.
                        entry.peakAbs = MeasurePeak(entry.planar);
                        entry.sourcePeakAbs = record.sourcePeakAbs;
                        entry.downmixed = !entry.downmix.empty() &&
                                          entry.sourceChannelCount == 2;
                        entry.skippedByCache = true;
                    }
                }
            }
        }
        if (!entry.skippedByCache) {
            if (!DecodeToPlanar(&entry, diag, result)) {
                return false;
            }
        }
        if (!ValidateLoopPoints(entry, i, diag)) {
            return false;
        }
        entries.push_back(std::move(entry));
    }

    // Duplicate ids cannot coexist in one registry, so they cannot
    // coexist in one pack; refusing at build time names both entries.
    // (v2 refuses at parse time already; this pass covers v1's minted
    // ids and stays as the single authority either way.)
    for (std::size_t i = 0; i < entries.size(); ++i) {
        for (std::size_t j = i + 1; j < entries.size(); ++j) {
            if (entries[i].soundId == entries[j].soundId) {
                return Fail(
                    diag,
                    "sounds[%zu] '%s' and sounds[%zu] '%s' resolve to "
                    "the same soundId %s — give one an explicit "
                    "distinct \"id\"",
                    i, entries[i].path.c_str(), j,
                    entries[j].path.c_str(),
                    harkenstudio::ObjectIdString(entries[i].soundId).c_str());
            }
        }
    }

    std::vector<std::uint8_t> packBytes;
    if (v2Container) {
        if (!BuildPackBytesV2(manifest, entries, &packBytes, diag)) {
            return false;
        }
    } else {
        if (!BuildPackBytes(entries, &packBytes, diag)) {
            return false;
        }
    }
    if (!WriteWholeFile(outputResolved, packBytes.data(),
                        packBytes.size())) {
        return Fail(diag, "cannot write output '%s'",
                    outputResolved.c_str());
    }

    // Report facts read back from the emitted image (manifest order ==
    // TOC order), so the report describes the actual output bytes. A
    // v2 image exposes the same 68-byte media records through its
    // directory — media is the lowest entry kind, so the canonical
    // order puts those records first, in manifest order — and the
    // directory checksum stands in for the TOC checksum.
    std::uint64_t totalEntryCount = entries.size();
    std::vector<harkenstudiobuild::PackTocEntry> tocReadback;
    tocReadback.reserve(entries.size());
    if (v2Container) {
        harkenstudiobuild::PackV2Header v2Header;
        harkenstudiobuild::ParsePackV2Header(packBytes.data(), &v2Header);
        reportPack.fileBytes = v2Header.fileBytes;
        reportPack.tocChecksum = v2Header.directoryChecksum;
        totalEntryCount = v2Header.entryCount;
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const harkenstudiobuild::PackV2DirectoryEntry dirEntry =
                harkenstudiobuild::ParsePackV2DirectoryEntry(
                    packBytes.data() + harkenstudiobuild::kPackV2HeaderBytes +
                    i * harkenstudiobuild::kPackV2DirectoryEntryBytes);
            tocReadback.push_back(harkenstudiobuild::ParsePackTocEntry(
                packBytes.data() + dirEntry.offset));
        }
    } else {
        harkenstudiobuild::PackHeader header;
        harkenstudiobuild::ParsePackHeader(packBytes.data(), &header);
        reportPack.fileBytes = header.fileBytes;
        reportPack.tocChecksum = header.tocChecksum;
        for (std::size_t i = 0; i < entries.size(); ++i) {
            tocReadback.push_back(harkenstudiobuild::ParsePackTocEntry(
                packBytes.data() + header.tocOffset +
                i * harkenstudiobuild::kPackTocEntryBytes));
        }
    }
    std::vector<CacheRecord> newRecords;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const harkenstudiobuild::PackTocEntry toc = tocReadback[i];
        const PackEntry& entry = entries[i];
        ReportEntry reportEntry;
        reportEntry.id = toc.soundId;
        reportEntry.source = entry.sourceText;
        reportEntry.frames = toc.frameCount;
        reportEntry.rate = toc.sampleRate;
        reportEntry.channels = toc.channelCount;
        reportEntry.checksum = toc.blobChecksum;
        reportEntry.peakAbs = entry.peakAbs;
        reportEntry.silent =
            entry.peakAbs <= harkenstudiojson::packc::kSilenceFloorPeak;
        reportEntry.clipped =
            entry.peakAbs > harkenstudiojson::packc::kClipPeak;
        reportEntry.skippedByCache = entry.skippedByCache;
        reportPack.entries.push_back(reportEntry);
        if (entry.skippedByCache) {
            ++reportPack.cacheHits;
        } else {
            ++reportPack.cacheMisses;
        }
        AddPeakWarnings(entry, &reportPack);

        if (cacheEnabled && entry.hashed) {
            CacheRecord record;
            record.sourcePath = entry.sourceText;
            record.sourceBytes = entry.sourceBytes;
            record.sourceHash = entry.sourceHash;
            record.codec = entry.codec;
            record.downmixPolicy = CacheDownmixPolicy(entry.downmix);
            record.downmixAlgorithmVersion =
                harkenstudiojson::packc::kDownmixAlgorithmVersion;
            record.sourceChannelCount = entry.sourceChannelCount;
            record.sourcePeakAbs = entry.sourcePeakAbs;
            record.sampleRate = toc.sampleRate;
            record.channelCount = toc.channelCount;
            record.frameCount = toc.frameCount;
            record.blobBytes = toc.blobBytes;
            record.blobChecksum = toc.blobChecksum;
            newRecords.push_back(std::move(record));
        }
    }
    if (cacheEnabled) {
        const std::string cacheText =
            harkenstudiojson::packc::EmitCacheJson(newRecords);
        if (!WriteWholeFile(cachePath, cacheText.data(),
                            cacheText.size())) {
            // The cache is an accelerator: failing to refresh it warns
            // and never fails a build whose output already landed.
            ReportWarning warning;
            warning.kind = "cache-ignored";
            warning.source = manifest.manifestPath;
            warning.reason = "cache file could not be written";
            report->warnings.push_back(std::move(warning));
            result->diagnosticLog +=
                "packc: note: cannot write cache '" + cachePath + "'\n";
        }
    }
    report->packs.push_back(std::move(reportPack));

    result->outputLog +=
        "packc: wrote " + std::to_string(static_cast<unsigned>(totalEntryCount))
        + " entr" + (totalEntryCount == 1 ? "y" : "ies") + " ("
        + std::to_string(packBytes.size()) + " bytes) -> " + outputText + "\n";
    return true;
}

// Builds the single-sound .stream (v1 manifests only; no cache — the
// container holds one sound and rebuilds are already decode-bound).
bool BuildOneStream(const Manifest& manifest, const std::string& outputText,
                    Report* report, Diag* diag, Result* result) {
    ReportPack reportPack;
    reportPack.manifest = manifest.manifestPath;
    reportPack.manifestVersion = manifest.version;
    reportPack.output = outputText;
    reportPack.container = "stream";

    std::uint32_t chunkFrames = kDefaultChunkFrames;
    if (manifest.chunkFrames != 0) {
        chunkFrames = manifest.chunkFrames;
    }
    if (manifest.chunkFrames > harkenstudiobuild::kMaxStreamChunkFrames) {
        return Fail(diag,
                    "manifest \"chunkFrames\" must be an integer in "
                    "[0, %u] (0 selects the default %u)",
                    harkenstudiobuild::kMaxStreamChunkFrames, kDefaultChunkFrames);
    }
    if (manifest.entries.size() != 1) {
        return Fail(diag,
                    "a .stream container carries exactly one sound; "
                    "manifest lists %zu",
                    manifest.entries.size());
    }
    const ManifestEntry& source = manifest.entries[0];
    PackEntry entry = MakePackEntry(source);
    if (!DecodeToPlanar(&entry, diag, result)) {
        return false;
    }
    if (!ValidateLoopPoints(entry, 0, diag)) {
        return false;
    }
    std::vector<std::uint8_t> streamBytes;
    if (!BuildStreamBytes(entry, chunkFrames, &streamBytes, diag)) {
        return false;
    }
    if (!WriteWholeFile(outputText, streamBytes.data(),
                        streamBytes.size())) {
        return Fail(diag, "cannot write output '%s'",
                    outputText.c_str());
    }

    harkenstudiobuild::StreamHeader header;
    harkenstudiobuild::ParseStreamHeader(streamBytes.data(), &header);
    reportPack.fileBytes = header.fileBytes;
    reportPack.tocChecksum = header.tocChecksum;
    ReportEntry reportEntry;
    reportEntry.id = entry.soundId;
    reportEntry.source = entry.sourceText;
    reportEntry.frames = entry.frameCount;
    reportEntry.rate = entry.sampleRate;
    reportEntry.channels = entry.channelCount;
    // Uniform meaning across containers: the checksum of the canonical
    // planar blob (for packs it equals the TOC blobChecksum).
    reportEntry.checksum = harkenstudiobuild::Fnv1a64(
        entry.planar.data(), entry.planar.size() * sizeof(float));
    reportEntry.peakAbs = entry.peakAbs;
    reportEntry.silent =
        entry.peakAbs <= harkenstudiojson::packc::kSilenceFloorPeak;
    reportEntry.clipped = entry.peakAbs > harkenstudiojson::packc::kClipPeak;
    reportEntry.skippedByCache = false;
    ++reportPack.cacheMisses;
    reportPack.entries.push_back(reportEntry);
    AddPeakWarnings(entry, &reportPack);
    report->packs.push_back(std::move(reportPack));

    result->outputLog += "packc: wrote 1 entry (" + std::to_string(streamBytes.size())
        + " bytes) -> " + outputText + "\n";
    return true;
}

// Ids declared by more than one manifest of this invocation cannot
// co-load into one engine; the report warns without failing the build
// (the packs may be alternatives, not companions).
void WarnDuplicateIdsAcrossManifests(Report* report) {
    struct Seen {
        harkenstudio::ObjectId id;
        std::vector<std::string> manifests;
    };
    std::vector<Seen> seen;
    for (const ReportPack& pack : report->packs) {
        for (const ReportEntry& entry : pack.entries) {
            Seen* record = nullptr;
            for (Seen& candidate : seen) {
                if (candidate.id == entry.id) {
                    record = &candidate;
                    break;
                }
            }
            if (record == nullptr) {
                seen.push_back({entry.id, {pack.manifest}});
                continue;
            }
            bool known = false;
            for (const std::string& manifest : record->manifests) {
                if (manifest == pack.manifest) {
                    known = true;
                    break;
                }
            }
            if (!known) {
                record->manifests.push_back(pack.manifest);
            }
        }
    }
    for (const Seen& record : seen) {
        if (record.manifests.size() < 2) {
            continue;
        }
        ReportWarning warning;
        warning.kind = "duplicate-id-across-manifests";
        warning.hasId = true;
        warning.id = record.id;
        warning.manifests = record.manifests;
        report->warnings.push_back(std::move(warning));
    }
}

// Resolution order for the report location: the flag wins; then the
// (single) v2 manifest's option; then beside the first output; last,
// with no output known (early refusals), beside the first manifest.
std::string ResolveReportPath(const Request& request,
                              const std::vector<Manifest>& manifests) {
    if (!request.reportPath.empty()) {
        return request.reportPath;
    }
    if (manifests.size() == 1 &&
        !manifests[0].reportPath.empty()) {
        return harkenstudiojson::packc::ResolveAgainst(
            harkenstudiojson::packc::DirectoryOf(manifests[0].manifestPath),
            manifests[0].reportPath);
    }
    if (!manifests.empty() &&
        !manifests[0].resolvedOutputPath.empty()) {
        return manifests[0].resolvedOutputPath + ".report.json";
    }
    if (!request.manifestPaths.empty()) {
        return request.manifestPaths.front() + ".report.json";
    }
    return "packc.report.json";
}

Result FinishWithReport(const Report& report, const std::string& reportPath,
                        const Diag& diag, Result result) {
    result.reportPath = reportPath;
    if (report.refused) {
        result.error = diag.message;
        result.diagnosticLog += "error: " + diag.message + "\n";
    }
    const std::string reportText = harkenstudiojson::packc::EmitReportJson(report);
    if (!WriteWholeFile(reportPath, reportText.data(), reportText.size())) {
        result.error = "cannot write report '" + reportPath + "'";
        result.diagnosticLog += "error: " + result.error + "\n";
        return result;
    }
    result.success = !report.refused;
    return result;
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
    result.checksum = harkenstudiobuild::Fnv1a64(result.samples->data(), bytes);
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
    using namespace harkenstudiojson::packc;
    entries->clear();
    Diag diag;
    const auto refuse = [&]() {
        if (error) *error = diag.message;
        return false;
    };
    harkenstudiojson::JsonValue root;
    if (!harkenstudiojson::ParseJson(logicSections, &root, &diag.message))
        return refuse();
    if (root.kind != harkenstudiojson::JsonValue::Kind::Object) {
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
    std::unordered_set<harkenstudio::ObjectId> identities;
    for (std::size_t i = 0; i < media.size(); ++i) {
        const auto& input = media[i];
        if (harkenstudio::IsNull(input.id) || !identities.insert(input.id).second) {
            Fail(&diag, "media[%zu] has a missing or duplicate identity", i);
            return refuse();
        }
        ManifestEntry source;
        source.soundId = input.id;
        source.explicitId = true;
        source.path = input.sourcePath;
        source.resolvedPath = input.sourcePath;
        source.codec = "pcm-f32";
        source.downmix = input.downmixToMono ? "mono" : "";
        source.loopable = input.loopable;
        source.loopStartFrame = input.loopStartFrame;
        source.loopEndFrame = input.loopEndFrame;
        PackEntry entry = MakePackEntry(source);
        if (!PrepareAudioLayout(&entry, input.interleavedSamples, input.sampleCount,
                                input.channels, input.sampleRate, &diag)
            || !ValidateLoopPoints(entry, i, &diag)) return refuse();
        manifest.entries.push_back(std::move(source));
        prepared.push_back(std::move(entry));

        EntryPlacement placement;
        placement.kind = harkenstudiobuild::kPackEntryKindMedia;
        placement.id = input.id;
        placement.index = i;
        placement.size = harkenstudiobuild::kPackV2MediaPayloadBytes;
        CompiledEntry resource;
        resource.kind = placement.kind;
        resource.vintage = EntryVintage(placement.kind);
        resource.id = placement.id;
        const auto audio = mediaCache->prepare(input);
        resource.media = audio.samples;
        resource.payload.resize(static_cast<std::size_t>(placement.size));
        if (!SerializeEntryPayload(manifest, prepared, placement, 0, nullptr,
                                   resource.payload.data(), &diag, &audio.checksum)) return refuse();
        compiled.push_back(std::move(resource));
    }
    if (!ValidateLogicSections(manifest, &diag)) return refuse();

    const auto placements = BuildEntryPlacements(manifest, prepared);
    compiled.reserve(placements.size());
    for (const auto& placement : placements) {
        if (placement.kind == harkenstudiobuild::kPackEntryKindMedia) continue;
        CompiledEntry entry;
        entry.kind = placement.kind;
        entry.vintage = EntryVintage(placement.kind);
        entry.id = placement.id;
        entry.relationOrdinal = harkenstudio::IsNull(placement.id) ? placement.index : 0;
        entry.payload.resize(static_cast<std::size_t>(placement.size));
        if (!SerializeEntryPayload(manifest, prepared, placement, 0, nullptr,
                                   entry.payload.data(), &diag)) return refuse();
        compiled.push_back(std::move(entry));
    }
    *entries = std::move(compiled);
    return true;
}

Result compile(const Request& request) {
    Result result;
    if (request.manifestPaths.empty()) {
        result.error = "No resource manifests were supplied";
        result.diagnosticLog = "error: " + result.error + "\n";
        return result;
    }
    const bool streamMode = request.container == Container::Stream;
    Report report;
    Diag diag;

    std::string firstText;
    if (!ReadTextFile(request.manifestPaths.front(), &firstText)) {
        Fail(&diag, "cannot read manifest '%s'",
             request.manifestPaths.front().c_str());
        report.refused = true;
        report.refusalMessage = diag.message;
        report.refusalManifest = request.manifestPaths.front();
        return FinishWithReport(report, ResolveReportPath(request, {}),
                                diag, std::move(result));
    }
    std::vector<Manifest> manifests;
    auto refuse = [&](const std::string& manifestPath) {
        report.refused = true;
        report.refusalMessage = diag.message;
        report.refusalManifest = manifestPath;
        return FinishWithReport(report, ResolveReportPath(request, manifests),
                                diag, std::move(result));
    };
    for (std::size_t i = 0; i < request.manifestPaths.size(); ++i) {
        std::string text = firstText;
        if (i > 0 && !ReadTextFile(request.manifestPaths[i], &text)) {
            Fail(&diag, "cannot read manifest '%s'", request.manifestPaths[i].c_str());
            return refuse(request.manifestPaths[i]);
        }
        Manifest manifest;
        if (!harkenstudiojson::packc::ParseManifestText(
                text, request.manifestPaths[i], streamMode, &manifest, &diag))
            return refuse(request.manifestPaths[i]);
        for (const auto& previous : manifests) {
            if (previous.resolvedOutputPath == manifest.resolvedOutputPath) {
                Fail(&diag, "manifests '%s' and '%s' name the same output '%s'",
                     previous.manifestPath.c_str(), manifest.manifestPath.c_str(),
                     manifest.outputPath.c_str());
                return refuse(manifest.manifestPath);
            }
        }
        manifests.push_back(std::move(manifest));
    }
    if (manifests.size() > 1 &&
        (!request.cachePath.empty() || !request.emitManifestPath.empty())) {
        Fail(&diag, "--cache-path and --emit-manifest apply to a single manifest only");
        return refuse(manifests.front().manifestPath);
    }
    if (streamMode && !request.cachePath.empty()) {
        Fail(&diag, "--cache-path does not apply to --stream builds");
        return refuse(manifests[0].manifestPath);
    }
    if (streamMode && !request.emitManifestPath.empty()) {
        Fail(&diag,
             "--emit-manifest does not apply to --stream builds — v2 "
             "manifests describe .pack builds only");
        return refuse(manifests[0].manifestPath);
    }

    for (Manifest& manifest : manifests) {
        const std::string& outputText = manifest.outputPath;
        const std::string& outputResolved = manifest.resolvedOutputPath;
        bool ok = false;
        if (streamMode) {
            ok = BuildOneStream(manifest, outputResolved, &report, &diag, &result);
        } else {
            ok = BuildOnePack(manifest, request, outputText, outputResolved,
                              &report, &diag, &result);
        }
        if (!ok) {
            return refuse(manifest.manifestPath);
        }
    }

    WarnDuplicateIdsAcrossManifests(&report);

    if (!request.emitManifestPath.empty()) {
        const std::string manifestText =
            harkenstudiojson::packc::EmitManifestV3(manifests.front());
        if (!WriteWholeFile(request.emitManifestPath, manifestText.data(),
                            manifestText.size())) {
            Fail(&diag, "cannot write manifest '%s'",
                 request.emitManifestPath.c_str());
            return refuse(manifests[0].manifestPath);
        }
    }

    return FinishWithReport(
        report, ResolveReportPath(request, manifests), diag, std::move(result));
}

}
