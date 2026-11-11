#pragma once

#include "JsonReader.h"
#include "StreamImage.h"
#include "WavLoader.h"
#include "harken/media/PackFormat.h"
#include "harken/media/PackFormatV2.h"
#include "harken/media/generated/harken_pack_effect_schema.g.h"
#include <harken/Harken.h>
#include <harken/ObjectId.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace harkentest {

namespace testpack {

using harkenhost::JsonValue;

struct Media {
    VnObjectId id{};
    std::string path;
    bool loopable = false;
    std::uint64_t loopStartFrame = 0;
    std::uint64_t loopEndFrame = 0;
    bool downmix = false;
    std::uint32_t sampleRate = 0;
    std::uint16_t channels = 0;
    std::uint64_t frames = 0;
    std::vector<float> planar;
};

struct Placement {
    std::uint32_t kind = 0;
    VnObjectId id{};
    std::uint64_t ordinal = 0;
    std::vector<std::uint8_t> payload;
    std::size_t media = SIZE_MAX;
};

struct NamedValue {
    const char* name;
    std::uint32_t value;
};

inline constexpr NamedValue kRolloffNames[] = {
    {"linear", VN_ROLLOFF_LINEAR},
    {"inverseSquare", VN_ROLLOFF_INVERSE_SQUARE},
    {"logarithmic", VN_ROLLOFF_LOGARITHMIC},
};

inline constexpr NamedValue kScopeNames[] = {
    {"voice", VN_TARGET_SCOPE_VOICE},
    {"bus", VN_TARGET_SCOPE_BUS},
    {"voiceEffectParam", VN_TARGET_SCOPE_VOICE_EFFECT_PARAM},
    {"busEffectParam", VN_TARGET_SCOPE_BUS_EFFECT_PARAM},
};

inline constexpr NamedValue kOrderNames[] = {
    {"weightedRandom", 0},
    {"roundRobin", 1},
};

inline constexpr NamedValue kTapNames[] = {
    {"postEffects", VN_DYNAMIC_TAP_BUS_POST_EFFECTS},
    {"preFader", VN_DYNAMIC_TAP_BUS_PRE_FADER},
    {"postFader", VN_DYNAMIC_TAP_BUS_POST_FADER},
};

class Reader {
public:
    bool ok = true;

    VnObjectId Id(const JsonValue& object, const char* key) {
        const JsonValue* value = object.Find(key);
        VnObjectId id{};
        if (value != nullptr &&
            (value->kind != JsonValue::Kind::String ||
             !harken::ParseObjectId(value->text, &id))) {
            ok = false;
        }
        return id;
    }

    float Float(const JsonValue& object, const char* key, float fallback = 0.0f) {
        const JsonValue* value = object.Find(key);
        return value == nullptr ? fallback : Number(*value);
    }

    float Number(const JsonValue& value) {
        float parsed = 0.0f;
        if (value.kind != JsonValue::Kind::Number ||
            std::from_chars(value.text.data(), value.text.data() + value.text.size(), parsed)
                    .ec != std::errc()) {
            ok = false;
        }
        return parsed;
    }

    std::uint64_t U64(const JsonValue& object, const char* key, std::uint64_t fallback = 0,
                      bool* present = nullptr) {
        const JsonValue* value = object.Find(key);
        if (present != nullptr) {
            *present = value != nullptr;
        }
        std::uint64_t parsed = fallback;
        if (value != nullptr && !value->AsU64(&parsed)) {
            ok = false;
        }
        return parsed;
    }

    std::uint32_t U32(const JsonValue& object, const char* key, std::uint32_t fallback = 0,
                      bool* present = nullptr) {
        return static_cast<std::uint32_t>(U64(object, key, fallback, present));
    }

    bool Bool(const JsonValue& object, const char* key) {
        const JsonValue* value = object.Find(key);
        return value != nullptr && value->kind == JsonValue::Kind::Bool && value->boolean;
    }

    template <std::size_t N>
    std::uint32_t Enum(const JsonValue& object, const char* key, const NamedValue (&names)[N],
                       std::uint32_t fallback = 0) {
        const JsonValue* value = object.Find(key);
        if (value == nullptr) {
            return fallback;
        }
        for (const NamedValue& named : names) {
            if (value->kind == JsonValue::Kind::String && value->text == named.name) {
                return named.value;
            }
        }
        ok = false;
        return fallback;
    }

    const std::vector<JsonValue>& Array(const JsonValue& object, const char* key) {
        static const std::vector<JsonValue> empty;
        const JsonValue* value = object.Find(key);
        if (value == nullptr) {
            return empty;
        }
        if (value->kind != JsonValue::Kind::Array) {
            ok = false;
            return empty;
        }
        return value->items;
    }

    template <class Point, std::size_t Points, class Handle, std::size_t Handles>
    std::uint32_t Curve(const JsonValue& curve, Point (&points)[Points],
                        Handle (&handles)[Handles]) {
        std::uint32_t count = 0;
        for (const JsonValue& pair : Array(curve, "points")) {
            if (pair.kind != JsonValue::Kind::Array || pair.items.size() != 2 || count >= Points) {
                ok = false;
                return count;
            }
            points[count].x = Number(pair.items[0]);
            points[count].y = Number(pair.items[1]);
            ++count;
        }
        std::size_t h = 0;
        for (const JsonValue& tuple : Array(curve, "handles")) {
            if (tuple.kind != JsonValue::Kind::Array || tuple.items.size() != 4 || h >= Handles) {
                ok = false;
                return count;
            }
            handles[h].h1x = Number(tuple.items[0]);
            handles[h].h1y = Number(tuple.items[1]);
            handles[h].h2x = Number(tuple.items[2]);
            handles[h].h2y = Number(tuple.items[3]);
            ++h;
        }
        return count;
    }

    std::uint32_t EffectType(const JsonValue& object) {
        const JsonValue* value = object.Find("type");
        const harken::PackEffectSchemaRow* row =
            value != nullptr && value->kind == JsonValue::Kind::String
                ? harken::FindPackEffectSchemaByManifestName(value->text.c_str())
                : nullptr;
        if (row == nullptr) {
            ok = false;
            return 0;
        }
        return row->effectType;
    }

    template <class Row>
    void Effect(const JsonValue& object, Row* row) {
        row->effectId = Id(object, "effectId");
        row->slot = U32(object, "slot");
        row->effectType = EffectType(object);
        const harken::PackEffectSchemaRow* schema =
            harken::FindPackEffectSchemaByType(row->effectType);
        row->descriptorVintage =
            U32(object, "descriptorVintage", schema == nullptr ? 0 : schema->descriptorVintage);
        row->bypass = Bool(object, "bypass") ? 1u : 0u;
        const auto& params = Array(object, "parameters");
        if (params.size() > row->params.size()) {
            ok = false;
            return;
        }
        row->paramCount = static_cast<std::uint32_t>(params.size());
        row->paramBytes = row->paramCount * sizeof(float);
        for (std::size_t p = 0; p < params.size(); ++p) {
            row->params[p] = Number(params[p]);
        }
    }
};

inline std::filesystem::path Utf8Path(const std::string& text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

inline std::vector<std::uint8_t> Sized(std::uint64_t bytes) {
    return std::vector<std::uint8_t>(static_cast<std::size_t>(bytes), 0);
}

inline bool LoadMedia(const std::filesystem::path& base, const JsonValue& item, Reader* read,
                      Media* media) {
    const JsonValue* path = item.Find("path");
    if (path == nullptr || path->kind != JsonValue::Kind::String) {
        return false;
    }
    std::filesystem::path resolved = Utf8Path(path->text);
    if (!resolved.is_absolute()) {
        resolved = base / resolved;
    }
    const std::u8string utf8 = resolved.u8string();
    media->path.assign(utf8.begin(), utf8.end());
    media->id = read->Id(item, "id");
    bool startPresent = false;
    bool endPresent = false;
    media->loopStartFrame = read->U64(item, "loopStartFrame", 0, &startPresent);
    media->loopEndFrame = read->U64(item, "loopEndFrame", 0, &endPresent);
    media->loopable = startPresent || endPresent;
    media->downmix = item.Find("downmix") != nullptr;
    harkenhost::LoadedWav wav;
    if (!harkenhost::LoadWav(media->path, &wav, media->downmix) ||
        (wav.channels > 2) || wav.samples.empty()) {
        return false;
    }
    media->sampleRate = wav.sampleRate;
    media->frames = wav.samples.size() / wav.channels;
    if (media->downmix && wav.channels == 2) {
        media->channels = 1;
        media->planar.resize(static_cast<std::size_t>(media->frames));
        for (std::size_t n = 0; n < media->planar.size(); ++n) {
            media->planar[n] = static_cast<float>(
                (static_cast<double>(wav.samples[n * 2]) +
                 static_cast<double>(wav.samples[n * 2 + 1])) *
                0.5);
        }
    } else {
        media->channels = static_cast<std::uint16_t>(wav.channels);
        media->planar = harkenhost::DeinterleaveFrames(wav.samples, wav.channels);
    }
    const std::uint64_t loopEnd = media->loopEndFrame == 0 ? media->frames : media->loopEndFrame;
    return !media->loopable || (loopEnd <= media->frames && media->loopStartFrame < loopEnd);
}

inline harken::PackTocEntry MediaToc(const Media& media, std::uint64_t blobOffset,
                                     const std::uint8_t* blob) {
    harken::PackTocEntry toc;
    toc.soundId = media.id;
    toc.sampleRate = media.sampleRate;
    toc.channelCount = media.channels;
    toc.sampleFormat = harken::kPackSampleFormatF32Planar;
    toc.frameCount = media.frames;
    toc.blobOffset = blobOffset;
    toc.blobBytes = media.planar.size() * sizeof(float);
    toc.blobChecksum = harken::Fnv1a64(blob, static_cast<std::size_t>(toc.blobBytes));
    toc.flags = media.loopable ? harken::kPackFlagLoopable : 0;
    toc.loopStartFrame = media.loopable ? media.loopStartFrame : 0;
    toc.loopEndFrame = media.loopable ? media.loopEndFrame : 0;
    return toc;
}

inline std::uint64_t AlignTo(std::uint64_t value, std::uint64_t align) {
    return (value + align - 1) / align * align;
}

inline std::vector<std::uint8_t> MediaOnlyImage(const std::vector<Media>& media) {
    const std::uint64_t tocOffset = harken::kPackMediaFileHeaderBytes;
    const std::uint64_t tocBytes = media.size() * harken::kPackMediaBytes;
    std::vector<std::uint64_t> blobOffsets(media.size());
    std::uint64_t fileBytes = tocOffset + tocBytes;
    std::uint64_t cursor = AlignTo(fileBytes, harken::kPackBlobAlign);
    for (std::size_t i = 0; i < media.size(); ++i) {
        blobOffsets[i] = cursor;
        fileBytes = cursor + media[i].planar.size() * sizeof(float);
        cursor = AlignTo(fileBytes, harken::kPackBlobAlign);
    }
    std::vector<std::uint8_t> bytes = Sized(fileBytes);
    std::vector<std::uint8_t> toc = Sized(tocBytes);
    for (std::size_t i = 0; i < media.size(); ++i) {
        std::memcpy(bytes.data() + blobOffsets[i], media[i].planar.data(),
                    media[i].planar.size() * sizeof(float));
        harken::SerializePackMedia(MediaToc(media[i], blobOffsets[i], bytes.data() + blobOffsets[i]),
                                   toc.data() + i * harken::kPackMediaBytes);
    }
    std::memcpy(bytes.data() + tocOffset, toc.data(), toc.size());
    harken::PackMediaFileHeader header;
    header.formatVersion = harken::kPackFormatVersion;
    header.headerBytes = harken::kPackMediaFileHeaderBytes;
    header.fileBytes = fileBytes;
    header.entryCount = static_cast<std::uint32_t>(media.size());
    header.tocOffset = static_cast<std::uint32_t>(tocOffset);
    header.tocChecksum = harken::Fnv1a64(toc.data(), toc.size());
    harken::SerializePackMediaFileHeader(header, bytes.data());
    return bytes;
}

inline std::vector<std::uint8_t> LogicImage(std::uint32_t packKind, const std::vector<Media>& media,
                                            std::vector<Placement> placements) {
    const std::uint64_t directoryBytes = placements.size() * harken::kPackDirectoryEntryBytes;
    std::vector<std::uint64_t> offsets(placements.size());
    std::uint64_t cursor = harken::kPackFileHeaderBytes + directoryBytes;
    for (std::size_t i = 0; i < placements.size(); ++i) {
        offsets[i] = AlignTo(cursor, harken::kPackPayloadAlign);
        cursor = offsets[i] + placements[i].payload.size();
    }
    std::vector<std::uint64_t> blobOffsets(media.size());
    std::uint64_t fileBytes = cursor;
    for (std::size_t i = 0; i < media.size(); ++i) {
        blobOffsets[i] = AlignTo(fileBytes, harken::kPackBlobAlign);
        fileBytes = blobOffsets[i] + media[i].planar.size() * sizeof(float);
    }
    std::vector<std::uint8_t> bytes = Sized(fileBytes);
    for (std::size_t i = 0; i < media.size(); ++i) {
        std::memcpy(bytes.data() + blobOffsets[i], media[i].planar.data(),
                    media[i].planar.size() * sizeof(float));
    }
    std::vector<std::uint8_t> directory = Sized(directoryBytes);
    for (std::size_t i = 0; i < placements.size(); ++i) {
        Placement& placement = placements[i];
        if (placement.media != SIZE_MAX) {
            const std::size_t m = placement.media;
            harken::SerializePackMedia(
                MediaToc(media[m], blobOffsets[m], bytes.data() + blobOffsets[m]),
                placement.payload.data());
        }
        std::memcpy(bytes.data() + offsets[i], placement.payload.data(), placement.payload.size());
        harken::PackDirectoryEntry entry;
        entry.kind = placement.kind;
        entry.vintage = harken::PackEntryVintage(placement.kind);
        entry.id = placement.id;
        entry.relationOrdinal = placement.ordinal;
        entry.offset = offsets[i];
        entry.size = placement.payload.size();
        entry.payloadChecksum = harken::Fnv1a64(bytes.data() + offsets[i], placement.payload.size());
        harken::SerializePackDirectoryEntry(entry,
                                            directory.data() + i * harken::kPackDirectoryEntryBytes);
    }
    if (!directory.empty()) {
        std::memcpy(bytes.data() + harken::kPackFileHeaderBytes, directory.data(), directory.size());
    }
    harken::PackFileHeader header;
    header.formatVersion = harken::kPackFormatVersion;
    header.headerBytes = harken::kPackFileHeaderBytes;
    header.fileBytes = fileBytes;
    header.packKind = packKind;
    header.entryCount = static_cast<std::uint32_t>(placements.size());
    header.directoryOffset = harken::kPackFileHeaderBytes;
    header.directoryChecksum = harken::Fnv1a64(directory.data(), directory.size());
    harken::SerializePackFileHeader(header, bytes.data());
    return bytes;
}

inline std::vector<std::uint8_t> RecipePayload(const JsonValue& item, Reader* read) {
    harken::PackVoiceRecipeEntryHeader header;
    header.playableId = read->Id(item, "playable");
    std::vector<harken::PackVoiceRecipeInstallWire> installs;
    for (const JsonValue& effect : read->Array(item, "effects")) {
        harken::PackVoiceRecipeInstallWire row{};
        read->Effect(effect, &row);
        installs.push_back(row);
    }
    std::sort(installs.begin(), installs.end(),
              [](const auto& a, const auto& b) { return a.slot < b.slot; });
    std::vector<VnBindingId> bindings;
    for (const JsonValue& binding : read->Array(item, "bindings")) {
        VnBindingId id{};
        if (binding.kind != JsonValue::Kind::String || !harken::ParseObjectId(binding.text, &id)) {
            read->ok = false;
        }
        bindings.push_back(id);
    }
    std::sort(bindings.begin(), bindings.end());
    header.installCount = static_cast<std::uint32_t>(installs.size());
    header.bindingCount = static_cast<std::uint32_t>(bindings.size());

    harken::PackVoiceRecipeTone tone;
    if (const JsonValue* source = item.Find("tone")) {
        tone.present = 1u;
        tone.sectionEnableMask =
            (read->Bool(*source, "lowpassEnabled") ? harken::kPackVoiceToneLowpassEnabled : 0u) |
            (read->Bool(*source, "highpassEnabled") ? harken::kPackVoiceToneHighpassEnabled : 0u) |
            (read->Bool(*source, "lowShelfEnabled") ? harken::kPackVoiceToneLowShelfEnabled : 0u) |
            (read->Bool(*source, "highShelfEnabled") ? harken::kPackVoiceToneHighShelfEnabled : 0u);
        tone.lowpassFrequencyHz = read->Float(*source, "lowpassFrequencyHz");
        tone.lowpassQ = read->Float(*source, "lowpassQ");
        tone.highpassFrequencyHz = read->Float(*source, "highpassFrequencyHz");
        tone.highpassQ = read->Float(*source, "highpassQ");
        tone.lowShelfFrequencyHz = read->Float(*source, "lowShelfFrequencyHz");
        tone.lowShelfQ = read->Float(*source, "lowShelfQ");
        tone.lowShelfGainDb = read->Float(*source, "lowShelfGainDb");
        tone.highShelfFrequencyHz = read->Float(*source, "highShelfFrequencyHz");
        tone.highShelfQ = read->Float(*source, "highShelfQ");
        tone.highShelfGainDb = read->Float(*source, "highShelfGainDb");
    }
    harken::PackVoiceRecipeSpatial spatial;
    spatial.distanceScale = 0.0f;
    if (const JsonValue* source = item.Find("spatial")) {
        spatial.attenuationId = read->Id(*source, "attenuation");
        spatial.distanceScale = read->Float(*source, "distanceScale", 1.0f);
    }
    harken::PackVoiceRecipePlayback playback;
    if (const JsonValue* source = item.Find("playback")) {
        playback.present = 1u;
        playback.loopCount = read->U32(*source, "loopCount");
    }

    struct InstallVisitor {
        harken::PackVoiceRecipeInstallWire row;
        const auto& Wire() const noexcept { return row; }
        bool Finish() const noexcept { return true; }
    };
    struct Visitor {
        const harken::PackVoiceRecipeEntryHeader& header;
        const harken::PackVoiceRecipeTone& tone;
        const harken::PackVoiceRecipeSpatial& spatial;
        const harken::PackVoiceRecipePlayback& playback;
        const std::vector<harken::PackVoiceRecipeInstallWire>& installs;
        const std::vector<VnBindingId>& bindings;
        const auto& Value() const noexcept { return header; }
        const auto& Header() const noexcept { return header; }
        const auto& Tone() const noexcept { return tone; }
        const auto& Spatial() const noexcept { return spatial; }
        const auto& Playback() const noexcept { return playback; }
        bool PrepareInstalls(std::uint64_t count) const noexcept { return count == installs.size(); }
        InstallVisitor Installs(std::uint64_t index) const noexcept {
            return {installs[static_cast<std::size_t>(index)]};
        }
        bool PrepareBindings(std::uint64_t count) const noexcept { return count == bindings.size(); }
        const auto& Bindings(std::uint64_t index) const noexcept {
            return bindings[static_cast<std::size_t>(index)];
        }
        bool Finish() const noexcept { return true; }
    } visitor{header, tone, spatial, playback, installs, bindings};

    std::vector<std::uint8_t> payload =
        Sized(harken::PackVoiceRecipePayloadBytes(header.installCount, header.bindingCount));
    harken::PackWireBuffer<std::uint8_t> buffer{payload.data(), payload.size()};
    if (!harken::AppendPackVoiceRecipePayload(buffer, visitor)) {
        read->ok = false;
    }
    return payload;
}

inline std::vector<std::uint8_t> ContainerPayload(const JsonValue& item, Reader* read) {
    harken::PackContainerEntryHeader header;
    header.containerId = read->Id(item, "id");
    header.selectionOrder = read->Enum(item, "order", kOrderNames);
    header.avoidRepeatWindow = read->U32(item, "avoidRepeatWindow");
    header.selectionVariableId = read->Id(item, "selectionVariable");
    header.defaultIndex = read->U32(item, "default", 0xFFFFFFFFu);
    std::vector<harken::PackContainerChild> children;
    for (const JsonValue& child : read->Array(item, "children")) {
        children.push_back({read->Id(child, "playable"), read->U32(child, "weight"),
                            read->U32(child, "matchValue"), read->U32(child, "delayMs"),
                            read->U32(child, "delayJitterMs")});
    }
    header.childCount = static_cast<std::uint32_t>(children.size());
    struct Visitor {
        const harken::PackContainerEntryHeader& header;
        const std::vector<harken::PackContainerChild>& children;
        const auto& Value() const noexcept { return header; }
        const auto& Header() const noexcept { return header; }
        bool PrepareChildren(std::uint64_t count) const noexcept { return count == children.size(); }
        harken::PackContainerChild Children(std::uint64_t index) const noexcept {
            return children[static_cast<std::size_t>(index)];
        }
        bool Finish() const noexcept { return true; }
    } visitor{header, children};
    std::vector<std::uint8_t> payload = Sized(harken::PackContainerPayloadBytes(header.childCount));
    harken::PackWireBuffer<std::uint8_t> buffer{payload.data(), payload.size()};
    if (!harken::AppendPackContainerPayload(buffer, visitor)) {
        read->ok = false;
    }
    return payload;
}

inline VnAttenuationDesc Attenuation(const JsonValue& item, Reader* read) {
    VnAttenuationDesc desc{};
    desc.structSize = sizeof(desc);
    desc.rolloff = read->Enum(item, "rolloff", kRolloffNames);
    desc.minDistance = read->Float(item, "minDistance");
    desc.maxDistance = read->Float(item, "maxDistance");
    desc.airAbsorptionNearHz = read->Float(item, "airAbsorptionNearHz");
    desc.airAbsorptionFarHz = read->Float(item, "airAbsorptionFarHz");
    desc.proximityRadius = read->Float(item, "proximityRadius");
    desc.proximityMaxBoostDb = read->Float(item, "proximityMaxBoostDb");
    desc.proximityFrequency = read->Float(item, "proximityFrequency", 250.0f);
    if (const JsonValue* cone = item.Find("cone")) {
        desc.coneEnabled = 1;
        desc.coneInnerDeg = read->Float(*cone, "innerDeg");
        desc.coneOuterDeg = read->Float(*cone, "outerDeg");
        desc.coneOuterGainDb = read->Float(*cone, "outerGainDb");
        desc.coneOuterLowpassHz = read->Float(*cone, "outerLowpassHz");
    }
    const auto curve = [&](const char* key, VnAttenuationCurve* out) {
        if (const JsonValue* source = item.Find(key)) {
            out->enabled = 1;
            out->pointCount = read->Curve(*source, out->points, out->handles);
        }
    };
    curve("volumeCurve", &desc.volumeCurve);
    curve("lowpassCurve", &desc.lowpassCurve);
    curve("widthCurve", &desc.widthCurve);
    return desc;
}

inline VnBindingDesc Binding(const JsonValue& item, Reader* read) {
    VnBindingDesc desc{};
    desc.structSize = sizeof(desc);
    desc.variableId = read->Id(item, "variable");
    desc.targetScopeKind = read->Enum(item, "scope", kScopeNames);
    desc.targetId = read->U32(item, "target");
    desc.transitionMs = read->U32(item, "transitionMs");
    desc.curve.structSize = sizeof(desc.curve);
    if (const JsonValue* curve = item.Find("curve")) {
        desc.curve.pointCount = read->Curve(*curve, desc.curve.points, desc.curve.handles);
    } else {
        read->ok = false;
    }
    return desc;
}

inline bool BuildLogicPlacements(const JsonValue& root, std::size_t mediaCount, Reader* read,
                                 std::vector<Placement>* out) {
    const auto entry = [&](std::uint32_t kind, VnObjectId id, std::uint64_t bytes,
                           std::uint64_t ordinal = 0) -> std::vector<std::uint8_t>& {
        out->push_back({kind, id, ordinal, Sized(bytes)});
        return out->back().payload;
    };
    for (std::size_t i = 0; i < mediaCount; ++i) {
        out->push_back({harken::kPackEntryKindMedia, {}, 0, Sized(harken::kPackMediaBytes), i});
    }
    for (const JsonValue& item : read->Array(root, "buses")) {
        harken::PackBusEntry bus;
        bus.busId = read->Id(item, "id");
        bus.parentBusId = read->Id(item, "parent");
        bus.volume = read->Float(item, "volume");
        bus.outputBusVolume = read->Float(item, "outputBusVolume");
        harken::SerializePackBus(bus, entry(harken::kPackEntryKindBus, bus.busId,
                                            harken::kPackBusBytes).data());
    }
    std::uint64_t ordinal = 0;
    for (const JsonValue& item : read->Array(root, "busSends")) {
        harken::PackBusSendEntry send;
        send.fromBusId = read->Id(item, "from");
        send.toBusId = read->Id(item, "to");
        send.levelDb = read->Float(item, "levelDb");
        harken::SerializePackBusSend(send, entry(harken::kPackEntryKindBusSend, {},
                                                 harken::kPackBusSendBytes, ordinal++).data());
    }
    for (const JsonValue& item : read->Array(root, "attenuations")) {
        harken::SerializePackAttenuation(
            Attenuation(item, read),
            entry(harken::kPackEntryKindAttenuation, read->Id(item, "id"),
                  harken::kPackAttenuationBytes).data());
    }
    for (const JsonValue& item : read->Array(root, "bindings")) {
        const VnBindingId id = read->Id(item, "id");
        harken::WritePackBindingDefinition(
            id, Binding(item, read),
            entry(harken::kPackEntryKindBindingDefinition, id,
                  harken::kPackBindingDefinitionBytes).data());
    }
    ordinal = 0;
    for (const JsonValue& item : read->Array(root, "attachments")) {
        harken::PackBindingAttachmentEntry attachment;
        attachment.bindingId = read->Id(item, "binding");
        attachment.scopeKind = read->Enum(item, "scope", kScopeNames);
        attachment.objectId = read->Id(item, "object");
        harken::SerializePackBindingAttachment(
            attachment, entry(harken::kPackEntryKindBindingAttachment, {},
                              harken::kPackBindingAttachmentBytes, ordinal++).data());
    }
    for (const JsonValue& item : read->Array(root, "containers")) {
        out->push_back({harken::kPackEntryKindContainer, read->Id(item, "id"), 0,
                        ContainerPayload(item, read)});
    }
    std::vector<std::pair<VnObjectId, const JsonValue*>> recipes;
    for (const JsonValue& item : read->Array(root, "voiceRecipes")) {
        recipes.emplace_back(read->Id(item, "playable"), &item);
    }
    std::stable_sort(recipes.begin(), recipes.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [id, item] : recipes) {
        out->push_back({harken::kPackEntryKindVoiceRecipe, id, 0, RecipePayload(*item, read)});
    }
    std::vector<harken::PackBusEffectEntry> busEffects;
    for (const JsonValue& item : read->Array(root, "busEffects")) {
        harken::PackBusEffectEntry effect;
        read->Effect(item, &effect);
        effect.busId = read->Id(item, "bus");
        effect.faderPointGuard = read->U32(item, "faderPointGuard");
        busEffects.push_back(effect);
    }
    std::stable_sort(busEffects.begin(), busEffects.end(), [](const auto& a, const auto& b) {
        return a.busId < b.busId || (a.busId == b.busId && a.slot < b.slot);
    });
    ordinal = 0;
    for (const auto& effect : busEffects) {
        harken::SerializePackBusEffect(effect, entry(harken::kPackEntryKindBusEffect, {},
                                                     harken::kPackBusEffectBytes, ordinal++).data());
    }
    std::vector<harken::PackEffectDynamicRouteEntry> routes;
    for (const JsonValue& item : read->Array(root, "dynamicRoutes")) {
        harken::PackEffectDynamicRouteEntry route;
        route.targetBusId = read->Id(item, "targetBus");
        route.effectSlot = read->U32(item, "effectSlot");
        route.routeSlot = read->U32(item, "routeSlot");
        route.expectedEffectType = VN_EFFECT_COMPRESSOR;
        route.signalForm = VN_DYNAMIC_SIGNAL_RAW_AUDIO;
        route.sourceKind = VN_DYNAMIC_SOURCE_BUS;
        route.tap = read->Enum(item, "tap", kTapNames);
        route.sourceId = read->Id(item, "sourceBus");
        route.levelDb = read->Float(item, "levelDb");
        routes.push_back(route);
    }
    std::stable_sort(routes.begin(), routes.end(), [](const auto& a, const auto& b) {
        if (a.targetBusId != b.targetBusId) {
            return a.targetBusId < b.targetBusId;
        }
        if (a.effectSlot != b.effectSlot) {
            return a.effectSlot < b.effectSlot;
        }
        return a.routeSlot < b.routeSlot;
    });
    ordinal = 0;
    for (const auto& route : routes) {
        harken::SerializePackEffectDynamicRoute(
            route, entry(harken::kPackEntryKindEffectDynamicRoute, {},
                         harken::kPackEffectDynamicRouteBytes, ordinal++).data());
    }
    return read->ok;
}

inline bool WriteBytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return out.good();
}

} // namespace testpack

// Writes the .pack (or, with "--stream", the .stream) a JSON description of
// Pack entries names. The description mirrors the Engine's Pack entry kinds
// one to one; this writer produces test inputs from the Engine's own format
// definitions and performs no authoring-side validation.
inline int BuildTestPack(const std::vector<std::string>& args) {
    bool stream = false;
    std::string descriptionPath;
    for (const std::string& arg : args) {
        if (arg == "--stream") {
            stream = true;
        } else {
            descriptionPath = arg;
        }
    }
    std::ifstream in(testpack::Utf8Path(descriptionPath), std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    harkenhost::JsonValue root;
    std::string error;
    if (!in.good() && !in.eof()) {
        return 1;
    }
    if (!harkenhost::ParseJson(text, &root, &error) ||
        root.kind != harkenhost::JsonValue::Kind::Object) {
        return 1;
    }
    testpack::Reader read;
    const harkenhost::JsonValue* output = root.Find("output");
    if (output == nullptr || output->kind != harkenhost::JsonValue::Kind::String) {
        return 1;
    }
    const std::filesystem::path base =
        testpack::Utf8Path(descriptionPath).parent_path();
    std::filesystem::path outputPath = testpack::Utf8Path(output->text);
    if (!outputPath.is_absolute()) {
        outputPath = base / outputPath;
    }
    std::vector<testpack::Media> media;
    for (const harkenhost::JsonValue& item : read.Array(root, "entries")) {
        testpack::Media entry;
        if (!testpack::LoadMedia(base, item, &read, &entry)) {
            return 1;
        }
        media.push_back(std::move(entry));
    }
    if (stream) {
        if (media.size() != 1) {
            return 1;
        }
        harkenhost::StreamSpec spec;
        spec.soundId = media[0].id;
        spec.sampleRate = media[0].sampleRate;
        spec.channelCount = media[0].channels;
        spec.chunkFrames = read.U32(root, "chunkFrames", 4096);
        spec.loopable = media[0].loopable;
        spec.loopStartFrame = media[0].loopStartFrame;
        spec.loopEndFrame = media[0].loopEndFrame;
        return read.ok && testpack::WriteBytes(
                              outputPath, harkenhost::BuildStreamBytes(spec, media[0].planar))
                   ? 0
                   : 1;
    }
    const harkenhost::JsonValue* packKind = root.Find("packKind");
    if (packKind == nullptr) {
        return read.ok && testpack::WriteBytes(outputPath, testpack::MediaOnlyImage(media)) ? 0
                                                                                            : 1;
    }
    std::vector<testpack::Placement> placements;
    if (!testpack::BuildLogicPlacements(root, media.size(), &read, &placements)) {
        return 1;
    }
    for (std::size_t i = 0; i < media.size(); ++i) {
        placements[i].id = media[i].id;
    }
    const std::uint32_t kind = packKind->text == "resident" ? harken::kPackKindResident
                                                             : harken::kPackKindContent;
    return testpack::WriteBytes(outputPath,
                                testpack::LogicImage(kind, media, std::move(placements)))
               ? 0
               : 1;
}

} // namespace harkentest
