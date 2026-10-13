// packc: the .pack / .stream compiler. Reads a manifest describing wav
// sources, decodes every source to the canonical float32 planar form,
// and writes one container per manifest. Host-side tooling; ships with
// the repository, not with the engine library.
//
// Manifest generations (full logic grammar in PackcManifest.h):
//   v1  packc <manifest.json> <output.pack>            (many sounds)
//       packc --stream <manifest.json> <output.stream> (one sound)
//   v2  packc <manifest.json> [<manifest2.json> ...]
// A v2 manifest ("manifestVersion": 2) carries its own output path and
// build options, so it is passed alone; several v2 manifests build
// several packs in one invocation. The generation is detected from the
// manifest content, never from argument form, so the v1 command line
// keeps working unchanged.
//
// A v2 manifest additionally carrying "packKind" ("resident" |
// "content") compiles to the formatVersion 2 container
// (PackFormatV2.h): typed logic entries — buses, sends, attenuation
// profiles, discrete declarations, meter wirings, bindings,
// attachments and containers beside the media entries, under
// the spec section 19.1 kind discipline. packKind absent keeps the v1
// container output byte-identical to the pre-logic tool.
//
// Flags (never required by the v1 forms):
//   --report <path>         validation report location override
//   --cache-path <path>     build-cache location (enables caching for v1)
//   --no-cache              disable the build cache
//   --emit-manifest <path>  write the build as a normalized v2 manifest
//
// Every build writes a machine-readable JSON validation report
// (PackcReport.h), by default beside the output; a refused build
// reports the refusal reason. Exit codes: 0 = all outputs written and
// the report emitted; 1 = refused (the report says why) or the report
// itself could not be written. Argument-shape errors (unknown flag,
// wrong positional count) print usage and exit 1 without a report — no
// manifest context exists to report against.
//
// Incremental rebuilds (PackcCache.h): with caching enabled, entries
// whose source bytes and build parameters are unchanged skip the decode
// pipeline and lift their canonical blobs out of the previous output
// after checksum verification. Assembly always runs the same layout
// code, so the emitted container is byte-identical to a clean build of
// the same inputs; a missing or corrupt cache degrades to a clean
// build, never to a wrong pack.
//
// Output is deterministic for identical input: entries in manifest
// order, TOC at a fixed offset, blobs packed in order at 64-byte
// alignment with zeroed padding, no timestamps — identical manifests
// and sources produce byte-identical containers, so outputs can be
// diffed and cached.
#include "JsonReader.h"
#include <harken_studio_build/AuthoredParameters.h>
#include "PackcCache.h"
#include "PackcManifest.h"
#include "PackcReport.h"
#include "WavLoader.h"

#include "harken_studio_build/PackFormat.h"
#include "harken_studio_build/PackFormatV2.h"
#include "harken_studio_build/StreamFormat.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <unordered_map>
#include <vector>

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

void PrintUsage() {
    std::fprintf(
        stderr,
        "usage: packc [--stream] <manifest.json> [<manifest.json> ...]\n"
        "\n"
        "flags: --report <path>        validation report location\n"
        "       --cache-path <path>    build-cache location\n"
        "       --no-cache             disable the build cache\n"
        "       --emit-manifest <path> write the normalized manifest\n"
        "\n"
        "Builds .pack containers (or, with --stream, a single-sound\n"
        "streaming .stream container) from the wav sources the manifest\n"
        "lists. A manifest (\"manifestVersion\": 3) names its own\n"
        "output; every build writes a JSON validation report. Output is\n"
        "byte-stable for identical input.\n");
}

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

// Decodes the entry's wav and converts it at the shared pack/stream
// boundary to canonical float32 planar samples. A requested stereo
// downmix performs one double-precision sum and one final float rounding
// per frame. Source and output peaks are measured for report warnings.
// Wide decode is opted into only for downmix entries, whose policy check
// below owns the >2-channel refusal; a preserve entry keeps LoadWav's
// mono/stereo bound so the build fails loudly at decode.
bool DecodeToPlanar(PackEntry* entry, Diag* diag) {
    harkenstudiojson::LoadedWav wav;
    if (!harkenstudiojson::LoadWav(entry->path, &wav,
                            !entry->downmix.empty())) {
        // LoadWav printed the specific reason to stderr already; the
        // report carries the summary form.
        return Fail(diag, "cannot open or decode '%s'",
                    entry->path.c_str());
    }
    entry->sampleRate = wav.sampleRate;
    entry->sourceChannelCount =
        static_cast<std::uint16_t>(wav.channels);
    entry->frameCount = wav.frames;
    const std::size_t frames = static_cast<std::size_t>(wav.frames);
    if (!entry->downmix.empty() && wav.channels > 2) {
        return Fail(diag,
                    "cannot downmix '%s' to mono: source has %u "
                    "channels; only mono or stereo is supported",
                    entry->path.c_str(), wav.channels);
    }

    if (!entry->downmix.empty() && wav.channels == 2) {
        entry->channelCount = 1;
        entry->downmixed = true;
        entry->planar.resize(frames);
        float sourcePeak = 0.0f;
        float peak = 0.0f;
        for (std::size_t n = 0; n < frames; ++n) {
            const float left = wav.samples[n * 2];
            const float right = wav.samples[n * 2 + 1];
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

    entry->channelCount = static_cast<std::uint16_t>(wav.channels);
    entry->planar.resize(wav.samples.size());
    float peak = 0.0f;
    for (std::uint32_t ch = 0; ch < wav.channels; ++ch) {
        float* plane = entry->planar.data() +
                       static_cast<std::size_t>(ch) * frames;
        for (std::size_t n = 0; n < frames; ++n) {
            const float sample = wav.samples[n * wav.channels + ch];
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
bool BuildPackBytesV2(const Manifest& manifest,
                      const std::vector<PackEntry>& entries,
                      std::vector<std::uint8_t>* out, Diag* diag) {
    struct Placement {
        std::uint32_t kind = 0;
        harkenstudio::ObjectId id{};
        std::size_t index = 0; // position within its manifest section
        std::uint64_t offset = 0;
        std::uint64_t size = 0;
    };
    std::vector<Placement> placements;
    const auto place = [&placements](std::uint32_t kind,
                                     harkenstudio::ObjectId id, std::size_t index,
                                     std::uint64_t size) {
        Placement placement;
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

    const std::uint32_t entryCount =
        static_cast<std::uint32_t>(placements.size());
    const std::uint64_t directoryBytes =
        static_cast<std::uint64_t>(entryCount) *
        harkenstudiobuild::kPackV2DirectoryEntryBytes;
    std::uint64_t cursor = harkenstudiobuild::kPackV2HeaderBytes + directoryBytes;
    for (Placement& placement : placements) {
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

    for (const Placement& placement : placements) {
        std::uint8_t* dst = out->data() + placement.offset;
        switch (placement.kind) {
        case harkenstudiobuild::kPackEntryKindMedia: {
            const PackEntry& entry = entries[placement.index];
            const std::uint64_t blobBytes =
                static_cast<std::uint64_t>(entry.planar.size()) *
                sizeof(float);
            harkenstudiobuild::PackTocEntry toc;
            toc.soundId = entry.soundId;
            toc.sampleRate = entry.sampleRate;
            toc.channelCount = entry.channelCount;
            toc.sampleFormat = harkenstudiobuild::kPackSampleFormatF32Planar;
            toc.frameCount = entry.frameCount;
            toc.blobOffset = blobOffsets[placement.index];
            toc.blobBytes = blobBytes;
            toc.blobChecksum = harkenstudiobuild::Fnv1a64(
                out->data() + blobOffsets[placement.index],
                static_cast<std::size_t>(blobBytes));
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
    }

    std::vector<std::uint8_t> directoryImage(
        static_cast<std::size_t>(directoryBytes), 0);
    for (std::size_t i = 0; i < placements.size(); ++i) {
        const Placement& placement = placements[i];
        harkenstudiobuild::PackV2DirectoryEntry entry;
        entry.kind = placement.kind;
        const bool dynamicDeploymentEntry =
            placement.kind == harkenstudiobuild::kPackEntryKindBusEffect ||
            placement.kind == harkenstudiobuild::kPackEntryKindEffectDynamicRoute;
        if (dynamicDeploymentEntry) {
            entry.vintage = harkenstudiobuild::kPackV2DynamicSchemaVintage;
        } else if (placement.kind == harkenstudiobuild::kPackEntryKindBus) {
            entry.vintage = harkenstudiobuild::kPackV2BusMixSchemaVintage;
        } else if (placement.kind == harkenstudiobuild::kPackEntryKindVoiceRecipe) {
            entry.vintage = harkenstudiobuild::kPackV2PlaybackRecipeSchemaVintage;
        } else if (placement.kind == harkenstudiobuild::kPackEntryKindAttenuation) {
            entry.vintage = harkenstudiobuild::kPackV2AuthoredToneSchemaVintage;
        } else {
            entry.vintage = harkenstudiobuild::kPackV2SchemaVintage;
        }
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


struct CliOptions {
    bool streamMode = false;
    bool noCache = false;
    std::vector<std::string> positionals;
    std::string reportPath;
    std::string cachePathOverride;
    std::string emitManifestPath;
};

// Returns false on an argument-shape error (usage is the reply; no
// manifest context exists yet to report against).
bool ParseArgs(int argc, char** argv, CliOptions* out) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--stream") {
            out->streamMode = true;
        } else if (arg == "--no-cache") {
            out->noCache = true;
        } else if (arg == "--report" || arg == "--cache-path" ||
                   arg == "--emit-manifest") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: %s needs a path value\n",
                             arg.c_str());
                return false;
            }
            const std::string value = argv[++i];
            if (arg == "--report") {
                out->reportPath = value;
            } else if (arg == "--cache-path") {
                out->cachePathOverride = value;
            } else {
                out->emitManifestPath = value;
            }
        } else if (arg.size() >= 2 && arg[0] == '-' && arg[1] == '-') {
            std::fprintf(stderr, "error: unknown flag %s\n",
                         arg.c_str());
            return false;
        } else {
            out->positionals.push_back(arg);
        }
    }
    return !out->positionals.empty();
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
bool BuildOnePack(const Manifest& manifest, const CliOptions& cli,
                  const std::string& outputText,
                  const std::string& outputResolved, Report* report,
                  Diag* diag) {
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
        cacheEnabled = manifest.cacheEnabled && !cli.noCache;
        if (!cli.cachePathOverride.empty()) {
            cachePath = cli.cachePathOverride;
        } else if (!manifest.cachePath.empty()) {
            cachePath = harkenstudiojson::packc::ResolveAgainst(
                harkenstudiojson::packc::DirectoryOf(manifest.manifestPath),
                manifest.cachePath);
        } else {
            cachePath = outputResolved + ".cache.json";
        }
    } else {
        cacheEnabled =
            !cli.cachePathOverride.empty() && !cli.noCache;
        cachePath = cli.cachePathOverride;
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
                std::fprintf(stderr, "packc: note: %s — building clean\n",
                             ignoreReason.c_str());
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
            if (!DecodeToPlanar(&entry, diag)) {
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
            std::fprintf(stderr,
                         "packc: note: cannot write cache '%s'\n",
                         cachePath.c_str());
        }
    }
    report->packs.push_back(std::move(reportPack));

    std::printf("packc: wrote %u entr%s (%llu bytes) -> %s\n",
                static_cast<unsigned>(totalEntryCount),
                totalEntryCount == 1 ? "y" : "ies",
                static_cast<unsigned long long>(packBytes.size()),
                outputText.c_str());
    return true;
}

// Builds the single-sound .stream (v1 manifests only; no cache — the
// container holds one sound and rebuilds are already decode-bound).
bool BuildOneStream(const Manifest& manifest, const std::string& outputText,
                    Report* report, Diag* diag) {
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
    if (!DecodeToPlanar(&entry, diag)) {
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

    std::printf("packc: wrote 1 entry (%llu bytes) -> %s\n",
                static_cast<unsigned long long>(streamBytes.size()),
                outputText.c_str());
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
std::string ResolveReportPath(const CliOptions& cli,
                              const std::vector<Manifest>& manifests) {
    if (!cli.reportPath.empty()) {
        return cli.reportPath;
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
    if (!cli.positionals.empty()) {
        return cli.positionals.front() + ".report.json";
    }
    return "packc.report.json";
}

int FinishWithReport(const Report& report, const std::string& reportPath,
                     const Diag& diag) {
    if (report.refused) {
        std::fprintf(stderr, "error: %s\n", diag.message.c_str());
    }
    const std::string reportText =
        harkenstudiojson::packc::EmitReportJson(report);
    if (!WriteWholeFile(reportPath, reportText.data(),
                        reportText.size())) {
        // The report is the contract's feedback surface; a build whose
        // feedback cannot land is not a finished build.
        std::fprintf(stderr, "error: cannot write report '%s'\n",
                     reportPath.c_str());
        return 1;
    }
    return report.refused ? 1 : 0;
}

} // namespace

int RunPackc(int argc, char** argv) {
    CliOptions cli;
    if (!ParseArgs(argc, argv, &cli)) {
        PrintUsage();
        return 1;
    }
    Report report;
    Diag diag;

    std::string firstText;
    if (!ReadTextFile(cli.positionals.front(), &firstText)) {
        Fail(&diag, "cannot read manifest '%s'",
             cli.positionals.front().c_str());
        report.refused = true;
        report.refusalMessage = diag.message;
        report.refusalManifest = cli.positionals.front();
        return FinishWithReport(report, ResolveReportPath(cli, {}),
                                diag);
    }
    std::vector<Manifest> manifests;
    auto refuse = [&](const std::string& manifestPath) {
        report.refused = true;
        report.refusalMessage = diag.message;
        report.refusalManifest = manifestPath;
        return FinishWithReport(report, ResolveReportPath(cli, manifests), diag);
    };
    for (std::size_t i = 0; i < cli.positionals.size(); ++i) {
        std::string text = firstText;
        if (i > 0 && !ReadTextFile(cli.positionals[i], &text)) {
            Fail(&diag, "cannot read manifest '%s'", cli.positionals[i].c_str());
            return refuse(cli.positionals[i]);
        }
        Manifest manifest;
        if (!harkenstudiojson::packc::ParseManifestText(
                text, cli.positionals[i], cli.streamMode, &manifest, &diag))
            return refuse(cli.positionals[i]);
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
        (!cli.cachePathOverride.empty() || !cli.emitManifestPath.empty())) {
        Fail(&diag, "--cache-path and --emit-manifest apply to a single manifest only");
        return refuse(manifests.front().manifestPath);
    }
    if (cli.streamMode && !cli.cachePathOverride.empty()) {
        Fail(&diag, "--cache-path does not apply to --stream builds");
        return refuse(manifests[0].manifestPath);
    }
    if (cli.streamMode && !cli.emitManifestPath.empty()) {
        Fail(&diag,
             "--emit-manifest does not apply to --stream builds — v2 "
             "manifests describe .pack builds only");
        return refuse(manifests[0].manifestPath);
    }

    for (Manifest& manifest : manifests) {
        const std::string& outputText = manifest.outputPath;
        const std::string& outputResolved = manifest.resolvedOutputPath;
        bool ok = false;
        if (cli.streamMode) {
            ok = BuildOneStream(manifest, outputResolved, &report, &diag);
        } else {
            ok = BuildOnePack(manifest, cli, outputText, outputResolved,
                              &report, &diag);
        }
        if (!ok) {
            return refuse(manifest.manifestPath);
        }
    }

    WarnDuplicateIdsAcrossManifests(&report);

    if (!cli.emitManifestPath.empty()) {
        const std::string manifestText =
            harkenstudiojson::packc::EmitManifestV3(manifests.front());
        if (!WriteWholeFile(cli.emitManifestPath, manifestText.data(),
                            manifestText.size())) {
            Fail(&diag, "cannot write manifest '%s'",
                 cli.emitManifestPath.c_str());
            return refuse(manifests[0].manifestPath);
        }
    }

    return FinishWithReport(
        report, ResolveReportPath(cli, manifests), diag);
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> arguments;
    arguments.reserve(argc);
    for (int i = 0; i < argc; ++i) {
        arguments.push_back(harkenstudiojson::PathUtf8(std::filesystem::path(argv[i])));
    }
    std::vector<char*> utf8Argv;
    utf8Argv.reserve(argc + 1);
    for (auto& argument : arguments) {
        utf8Argv.push_back(argument.data());
    }
    utf8Argv.push_back(nullptr);
    return RunPackc(argc, utf8Argv.data());
}
#else
int main(int argc, char** argv) {
    return RunPackc(argc, argv);
}
#endif
