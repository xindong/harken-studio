// packc manifest model: parsing for both manifest generations and the
// normalized v2 emitter.
//
// Two manifest generations share one entry model:
//
// v1 (no "manifestVersion" member — the original shape, kept working
// verbatim): a top-level "sounds" array; ids optional (absent ids are
// minted from content by the tool); the output path arrives on the
// command line; in --stream mode an optional top-level "chunkFrames".
//
// v2 ("manifestVersion": 2 — the canonical authoring input): carries its
// own output path and build options, and every entry names its id
// explicitly. Explicit ids are required because the stable u64 id is the
// asset's identity and the authoring layer owns assigning it; minting
// ids from content would silently re-identify a sound whenever its
// samples change. Shape:
//
//   {
//     "manifestVersion": 2,
//     "output": "ui.pack",
//     "options": {                       // optional
//       "cache": true,                   // default true
//       "cachePath": "ui.pack.cache.json",
//       "reportPath": "ui.pack.report.json"
//     },
//     "entries": [
//       { "path": "click.wav", "id": 42,
//         "loopStartFrame": 0, "loopEndFrame": 0,   // optional pair
//         "codec": "pcm-f32",                       // optional
//         "downmix": "mono" }                       // optional
//     ]
//   }
//
// The version member doubles as the dispatcher: its presence selects the
// v2 rules, its absence the v1 rules, so no heuristic ever guesses.
// Unknown keys are refused everywhere in both generations (a typo
// silently ignored would ship wrong content). "codec" accepts only
// "pcm-f32" — the field exists so future codecs extend the manifest
// without a shape change, and an unknown value must refuse rather than
// silently build uncompressed.
//
// Each v1 or v2 media entry may carry "downmix": "mono". Absence
// preserves the source channel layout. Mono sources remain mono; stereo
// sources use the tool's versioned deterministic stereo-to-mono
// transform; sources with more than two channels refuse rather than
// guessing a speaker layout.
//
// A v2 manifest may additionally select the v2 (logic-entry) container
// by carrying a top-level "packKind": "resident" | "content". packKind
// absent keeps today's v1 container output byte-identical. With
// packKind present, the optional logic sections — "buses", "busSends",
// "attenuations", "bindings", "attachments", and "containers" —
// compile into typed v2 entries (PackFormatV2.h). Studio shipping
// export does not emit the containers or voiceRecipes sections; packc
// still accepts both for non-canonical / unit-test manifests. Kind
// discipline, the compiler half of the spec section 19.1 defense in depth:
// a resident pack carries no media entries, no containers, and
// only bus-scope bindings and attachments; a content pack refuses the
// resident-only infrastructure sections (buses, sends, and attenuations).
// Logic sections without a packKind refuse — logic entries
// require a pack kind. Cross-pack references are legal (reference
// closure is proven at load against the loaded packs), so reference
// validation here is limited to intra-pack duplicates and
// self-referential illegality; field validation mirrors the engine's
// registration contracts so a pack that compiles also registers.
//
// Path bases differ by generation on purpose: v1 resolves relative paths
// against the process working directory (the original contract), while
// v2 resolves them against the manifest file's own directory — a checked
// -in authoring file must mean the same sources no matter where the tool
// runs. Resolution never influences output bytes; paths are not stored
// in containers.
#ifndef HARKEN_HOST_PACKC_MANIFEST_H
#define HARKEN_HOST_PACKC_MANIFEST_H

#include <harken_studio/ObjectId.h>
#include <harken_studio_build/AuthoredParameters.h>
#include <harken_studio_build/BindingTransition.h>
#include "../../Common/JsonReader.h"
#include "../../Common/JsonWriter.h"

#include <harken_studio/AudioValues.h>
#include "harken_studio_build/PackFormatV2.h"
#include "harken_studio_build/EffectEncoding.h"
#include "harken_studio_build/EffectParamLocator.h"
#include <harken_studio_build/BindingTargets.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include "../../Common/Utf8Path.h"
#include <string>
#include <vector>

namespace harkenstudiojson {
namespace packc {

inline constexpr std::uint32_t kLegacySelectionWeightedRandom = 0;
inline constexpr std::uint32_t kLegacySelectionRoundRobin = 1;

// First-refusal message carrier. The tool prints it to stderr AND echoes
// it into the validation report, so both surfaces state the same reason.
struct Diag {
    std::string message;
};

inline bool Fail(Diag* diag, const char* format, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof buffer, format, args);
    va_end(args);
    if (diag->message.empty()) {
        diag->message = buffer;
    }
    return false;
}

inline constexpr char kCodecPcmF32[] = "pcm-f32";
inline constexpr char kDownmixMono[] = "mono";

struct ManifestEntry {
    std::string path;         // as written in the manifest
    std::string resolvedPath; // path to open (base rules above)
    harkenstudio::ObjectId soundId{};
    bool explicitId = false;
    bool loopable = false;
    std::uint64_t loopStartFrame = 0;
    std::uint64_t loopEndFrame = 0; // 0 = whole file
    std::string codec = kCodecPcmF32;
    std::string downmix; // "" = preserve source channels; "mono" = mono
};

// --- v2 logic-section working forms ---
// Parsed images of the logic-entry sections a v2 manifest may carry
// alongside "packKind". Enum-valued members hold the public VN_* values
// (mapped from the manifest spelling at parse time).

// One parsed curve object ({"points": [[x, y], ...], "handles": [...]});
// serves attenuation override curves and binding curves alike. Presence
// is tracked by the owner — an absent attenuation override curve means
// "built-in default" — so the curve carries no enabled flag here.
struct ManifestCurve {
    std::uint32_t pointCount = 0;
    float pointX[harkenstudio::kCurveMaxPoints] = {};
    float pointY[harkenstudio::kCurveMaxPoints] = {};
    harkenstudiobuild::ParameterCurveHandles handles[harkenstudio::kCurveMaxPoints - 1] = {};
};

struct ManifestBus {
    harkenstudio::ObjectId id{};
    harkenstudio::ObjectId parent{};
    float volume = 0.0f;
    float outputBusVolume = 0.0f;
    float makeUpGain = 0.0f;
};

struct ManifestBusSend {
    harkenstudio::ObjectId from{};
    harkenstudio::ObjectId to{};
    float levelDb = 0.0f;
};

struct ManifestBusEffect {
    harkenstudio::ObjectId effectId{};
    harkenstudio::ObjectId bus{};
    std::uint32_t slot = 0;
    std::uint32_t type = harkenstudio::kEffectGain;
    std::uint32_t descriptorVintage = 0;
    bool bypass = false;
    std::uint32_t faderPointGuard = 0;
    std::vector<float> parameters;
};

struct ManifestDynamicRoute {
    harkenstudio::ObjectId targetBus{};
    std::uint32_t effectSlot = 0;
    std::uint32_t routeSlot = 0;
    std::uint32_t expectedEffectType = harkenstudio::kEffectCompressor;
    std::uint32_t signalForm = harkenstudio::kDynamicSignalRawAudio;
    std::uint32_t sourceKind = harkenstudio::kDynamicSourceBus;
    std::uint32_t tap = harkenstudio::kDynamicTapBusPostEffects;
    harkenstudio::ObjectId sourceBus{};
    float levelDb = 0.0f;
};

struct ManifestAttenuation {
    harkenstudio::ObjectId id{};
    std::uint32_t rolloff = harkenstudio::kRolloffLinear;
    float minDistance = 0.0f;
    float maxDistance = 0.0f;
    float airAbsorptionNearHz = 0.0f;
    float airAbsorptionFarHz = 0.0f;
    float proximityRadius = 0.0f;
    float proximityMaxBoostDb = 0.0f;
    float proximityFrequency = 250.0f;
    bool hasProximityFrequency = false;
    bool coneEnabled = false;
    float coneInnerDeg = 0.0f;
    float coneOuterDeg = 0.0f;
    float coneOuterGainDb = 0.0f;
    float coneOuterLowpassHz = 0.0f;
    bool hasVolumeCurve = false;
    bool hasLowpassCurve = false;
    bool hasWidthCurve = false;
    ManifestCurve volumeCurve;
    ManifestCurve lowpassCurve;
    ManifestCurve widthCurve;
};

struct ManifestBinding {
    harkenstudio::ObjectId id{};
    harkenstudio::ObjectId variable{};
    std::uint32_t scope = harkenstudio::kTargetScopeVoice;
    std::uint32_t target = 0;
    std::uint32_t transitionMs = 0;
    ManifestCurve curve;
};

struct ManifestAttachment {
    harkenstudio::ObjectId binding{};
    std::uint32_t scope = harkenstudio::kTargetScopeVoice;
    harkenstudio::ObjectId object{};
};

struct ManifestVoiceRecipeEffect {
    harkenstudio::ObjectId effectId{};
    std::uint32_t slot = 0;
    std::uint32_t type = harkenstudio::kEffectGain;
    std::uint32_t descriptorVintage = 0;
    bool bypass = false;
    std::vector<float> parameters;
};

struct ManifestVoiceRecipeTone {
    bool present = false;
    bool lowpassEnabled = false;
    bool highpassEnabled = false;
    bool lowShelfEnabled = false;
    bool highShelfEnabled = false;
    float lowpassFrequencyHz = 20000.0f;
    float lowpassQ = 0.70710678f;
    float highpassFrequencyHz = 20.0f;
    float highpassQ = 0.70710678f;
    float lowShelfFrequencyHz = 200.0f;
    float lowShelfQ = 0.70710678f;
    float lowShelfGainDb = 0.0f;
    float highShelfFrequencyHz = 6000.0f;
    float highShelfQ = 0.70710678f;
    float highShelfGainDb = 0.0f;
};

struct ManifestVoiceRecipeSpatial {
    bool present = false;
    harkenstudio::ObjectId attenuation{};
    float distanceScale = 1.0f;
};

struct ManifestVoiceRecipePlayback {
    bool present = false;
    std::uint32_t loopCount = 0;
};

struct ManifestVoiceRecipe {
    harkenstudio::ObjectId playable{};
    ManifestVoiceRecipeTone tone;
    ManifestVoiceRecipeSpatial spatial;
    ManifestVoiceRecipePlayback playback;
    std::vector<ManifestVoiceRecipeEffect> effects;
    std::vector<harkenstudio::ObjectId> bindings;
};

struct ManifestContainerChild {
    harkenstudio::ObjectId playable{};
    std::uint32_t weight = 0;
    std::uint32_t matchValue = 0;
    std::uint32_t delayMs = 0;
    std::uint32_t delayJitterMs = 0;
};

struct ManifestContainer {
    harkenstudio::ObjectId id{};
    std::uint32_t order = kLegacySelectionWeightedRandom;
    harkenstudio::ObjectId selectionVariable{}; // 0 = no match filter
    std::uint32_t avoidRepeatWindow = 0;
    std::uint32_t defaultIndex = 0xFFFFFFFFu; // = absent
    std::vector<ManifestContainerChild> children;
};

struct Manifest {
    int version = 3;
    std::string manifestPath; // as given on the command line
    std::vector<ManifestEntry> entries;

    // v2 only. The "resolved" forms are what the tool opens/writes; the
    // plain forms are the manifest's own text, echoed by the emitter and
    // the report.
    std::string outputPath;
    std::string resolvedOutputPath;
    bool cacheEnabled = true;
    std::string cachePath;   // "" = default (beside the output)
    std::string reportPath;  // "" = default (beside the output)

    // v1 --stream only: 0 = manifest did not choose (default applies).
    std::uint32_t chunkFrames = 0;

    // v2 only. "" = no "packKind" member — the v1 container output, the
    // pre-logic behavior kept byte-identical; "resident" or "content"
    // selects the v2 container. The logic sections below are legal only
    // alongside a pack kind.
    std::string packKind;
    std::vector<ManifestBus> buses;
    std::vector<ManifestBusSend> busSends;
    std::vector<ManifestBusEffect> busEffects;
    std::vector<ManifestDynamicRoute> dynamicRoutes;
    std::vector<ManifestAttenuation> attenuations;
    std::vector<ManifestBinding> bindings;
    std::vector<ManifestVoiceRecipe> voiceRecipes;
    std::vector<ManifestAttachment> attachments;
    std::vector<ManifestContainer> containers;
};

inline bool ManifestHasLogicSections(const Manifest& manifest) {
    return !manifest.buses.empty() || !manifest.busSends.empty() ||
           !manifest.busEffects.empty() || !manifest.dynamicRoutes.empty() ||
           !manifest.attenuations.empty() || !manifest.bindings.empty() ||
           !manifest.voiceRecipes.empty() ||
           !manifest.attachments.empty() || !manifest.containers.empty();
}

inline std::string ResolveAgainst(const std::string& baseDir,
                                  const std::string& path) {
    if (baseDir.empty() || Utf8Path(path).is_absolute()) {
        return path;
    }
    return PathUtf8(Utf8Path(baseDir) / Utf8Path(path));
}

inline std::string DirectoryOf(const std::string& filePath) {
    return PathUtf8(Utf8Path(filePath).parent_path());
}

// Shared by both generations: the optional non-negative-integer member
// contract (present-but-malformed refuses; absent is fine).
inline bool ReadOptionalU64(const JsonValue& object, const char* key,
                            const char* arrayName, std::size_t entryIndex,
                            bool* present, std::uint64_t* out,
                            Diag* diag) {
    const JsonValue* value = object.Find(key);
    if (value == nullptr) {
        *present = false;
        return true;
    }
    if (!value->AsU64(out)) {
        return Fail(diag,
                    "manifest %s[%zu].%s must be a non-negative integer",
                    arrayName, entryIndex, key);
    }
    *present = true;
    return true;
}

// One entry object under either generation's array. v2 additionally
// requires the explicit id and accepts the codec member.
inline bool ReadObjectIdField(const JsonValue& object, const char* key,
    const std::string& context, bool required, bool* present,
    harkenstudio::ObjectId* out, Diag* diag) {
    const auto* value = object.Find(key);
    if (present) *present = value != nullptr;
    if (!value)
        return !required || Fail(diag, "manifest %s requires a \"%s\" UUID", context.c_str(), key);
    if (value->kind != JsonValue::Kind::String || !harkenstudio::ParseObjectId(value->text, out))
        return Fail(diag, "manifest %s.%s must be a UUID string", context.c_str(), key);
    return true;
}

inline bool ParseEntry(const JsonValue& item, std::size_t index,
                       bool v2, const std::string& baseDir,
                       ManifestEntry* out, Diag* diag) {
    const char* arrayName = v2 ? "entries" : "sounds";
    if (item.kind != JsonValue::Kind::Object) {
        return Fail(diag, "manifest %s[%zu] must be an object", arrayName,
                    index);
    }
    for (const auto& member : item.members) {
        const bool known =
            member.first == "path" || member.first == "id" ||
            member.first == "loopStartFrame" ||
            member.first == "loopEndFrame" ||
            member.first == "downmix" ||
            (v2 && member.first == "codec");
        if (!known) {
            return Fail(diag, "unknown key \"%s\" in manifest %s[%zu]",
                        member.first.c_str(), arrayName, index);
        }
    }
    const JsonValue* path = item.Find("path");
    if (path == nullptr || path->kind != JsonValue::Kind::String ||
        path->text.empty()) {
        return Fail(diag,
                    "manifest %s[%zu] requires a non-empty \"path\" "
                    "string",
                    arrayName, index);
    }
    out->path = path->text;
    out->resolvedPath = ResolveAgainst(baseDir, path->text);
    bool loopStartPresent = false;
    bool loopEndPresent = false;
    if (!ReadObjectIdField(item, "id", std::string(arrayName) + "[" + std::to_string(index) + "]", true, &out->explicitId,
                         &out->soundId, diag) ||
        !ReadOptionalU64(item, "loopStartFrame", arrayName, index,
                         &loopStartPresent, &out->loopStartFrame, diag) ||
        !ReadOptionalU64(item, "loopEndFrame", arrayName, index,
                         &loopEndPresent, &out->loopEndFrame, diag)) {
        return false;
    }
    out->loopable = loopStartPresent || loopEndPresent;
    if (v2 && !out->explicitId) {
        return Fail(diag,
                    "manifest entries[%zu] requires an explicit \"id\" — "
                    "v2 manifests never mint ids from content",
                    index);
    }
    out->codec = kCodecPcmF32;
    const JsonValue* downmix = item.Find("downmix");
    if (downmix != nullptr) {
        if (downmix->kind != JsonValue::Kind::String ||
            downmix->text != kDownmixMono) {
            return Fail(diag,
                        "manifest %s[%zu].downmix: only \"%s\" is "
                        "supported",
                        arrayName, index, kDownmixMono);
        }
        out->downmix = downmix->text;
    }
    if (v2) {
        const JsonValue* codec = item.Find("codec");
        if (codec != nullptr) {
            if (codec->kind != JsonValue::Kind::String ||
                codec->text != kCodecPcmF32) {
                return Fail(diag,
                            "manifest entries[%zu].codec: only \"%s\" is "
                            "supported",
                            index, kCodecPcmF32);
            }
        }
    }
    return true;
}

// --- logic-section parsing ---
// Context strings name the exact object being read (for example
// "containers[2].children[0]"), so every refusal names its location without
// threading section/index pairs through each helper.

inline std::string IndexedName(const char* section, std::size_t index) {
    return std::string(section) + "[" + std::to_string(index) + "]";
}

inline bool RefuseUnknownKeys(const JsonValue& object,
                              const char* const* known,
                              std::size_t knownCount,
                              const std::string& context, Diag* diag) {
    for (const auto& member : object.members) {
        bool found = false;
        for (std::size_t i = 0; i < knownCount; ++i) {
            if (member.first == known[i]) {
                found = true;
                break;
            }
        }
        if (!found) {
            return Fail(diag, "unknown key \"%s\" in manifest %s",
                        member.first.c_str(), context.c_str());
        }
    }
    return true;
}

// Converts a Number literal to a finite float. The JSON grammar the
// reader enforces is a subset of what from_chars accepts, and the
// conversion is locale-free — the emitter's %.9g literals parse back to
// the identical f32, which the round-trip fixpoint relies on.
inline bool ParseFiniteFloat(const JsonValue& value, float* out) {
    if (value.kind != JsonValue::Kind::Number || value.text.empty()) {
        return false;
    }
    const char* begin = value.text.c_str();
    const char* end = begin + value.text.size();
    float parsed = 0.0f;
    const std::from_chars_result result =
        std::from_chars(begin, end, parsed);
    if (result.ec != std::errc() || result.ptr != end ||
        !std::isfinite(parsed)) {
        return false;
    }
    *out = parsed;
    return true;
}

// Member readers over one section object. Absent optional members leave
// *out at its caller-set default; present-but-malformed always refuses.
inline bool ReadU64Field(const JsonValue& object, const char* key,
                         const std::string& context, bool required,
                         bool* present, std::uint64_t* out, Diag* diag) {
    const JsonValue* value = object.Find(key);
    if (value == nullptr) {
        if (present != nullptr) {
            *present = false;
        }
        if (required) {
            return Fail(diag, "manifest %s requires a \"%s\" member",
                        context.c_str(), key);
        }
        return true;
    }
    if (!value->AsU64(out)) {
        return Fail(diag,
                    "manifest %s.%s must be a non-negative integer",
                    context.c_str(), key);
    }
    if (present != nullptr) {
        *present = true;
    }
    return true;
}

inline bool ReadU32Field(const JsonValue& object, const char* key,
                         const std::string& context, bool required,
                         bool* present, std::uint32_t* out, Diag* diag) {
    std::uint64_t wide = *out;
    bool was = false;
    if (!ReadU64Field(object, key, context, required, &was, &wide,
                      diag)) {
        return false;
    }
    if (was) {
        if (wide > 0xFFFFFFFFull) {
            return Fail(diag,
                        "manifest %s.%s must be a non-negative 32-bit "
                        "integer",
                        context.c_str(), key);
        }
        *out = static_cast<std::uint32_t>(wide);
    }
    if (present != nullptr) {
        *present = was;
    }
    return true;
}

inline bool ReadFloatField(const JsonValue& object, const char* key,
                           const std::string& context, bool required,
                           bool* present, float* out, Diag* diag) {
    const JsonValue* value = object.Find(key);
    if (value == nullptr) {
        if (present != nullptr) {
            *present = false;
        }
        if (required) {
            return Fail(diag, "manifest %s requires a \"%s\" member",
                        context.c_str(), key);
        }
        return true;
    }
    if (!ParseFiniteFloat(*value, out)) {
        return Fail(diag, "manifest %s.%s must be a finite number",
                    context.c_str(), key);
    }
    if (present != nullptr) {
        *present = true;
    }
    return true;
}

// Name<->value tables for the enum-valued manifest members. The names
// are the manifest spelling; the values are the public VN_* constants
// the wire schemas carry.
struct NamedValue {
    const char* name;
    std::uint32_t value;
};

inline constexpr NamedValue kRolloffNames[] = {
    {"linear", harkenstudio::kRolloffLinear},
    {"inverseSquare", harkenstudio::kRolloffInverseSquare},
    {"logarithmic", harkenstudio::kRolloffLogarithmic},
};

inline constexpr NamedValue kTargetScopeNames[] = {
    {"voice", harkenstudio::kTargetScopeVoice},
    {"bus", harkenstudio::kTargetScopeBus},
    {"voiceEffectParam", harkenstudio::kTargetScopeVoiceEffectParam},
    {"busEffectParam", harkenstudio::kTargetScopeBusEffectParam},
};

inline constexpr NamedValue kSelectionOrderNames[] = {
    {"weightedRandom", kLegacySelectionWeightedRandom},
    {"roundRobin", kLegacySelectionRoundRobin},
};

inline const char* EnumNameOf(const NamedValue* names,
                              std::size_t nameCount,
                              std::uint32_t value) {
    for (std::size_t i = 0; i < nameCount; ++i) {
        if (names[i].value == value) {
            return names[i].name;
        }
    }
    return "";
}

// choices is the human-readable list for the refusal (for example
// "\"voice\" or \"bus\"").
inline bool ReadEnumField(const JsonValue& object, const char* key,
                          const std::string& context, bool required,
                          const NamedValue* names, std::size_t nameCount,
                          const char* choices, std::uint32_t* out,
                          Diag* diag) {
    const JsonValue* value = object.Find(key);
    if (value == nullptr) {
        if (required) {
            return Fail(diag, "manifest %s requires a \"%s\" member",
                        context.c_str(), key);
        }
        return true;
    }
    if (value->kind == JsonValue::Kind::String) {
        for (std::size_t i = 0; i < nameCount; ++i) {
            if (value->text == names[i].name) {
                *out = names[i].value;
                return true;
            }
        }
    }
    return Fail(diag, "manifest %s.%s must be %s", context.c_str(), key,
                choices);
}

enum class CurveValueDomain {
    VolumeDb,
    LowpassHz,
    Width,
    Binding,
};

inline bool CurveYInDomain(float value, CurveValueDomain domain) {
    switch (domain) {
    case CurveValueDomain::VolumeDb:
        return value >= -96.0f && value <= 0.0f;
    case CurveValueDomain::LowpassHz:
        return value > 0.0f;
    case CurveValueDomain::Width:
        return value >= 0.0f && value <= 1.0f;
    case CurveValueDomain::Binding:
        return true;
    }
    return false;
}

inline const char* CurveDomainDescription(CurveValueDomain domain) {
    switch (domain) {
    case CurveValueDomain::VolumeDb:
        return "within [-96, 0]";
    case CurveValueDomain::LowpassHz:
        return "> 0";
    case CurveValueDomain::Width:
        return "within [0, 1]";
    case CurveValueDomain::Binding:
        return "finite";
    }
    return "valid";
}

// One curve object. The caller supplies the authored value domain; this
// generic parser validates geometry without inferring semantics from a key
// name. Curves carry 1..8 finite, strictly x-increasing points. Multi-point
// curves require exactly one absolute (h1x,h1y,h2x,h2y) tuple per segment;
// single-point curves must omit handles.
inline bool ParseCurveObject(const JsonValue& value,
                             const std::string& context,
                             CurveValueDomain domain,
                             ManifestCurve* out, Diag* diag) {
    if (value.kind != JsonValue::Kind::Object) {
        return Fail(diag, "manifest %s must be a curve object",
                    context.c_str());
    }
    static constexpr const char* kKeys[] = {"points", "handles"};
    if (!RefuseUnknownKeys(value, kKeys, 2, context, diag)) {
        return false;
    }
    const JsonValue* points = value.Find("points");
    if (points == nullptr || points->kind != JsonValue::Kind::Array ||
        points->items.empty() ||
        points->items.size() > harkenstudio::kCurveMaxPoints) {
        return Fail(diag,
                    "manifest %s.points must be an array of 1..%d "
                    "[x, y] pairs",
                    context.c_str(), harkenstudio::kCurveMaxPoints);
    }
    out->pointCount = static_cast<std::uint32_t>(points->items.size());
    for (std::size_t i = 0; i < points->items.size(); ++i) {
        const JsonValue& pair = points->items[i];
        float x = 0.0f;
        float y = 0.0f;
        if (pair.kind != JsonValue::Kind::Array ||
            pair.items.size() != 2 ||
            !ParseFiniteFloat(pair.items[0], &x) ||
            !ParseFiniteFloat(pair.items[1], &y)) {
            return Fail(diag,
                        "manifest %s.points[%zu] must be an [x, y] pair "
                        "of finite numbers",
                        context.c_str(), i);
        }
        if (i > 0 && !(x > out->pointX[i - 1])) {
            return Fail(diag,
                        "manifest %s.points must have strictly "
                        "increasing x values",
                        context.c_str());
        }
        if (!CurveYInDomain(y, domain)) {
            return Fail(diag,
                        "manifest %s.points[%zu][1] must be %s",
                        context.c_str(), i,
                        CurveDomainDescription(domain));
        }
        out->pointX[i] = x;
        out->pointY[i] = y;
    }
    const std::uint32_t segments = out->pointCount - 1;
    const JsonValue* handles = value.Find("handles");
    if (handles == nullptr) {
        if (segments != 0) {
            return Fail(diag,
                        "manifest %s.handles must carry exactly one "
                        "handle tuple per segment (%u)",
                        context.c_str(), segments);
        }
        return true;
    }
    if (segments == 0) {
        return Fail(diag,
                    "manifest %s.handles must be omitted for a "
                    "single-point curve",
                    context.c_str());
    }
    if (handles->kind != JsonValue::Kind::Array ||
        handles->items.size() != segments) {
        return Fail(diag,
                    "manifest %s.handles must carry exactly one "
                    "handle tuple per segment (%u)",
                    context.c_str(), segments);
    }
    for (std::size_t i = 0; i < handles->items.size(); ++i) {
        const JsonValue& tuple = handles->items[i];
        harkenstudiobuild::ParameterCurveHandles handle{};
        if (tuple.kind != JsonValue::Kind::Array ||
            tuple.items.size() != 4 ||
            !ParseFiniteFloat(tuple.items[0], &handle.h1x) ||
            !ParseFiniteFloat(tuple.items[1], &handle.h1y) ||
            !ParseFiniteFloat(tuple.items[2], &handle.h2x) ||
            !ParseFiniteFloat(tuple.items[3], &handle.h2y)) {
            return Fail(diag,
                        "manifest %s.handles[%zu] must be an "
                        "[h1x, h1y, h2x, h2y] tuple of finite numbers",
                        context.c_str(), i);
        }
        const float xa = out->pointX[i];
        const float xb = out->pointX[i + 1];
        if (handle.h1x < xa || handle.h1x > xb ||
            handle.h2x < xa || handle.h2x > xb) {
            return Fail(diag,
                        "manifest %s.handles[%zu] x coordinates must "
                        "stay within segment [%g, %g]",
                        context.c_str(), i, static_cast<double>(xa),
                        static_cast<double>(xb));
        }
        if (!CurveYInDomain(handle.h1y, domain) ||
            !CurveYInDomain(handle.h2y, domain)) {
            return Fail(diag,
                        "manifest %s.handles[%zu] y coordinates must "
                        "be %s",
                        context.c_str(), i,
                        CurveDomainDescription(domain));
        }
        out->handles[i] = handle;
    }
    return true;
}

inline bool ParseBusesSection(const JsonValue& section, Manifest* out,
                              Diag* diag) {
    for (std::size_t i = 0; i < section.items.size(); ++i) {
        const JsonValue& item = section.items[i];
        const std::string context = IndexedName("buses", i);
        if (item.kind != JsonValue::Kind::Object) {
            return Fail(diag, "manifest %s must be an object",
                        context.c_str());
        }
        static constexpr const char* kKeys[] = {
            "id", "parent", "volume", "outputBusVolume", "makeUpGain"};
        if (!RefuseUnknownKeys(item, kKeys, 5, context, diag)) {
            return false;
        }
        ManifestBus bus;
        if (!ReadObjectIdField(item, "id", context, true, nullptr, &bus.id,
                          diag) ||
            !ReadObjectIdField(item, "parent", context, true, nullptr,
                          &bus.parent, diag) ||
            !ReadFloatField(item, "volume", context, false, nullptr,
                            &bus.volume, diag) ||
            !ReadFloatField(item, "outputBusVolume", context, false,
                            nullptr, &bus.outputBusVolume, diag) ||
            !ReadFloatField(item, "makeUpGain", context, false, nullptr,
                            &bus.makeUpGain, diag)) {
            return false;
        }
        if (!std::isfinite(bus.volume +
                           bus.makeUpGain)) {
            return Fail(diag,
                        "manifest %s mix sum of volume, outputBusVolume, "
                        "and makeUpGain must be finite",
                        context.c_str());
        }
        if (harkenstudio::IsNull(bus.id)) {
            return Fail(diag,
                        "manifest %s.id must be a positive integer — "
                        "bus id 0 is reserved",
                        context.c_str());
        }
        if (bus.id == bus.parent) {
            return Fail(diag, "manifest %s parents the bus to itself",
                        context.c_str());
        }
        out->buses.push_back(bus);
    }
    return true;
}

inline bool ParseBusSendsSection(const JsonValue& section, Manifest* out,
                                 Diag* diag) {
    for (std::size_t i = 0; i < section.items.size(); ++i) {
        const JsonValue& item = section.items[i];
        const std::string context = IndexedName("busSends", i);
        if (item.kind != JsonValue::Kind::Object) {
            return Fail(diag, "manifest %s must be an object",
                        context.c_str());
        }
        static constexpr const char* kKeys[] = {"from", "to", "levelDb"};
        if (!RefuseUnknownKeys(item, kKeys, 3, context, diag)) {
            return false;
        }
        ManifestBusSend send;
        if (!ReadObjectIdField(item, "from", context, true, nullptr,
                          &send.from, diag) ||
            !ReadObjectIdField(item, "to", context, true, nullptr, &send.to,
                          diag) ||
            !ReadFloatField(item, "levelDb", context, true, nullptr,
                            &send.levelDb, diag)) {
            return false;
        }
        if (send.from == send.to) {
            return Fail(diag,
                        "manifest %s connects bus %s to itself",
                        context.c_str(),
                        harkenstudio::ObjectIdString(send.from).c_str());
        }
        out->busSends.push_back(send);
    }
    return true;
}

inline bool ParseAttenuationsSection(const JsonValue& section,
                                     Manifest* out, Diag* diag) {
    for (std::size_t i = 0; i < section.items.size(); ++i) {
        const JsonValue& item = section.items[i];
        const std::string context = IndexedName("attenuations", i);
        if (item.kind != JsonValue::Kind::Object) {
            return Fail(diag, "manifest %s must be an object",
                        context.c_str());
        }
        static constexpr const char* kKeys[] = {
            "id",          "rolloff",
            "minDistance", "maxDistance",
            "airAbsorptionNearHz", "airAbsorptionFarHz",
            "proximityRadius",     "proximityMaxBoostDb",
            "proximityFrequency",
            "cone",        "volumeCurve",
            "lowpassCurve", "widthCurve"};
        if (!RefuseUnknownKeys(item, kKeys, 13, context, diag)) {
            return false;
        }
        ManifestAttenuation attenuation;
        if (!ReadObjectIdField(item, "id", context, true, nullptr,
                          &attenuation.id, diag) ||
            !ReadEnumField(item, "rolloff", context, true, kRolloffNames,
                           3,
                           "\"linear\", \"inverseSquare\", or "
                           "\"logarithmic\"",
                           &attenuation.rolloff, diag) ||
            !ReadFloatField(item, "minDistance", context, true, nullptr,
                            &attenuation.minDistance, diag) ||
            !ReadFloatField(item, "maxDistance", context, true, nullptr,
                            &attenuation.maxDistance, diag) ||
            !ReadFloatField(item, "airAbsorptionNearHz", context, true,
                            nullptr, &attenuation.airAbsorptionNearHz,
                            diag) ||
            !ReadFloatField(item, "airAbsorptionFarHz", context, true,
                            nullptr, &attenuation.airAbsorptionFarHz,
                            diag) ||
            !ReadFloatField(item, "proximityRadius", context, false,
                            nullptr, &attenuation.proximityRadius,
                            diag) ||
            !ReadFloatField(item, "proximityMaxBoostDb", context, false,
                            nullptr, &attenuation.proximityMaxBoostDb,
                            diag) ||
            !ReadFloatField(item, "proximityFrequency", context, false,
                            &attenuation.hasProximityFrequency,
                            &attenuation.proximityFrequency, diag)) {
            return false;
        }
        // The registration bounds (spatializer profile build): a
        // non-empty distance span anchored above zero, positive
        // log-domain air endpoints, a non-negative proximity radius.
        if (!(attenuation.minDistance > 0.0f) ||
            !(attenuation.maxDistance > attenuation.minDistance)) {
            return Fail(diag,
                        "manifest %s requires 0 < minDistance < "
                        "maxDistance",
                        context.c_str());
        }
        if (!(attenuation.airAbsorptionNearHz > 0.0f) ||
            !(attenuation.airAbsorptionFarHz > 0.0f)) {
            return Fail(diag,
                        "manifest %s requires airAbsorptionNearHz > 0 "
                        "and airAbsorptionFarHz > 0",
                        context.c_str());
        }
        if (attenuation.proximityRadius < 0.0f) {
            return Fail(diag,
                        "manifest %s.proximityRadius must be >= 0",
                        context.c_str());
        }
        if (!(attenuation.proximityFrequency > 0.0f)) {
            return Fail(diag,
                        "manifest %s.proximityFrequency must be > 0",
                        context.c_str());
        }
        const JsonValue* cone = item.Find("cone");
        if (cone != nullptr) {
            const std::string coneContext = context + ".cone";
            if (cone->kind != JsonValue::Kind::Object) {
                return Fail(diag, "manifest %s must be an object",
                            coneContext.c_str());
            }
            static constexpr const char* kConeKeys[] = {
                "innerDeg", "outerDeg", "outerGainDb", "outerLowpassHz"};
            if (!RefuseUnknownKeys(*cone, kConeKeys, 4, coneContext,
                                   diag)) {
                return false;
            }
            attenuation.coneEnabled = true;
            if (!ReadFloatField(*cone, "innerDeg", coneContext, true,
                                nullptr, &attenuation.coneInnerDeg,
                                diag) ||
                !ReadFloatField(*cone, "outerDeg", coneContext, true,
                                nullptr, &attenuation.coneOuterDeg,
                                diag) ||
                !ReadFloatField(*cone, "outerGainDb", coneContext, true,
                                nullptr, &attenuation.coneOuterGainDb,
                                diag) ||
                !ReadFloatField(*cone, "outerLowpassHz", coneContext,
                                true, nullptr,
                                &attenuation.coneOuterLowpassHz, diag)) {
                return false;
            }
            if (attenuation.coneInnerDeg < 0.0f ||
                attenuation.coneOuterDeg < attenuation.coneInnerDeg ||
                attenuation.coneOuterDeg > 180.0f) {
                return Fail(diag,
                            "manifest %s requires 0 <= innerDeg <= "
                            "outerDeg <= 180",
                            coneContext.c_str());
            }
            if (!(attenuation.coneOuterLowpassHz > 0.0f)) {
                return Fail(diag,
                            "manifest %s.outerLowpassHz must be > 0",
                            coneContext.c_str());
            }
        }
        const JsonValue* volumeCurve = item.Find("volumeCurve");
        if (volumeCurve != nullptr) {
            attenuation.hasVolumeCurve = true;
            if (!ParseCurveObject(*volumeCurve, context + ".volumeCurve",
                                  CurveValueDomain::VolumeDb,
                                  &attenuation.volumeCurve, diag)) {
                return false;
            }
        }
        const JsonValue* lowpassCurve = item.Find("lowpassCurve");
        if (lowpassCurve != nullptr) {
            attenuation.hasLowpassCurve = true;
            if (!ParseCurveObject(*lowpassCurve,
                                  context + ".lowpassCurve",
                                  CurveValueDomain::LowpassHz,
                                  &attenuation.lowpassCurve, diag)) {
                return false;
            }
        }
        const JsonValue* widthCurve = item.Find("widthCurve");
        if (widthCurve != nullptr) {
            attenuation.hasWidthCurve = true;
            if (!ParseCurveObject(*widthCurve, context + ".widthCurve",
                                  CurveValueDomain::Width,
                                  &attenuation.widthCurve, diag)) {
                return false;
            }
        }
        out->attenuations.push_back(attenuation);
    }
    return true;
}

inline bool EffectParamTargetIsBindable(std::uint32_t target) {
    const harkenstudiobuild::EffectParamLocator locator =
        harkenstudiobuild::UnpackEffectParamLocator(target);
    const auto* effect = harkenstudiobuild::FindPackEffectSchemaByType(locator.effectType);
    return effect && locator.paramIndex < effect->paramCount
        && (locator.effectType != harkenstudio::kEffectFeedbackDelay || locator.paramIndex >= 2);
}

inline bool ParseBindingsSection(const JsonValue& section, Manifest* out,
                                 Diag* diag) {
    for (std::size_t i = 0; i < section.items.size(); ++i) {
        const JsonValue& item = section.items[i];
        const std::string context = IndexedName("bindings", i);
        if (item.kind != JsonValue::Kind::Object) {
            return Fail(diag, "manifest %s must be an object",
                        context.c_str());
        }
        static constexpr const char* kKeys[] = {
            "id", "variable", "scope", "target", "transitionMs",
            "curve"};
        if (!RefuseUnknownKeys(item, kKeys, 6, context, diag)) {
            return false;
        }
        ManifestBinding binding;
        if (!ReadObjectIdField(item, "id", context, true, nullptr,
                          &binding.id, diag) ||
            !ReadObjectIdField(item, "variable", context, true, nullptr,
                          &binding.variable, diag) ||
            !ReadEnumField(item, "scope", context, true,
                           kTargetScopeNames, 4,
                           "\"voice\", \"bus\", \"voiceEffectParam\", or "
                           "\"busEffectParam\"", &binding.scope,
                           diag) ||
            !ReadU32Field(item, "target", context, true, nullptr,
                          &binding.target, diag) ||
            !ReadU32Field(item, "transitionMs", context, false, nullptr,
                          &binding.transitionMs, diag)) {
            return false;
        }
        if (!harkenstudiobuild::IsValidBindingTransitionMs(binding.transitionMs)) {
            return Fail(diag, "manifest %s.transitionMs must be 0..60000", context.c_str());
        }
        const auto* targetMeta = harkenstudiobuild::FindTargetMeta(binding.scope, binding.target);
        if ((binding.scope == harkenstudio::kTargetScopeVoice || binding.scope == harkenstudio::kTargetScopeBus)
            && !targetMeta) {
            return Fail(diag, "manifest %s.target is unknown for its scope", context.c_str());
        }
        if (binding.scope == harkenstudio::kTargetScopeVoiceEffectParam ||
            binding.scope == harkenstudio::kTargetScopeBusEffectParam) {
            if (!EffectParamTargetIsBindable(binding.target)) {
                return Fail(diag,
                            "manifest %s.target must locate a known, "
                            "bindable effect parameter",
                            context.c_str());
            }
            if (binding.transitionMs != 0) {
                return Fail(diag,
                            "manifest %s.transitionMs must be 0 for an "
                            "effect-parameter target",
                            context.c_str());
            }
        }
        const bool supportsTransition = targetMeta && targetMeta->supportsTransition;
        if (binding.transitionMs != 0 && !supportsTransition) {
            return Fail(diag,
                        "manifest %s.transitionMs must be 0 — this target "
                        "does not carry a ramp",
                        context.c_str());
        }
        const JsonValue* curve = item.Find("curve");
        if (curve == nullptr) {
            return Fail(diag,
                        "manifest %s requires a \"curve\" member",
                        context.c_str());
        }
        if (!ParseCurveObject(*curve, context + ".curve",
                              CurveValueDomain::Binding, &binding.curve,
                              diag)) {
            return false;
        }
        out->bindings.push_back(binding);
    }
    return true;
}

inline bool ParseRecipeEffectType(const JsonValue& value,
                                  std::uint32_t* out) {
    if (value.kind != JsonValue::Kind::String) {
        return false;
    }
    const harkenstudiobuild::PackEffectSchemaRow* row =
        harkenstudiobuild::FindPackEffectSchemaByManifestName(value.text.c_str());
    if (row == nullptr) {
        return false;
    }
    *out = row->effectType;
    return true;
}

inline std::uint32_t RecipeParamCount(std::uint32_t type) {
    const harkenstudiobuild::PackEffectSchemaRow* row =
        harkenstudiobuild::FindPackEffectSchemaByType(type);
    return row == nullptr ? 0 : row->paramCount;
}

inline const char* RecipeEffectTypeName(std::uint32_t type) {
    const harkenstudiobuild::PackEffectSchemaRow* row =
        harkenstudiobuild::FindPackEffectSchemaByType(type);
    return row == nullptr ? "" : row->manifestName;
}

inline bool RecipeParamInAuthoredRange(std::uint32_t type,
                                       std::uint32_t index, float value) {
    return harkenstudiobuild::PackEffectParamInAuthoredRange(type, index, value);
}

inline bool ParseBusEffectsSection(const JsonValue& section,
                                   Manifest* out, Diag* diag) {
    for (std::size_t i = 0; i < section.items.size(); ++i) {
        const JsonValue& item = section.items[i];
        const std::string context = IndexedName("busEffects", i);
        if (item.kind != JsonValue::Kind::Object) {
            return Fail(diag, "manifest %s must be an object",
                        context.c_str());
        }
        static constexpr const char* kKeys[] = {
            "bus", "slot", "type", "descriptorVintage", "bypass",
            "faderPointGuard", "parameters", "effectId"};
        if (!RefuseUnknownKeys(item, kKeys, 8, context, diag)) {
            return false;
        }
        ManifestBusEffect effect;
        bool hasDescriptorVintage = false;
        if (!ReadObjectIdField(item, "effectId", context, false, nullptr,
                          &effect.effectId, diag) ||
            !ReadObjectIdField(item, "bus", context, true, nullptr,
                          &effect.bus, diag) ||
            !ReadU32Field(item, "slot", context, true, nullptr,
                          &effect.slot, diag) ||
            !ReadU32Field(item, "descriptorVintage", context, false,
                          &hasDescriptorVintage,
                          &effect.descriptorVintage, diag) ||
            !ReadU32Field(item, "faderPointGuard", context, true, nullptr,
                          &effect.faderPointGuard, diag)) {
            return false;
        }
        const JsonValue* type = item.Find("type");
        const JsonValue* bypass = item.Find("bypass");
        const JsonValue* params = item.Find("parameters");
        if (harkenstudio::IsNull(effect.bus)) {
            return Fail(diag, "manifest %s.bus must be nonzero",
                        context.c_str());
        }
        if (effect.slot >= harkenstudiobuild::kPackV2VoiceRecipeMaxInstalls) {
            return Fail(diag, "manifest %s.slot must be 0..%u",
                        context.c_str(),
                        harkenstudiobuild::kPackV2VoiceRecipeMaxInstalls - 1);
        }
        if (effect.faderPointGuard >
            harkenstudiobuild::kPackV2VoiceRecipeMaxInstalls) {
            return Fail(diag,
                        "manifest %s.faderPointGuard must be 0..%u",
                        context.c_str(),
                        harkenstudiobuild::kPackV2VoiceRecipeMaxInstalls);
        }
        if (type == nullptr || !ParseRecipeEffectType(*type, &effect.type)) {
            return Fail(diag,
                        "manifest %s.type must name a registered effect",
                        context.c_str());
        }
        const harkenstudiobuild::PackEffectSchemaRow* effectSchema =
            harkenstudiobuild::FindPackEffectSchemaByType(effect.type);
        if (!hasDescriptorVintage) {
            effect.descriptorVintage = effectSchema->descriptorVintage;
        }
        if (effect.descriptorVintage != effectSchema->descriptorVintage) {
            return Fail(diag,
                        "manifest %s.descriptorVintage must be %u",
                        context.c_str(),
                        effectSchema->descriptorVintage);
        }
        if (bypass != nullptr) {
            if (bypass->kind != JsonValue::Kind::Bool) {
                return Fail(diag, "manifest %s.bypass must be a boolean",
                            context.c_str());
            }
            effect.bypass = bypass->boolean;
        }
        const std::uint32_t expected = RecipeParamCount(effect.type);
        if (params == nullptr || params->kind != JsonValue::Kind::Array ||
            params->items.size() != expected) {
            return Fail(diag,
                        "manifest %s.parameters must contain exactly %u "
                        "values",
                        context.c_str(), expected);
        }
        for (std::uint32_t p = 0; p < expected; ++p) {
            float value = 0.0f;
            if (!ParseFiniteFloat(params->items[p], &value) ||
                !RecipeParamInAuthoredRange(effect.type, p, value)) {
                return Fail(diag,
                            "manifest %s.parameters[%u] is outside the "
                            "effect's authored range",
                            context.c_str(), p);
            }
            effect.parameters.push_back(value);
        }
        out->busEffects.push_back(std::move(effect));
    }
    std::sort(out->busEffects.begin(), out->busEffects.end(),
              [](const ManifestBusEffect& a, const ManifestBusEffect& b) {
                  return a.bus < b.bus ||
                         (a.bus == b.bus && a.slot < b.slot);
              });
    for (std::size_t i = 1; i < out->busEffects.size(); ++i) {
        if (out->busEffects[i - 1].bus == out->busEffects[i].bus &&
            out->busEffects[i - 1].slot == out->busEffects[i].slot) {
            return Fail(diag,
                        "manifest busEffects repeats target (%s,%u)",
                        harkenstudio::ObjectIdString(out->busEffects[i].bus).c_str(),
                        out->busEffects[i].slot);
        }
    }
    return true;
}

inline bool ParseDynamicTap(const JsonValue& value, std::uint32_t* out) {
    if (value.kind != JsonValue::Kind::String) {
        return false;
    }
    if (value.text == "postEffects") {
        *out = harkenstudio::kDynamicTapBusPostEffects;
        return true;
    }
    if (value.text == "preFader") {
        *out = harkenstudio::kDynamicTapBusPreFader;
        return true;
    }
    if (value.text == "postFader") {
        *out = harkenstudio::kDynamicTapBusPostFader;
        return true;
    }
    return false;
}

inline const char* DynamicTapName(std::uint32_t tap) {
    switch (tap) {
    case harkenstudio::kDynamicTapBusPostEffects:
        return "postEffects";
    case harkenstudio::kDynamicTapBusPreFader:
        return "preFader";
    case harkenstudio::kDynamicTapBusPostFader:
        return "postFader";
    default:
        return "";
    }
}

inline bool ParseDynamicRoutesSection(const JsonValue& section,
                                      Manifest* out, Diag* diag) {
    for (std::size_t i = 0; i < section.items.size(); ++i) {
        const JsonValue& item = section.items[i];
        const std::string context = IndexedName("dynamicRoutes", i);
        if (item.kind != JsonValue::Kind::Object) {
            return Fail(diag, "manifest %s must be an object",
                        context.c_str());
        }
        static constexpr const char* kKeys[] = {
            "targetBus", "effectSlot", "routeSlot", "sourceBus",
            "tap", "levelDb"};
        if (!RefuseUnknownKeys(item, kKeys, 6, context, diag)) {
            return false;
        }
        ManifestDynamicRoute route;
        if (!ReadObjectIdField(item, "targetBus", context, true, nullptr,
                          &route.targetBus, diag) ||
            !ReadU32Field(item, "effectSlot", context, true, nullptr,
                          &route.effectSlot, diag) ||
            !ReadU32Field(item, "routeSlot", context, true, nullptr,
                          &route.routeSlot, diag) ||
            !ReadObjectIdField(item, "sourceBus", context, true, nullptr,
                          &route.sourceBus, diag) ||
            !ReadFloatField(item, "levelDb", context, true, nullptr,
                            &route.levelDb, diag)) {
            return false;
        }
        const JsonValue* tap = item.Find("tap");
        if (harkenstudio::IsNull(route.targetBus) || harkenstudio::IsNull(route.sourceBus)) {
            return Fail(diag,
                        "manifest %s targetBus and sourceBus must be nonzero",
                        context.c_str());
        }
        if (route.effectSlot >= harkenstudiobuild::kPackV2VoiceRecipeMaxInstalls ||
            route.routeSlot >= 4) {
            return Fail(diag,
                        "manifest %s effectSlot must be 0..%u and "
                        "routeSlot must be 0..3",
                        context.c_str(),
                        harkenstudiobuild::kPackV2VoiceRecipeMaxInstalls - 1);
        }
        if (tap == nullptr || !ParseDynamicTap(*tap, &route.tap)) {
            return Fail(diag,
                        "manifest %s.tap must be \"postEffects\", "
                        "\"preFader\", or \"postFader\"",
                        context.c_str());
        }
        if (route.levelDb < -96.0f || route.levelDb > 24.0f) {
            return Fail(diag,
                        "manifest %s.levelDb must be within [-96, 24]",
                        context.c_str());
        }
        out->dynamicRoutes.push_back(route);
    }
    std::sort(
        out->dynamicRoutes.begin(), out->dynamicRoutes.end(),
        [](const ManifestDynamicRoute& a, const ManifestDynamicRoute& b) {
            if (a.targetBus != b.targetBus) {
                return a.targetBus < b.targetBus;
            }
            if (a.effectSlot != b.effectSlot) {
                return a.effectSlot < b.effectSlot;
            }
            return a.routeSlot < b.routeSlot;
        });
    for (std::size_t i = 1; i < out->dynamicRoutes.size(); ++i) {
        const ManifestDynamicRoute& a = out->dynamicRoutes[i - 1];
        const ManifestDynamicRoute& b = out->dynamicRoutes[i];
        if (a.targetBus == b.targetBus &&
            a.effectSlot == b.effectSlot &&
            a.routeSlot == b.routeSlot) {
            return Fail(diag,
                        "manifest dynamicRoutes repeats target "
                        "(%s,%u,%u)",
                        harkenstudio::ObjectIdString(b.targetBus).c_str(),
                        b.effectSlot, b.routeSlot);
        }
    }
    for (const ManifestDynamicRoute& route : out->dynamicRoutes) {
        bool declaredTarget = false;
        for (const ManifestBusEffect& effect : out->busEffects) {
            if (effect.bus == route.targetBus &&
                effect.slot == route.effectSlot &&
                effect.type == harkenstudio::kEffectCompressor) {
                declaredTarget = true;
                break;
            }
        }
        if (!declaredTarget) {
            return Fail(diag,
                        "manifest dynamic route target (%s,%u) must "
                        "name a Compressor in busEffects",
                        harkenstudio::ObjectIdString(route.targetBus).c_str(),
                        route.effectSlot);
        }
    }
    return true;
}

inline bool ParseVoiceRecipesSection(const JsonValue& section,
                                     Manifest* out, Diag* diag) {
    for (std::size_t i = 0; i < section.items.size(); ++i) {
        const JsonValue& item = section.items[i];
        const std::string context = IndexedName("voiceRecipes", i);
        if (item.kind != JsonValue::Kind::Object) {
            return Fail(diag, "manifest %s must be an object",
                        context.c_str());
        }
        static constexpr const char* kKeys[] = {
            "playable", "tone", "spatial", "playback", "effects",
            "bindings"};
        if (!RefuseUnknownKeys(item, kKeys, 6, context, diag)) {
            return false;
        }
        ManifestVoiceRecipe recipe;
        if (!ReadObjectIdField(item, "playable", context, true, nullptr,
                          &recipe.playable, diag)) {
            return false;
        }
        if (harkenstudio::IsNull(recipe.playable)) {
            return Fail(diag, "manifest %s.playable must be nonzero",
                        context.c_str());
        }
        const JsonValue* tone = item.Find("tone");
        const JsonValue* spatial = item.Find("spatial");
        const JsonValue* playback = item.Find("playback");
        const JsonValue* effects = item.Find("effects");
        const JsonValue* bindings = item.Find("bindings");
        if (effects == nullptr || effects->kind != JsonValue::Kind::Array ||
            effects->items.size() > harkenstudiobuild::kPackV2VoiceRecipeMaxInstalls) {
            return Fail(diag,
                        "manifest %s.effects must contain at most %u entries",
                        context.c_str(),
                        harkenstudiobuild::kPackV2VoiceRecipeMaxInstalls);
        }
        if (bindings == nullptr ||
            bindings->kind != JsonValue::Kind::Array ||
            bindings->items.size() > harkenstudiobuild::kPackV2VoiceRecipeMaxBindings) {
            return Fail(diag,
                        "manifest %s.bindings must be an array of at most %u "
                        "ids",
                        context.c_str(),
                        harkenstudiobuild::kPackV2VoiceRecipeMaxBindings);
        }
        if (tone != nullptr) {
            const std::string toneContext = context + ".tone";
            if (tone->kind != JsonValue::Kind::Object) {
                return Fail(diag, "manifest %s must be an object",
                            toneContext.c_str());
            }
            static constexpr const char* kToneKeys[] = {
                "lowpassEnabled", "lowpassFrequencyHz", "lowpassQ",
                "highpassEnabled", "highpassFrequencyHz", "highpassQ",
                "lowShelfEnabled", "lowShelfFrequencyHz", "lowShelfQ",
                "lowShelfGainDb", "highShelfEnabled",
                "highShelfFrequencyHz", "highShelfQ",
                "highShelfGainDb"};
            if (!RefuseUnknownKeys(*tone, kToneKeys, 14, toneContext,
                                   diag)) {
                return false;
            }
            const auto readBool = [&](const char* key, bool* value) {
                const JsonValue* field = tone->Find(key);
                if (field == nullptr || field->kind != JsonValue::Kind::Bool) {
                    return Fail(diag, "manifest %s.%s must be a boolean",
                                toneContext.c_str(), key);
                }
                *value = field->boolean;
                return true;
            };
            recipe.tone.present = true;
            if (!readBool("lowpassEnabled",
                          &recipe.tone.lowpassEnabled) ||
                !readBool("highpassEnabled",
                          &recipe.tone.highpassEnabled) ||
                !readBool("lowShelfEnabled",
                          &recipe.tone.lowShelfEnabled) ||
                !readBool("highShelfEnabled",
                          &recipe.tone.highShelfEnabled) ||
                !ReadFloatField(*tone, "lowpassFrequencyHz", toneContext,
                                true, nullptr,
                                &recipe.tone.lowpassFrequencyHz, diag) ||
                !ReadFloatField(*tone, "lowpassQ", toneContext, true,
                                nullptr, &recipe.tone.lowpassQ, diag) ||
                !ReadFloatField(*tone, "highpassFrequencyHz", toneContext,
                                true, nullptr,
                                &recipe.tone.highpassFrequencyHz, diag) ||
                !ReadFloatField(*tone, "highpassQ", toneContext, true,
                                nullptr, &recipe.tone.highpassQ, diag) ||
                !ReadFloatField(*tone, "lowShelfFrequencyHz", toneContext,
                                true, nullptr,
                                &recipe.tone.lowShelfFrequencyHz, diag) ||
                !ReadFloatField(*tone, "lowShelfQ", toneContext, true,
                                nullptr, &recipe.tone.lowShelfQ, diag) ||
                !ReadFloatField(*tone, "lowShelfGainDb", toneContext,
                                true, nullptr,
                                &recipe.tone.lowShelfGainDb, diag) ||
                !ReadFloatField(*tone, "highShelfFrequencyHz", toneContext,
                                true, nullptr,
                                &recipe.tone.highShelfFrequencyHz, diag) ||
                !ReadFloatField(*tone, "highShelfQ", toneContext, true,
                                nullptr, &recipe.tone.highShelfQ, diag) ||
                !ReadFloatField(*tone, "highShelfGainDb", toneContext,
                                true, nullptr,
                                &recipe.tone.highShelfGainDb, diag)) {
                return false;
            }
            const auto validFrequency = [](float value) {
                return value > 0.0f;
            };
            const auto validQ = [](float value) {
                return value >= 0.1f && value <= 10.0f;
            };
            const auto validGain = [](float value) {
                return value >= -48.0f && value <= 48.0f;
            };
            if (!validFrequency(recipe.tone.lowpassFrequencyHz) ||
                !validFrequency(recipe.tone.highpassFrequencyHz) ||
                !validFrequency(recipe.tone.lowShelfFrequencyHz) ||
                !validFrequency(recipe.tone.highShelfFrequencyHz) ||
                !validQ(recipe.tone.lowpassQ) ||
                !validQ(recipe.tone.highpassQ) ||
                !validQ(recipe.tone.lowShelfQ) ||
                !validQ(recipe.tone.highShelfQ) ||
                !validGain(recipe.tone.lowShelfGainDb) ||
                !validGain(recipe.tone.highShelfGainDb)) {
                return Fail(diag,
                            "manifest %s contains an out-of-range Tone value",
                            toneContext.c_str());
            }
        }
        if (spatial != nullptr) {
            const std::string spatialContext = context + ".spatial";
            if (spatial->kind != JsonValue::Kind::Object) {
                return Fail(diag, "manifest %s must be an object",
                            spatialContext.c_str());
            }
            static constexpr const char* kSpatialKeys[] = {
                "attenuation", "distanceScale"};
            if (!RefuseUnknownKeys(*spatial, kSpatialKeys, 2,
                                   spatialContext, diag) ||
                !ReadObjectIdField(*spatial, "attenuation", spatialContext,
                              true, nullptr, &recipe.spatial.attenuation,
                              diag) ||
                !ReadFloatField(*spatial, "distanceScale", spatialContext,
                                true, nullptr,
                                &recipe.spatial.distanceScale, diag)) {
                return false;
            }
            if (harkenstudio::IsNull(recipe.spatial.attenuation) ||
                recipe.spatial.distanceScale <= 0.0f) {
                return Fail(diag,
                            "manifest %s requires a nonzero attenuation and "
                            "positive distanceScale",
                            spatialContext.c_str());
            }
            recipe.spatial.present = true;
        }
        if (playback != nullptr) {
            const std::string playbackContext = context + ".playback";
            if (playback->kind != JsonValue::Kind::Object) {
                return Fail(diag, "manifest %s must be an object",
                            playbackContext.c_str());
            }
            static constexpr const char* kPlaybackKeys[] = {"loopCount"};
            if (!RefuseUnknownKeys(*playback, kPlaybackKeys, 1,
                                   playbackContext, diag) ||
                !ReadU32Field(*playback, "loopCount", playbackContext, true,
                              nullptr, &recipe.playback.loopCount, diag)) {
                return false;
            }
            recipe.playback.present = true;
        }
        if (tone == nullptr && spatial == nullptr && playback == nullptr &&
            effects->items.empty() && bindings->items.empty()) {
            return Fail(diag,
                        "manifest %s must contain Tone, Spatial, Playback, "
                        "an Effect, or a Binding",
                        context.c_str());
        }
        for (std::size_t e = 0; e < effects->items.size(); ++e) {
            const JsonValue& effect = effects->items[e];
            const std::string effectContext =
                context + ".effects[" + std::to_string(e) + "]";
            if (effect.kind != JsonValue::Kind::Object) {
                return Fail(diag, "manifest %s must be an object",
                            effectContext.c_str());
            }
            static constexpr const char* kEffectKeys[] = {
                "slot", "type", "descriptorVintage", "bypass",
                "parameters", "effectId"};
            if (!RefuseUnknownKeys(effect, kEffectKeys, 6, effectContext,
                                   diag)) {
                return false;
            }
            ManifestVoiceRecipeEffect parsed;
            bool hasDescriptorVintage = false;
            if (!ReadObjectIdField(effect, "effectId", effectContext, false, nullptr,
                              &parsed.effectId, diag) ||
                !ReadU32Field(effect, "slot", effectContext, true, nullptr,
                              &parsed.slot, diag) ||
                !ReadU32Field(effect, "descriptorVintage", effectContext,
                              false, &hasDescriptorVintage,
                              &parsed.descriptorVintage,
                              diag)) {
                return false;
            }
            const JsonValue* type = effect.Find("type");
            const JsonValue* bypass = effect.Find("bypass");
            const JsonValue* params = effect.Find("parameters");
            if (type == nullptr || !ParseRecipeEffectType(*type, &parsed.type)) {
                return Fail(diag,
                            "manifest %s.type must be one of %s",
                            effectContext.c_str(),
                            harkenstudiobuild::kPackEffectManifestNameList);
            }
            const harkenstudiobuild::PackEffectSchemaRow* effectSchema =
                harkenstudiobuild::FindPackEffectSchemaByType(parsed.type);
            if (!hasDescriptorVintage) {
                parsed.descriptorVintage =
                    effectSchema->descriptorVintage;
            }
            if (parsed.descriptorVintage !=
                effectSchema->descriptorVintage) {
                return Fail(diag,
                            "manifest %s.descriptorVintage must be %u",
                            effectContext.c_str(),
                            effectSchema->descriptorVintage);
            }
            if (parsed.slot >= harkenstudiobuild::kPackV2VoiceRecipeMaxInstalls) {
                return Fail(diag, "manifest %s.slot must be 0..%u",
                            effectContext.c_str(),
                            harkenstudiobuild::kPackV2VoiceRecipeMaxInstalls - 1);
            }
            if (bypass != nullptr) {
                if (bypass->kind != JsonValue::Kind::Bool) {
                    return Fail(diag, "manifest %s.bypass must be a boolean",
                                effectContext.c_str());
                }
                parsed.bypass = bypass->boolean;
            }
            const std::uint32_t expected = RecipeParamCount(parsed.type);
            if (params == nullptr || params->kind != JsonValue::Kind::Array ||
                params->items.size() != expected) {
                return Fail(diag,
                            "manifest %s.parameters must contain exactly %u "
                            "values",
                            effectContext.c_str(), expected);
            }
            for (std::uint32_t p = 0; p < expected; ++p) {
                float value = 0.0f;
                if (!ParseFiniteFloat(params->items[p], &value) ||
                    !RecipeParamInAuthoredRange(parsed.type, p, value)) {
                    return Fail(diag,
                                "manifest %s.parameters[%u] is outside the "
                                "effect's authored range",
                                effectContext.c_str(), p);
                }
                parsed.parameters.push_back(value);
            }
            recipe.effects.push_back(std::move(parsed));
        }
        for (const JsonValue& binding : bindings->items) {
            harkenstudio::ObjectId id{};
            if (binding.kind != JsonValue::Kind::String || !harkenstudio::ParseObjectId(binding.text, &id) || harkenstudio::IsNull(id)) {
                return Fail(diag,
                            "manifest %s.bindings must contain nonzero "
                            "integer ids",
                            context.c_str());
            }
            recipe.bindings.push_back(id);
        }
        std::sort(recipe.effects.begin(), recipe.effects.end(),
                  [](const ManifestVoiceRecipeEffect& a,
                     const ManifestVoiceRecipeEffect& b) {
                      return a.slot < b.slot;
                  });
        for (std::size_t e = 1; e < recipe.effects.size(); ++e) {
            if (recipe.effects[e - 1].slot == recipe.effects[e].slot) {
                return Fail(diag,
                            "manifest %s.effects repeats slot %u",
                            context.c_str(), recipe.effects[e].slot);
            }
        }
        std::sort(recipe.bindings.begin(), recipe.bindings.end());
        if (std::adjacent_find(recipe.bindings.begin(),
                               recipe.bindings.end()) !=
            recipe.bindings.end()) {
            return Fail(diag, "manifest %s.bindings repeats an id",
                        context.c_str());
        }
        out->voiceRecipes.push_back(std::move(recipe));
    }
    std::sort(out->voiceRecipes.begin(), out->voiceRecipes.end(),
              [](const ManifestVoiceRecipe& a,
                 const ManifestVoiceRecipe& b) {
                  return a.playable < b.playable;
              });
    return true;
}

inline bool ParseAttachmentsSection(const JsonValue& section,
                                    Manifest* out, Diag* diag) {
    for (std::size_t i = 0; i < section.items.size(); ++i) {
        const JsonValue& item = section.items[i];
        const std::string context = IndexedName("attachments", i);
        if (item.kind != JsonValue::Kind::Object) {
            return Fail(diag, "manifest %s must be an object",
                        context.c_str());
        }
        static constexpr const char* kKeys[] = {"binding", "scope",
                                                "object"};
        if (!RefuseUnknownKeys(item, kKeys, 3, context, diag)) {
            return false;
        }
        ManifestAttachment attachment;
        if (!ReadObjectIdField(item, "binding", context, true, nullptr,
                          &attachment.binding, diag) ||
            !ReadEnumField(item, "scope", context, true,
                           kTargetScopeNames, 4,
                           "\"voice\", \"bus\", \"voiceEffectParam\", or "
                           "\"busEffectParam\"", &attachment.scope,
                           diag) ||
            !ReadObjectIdField(item, "object", context, true, nullptr,
                          &attachment.object, diag)) {
            return false;
        }
        if (attachment.scope == harkenstudio::kTargetScopeVoiceEffectParam) {
            return Fail(diag,
                        "manifest %s.scope does not support voice-effect-"
                        "parameter attachments; use voiceRecipes[].bindings",
                        context.c_str());
        }
        out->attachments.push_back(attachment);
    }
    return true;
}

inline bool ParseContainersSection(const JsonValue& section,
                                   Manifest* out, Diag* diag) {
    for (std::size_t i = 0; i < section.items.size(); ++i) {
        const JsonValue& item = section.items[i];
        const std::string context = IndexedName("containers", i);
        if (item.kind != JsonValue::Kind::Object) {
            return Fail(diag, "manifest %s must be an object",
                        context.c_str());
        }
        static constexpr const char* kKeys[] = {
            "id",      "order",   "selectionVariable",
            "avoidRepeatWindow", "default", "children"};
        if (!RefuseUnknownKeys(item, kKeys, 6, context, diag)) {
            return false;
        }
        ManifestContainer container;
        bool defaultPresent = false;
        std::uint32_t defaultIndex = 0;
        if (!ReadObjectIdField(item, "id", context, true, nullptr,
                          &container.id, diag) ||
            !ReadEnumField(item, "order", context, true,
                           kSelectionOrderNames, 2,
                           "\"weightedRandom\" or \"roundRobin\"",
                           &container.order, diag) ||
            !ReadObjectIdField(item, "selectionVariable", context, false,
                          nullptr, &container.selectionVariable,
                          diag) ||
            !ReadU32Field(item, "avoidRepeatWindow", context, false,
                          nullptr, &container.avoidRepeatWindow,
                          diag) ||
            !ReadU32Field(item, "default", context, false,
                          &defaultPresent, &defaultIndex, diag)) {
            return false;
        }
        if (defaultPresent) {
            container.defaultIndex = defaultIndex;
        }
        const JsonValue* children = item.Find("children");
        if (children == nullptr ||
            children->kind != JsonValue::Kind::Array ||
            children->items.empty()) {
            return Fail(diag,
                        "manifest %s requires a non-empty \"children\" "
                        "array",
                        context.c_str());
        }
        for (std::size_t c = 0; c < children->items.size(); ++c) {
            const JsonValue& childItem = children->items[c];
            const std::string childContext =
                context + "." + IndexedName("children", c);
            if (childItem.kind != JsonValue::Kind::Object) {
                return Fail(diag, "manifest %s must be an object",
                            childContext.c_str());
            }
            static constexpr const char* kChildKeys[] = {
                "playable", "weight", "matchValue", "delayMs",
                "delayJitterMs"};
            if (!RefuseUnknownKeys(childItem, kChildKeys, 5,
                                   childContext, diag)) {
                return false;
            }
            ManifestContainerChild child;
            if (!ReadObjectIdField(childItem, "playable", childContext, true,
                              nullptr, &child.playable, diag) ||
                !ReadU32Field(childItem, "weight", childContext, true,
                              nullptr, &child.weight, diag) ||
                !ReadU32Field(childItem, "matchValue", childContext,
                              false, nullptr, &child.matchValue,
                              diag) ||
                !ReadU32Field(childItem, "delayMs", childContext, false,
                              nullptr, &child.delayMs, diag) ||
                !ReadU32Field(childItem, "delayJitterMs", childContext,
                              false, nullptr, &child.delayJitterMs,
                              diag)) {
                return false;
            }
            if (child.delayJitterMs > 60000u) {
                return Fail(diag,
                            "manifest %s.delayJitterMs must be <= 60000",
                            childContext.c_str());
            }
            container.children.push_back(child);
        }
        if (container.defaultIndex != 0xFFFFFFFFu) {
            const std::uint32_t childCount =
                static_cast<std::uint32_t>(container.children.size());
            if (container.defaultIndex >= childCount) {
                return Fail(diag,
                            "manifest %s.default must name a child "
                            "index below %u",
                            context.c_str(), childCount);
            }
            if (container.children[container.defaultIndex].weight == 0) {
                return Fail(diag,
                            "manifest %s.default names a zero-weight "
                            "child — the fallback must stay eligible",
                            context.c_str());
            }
        }
        out->containers.push_back(std::move(container));
    }
    return true;
}

inline bool ParseLogicSection(const JsonValue& root, const char* key,
                              bool (*parse)(const JsonValue&, Manifest*,
                                            Diag*),
                              Manifest* out, Diag* diag) {
    const JsonValue* section = root.Find(key);
    if (section == nullptr) {
        return true;
    }
    if (section->kind != JsonValue::Kind::Array) {
        return Fail(diag, "manifest \"%s\" must be an array", key);
    }
    return parse(*section, out, diag);
}

// Cross-entry rules over the parsed logic sections: the pack-kind entry
// discipline (the compiler half of the spec section 19.1 defense in
// depth), intra-pack duplicate refusals, and the flat playable-id-space
// collision between media and containers. Cross-pack references stay
// legal on purpose — closure is proven at load, not here.
inline bool ValidateLogicSections(const Manifest& manifest, Diag* diag) {
    if (manifest.packKind.empty()) {
        if (ManifestHasLogicSections(manifest)) {
            return Fail(diag,
                        "manifest carries logic sections but no "
                        "\"packKind\" — logic entries require a pack "
                        "kind");
        }
        return true;
    }
    if (manifest.packKind == "resident") {
        if (!manifest.entries.empty()) {
            return Fail(diag,
                        "packKind \"resident\" refuses media entries — "
                        "resident packs carry logic infrastructure "
                        "only");
        }
        if (!manifest.containers.empty()) {
            return Fail(diag,
                        "packKind \"resident\" refuses the "
                        "\"containers\" section — containers are "
                        "content-pack cargo");
        }
        if (!manifest.voiceRecipes.empty()) {
            return Fail(diag,
                        "packKind \"resident\" refuses the "
                        "\"voiceRecipes\" section");
        }
        for (std::size_t i = 0; i < manifest.bindings.size(); ++i) {
            if (manifest.bindings[i].scope == harkenstudio::kTargetScopeVoice ||
                manifest.bindings[i].scope ==
                    harkenstudio::kTargetScopeVoiceEffectParam) {
                return Fail(diag,
                            "packKind \"resident\" requires scope "
                            "\"bus\" on bindings[%zu] — voice-scope "
                            "bindings reference content-pack playables",
                            i);
            }
        }
        for (std::size_t i = 0; i < manifest.attachments.size(); ++i) {
            if (manifest.attachments[i].scope != harkenstudio::kTargetScopeBus &&
                manifest.attachments[i].scope !=
                    harkenstudio::kTargetScopeBusEffectParam) {
                return Fail(diag,
                            "packKind \"resident\" requires scope "
                            "\"bus\" or \"busEffectParam\" on "
                            "attachments[%zu] — voice-scope attachments "
                            "reference content-pack playables",
                            i);
            }
        }
    } else {
        if (!manifest.buses.empty()) {
            return Fail(diag,
                        "packKind \"content\" refuses the \"buses\" "
                        "section — bus topology is resident-pack "
                        "infrastructure");
        }
        if (!manifest.busSends.empty()) {
            return Fail(diag,
                        "packKind \"content\" refuses the \"busSends\" "
                        "section — send edges are resident-pack "
                        "infrastructure");
        }
        if (!manifest.busEffects.empty()) {
            return Fail(diag,
                        "packKind \"content\" refuses the "
                        "\"busEffects\" section");
        }
        if (!manifest.dynamicRoutes.empty()) {
            return Fail(diag,
                        "packKind \"content\" refuses the "
                        "\"dynamicRoutes\" section");
        }
        if (!manifest.attenuations.empty()) {
            return Fail(diag,
                        "packKind \"content\" refuses the "
                        "\"attenuations\" section — attenuation "
                        "profiles are resident-pack infrastructure");
        }
    }
    const std::size_t totalEntries =
        manifest.entries.size() + manifest.buses.size() +
        manifest.busSends.size() + manifest.busEffects.size() +
        manifest.dynamicRoutes.size() + manifest.attenuations.size() +
        manifest.bindings.size() + manifest.voiceRecipes.size() +
        manifest.attachments.size() + manifest.containers.size();
    if (totalEntries == 0 && manifest.packKind == "resident") {
        return Fail(diag,
                    "packKind \"resident\" describes an empty pack — "
                    "resident packs carry at least one logic entry");
    }
    // Intra-pack duplicates. Each refusal names both offenders, the
    // existing duplicate-id message discipline.
    for (std::size_t i = 0; i < manifest.buses.size(); ++i) {
        for (std::size_t j = i + 1; j < manifest.buses.size(); ++j) {
            if (manifest.buses[i].id == manifest.buses[j].id) {
                return Fail(diag,
                            "buses[%zu] and buses[%zu] declare the same "
                            "id %s",
                            i, j,
                            harkenstudio::ObjectIdString(manifest.buses[i].id).c_str());
            }
        }
    }
    for (std::size_t i = 0; i < manifest.busSends.size(); ++i) {
        for (std::size_t j = i + 1; j < manifest.busSends.size(); ++j) {
            if (manifest.busSends[i].from == manifest.busSends[j].from &&
                manifest.busSends[i].to == manifest.busSends[j].to) {
                return Fail(diag,
                            "busSends[%zu] and busSends[%zu] repeat the "
                            "send edge %s -> %s",
                            i, j,
                            harkenstudio::ObjectIdString(manifest.busSends[i].from).c_str(),
                            harkenstudio::ObjectIdString(manifest.busSends[i].to).c_str());
            }
        }
    }
    for (std::size_t i = 0; i < manifest.attenuations.size(); ++i) {
        for (std::size_t j = i + 1; j < manifest.attenuations.size();
             ++j) {
            if (manifest.attenuations[i].id ==
                manifest.attenuations[j].id) {
                return Fail(diag,
                            "attenuations[%zu] and attenuations[%zu] "
                            "declare the same id %s",
                            i, j,
                            harkenstudio::ObjectIdString(manifest.attenuations[i].id).c_str());
            }
        }
    }
    for (std::size_t i = 0; i < manifest.bindings.size(); ++i) {
        for (std::size_t j = i + 1; j < manifest.bindings.size(); ++j) {
            if (manifest.bindings[i].id == manifest.bindings[j].id) {
                return Fail(diag,
                            "bindings[%zu] and bindings[%zu] declare "
                            "the same id %s",
                            i, j,
                            harkenstudio::ObjectIdString(manifest.bindings[i].id).c_str());
            }
        }
    }
    for (std::size_t i = 0; i < manifest.voiceRecipes.size(); ++i) {
        for (std::size_t j = i + 1; j < manifest.voiceRecipes.size(); ++j) {
            if (manifest.voiceRecipes[i].playable ==
                manifest.voiceRecipes[j].playable) {
                return Fail(diag,
                            "voiceRecipes[%zu] and voiceRecipes[%zu] "
                            "declare the same playable %s",
                            i, j,
                            harkenstudio::ObjectIdString(manifest.voiceRecipes[i].playable).c_str());
            }
        }
    }
    for (std::size_t i = 0; i < manifest.attachments.size(); ++i) {
        for (std::size_t j = i + 1; j < manifest.attachments.size();
             ++j) {
            if (manifest.attachments[i].binding ==
                    manifest.attachments[j].binding &&
                manifest.attachments[i].scope ==
                    manifest.attachments[j].scope &&
                manifest.attachments[i].object ==
                    manifest.attachments[j].object) {
                return Fail(diag,
                            "attachments[%zu] and attachments[%zu] "
                            "repeat the same attachment (binding %s)",
                            i, j,
                            harkenstudio::ObjectIdString(manifest.attachments[i].binding).c_str());
            }
        }
    }
    for (std::size_t i = 0; i < manifest.containers.size(); ++i) {
        for (std::size_t j = i + 1; j < manifest.containers.size();
             ++j) {
            if (manifest.containers[i].id == manifest.containers[j].id) {
                return Fail(diag,
                            "containers[%zu] and containers[%zu] "
                            "declare the same id %s",
                            i, j,
                            harkenstudio::ObjectIdString(manifest.containers[i].id).c_str());
            }
        }
    }
    // Media and containers share the flat playable-id space, so their
    // ids are unique across BOTH sets, not just within each.
    for (std::size_t i = 0; i < manifest.containers.size(); ++i) {
        for (std::size_t j = 0; j < manifest.entries.size(); ++j) {
            if (manifest.containers[i].id ==
                manifest.entries[j].soundId) {
                return Fail(diag,
                            "containers[%zu] id %s collides with "
                            "media entry '%s' — containers and sounds "
                            "share one playable id space",
                            i,
                            harkenstudio::ObjectIdString(manifest.containers[i].id).c_str(),
                            manifest.entries[j].path.c_str());
            }
        }
    }
    return true;
}


inline bool ParseManifestV3(const JsonValue& root, Manifest* out,
                            Diag* diag) {
    out->version = 3;
    for (const auto& member : root.members) {
        const bool known =
            member.first == "manifestVersion" || member.first == "chunkFrames" ||
            member.first == "output" || member.first == "options" ||
            member.first == "entries" || member.first == "packKind" ||
            member.first == "buses" || member.first == "busSends" ||
            member.first == "busEffects" ||
            member.first == "dynamicRoutes" ||
            member.first == "attenuations" ||
            member.first == "discreteVariables" ||
            member.first == "meterWirings" ||
            member.first == "bindings" ||
            member.first == "voiceRecipes" ||
            member.first == "attachments" ||
            member.first == "containers";
        if (!known) {
            return Fail(diag, "unknown manifest key \"%s\"",
                        member.first.c_str());
        }
    }
    const JsonValue* version = root.Find("manifestVersion");
    std::uint64_t versionValue = 0;
    if (version == nullptr || !version->AsU64(&versionValue) ||
        versionValue != 3) {
        return Fail(diag, "\"manifestVersion\" must be the integer 3");
    }
    if (!ReadU32Field(root, "chunkFrames", "", false, nullptr, &out->chunkFrames, diag))
        return false;
    const std::string baseDir = DirectoryOf(out->manifestPath);
    const JsonValue* output = root.Find("output");
    if (output == nullptr || output->kind != JsonValue::Kind::String ||
        output->text.empty()) {
        return Fail(diag,
                    "manifest requires a non-empty \"output\" string "
                    "(the .pack path)");
    }
    out->outputPath = output->text;
    out->resolvedOutputPath = ResolveAgainst(baseDir, output->text);
    const JsonValue* options = root.Find("options");
    if (options != nullptr) {
        if (options->kind != JsonValue::Kind::Object) {
            return Fail(diag, "manifest \"options\" must be an object");
        }
        for (const auto& member : options->members) {
            if (member.first == "cache") {
                if (member.second.kind != JsonValue::Kind::Bool) {
                    return Fail(diag,
                                "manifest options.cache must be a "
                                "boolean");
                }
                out->cacheEnabled = member.second.boolean;
            } else if (member.first == "cachePath" ||
                       member.first == "reportPath") {
                if (member.second.kind != JsonValue::Kind::String ||
                    member.second.text.empty()) {
                    return Fail(diag,
                                "manifest options.%s must be a non-empty "
                                "string",
                                member.first.c_str());
                }
                if (member.first == "cachePath") {
                    out->cachePath = member.second.text;
                } else {
                    out->reportPath = member.second.text;
                }
            } else {
                return Fail(diag,
                            "unknown key \"%s\" in manifest options",
                            member.first.c_str());
            }
        }
    }
    const JsonValue* packKind = root.Find("packKind");
    if (packKind != nullptr) {
        if (packKind->kind != JsonValue::Kind::String ||
            (packKind->text != "resident" &&
             packKind->text != "content")) {
            return Fail(diag,
                        "manifest \"packKind\" must be \"resident\" or "
                        "\"content\"");
        }
        out->packKind = packKind->text;
    }
    const JsonValue* entries = root.Find("entries");
    if (out->packKind.empty()) {
        // The pre-logic contract, refusal messages included: a v1
        // container carries at least one sound.
        if (entries == nullptr ||
            entries->kind != JsonValue::Kind::Array) {
            return Fail(diag, "manifest requires an \"entries\" array");
        }
        if (entries->items.empty()) {
            return Fail(diag,
                        "manifest \"entries\" array is empty — a pack "
                        "carries at least one sound");
        }
    } else if (entries != nullptr &&
               entries->kind != JsonValue::Kind::Array) {
        // A pack kind relaxes presence (a resident pack has no media;
        // a content pack may be logic-only) but never the shape.
        return Fail(diag, "manifest \"entries\" must be an array");
    }
    if (entries != nullptr) {
        for (std::size_t i = 0; i < entries->items.size(); ++i) {
            ManifestEntry entry;
            if (!ParseEntry(entries->items[i], i, true, baseDir, &entry,
                            diag)) {
                return false;
            }
            out->entries.push_back(std::move(entry));
        }
    }
    // Explicit ids admit the duplicate check at parse time; refusing
    // before any decode names the collision as early as possible. (The
    // post-decode check still runs for v1, whose minted ids exist only
    // after decoding.)
    for (std::size_t i = 0; i < out->entries.size(); ++i) {
        for (std::size_t j = i + 1; j < out->entries.size(); ++j) {
            if (out->entries[i].soundId == out->entries[j].soundId) {
                return Fail(
                    diag,
                    "entries[%zu] '%s' and entries[%zu] '%s' declare the "
                    "same id %s",
                    i, out->entries[i].path.c_str(), j,
                    out->entries[j].path.c_str(),
                    harkenstudio::ObjectIdString(out->entries[i].soundId).c_str());
            }
        }
    }
    if (root.Find("discreteVariables") != nullptr) {
        return Fail(diag,
                    "manifest section \"discreteVariables\" is not "
                    "supported by the current Variable world format");
    }
    if (root.Find("meterWirings") != nullptr) {
        return Fail(diag,
                    "manifest section \"meterWirings\" is not supported "
                    "by the current Variable world format");
    }
    if (!ParseLogicSection(root, "buses", ParseBusesSection, out,
                           diag) ||
        !ParseLogicSection(root, "busSends", ParseBusSendsSection, out,
                           diag) ||
        !ParseLogicSection(root, "busEffects", ParseBusEffectsSection,
                           out, diag) ||
        !ParseLogicSection(root, "dynamicRoutes",
                           ParseDynamicRoutesSection, out, diag) ||
        !ParseLogicSection(root, "attenuations",
                           ParseAttenuationsSection, out, diag) ||
        !ParseLogicSection(root, "bindings", ParseBindingsSection, out,
                           diag) ||
        !ParseLogicSection(root, "voiceRecipes",
                           ParseVoiceRecipesSection, out, diag) ||
        !ParseLogicSection(root, "attachments",
                           ParseAttachmentsSection, out, diag) ||
        !ParseLogicSection(root, "containers", ParseContainersSection,
                           out, diag)) {
        return false;
    }
    return ValidateLogicSections(*out, diag);
}

inline bool ParseManifestText(const std::string& text,
    const std::string& manifestPath, bool streamMode, Manifest* out, Diag* diag) {
    JsonValue root;
    std::string error;
    if (!ParseJson(text, &root, &error))
        return Fail(diag, "manifest is not valid JSON: %s", error.c_str());
    if (root.kind != JsonValue::Kind::Object)
        return Fail(diag, "manifest root must be an object");
    if (!streamMode && root.Find("chunkFrames"))
        return Fail(diag, "chunkFrames applies only to --stream builds");
    out->manifestPath = manifestPath;
    if (!ParseManifestV3(root, out, diag)) return false;
    if (streamMode && (out->entries.size() != 1 || !out->packKind.empty()))
        return Fail(diag, "--stream requires exactly one media entry and no packKind");
    return true;
}

// %.9g float literal: nine significant digits round-trip an f32
// exactly (the report's FormatFloat discipline), so emit-then-parse
// reproduces the identical float value.
inline std::string FloatLiteral(float value) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.9g",
                  static_cast<double>(value));
    return std::string(buffer);
}

inline void EmitCurveObject(JsonWriter* writer, const char* key,
                            const ManifestCurve& curve) {
    writer->Key(key);
    writer->BeginObject();
    writer->Key("points");
    writer->BeginArray();
    for (std::uint32_t i = 0; i < curve.pointCount; ++i) {
        writer->BeginArray();
        writer->ValueRawNumber(FloatLiteral(curve.pointX[i]));
        writer->ValueRawNumber(FloatLiteral(curve.pointY[i]));
        writer->EndArray();
    }
    writer->EndArray();
    // A single-point curve has no segments, so handles must be omitted.
    if (curve.pointCount > 1) {
        writer->Key("handles");
        writer->BeginArray();
        for (std::uint32_t i = 0; i + 1 < curve.pointCount; ++i) {
            const harkenstudiobuild::ParameterCurveHandles& handle = curve.handles[i];
            writer->BeginArray();
            writer->ValueRawNumber(FloatLiteral(handle.h1x));
            writer->ValueRawNumber(FloatLiteral(handle.h1y));
            writer->ValueRawNumber(FloatLiteral(handle.h2x));
            writer->ValueRawNumber(FloatLiteral(handle.h2y));
            writer->EndArray();
        }
        writer->EndArray();
    }
    writer->EndObject();
}

// Logic sections in entry-kind order, each emitted only when non-empty
// (absent and empty normalize to the same parsed image, keeping the
// fixpoint). Optional scalar members with in-band defaults are always
// spelled out — the codec normalization rule; members whose absence is
// a distinct state (cone, curves, container default) are
// emitted only when present.
inline void EmitLogicSections(JsonWriter* writer,
                              const Manifest& manifest) {
    if (!manifest.buses.empty()) {
        writer->Key("buses");
        writer->BeginArray();
        for (const ManifestBus& bus : manifest.buses) {
            writer->BeginObject();
            writer->Key("id");
            writer->ValueString(harkenstudio::ObjectIdString(bus.id));
            writer->Key("parent");
            writer->ValueString(harkenstudio::ObjectIdString(bus.parent));
            writer->Key("volume");
            writer->ValueRawNumber(FloatLiteral(bus.volume));
            writer->Key("outputBusVolume");
            writer->ValueRawNumber(FloatLiteral(bus.outputBusVolume));
            writer->Key("makeUpGain");
            writer->ValueRawNumber(FloatLiteral(bus.makeUpGain));
            writer->EndObject();
        }
        writer->EndArray();
    }
    if (!manifest.busSends.empty()) {
        writer->Key("busSends");
        writer->BeginArray();
        for (const ManifestBusSend& send : manifest.busSends) {
            writer->BeginObject();
            writer->Key("from");
            writer->ValueString(harkenstudio::ObjectIdString(send.from));
            writer->Key("to");
            writer->ValueString(harkenstudio::ObjectIdString(send.to));
            writer->Key("levelDb");
            writer->ValueRawNumber(FloatLiteral(send.levelDb));
            writer->EndObject();
        }
        writer->EndArray();
    }
    if (!manifest.busEffects.empty()) {
        writer->Key("busEffects");
        writer->BeginArray();
        for (const ManifestBusEffect& effect : manifest.busEffects) {
            writer->BeginObject();
            writer->Key("effectId");
            writer->ValueString(harkenstudio::ObjectIdString(effect.effectId));
            writer->Key("bus");
            writer->ValueString(harkenstudio::ObjectIdString(effect.bus));
            writer->Key("slot");
            writer->ValueU64(effect.slot);
            writer->Key("type");
            writer->ValueString(RecipeEffectTypeName(effect.type));
            writer->Key("descriptorVintage");
            writer->ValueU64(effect.descriptorVintage);
            writer->Key("bypass");
            writer->ValueBool(effect.bypass);
            writer->Key("faderPointGuard");
            writer->ValueU64(effect.faderPointGuard);
            writer->Key("parameters");
            writer->BeginArray();
            for (float value : effect.parameters) {
                writer->ValueRawNumber(FloatLiteral(value));
            }
            writer->EndArray();
            writer->EndObject();
        }
        writer->EndArray();
    }
    if (!manifest.dynamicRoutes.empty()) {
        writer->Key("dynamicRoutes");
        writer->BeginArray();
        for (const ManifestDynamicRoute& route : manifest.dynamicRoutes) {
            writer->BeginObject();
            writer->Key("targetBus");
            writer->ValueString(harkenstudio::ObjectIdString(route.targetBus));
            writer->Key("effectSlot");
            writer->ValueU64(route.effectSlot);
            writer->Key("routeSlot");
            writer->ValueU64(route.routeSlot);
            writer->Key("sourceBus");
            writer->ValueString(harkenstudio::ObjectIdString(route.sourceBus));
            writer->Key("tap");
            writer->ValueString(DynamicTapName(route.tap));
            writer->Key("levelDb");
            writer->ValueRawNumber(FloatLiteral(route.levelDb));
            writer->EndObject();
        }
        writer->EndArray();
    }
    if (!manifest.attenuations.empty()) {
        writer->Key("attenuations");
        writer->BeginArray();
        for (const ManifestAttenuation& attenuation :
             manifest.attenuations) {
            writer->BeginObject();
            writer->Key("id");
            writer->ValueString(harkenstudio::ObjectIdString(attenuation.id));
            writer->Key("rolloff");
            writer->ValueString(
                EnumNameOf(kRolloffNames, 3, attenuation.rolloff));
            writer->Key("minDistance");
            writer->ValueRawNumber(
                FloatLiteral(attenuation.minDistance));
            writer->Key("maxDistance");
            writer->ValueRawNumber(
                FloatLiteral(attenuation.maxDistance));
            writer->Key("airAbsorptionNearHz");
            writer->ValueRawNumber(
                FloatLiteral(attenuation.airAbsorptionNearHz));
            writer->Key("airAbsorptionFarHz");
            writer->ValueRawNumber(
                FloatLiteral(attenuation.airAbsorptionFarHz));
            writer->Key("proximityRadius");
            writer->ValueRawNumber(
                FloatLiteral(attenuation.proximityRadius));
            writer->Key("proximityMaxBoostDb");
            writer->ValueRawNumber(
                FloatLiteral(attenuation.proximityMaxBoostDb));
            writer->Key("proximityFrequency");
            writer->ValueRawNumber(
                FloatLiteral(attenuation.proximityFrequency));
            if (attenuation.coneEnabled) {
                writer->Key("cone");
                writer->BeginObject();
                writer->Key("innerDeg");
                writer->ValueRawNumber(
                    FloatLiteral(attenuation.coneInnerDeg));
                writer->Key("outerDeg");
                writer->ValueRawNumber(
                    FloatLiteral(attenuation.coneOuterDeg));
                writer->Key("outerGainDb");
                writer->ValueRawNumber(
                    FloatLiteral(attenuation.coneOuterGainDb));
                writer->Key("outerLowpassHz");
                writer->ValueRawNumber(
                    FloatLiteral(attenuation.coneOuterLowpassHz));
                writer->EndObject();
            }
            if (attenuation.hasVolumeCurve) {
                EmitCurveObject(writer, "volumeCurve",
                                attenuation.volumeCurve);
            }
            if (attenuation.hasLowpassCurve) {
                EmitCurveObject(writer, "lowpassCurve",
                                attenuation.lowpassCurve);
            }
            if (attenuation.hasWidthCurve) {
                EmitCurveObject(writer, "widthCurve",
                                attenuation.widthCurve);
            }
            writer->EndObject();
        }
        writer->EndArray();
    }
    if (!manifest.bindings.empty()) {
        writer->Key("bindings");
        writer->BeginArray();
        for (const ManifestBinding& binding : manifest.bindings) {
            writer->BeginObject();
            writer->Key("id");
            writer->ValueString(harkenstudio::ObjectIdString(binding.id));
            writer->Key("variable");
            writer->ValueString(harkenstudio::ObjectIdString(binding.variable));
            writer->Key("scope");
            writer->ValueString(
                EnumNameOf(kTargetScopeNames, 4, binding.scope));
            writer->Key("target");
            writer->ValueU64(binding.target);
            writer->Key("transitionMs");
            writer->ValueU64(binding.transitionMs);
            EmitCurveObject(writer, "curve", binding.curve);
            writer->EndObject();
        }
        writer->EndArray();
    }
    if (!manifest.attachments.empty()) {
        writer->Key("attachments");
        writer->BeginArray();
        for (const ManifestAttachment& attachment :
             manifest.attachments) {
            writer->BeginObject();
            writer->Key("binding");
            writer->ValueString(harkenstudio::ObjectIdString(attachment.binding));
            writer->Key("scope");
            writer->ValueString(
                EnumNameOf(kTargetScopeNames, 4, attachment.scope));
            writer->Key("object");
            writer->ValueString(harkenstudio::ObjectIdString(attachment.object));
            writer->EndObject();
        }
        writer->EndArray();
    }
    if (!manifest.containers.empty()) {
        writer->Key("containers");
        writer->BeginArray();
        for (const ManifestContainer& container : manifest.containers) {
            writer->BeginObject();
            writer->Key("id");
            writer->ValueString(harkenstudio::ObjectIdString(container.id));
            writer->Key("order");
            writer->ValueString(
                EnumNameOf(kSelectionOrderNames, 2, container.order));
            writer->Key("selectionVariable");
            writer->ValueString(harkenstudio::ObjectIdString(container.selectionVariable));
            writer->Key("avoidRepeatWindow");
            writer->ValueU64(container.avoidRepeatWindow);
            if (container.defaultIndex != 0xFFFFFFFFu) {
                writer->Key("default");
                writer->ValueU64(container.defaultIndex);
            }
            writer->Key("children");
            writer->BeginArray();
            for (const ManifestContainerChild& child :
                 container.children) {
                writer->BeginObject();
                writer->Key("playable");
                writer->ValueString(harkenstudio::ObjectIdString(child.playable));
                writer->Key("weight");
                writer->ValueU64(child.weight);
                writer->Key("matchValue");
                writer->ValueU64(child.matchValue);
                writer->Key("delayMs");
                writer->ValueU64(child.delayMs);
                writer->Key("delayJitterMs");
                writer->ValueU64(child.delayJitterMs);
                writer->EndObject();
            }
            writer->EndArray();
            writer->EndObject();
        }
        writer->EndArray();
    }
    if (!manifest.voiceRecipes.empty()) {
        writer->Key("voiceRecipes");
        writer->BeginArray();
        for (const ManifestVoiceRecipe& recipe : manifest.voiceRecipes) {
            writer->BeginObject();
            writer->Key("playable");
            writer->ValueString(harkenstudio::ObjectIdString(recipe.playable));
            if (recipe.tone.present) {
                writer->Key("tone");
                writer->BeginObject();
                writer->Key("lowpassEnabled");
                writer->ValueBool(recipe.tone.lowpassEnabled);
                writer->Key("lowpassFrequencyHz");
                writer->ValueRawNumber(
                    FloatLiteral(recipe.tone.lowpassFrequencyHz));
                writer->Key("lowpassQ");
                writer->ValueRawNumber(FloatLiteral(recipe.tone.lowpassQ));
                writer->Key("highpassEnabled");
                writer->ValueBool(recipe.tone.highpassEnabled);
                writer->Key("highpassFrequencyHz");
                writer->ValueRawNumber(
                    FloatLiteral(recipe.tone.highpassFrequencyHz));
                writer->Key("highpassQ");
                writer->ValueRawNumber(FloatLiteral(recipe.tone.highpassQ));
                writer->Key("lowShelfEnabled");
                writer->ValueBool(recipe.tone.lowShelfEnabled);
                writer->Key("lowShelfFrequencyHz");
                writer->ValueRawNumber(
                    FloatLiteral(recipe.tone.lowShelfFrequencyHz));
                writer->Key("lowShelfQ");
                writer->ValueRawNumber(FloatLiteral(recipe.tone.lowShelfQ));
                writer->Key("lowShelfGainDb");
                writer->ValueRawNumber(
                    FloatLiteral(recipe.tone.lowShelfGainDb));
                writer->Key("highShelfEnabled");
                writer->ValueBool(recipe.tone.highShelfEnabled);
                writer->Key("highShelfFrequencyHz");
                writer->ValueRawNumber(
                    FloatLiteral(recipe.tone.highShelfFrequencyHz));
                writer->Key("highShelfQ");
                writer->ValueRawNumber(FloatLiteral(recipe.tone.highShelfQ));
                writer->Key("highShelfGainDb");
                writer->ValueRawNumber(
                    FloatLiteral(recipe.tone.highShelfGainDb));
                writer->EndObject();
            }
            if (recipe.spatial.present) {
                writer->Key("spatial");
                writer->BeginObject();
                writer->Key("attenuation");
                writer->ValueString(harkenstudio::ObjectIdString(recipe.spatial.attenuation));
                writer->Key("distanceScale");
                writer->ValueRawNumber(
                    FloatLiteral(recipe.spatial.distanceScale));
                writer->EndObject();
            }
            if (recipe.playback.present) {
                writer->Key("playback");
                writer->BeginObject();
                writer->Key("loopCount");
                writer->ValueU64(recipe.playback.loopCount);
                writer->EndObject();
            }
            writer->Key("effects");
            writer->BeginArray();
            for (const ManifestVoiceRecipeEffect& effect : recipe.effects) {
                writer->BeginObject();
                writer->Key("effectId");
                writer->ValueString(harkenstudio::ObjectIdString(effect.effectId));
                writer->Key("slot");
                writer->ValueU64(effect.slot);
                writer->Key("type");
                writer->ValueString(RecipeEffectTypeName(effect.type));
                writer->Key("descriptorVintage");
                writer->ValueU64(effect.descriptorVintage);
                writer->Key("bypass");
                writer->ValueBool(effect.bypass);
                writer->Key("parameters");
                writer->BeginArray();
                for (float parameter : effect.parameters) {
                    writer->ValueRawNumber(FloatLiteral(parameter));
                }
                writer->EndArray();
                writer->EndObject();
            }
            writer->EndArray();
            writer->Key("bindings");
            writer->BeginArray();
            for (harkenstudio::ObjectId binding : recipe.bindings) {
                writer->ValueString(harkenstudio::ObjectIdString(binding));
            }
            writer->EndArray();
            writer->EndObject();
        }
        writer->EndArray();
    }
}

// Normalized v2 image: fixed member order, explicit ids, loop members
// only when loopable, codec always spelled out. Emitting the same
// logical manifest always yields identical bytes, and re-emitting a
// parse of the emitted text is a fixpoint — that is the round-trip
// contract tests pin. Paths are echoed exactly as written; callers that
// emit to a different directory than the source manifest must account
// for relative-path bases themselves. Logic members appear only when
// present — packKind after "options", entries next, then the logic
// sections in entry-kind order — so a manifest without them emits
// byte-identically to the pre-logic tool.
inline std::string EmitManifestV3(const Manifest& manifest) {
    JsonWriter writer;
    writer.BeginObject();
    writer.Key("manifestVersion");
    writer.ValueU64(3);
    writer.Key("output");
    writer.ValueString(manifest.outputPath);
    writer.Key("options");
    writer.BeginObject();
    writer.Key("cache");
    writer.ValueBool(manifest.cacheEnabled);
    if (!manifest.cachePath.empty()) {
        writer.Key("cachePath");
        writer.ValueString(manifest.cachePath);
    }
    if (!manifest.reportPath.empty()) {
        writer.Key("reportPath");
        writer.ValueString(manifest.reportPath);
    }
    writer.EndObject();
    if (!manifest.packKind.empty()) {
        writer.Key("packKind");
        writer.ValueString(manifest.packKind);
    }
    if (!manifest.entries.empty()) {
        writer.Key("entries");
        writer.BeginArray();
        for (const ManifestEntry& entry : manifest.entries) {
            writer.BeginObject();
            writer.Key("path");
            writer.ValueString(entry.path);
            writer.Key("id");
            writer.ValueString(harkenstudio::ObjectIdString(entry.soundId));
            if (entry.loopable) {
                writer.Key("loopStartFrame");
                writer.ValueU64(entry.loopStartFrame);
                writer.Key("loopEndFrame");
                writer.ValueU64(entry.loopEndFrame);
            }
            writer.Key("codec");
            writer.ValueString(entry.codec);
            if (!entry.downmix.empty()) {
                writer.Key("downmix");
                writer.ValueString(entry.downmix);
            }
            writer.EndObject();
        }
        writer.EndArray();
    }
    EmitLogicSections(&writer, manifest);
    writer.EndObject();
    return writer.Finish();
}

} // namespace packc
} // namespace harkenstudiojson

#endif // HARKEN_HOST_PACKC_MANIFEST_H
