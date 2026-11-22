#ifndef HARKEN_HOST_PACKC_MANIFEST_H
#define HARKEN_HOST_PACKC_MANIFEST_H

#include <harken_studio/ObjectId.h>
#include <harken_studio_build/AuthoredParameters.h>
#include <harken_studio_build/BindingTransition.h>
#include "../../Common/JsonReader.h"

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
#include <string>
#include <vector>

namespace harken::studio::pack {

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

struct ManifestEntry {
    harken::studio::types::ObjectId soundId{};
    std::string path;
};

// Parsed images of the logic-entry sections a v2 manifest may carry
// alongside "packKind". Enum-valued members hold the public VN_* values
// (mapped from the manifest spelling at parse time).

// One parsed curve object ({"points": [[x, y], ...], "handles": [...]});
// serves attenuation override curves and binding curves alike. Presence
// is tracked by the owner — an absent attenuation override curve means
// "built-in default" — so the curve carries no enabled flag here.
struct ManifestCurve {
    std::uint32_t pointCount = 0;
    float pointX[harken::studio::types::kCurveMaxPoints] = {};
    float pointY[harken::studio::types::kCurveMaxPoints] = {};
    harken::studio::build::ParameterCurveHandles handles[harken::studio::types::kCurveMaxPoints - 1] = {};
};

struct ManifestBus {
    harken::studio::types::ObjectId id{};
    harken::studio::types::ObjectId parent{};
    float volume = 0.0f;
    float outputBusVolume = 0.0f;
};

struct ManifestBusSend {
    harken::studio::types::ObjectId from{};
    harken::studio::types::ObjectId to{};
    float levelDb = 0.0f;
};

struct ManifestBusEffect {
    harken::studio::types::ObjectId effectId{};
    harken::studio::types::ObjectId bus{};
    std::uint32_t slot = 0;
    std::uint32_t type = harken::studio::types::kEffectGain;
    std::uint32_t descriptorVintage = 0;
    bool bypass = false;
    std::uint32_t faderPointGuard = 0;
    std::vector<float> parameters;
};

struct ManifestDynamicRoute {
    harken::studio::types::ObjectId targetBus{};
    std::uint32_t effectSlot = 0;
    std::uint32_t routeSlot = 0;
    std::uint32_t expectedEffectType = harken::studio::types::kEffectCompressor;
    std::uint32_t signalForm = harken::studio::types::kDynamicSignalRawAudio;
    std::uint32_t sourceKind = harken::studio::types::kDynamicSourceBus;
    std::uint32_t tap = harken::studio::types::kDynamicTapBusPostEffects;
    harken::studio::types::ObjectId sourceBus{};
    float levelDb = 0.0f;
};

struct ManifestAttenuation {
    harken::studio::types::ObjectId id{};
    std::uint32_t rolloff = harken::studio::types::kRolloffLinear;
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
    harken::studio::types::ObjectId id{};
    harken::studio::types::ObjectId variable{};
    std::uint32_t scope = harken::studio::types::kTargetScopeVoice;
    std::uint32_t target = 0;
    std::uint32_t transitionMs = 0;
    ManifestCurve curve;
};

struct ManifestAttachment {
    harken::studio::types::ObjectId binding{};
    std::uint32_t scope = harken::studio::types::kTargetScopeVoice;
    harken::studio::types::ObjectId object{};
};

struct ManifestVoiceRecipeEffect {
    harken::studio::types::ObjectId effectId{};
    std::uint32_t slot = 0;
    std::uint32_t type = harken::studio::types::kEffectGain;
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
    harken::studio::types::ObjectId attenuation{};
    float distanceScale = 1.0f;
};

struct ManifestVoiceRecipePlayback {
    bool present = false;
    std::uint32_t loopCount = 0;
};

struct ManifestVoiceRecipe {
    harken::studio::types::ObjectId playable{};
    ManifestVoiceRecipeTone tone;
    ManifestVoiceRecipeSpatial spatial;
    ManifestVoiceRecipePlayback playback;
    std::vector<ManifestVoiceRecipeEffect> effects;
    std::vector<harken::studio::types::ObjectId> bindings;
};

struct ManifestContainerChild {
    harken::studio::types::ObjectId playable{};
    std::uint32_t weight = 0;
    std::uint32_t matchValue = 0;
    std::uint32_t delayMs = 0;
    std::uint32_t delayJitterMs = 0;
};

struct ManifestContainer {
    harken::studio::types::ObjectId id{};
    std::uint32_t order = kLegacySelectionWeightedRandom;
    harken::studio::types::ObjectId selectionVariable{}; // 0 = no match filter
    std::uint32_t avoidRepeatWindow = 0;
    std::uint32_t defaultIndex = 0xFFFFFFFFu; // = absent
    std::vector<ManifestContainerChild> children;
};

struct Manifest {
    std::vector<ManifestEntry> entries;
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

inline bool ReadObjectIdField(const json::JsonValue& object, const char* key,
    const std::string& context, bool required, bool* present,
    harken::studio::types::ObjectId* out, Diag* diag) {
    const auto* value = object.Find(key);
    if (present) *present = value != nullptr;
    if (!value)
        return !required || Fail(diag, "manifest %s requires a \"%s\" UUID", context.c_str(), key);
    if (value->kind != json::JsonValue::Kind::String || !harken::studio::types::ParseObjectId(value->text, out))
        return Fail(diag, "manifest %s.%s must be a UUID string", context.c_str(), key);
    return true;
}

inline std::string IndexedName(const char* section, std::size_t index) {
    return std::string(section) + "[" + std::to_string(index) + "]";
}

inline bool RefuseUnknownKeys(const json::JsonValue& object,
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
inline bool ParseFiniteFloat(const json::JsonValue& value, float* out) {
    if (value.kind != json::JsonValue::Kind::Number || value.text.empty()) {
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
inline bool ReadU64Field(const json::JsonValue& object, const char* key,
                         const std::string& context, bool required,
                         bool* present, std::uint64_t* out, Diag* diag) {
    const json::JsonValue* value = object.Find(key);
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

inline bool ReadU32Field(const json::JsonValue& object, const char* key,
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

inline bool ReadFloatField(const json::JsonValue& object, const char* key,
                           const std::string& context, bool required,
                           bool* present, float* out, Diag* diag) {
    const json::JsonValue* value = object.Find(key);
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
    {"linear", harken::studio::types::kRolloffLinear},
    {"inverseSquare", harken::studio::types::kRolloffInverseSquare},
    {"logarithmic", harken::studio::types::kRolloffLogarithmic},
};

inline constexpr NamedValue kTargetScopeNames[] = {
    {"voice", harken::studio::types::kTargetScopeVoice},
    {"bus", harken::studio::types::kTargetScopeBus},
    {"voiceEffectParam", harken::studio::types::kTargetScopeVoiceEffectParam},
    {"busEffectParam", harken::studio::types::kTargetScopeBusEffectParam},
};

inline constexpr NamedValue kSelectionOrderNames[] = {
    {"weightedRandom", kLegacySelectionWeightedRandom},
    {"roundRobin", kLegacySelectionRoundRobin},
};

inline bool ReadEnumField(const json::JsonValue& object, const char* key,
                          const std::string& context, bool required,
                          const NamedValue* names, std::size_t nameCount,
                          const char* choices, std::uint32_t* out,
                          Diag* diag) {
    const json::JsonValue* value = object.Find(key);
    if (value == nullptr) {
        if (required) {
            return Fail(diag, "manifest %s requires a \"%s\" member",
                        context.c_str(), key);
        }
        return true;
    }
    if (value->kind == json::JsonValue::Kind::String) {
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
inline bool ParseCurveObject(const json::JsonValue& value,
                             const std::string& context,
                             CurveValueDomain domain,
                             ManifestCurve* out, Diag* diag) {
    if (value.kind != json::JsonValue::Kind::Object) {
        return Fail(diag, "manifest %s must be a curve object",
                    context.c_str());
    }
    static constexpr const char* kKeys[] = {"points", "handles"};
    if (!RefuseUnknownKeys(value, kKeys, 2, context, diag)) {
        return false;
    }
    const json::JsonValue* points = value.Find("points");
    if (points == nullptr || points->kind != json::JsonValue::Kind::Array ||
        points->items.empty() ||
        points->items.size() > harken::studio::types::kCurveMaxPoints) {
        return Fail(diag,
                    "manifest %s.points must be an array of 1..%d "
                    "[x, y] pairs",
                    context.c_str(), harken::studio::types::kCurveMaxPoints);
    }
    out->pointCount = static_cast<std::uint32_t>(points->items.size());
    for (std::size_t i = 0; i < points->items.size(); ++i) {
        const json::JsonValue& pair = points->items[i];
        float x = 0.0f;
        float y = 0.0f;
        if (pair.kind != json::JsonValue::Kind::Array ||
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
    const json::JsonValue* handles = value.Find("handles");
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
    if (handles->kind != json::JsonValue::Kind::Array ||
        handles->items.size() != segments) {
        return Fail(diag,
                    "manifest %s.handles must carry exactly one "
                    "handle tuple per segment (%u)",
                    context.c_str(), segments);
    }
    for (std::size_t i = 0; i < handles->items.size(); ++i) {
        const json::JsonValue& tuple = handles->items[i];
        harken::studio::build::ParameterCurveHandles handle{};
        if (tuple.kind != json::JsonValue::Kind::Array ||
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

inline bool ParseBusesSection(const json::JsonValue& section, Manifest* out,
                              Diag* diag) {
    for (std::size_t i = 0; i < section.items.size(); ++i) {
        const json::JsonValue& item = section.items[i];
        const std::string context = IndexedName("buses", i);
        if (item.kind != json::JsonValue::Kind::Object) {
            return Fail(diag, "manifest %s must be an object",
                        context.c_str());
        }
        static constexpr const char* kKeys[] = {
            "id", "parent", "volume", "outputBusVolume"};
        if (!RefuseUnknownKeys(item, kKeys, 4, context, diag)) {
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
                            nullptr, &bus.outputBusVolume, diag)) {
            return false;
        }
        if (harken::studio::types::IsNull(bus.id)) {
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

inline bool ParseBusSendsSection(const json::JsonValue& section, Manifest* out,
                                 Diag* diag) {
    for (std::size_t i = 0; i < section.items.size(); ++i) {
        const json::JsonValue& item = section.items[i];
        const std::string context = IndexedName("busSends", i);
        if (item.kind != json::JsonValue::Kind::Object) {
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
                        harken::studio::types::ObjectIdString(send.from).c_str());
        }
        out->busSends.push_back(send);
    }
    return true;
}

inline bool ParseAttenuationsSection(const json::JsonValue& section,
                                     Manifest* out, Diag* diag) {
    for (std::size_t i = 0; i < section.items.size(); ++i) {
        const json::JsonValue& item = section.items[i];
        const std::string context = IndexedName("attenuations", i);
        if (item.kind != json::JsonValue::Kind::Object) {
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
        const json::JsonValue* cone = item.Find("cone");
        if (cone != nullptr) {
            const std::string coneContext = context + ".cone";
            if (cone->kind != json::JsonValue::Kind::Object) {
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
        const json::JsonValue* volumeCurve = item.Find("volumeCurve");
        if (volumeCurve != nullptr) {
            attenuation.hasVolumeCurve = true;
            if (!ParseCurveObject(*volumeCurve, context + ".volumeCurve",
                                  CurveValueDomain::VolumeDb,
                                  &attenuation.volumeCurve, diag)) {
                return false;
            }
        }
        const json::JsonValue* lowpassCurve = item.Find("lowpassCurve");
        if (lowpassCurve != nullptr) {
            attenuation.hasLowpassCurve = true;
            if (!ParseCurveObject(*lowpassCurve,
                                  context + ".lowpassCurve",
                                  CurveValueDomain::LowpassHz,
                                  &attenuation.lowpassCurve, diag)) {
                return false;
            }
        }
        const json::JsonValue* widthCurve = item.Find("widthCurve");
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
    const harken::studio::build::EffectParamLocator locator =
        harken::studio::build::UnpackEffectParamLocator(target);
    const auto* effect = harken::studio::build::FindPackEffectSchemaByType(locator.effectType);
    return effect && locator.paramIndex < effect->paramCount
        && (locator.effectType != harken::studio::types::kEffectFeedbackDelay || locator.paramIndex >= 2);
}

inline bool ParseBindingsSection(const json::JsonValue& section, Manifest* out,
                                 Diag* diag) {
    for (std::size_t i = 0; i < section.items.size(); ++i) {
        const json::JsonValue& item = section.items[i];
        const std::string context = IndexedName("bindings", i);
        if (item.kind != json::JsonValue::Kind::Object) {
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
        if (!harken::studio::build::IsValidBindingTransitionMs(binding.transitionMs)) {
            return Fail(diag, "manifest %s.transitionMs must be 0..60000", context.c_str());
        }
        const auto* targetMeta = harken::studio::build::FindTargetMeta(binding.scope, binding.target);
        if ((binding.scope == harken::studio::types::kTargetScopeVoice || binding.scope == harken::studio::types::kTargetScopeBus)
            && !targetMeta) {
            return Fail(diag, "manifest %s.target is unknown for its scope", context.c_str());
        }
        if (binding.scope == harken::studio::types::kTargetScopeVoiceEffectParam ||
            binding.scope == harken::studio::types::kTargetScopeBusEffectParam) {
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
        const json::JsonValue* curve = item.Find("curve");
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

inline bool ParseRecipeEffectType(const json::JsonValue& value,
                                  std::uint32_t* out) {
    if (value.kind != json::JsonValue::Kind::String) {
        return false;
    }
    const harken::studio::build::PackEffectSchemaRow* row =
        harken::studio::build::FindPackEffectSchemaByManifestName(value.text.c_str());
    if (row == nullptr) {
        return false;
    }
    *out = row->effectType;
    return true;
}

inline std::uint32_t RecipeParamCount(std::uint32_t type) {
    const harken::studio::build::PackEffectSchemaRow* row =
        harken::studio::build::FindPackEffectSchemaByType(type);
    return row == nullptr ? 0 : row->paramCount;
}

inline bool RecipeParamInAuthoredRange(std::uint32_t type,
                                       std::uint32_t index, float value) {
    return harken::studio::build::PackEffectParamInAuthoredRange(type, index, value);
}

inline bool ParseBusEffectsSection(const json::JsonValue& section,
                                   Manifest* out, Diag* diag) {
    for (std::size_t i = 0; i < section.items.size(); ++i) {
        const json::JsonValue& item = section.items[i];
        const std::string context = IndexedName("busEffects", i);
        if (item.kind != json::JsonValue::Kind::Object) {
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
        const json::JsonValue* type = item.Find("type");
        const json::JsonValue* bypass = item.Find("bypass");
        const json::JsonValue* params = item.Find("parameters");
        if (harken::studio::types::IsNull(effect.bus)) {
            return Fail(diag, "manifest %s.bus must be nonzero",
                        context.c_str());
        }
        if (effect.slot >= HK_PACK_VOICE_RECIPE_MAX_INSTALLS) {
            return Fail(diag, "manifest %s.slot must be 0..%u",
                        context.c_str(),
                        HK_PACK_VOICE_RECIPE_MAX_INSTALLS - 1);
        }
        if (effect.faderPointGuard >
            HK_PACK_VOICE_RECIPE_MAX_INSTALLS) {
            return Fail(diag,
                        "manifest %s.faderPointGuard must be 0..%u",
                        context.c_str(),
                        HK_PACK_VOICE_RECIPE_MAX_INSTALLS);
        }
        if (type == nullptr || !ParseRecipeEffectType(*type, &effect.type)) {
            return Fail(diag,
                        "manifest %s.type must name a registered effect",
                        context.c_str());
        }
        const harken::studio::build::PackEffectSchemaRow* effectSchema =
            harken::studio::build::FindPackEffectSchemaByType(effect.type);
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
            if (bypass->kind != json::JsonValue::Kind::Bool) {
                return Fail(diag, "manifest %s.bypass must be a boolean",
                            context.c_str());
            }
            effect.bypass = bypass->boolean;
        }
        const std::uint32_t expected = RecipeParamCount(effect.type);
        if (params == nullptr || params->kind != json::JsonValue::Kind::Array ||
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
                        harken::studio::types::ObjectIdString(out->busEffects[i].bus).c_str(),
                        out->busEffects[i].slot);
        }
    }
    return true;
}

inline bool ParseDynamicTap(const json::JsonValue& value, std::uint32_t* out) {
    if (value.kind != json::JsonValue::Kind::String) {
        return false;
    }
    if (value.text == "postEffects") {
        *out = harken::studio::types::kDynamicTapBusPostEffects;
        return true;
    }
    if (value.text == "preFader") {
        *out = harken::studio::types::kDynamicTapBusPreFader;
        return true;
    }
    if (value.text == "postFader") {
        *out = harken::studio::types::kDynamicTapBusPostFader;
        return true;
    }
    return false;
}

inline bool ParseDynamicRoutesSection(const json::JsonValue& section,
                                      Manifest* out, Diag* diag) {
    for (std::size_t i = 0; i < section.items.size(); ++i) {
        const json::JsonValue& item = section.items[i];
        const std::string context = IndexedName("dynamicRoutes", i);
        if (item.kind != json::JsonValue::Kind::Object) {
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
        const json::JsonValue* tap = item.Find("tap");
        if (harken::studio::types::IsNull(route.targetBus) || harken::studio::types::IsNull(route.sourceBus)) {
            return Fail(diag,
                        "manifest %s targetBus and sourceBus must be nonzero",
                        context.c_str());
        }
        if (route.effectSlot >= HK_PACK_VOICE_RECIPE_MAX_INSTALLS ||
            route.routeSlot >= 4) {
            return Fail(diag,
                        "manifest %s effectSlot must be 0..%u and "
                        "routeSlot must be 0..3",
                        context.c_str(),
                        HK_PACK_VOICE_RECIPE_MAX_INSTALLS - 1);
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
                        harken::studio::types::ObjectIdString(b.targetBus).c_str(),
                        b.effectSlot, b.routeSlot);
        }
    }
    for (const ManifestDynamicRoute& route : out->dynamicRoutes) {
        bool declaredTarget = false;
        for (const ManifestBusEffect& effect : out->busEffects) {
            if (effect.bus == route.targetBus &&
                effect.slot == route.effectSlot &&
                effect.type == harken::studio::types::kEffectCompressor) {
                declaredTarget = true;
                break;
            }
        }
        if (!declaredTarget) {
            return Fail(diag,
                        "manifest dynamic route target (%s,%u) must "
                        "name a Compressor in busEffects",
                        harken::studio::types::ObjectIdString(route.targetBus).c_str(),
                        route.effectSlot);
        }
    }
    return true;
}

inline bool ParseVoiceRecipesSection(const json::JsonValue& section,
                                     Manifest* out, Diag* diag) {
    for (std::size_t i = 0; i < section.items.size(); ++i) {
        const json::JsonValue& item = section.items[i];
        const std::string context = IndexedName("voiceRecipes", i);
        if (item.kind != json::JsonValue::Kind::Object) {
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
        if (harken::studio::types::IsNull(recipe.playable)) {
            return Fail(diag, "manifest %s.playable must be nonzero",
                        context.c_str());
        }
        const json::JsonValue* tone = item.Find("tone");
        const json::JsonValue* spatial = item.Find("spatial");
        const json::JsonValue* playback = item.Find("playback");
        const json::JsonValue* effects = item.Find("effects");
        const json::JsonValue* bindings = item.Find("bindings");
        if (effects == nullptr || effects->kind != json::JsonValue::Kind::Array ||
            effects->items.size() > HK_PACK_VOICE_RECIPE_MAX_INSTALLS) {
            return Fail(diag,
                        "manifest %s.effects must contain at most %u entries",
                        context.c_str(),
                        HK_PACK_VOICE_RECIPE_MAX_INSTALLS);
        }
        if (bindings == nullptr ||
            bindings->kind != json::JsonValue::Kind::Array ||
            bindings->items.size() > HK_PACK_VOICE_RECIPE_MAX_BINDINGS) {
            return Fail(diag,
                        "manifest %s.bindings must be an array of at most %u "
                        "ids",
                        context.c_str(),
                        HK_PACK_VOICE_RECIPE_MAX_BINDINGS);
        }
        if (tone != nullptr) {
            const std::string toneContext = context + ".tone";
            if (tone->kind != json::JsonValue::Kind::Object) {
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
                const json::JsonValue* field = tone->Find(key);
                if (field == nullptr || field->kind != json::JsonValue::Kind::Bool) {
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
            if (spatial->kind != json::JsonValue::Kind::Object) {
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
            if (harken::studio::types::IsNull(recipe.spatial.attenuation) ||
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
            if (playback->kind != json::JsonValue::Kind::Object) {
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
            const json::JsonValue& effect = effects->items[e];
            const std::string effectContext =
                context + ".effects[" + std::to_string(e) + "]";
            if (effect.kind != json::JsonValue::Kind::Object) {
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
            const json::JsonValue* type = effect.Find("type");
            const json::JsonValue* bypass = effect.Find("bypass");
            const json::JsonValue* params = effect.Find("parameters");
            if (type == nullptr || !ParseRecipeEffectType(*type, &parsed.type)) {
                return Fail(diag,
                            "manifest %s.type must be one of %s",
                            effectContext.c_str(),
                            harken::studio::build::kPackEffectManifestNameList);
            }
            const harken::studio::build::PackEffectSchemaRow* effectSchema =
                harken::studio::build::FindPackEffectSchemaByType(parsed.type);
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
            if (parsed.slot >= HK_PACK_VOICE_RECIPE_MAX_INSTALLS) {
                return Fail(diag, "manifest %s.slot must be 0..%u",
                            effectContext.c_str(),
                            HK_PACK_VOICE_RECIPE_MAX_INSTALLS - 1);
            }
            if (bypass != nullptr) {
                if (bypass->kind != json::JsonValue::Kind::Bool) {
                    return Fail(diag, "manifest %s.bypass must be a boolean",
                                effectContext.c_str());
                }
                parsed.bypass = bypass->boolean;
            }
            const std::uint32_t expected = RecipeParamCount(parsed.type);
            if (params == nullptr || params->kind != json::JsonValue::Kind::Array ||
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
        for (const json::JsonValue& binding : bindings->items) {
            harken::studio::types::ObjectId id{};
            if (binding.kind != json::JsonValue::Kind::String || !harken::studio::types::ParseObjectId(binding.text, &id) || harken::studio::types::IsNull(id)) {
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

inline bool ParseAttachmentsSection(const json::JsonValue& section,
                                    Manifest* out, Diag* diag) {
    for (std::size_t i = 0; i < section.items.size(); ++i) {
        const json::JsonValue& item = section.items[i];
        const std::string context = IndexedName("attachments", i);
        if (item.kind != json::JsonValue::Kind::Object) {
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
        if (attachment.scope == harken::studio::types::kTargetScopeVoiceEffectParam) {
            return Fail(diag,
                        "manifest %s.scope does not support voice-effect-"
                        "parameter attachments; use voiceRecipes[].bindings",
                        context.c_str());
        }
        out->attachments.push_back(attachment);
    }
    return true;
}

inline bool ParseContainersSection(const json::JsonValue& section,
                                   Manifest* out, Diag* diag) {
    for (std::size_t i = 0; i < section.items.size(); ++i) {
        const json::JsonValue& item = section.items[i];
        const std::string context = IndexedName("containers", i);
        if (item.kind != json::JsonValue::Kind::Object) {
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
        const json::JsonValue* children = item.Find("children");
        if (children == nullptr ||
            children->kind != json::JsonValue::Kind::Array ||
            children->items.empty()) {
            return Fail(diag,
                        "manifest %s requires a non-empty \"children\" "
                        "array",
                        context.c_str());
        }
        for (std::size_t c = 0; c < children->items.size(); ++c) {
            const json::JsonValue& childItem = children->items[c];
            const std::string childContext =
                context + "." + IndexedName("children", c);
            if (childItem.kind != json::JsonValue::Kind::Object) {
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

inline bool ParseLogicSection(const json::JsonValue& root, const char* key,
                              bool (*parse)(const json::JsonValue&, Manifest*,
                                            Diag*),
                              Manifest* out, Diag* diag) {
    const json::JsonValue* section = root.Find(key);
    if (section == nullptr) {
        return true;
    }
    if (section->kind != json::JsonValue::Kind::Array) {
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
            if (manifest.bindings[i].scope == harken::studio::types::kTargetScopeVoice ||
                manifest.bindings[i].scope ==
                    harken::studio::types::kTargetScopeVoiceEffectParam) {
                return Fail(diag,
                            "packKind \"resident\" requires scope "
                            "\"bus\" on bindings[%zu] — voice-scope "
                            "bindings reference content-pack playables",
                            i);
            }
        }
        for (std::size_t i = 0; i < manifest.attachments.size(); ++i) {
            if (manifest.attachments[i].scope != harken::studio::types::kTargetScopeBus &&
                manifest.attachments[i].scope !=
                    harken::studio::types::kTargetScopeBusEffectParam) {
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
                            harken::studio::types::ObjectIdString(manifest.buses[i].id).c_str());
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
                            harken::studio::types::ObjectIdString(manifest.busSends[i].from).c_str(),
                            harken::studio::types::ObjectIdString(manifest.busSends[i].to).c_str());
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
                            harken::studio::types::ObjectIdString(manifest.attenuations[i].id).c_str());
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
                            harken::studio::types::ObjectIdString(manifest.bindings[i].id).c_str());
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
                            harken::studio::types::ObjectIdString(manifest.voiceRecipes[i].playable).c_str());
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
                            harken::studio::types::ObjectIdString(manifest.attachments[i].binding).c_str());
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
                            harken::studio::types::ObjectIdString(manifest.containers[i].id).c_str());
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
                            harken::studio::types::ObjectIdString(manifest.containers[i].id).c_str(),
                            manifest.entries[j].path.c_str());
            }
        }
    }
    return true;
}


} // namespace harken::studio::pack

#endif // HARKEN_HOST_PACKC_MANIFEST_H
