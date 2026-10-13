#include "EditorWaveformPublication.h"

#include "JsonReader.h"
#include "JsonWriter.h"
#include "PackSidecar.h"
#include "harken_studio_build/WorldCodec.h"
#include "harken_studio_build/MediaContentRevision.h"
#include "harken_studio_build/PackImage.h"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <set>

namespace harkenstudiobuild::tooling {
namespace {
using harkenstudiojson::JsonValue;
using harkenstudiojson::JsonWriter;
using harkenstudiojson::PackSidecarText;

bool U64(const JsonValue& row, const char* key, std::uint64_t expected, bool numeric = false) {
    const auto* value = row.Find(key);
    if (!value) return false;
    std::uint64_t decoded = 0;
    return numeric ? value->AsU64(&decoded) && decoded == expected
        : value->kind == JsonValue::Kind::String &&
            harkenstudiojson::PackSidecarU64(value->text, &decoded) && decoded == expected;
}

std::string Guid(const std::array<std::uint8_t, 16>& bytes) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string text;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) text += '-';
        text += hex[bytes[i] >> 4];
        text += hex[bytes[i] & 15];
    }
    return text;
}

std::string Name(const GameplayCatalogImage& catalog, std::uint32_t offset, std::uint16_t length) {
    return {reinterpret_cast<const char*>(catalog.namePool.data() + offset), length};
}

bool ObjectId(const JsonValue& row, const char* key, harkenstudio::ObjectId expected) {
    harkenstudio::ObjectId decoded{};
    return ParseObjectId(PackSidecarText(row, key), &decoded) && decoded == expected;
}

struct ParsedPack {
    const EditorMetadataPackBytes* source = nullptr;
    const harkenstudiojson::PackSidecarEntry* manifest = nullptr;
    PackCanonicalImage image;
    CurrentWorldBodyImage world;
};
using PackMap = std::map<std::uint64_t, std::unique_ptr<ParsedPack>>;

bool CheckCatalog(const JsonValue& root, const ParsedPack& init) {
    const auto* json = root.Find("gameplayCatalog");
    if (!json || !harkenstudiojson::PackSidecarKeys(*json, {"schema", "projectId", "initPackStableId",
            "initContentRevision", "digest", "actions", "retired"})) return false;
    const auto& catalog = init.image.gameplayCatalog;
    const auto& header = catalog.header;
    if (!U64(*json, "schema", 2, true) || !U64(*json, "digest", header.digest) ||
        !U64(*json, "initPackStableId", header.initPackStableId) ||
        !U64(*json, "initContentRevision", header.initContentRevision, true) ||
        PackSidecarText(*json, "projectId") != Guid(header.projectGuid)) return false;
    const auto* active = json->Find("actions");
    const auto* retired = json->Find("retired");
    if (!active || active->kind != JsonValue::Kind::Array || active->items.size() != catalog.active.size() ||
        !retired || retired->kind != JsonValue::Kind::Array || retired->items.size() != catalog.retired.size()) return false;
    std::set<harkenstudio::ObjectId> seen;
    for (const auto& row : active->items) {
        if (!harkenstudiojson::PackSidecarKeys(row, {"name", "actionStableId", "nameBindingRevision",
                "callableFingerprint", "ownerPackStableId", "providerClosureFingerprint"})) return false;
        harkenstudio::ObjectId id{};
        if (!ParseObjectId(PackSidecarText(row, "actionStableId"), &id) || !seen.insert(id).second) return false;
        const auto match = std::find_if(catalog.active.begin(), catalog.active.end(),
            [id](const auto& entry) { return entry.actionStableId == id; });
        if (match == catalog.active.end()) return false;
        const auto name = Name(catalog, match->nameOffset, match->nameLength);
        if (PackSidecarText(row, "name") != name ||
            !U64(row, "nameBindingRevision", match->nameBindingRevision) ||
            !U64(row, "callableFingerprint", match->callableFingerprint) ||
            !U64(row, "ownerPackStableId", match->ownerPackStableId) ||
            !U64(row, "providerClosureFingerprint", match->providerClosureFingerprint) ||
            GameplayCallableFingerprint(match->actionStableId, match->nameBindingRevision, name) != match->callableFingerprint) return false;
    }
    std::set<std::string> retiredNames;
    for (const auto& row : retired->items) {
        if (!harkenstudiojson::PackSidecarKeys(row, {"name", "lastActionStableId",
                "lastNameBindingRevision", "reason"})) return false;
        const auto name = PackSidecarText(row, "name");
        if (!retiredNames.insert(name).second) return false;
        const auto match = std::find_if(catalog.retired.begin(), catalog.retired.end(), [&](const auto& entry) {
            return Name(catalog, entry.nameOffset, entry.nameLength) == name;
        });
        if (match == catalog.retired.end() || !ObjectId(row, "lastActionStableId", match->lastActionStableId) ||
            !U64(row, "lastNameBindingRevision", match->lastNameBindingRevision)) return false;
        const auto reason = PackSidecarText(row, "reason");
        const char* expected = match->reason == kGameplayCatalogRetiredReasonDeleted ? "deleted" :
            match->reason == kGameplayCatalogRetiredReasonReused ? "reused" : "renamed";
        if (reason != expected) return false;
    }
    return true;
}

bool Closure(std::uint64_t id, const PackMap& packs, std::set<std::uint64_t>& visited,
    std::set<std::uint64_t>& visiting) {
    std::vector<std::pair<std::uint64_t, bool>> pending{{id, false}};
    while (!pending.empty()) {
        const auto [next, expanded] = pending.back();
        pending.pop_back();
        if (expanded) { visiting.erase(next); visited.insert(next); continue; }
        if (visited.count(next)) continue;
        if (!visiting.insert(next).second) return false;
        const auto found = packs.find(next);
        if (found == packs.end()) return false;
        pending.push_back({next, true});
        for (const auto& edge : found->second->image.dependencyEdges)
            pending.push_back({edge.prerequisitePackStableId, false});
    }
    return true;
}

bool ZeroTime(const AuthoredTimeRationalV1& time) {
    return time.domain == AuthoredTimeDomainV1::AbsoluteNanoseconds && time.numerator == 0 && time.denominator == 1;
}

void Text(JsonWriter& json, const char* key, const std::string& value) { json.Key(key); json.ValueString(value); }
void Id(JsonWriter& json, const char* key, std::uint64_t value) { Text(json, key, JsonWriter::DecimalU64(value)); }
void Id(JsonWriter& json, const char* key, harkenstudio::ObjectId value) { Text(json, key, ObjectIdString(value)); }
void Number(JsonWriter& json, const char* key, std::uint64_t value) { json.Key(key); json.ValueU64(value); }

std::string Float(float sample) {
    if (sample == 0) return "0";
    std::array<char, 48> bytes{};
    const auto result = std::to_chars(bytes.data(), bytes.data() + bytes.size(), sample,
        std::chars_format::general, std::numeric_limits<float>::max_digits10);
    return {bytes.data(), result.ptr};
}

const PlayablePropertyProgramViewV1* FindProperty(harkenstudio::ObjectId id,
    const std::set<std::uint64_t>& closure, const PackMap& packs, const ParsedPack** owner = nullptr) {
    const PlayablePropertyProgramViewV1* result = nullptr;
    for (auto packId : closure) {
        const auto* pack = packs.at(packId).get();
        const auto* world = pack->world.ProductionView();
        for (std::uint32_t i = 0; world && i < world->playablePropertyCount; ++i) {
            const auto& row = world->playableProperties[i];
            if (row.stableId != id) continue;
            if (result) return nullptr;
            result = &row;
            if (owner) *owner = pack;
        }
    }
    return result;
}

bool SafePayload(const PlayablePropertyPayloadViewV1& payload, std::string* reason) {
    *reason = "authored-pitch-or-loop";
    if (((payload.presentMask & 2u) && std::bit_cast<float>(static_cast<std::uint32_t>(payload.values[1].bits)) != 0) ||
        ((payload.presentMask & 8u) && payload.values[3].bits != 1)) return false;
    *reason = "effects-or-sends";
    return payload.effectNodeCount == 0 && payload.effectEdgeCount == 0 && payload.sendCount == 0;
}

bool SafeProperty(const PlayablePropertyProgramViewV1& property,
    const std::set<std::uint64_t>& closure, const PackMap& packs,
    std::set<harkenstudio::ObjectId>& path, std::string* reason) {
    path.insert(property.stableId);
    if (!SafePayload(property.payload, reason)) return false;
    for (std::uint32_t i = 0; i < property.structuralAncestorCount; ++i) {
        const auto& ancestor = property.structuralAncestors[i];
        path.insert(ancestor.stableId);
        if (!SafePayload(ancestor.payload, reason)) return false;
    }
    if (property.structuralAncestorCount == 0) {
        for (std::uint32_t i = 0; i < property.organizationLayerAncestorCount; ++i) {
            const auto& ref = property.organizationLayerAncestors[i];
            const auto* ancestor = FindProperty(ref.layerStableId, closure, packs);
            *reason = "unresolved-properties";
            if (!ancestor || ancestor->programSemanticFingerprint != ref.layerProgramSemanticFingerprint) return false;
            path.insert(ancestor->stableId);
            if (!SafePayload(ancestor->payload, reason)) return false;
        }
    }
    return true;
}

bool DurationKnown(const ActionStepProgramViewV1& step, const PackTocEntry& media,
    const PlayablePropertyProgramViewV1& property, const PlayablePropertyProgramViewV1* container,
    const PlayableOccurrencePropertyViewV1* occurrence, const std::set<std::uint64_t>& closure,
    const PackMap& packs, std::string* reason) {
    *reason = "authored-timing";
    if (!ZeroTime(step.delay) || !ZeroTime(step.fade) || !ZeroTime(step.cooldown) ||
        step.quantizeTiming != ActionQuantizeTimingV1::Immediate || !IsNull(step.quantizeReference.stableId) ||
        step.quantizeReference.expectedProgramSemanticFingerprint != 0) return false;
    *reason = "looping-media";
    if (media.flags != 0 || media.loopStartFrame != 0 || media.loopEndFrame != 0) return false;
    std::set<harkenstudio::ObjectId> path;
    if (!SafeProperty(property, closure, packs, path, reason) ||
        (container && !SafeProperty(*container, closure, packs, path, reason)) ||
        (occurrence && !SafePayload(occurrence->payload, reason))) return false;
    for (auto id : closure) {
        const auto& pack = packs.at(id)->image;
        *reason = "effects-or-sends";
        if (!pack.busEffects.empty() || !pack.busSends.empty() || !pack.dynamicRoutes.empty()) return false;
        for (const auto& attachment : pack.bindingAttachments) {
            // Bindings on output buses and authored ancestors can change playback timing too.
            if (attachment.scopeKind != harkenstudio::kTargetScopeVoice || path.count(attachment.objectId)) {
                *reason = "dynamic-binding";
                return false;
            }
        }
    }
    reason->clear();
    return true;
}

struct Candidate {
    std::string occurrence;
    std::string sourceKey;
    const PlayablePropertyProgramViewV1* property = nullptr;
    const PlayableOccurrencePropertyViewV1* overrides = nullptr;
    const ParsedPack* mediaOwner = nullptr;
    const PackTocEntry* media = nullptr;
};
using SourceTable = std::map<std::string, std::string>;

std::string SourceKey(const Candidate& candidate) {
    const auto& p = *candidate.property;
    return ObjectIdString(p.stableId) + ":" + std::to_string(p.programSemanticFingerprint) + ":" +
        std::to_string(p.contentRevision) + ":" + std::to_string(candidate.mediaOwner->image.packStableId) + ":" +
        std::to_string(candidate.mediaOwner->manifest->contentHash);
}

bool WriteSource(const Candidate& candidate, std::string* output, std::string* error) {
    const auto& media = *candidate.media;
    const auto& property = *candidate.property;
    const auto* bytes = candidate.mediaOwner->source->bytes.data() + media.blobOffset;
    const auto sampleBits = [&](std::uint32_t c, std::uint64_t f) {
        return LoadLE32(bytes + (c * media.frameCount + f) * sizeof(float));
    };
    // Hash the final planar PCM in canonical frame/channel order without a second PCM allocation.
    static constexpr char domain[] = "HARKEN-MEDIA-PCM-V1";
    MediaContentRevisionHasherV1 hash(domain, sizeof(domain));
    hash.AddU32(media.sampleRate); hash.AddU32(media.channelCount); hash.AddU64(media.frameCount);
    for (std::uint64_t f = 0; f < media.frameCount; ++f) for (std::uint32_t c = 0; c < media.channelCount; ++c) {
        auto bits = sampleBits(c, f);
        if ((bits & 0x7f800000u) == 0x7f800000u) { *error = "Sound content revision does not match finite final PCM"; return false; }
        if ((bits & 0x7fffffffu) == 0) bits = 0;
        hash.AddU32(bits);
    }
    if ((media.flags & kPackFlagLoopable) && media.loopEndFrame) {
        hash.AddU64(media.loopStartFrame); hash.AddU64(media.loopEndFrame);
    }
    if (hash.Finish() != property.contentRevision) { *error = "Sound content revision does not match finite final PCM"; return false; }
    const auto buckets = static_cast<std::uint32_t>(std::min<std::uint64_t>(media.frameCount, kEditorWaveformMaxBuckets));
    JsonWriter json;
    json.BeginObject();
    Text(json, "key", candidate.sourceKey);
    Id(json, "soundStableId", property.stableId);
    Id(json, "propertyFingerprint", property.programSemanticFingerprint);
    Id(json, "contentRevision", property.contentRevision);
    Id(json, "ownerPackStableId", candidate.mediaOwner->image.packStableId);
    Id(json, "contentHash", candidate.mediaOwner->manifest->contentHash);
    Id(json, "frameCount", media.frameCount);
    Number(json, "sampleRate", media.sampleRate); Number(json, "channelCount", media.channelCount); Number(json, "bucketCount", buckets);
    json.Key("channels"); json.BeginArray();
    for (std::uint32_t c = 0; c < media.channelCount; ++c) {
        std::vector<float> minima(buckets), maxima(buckets);
        const auto boundary = [&](std::uint64_t k) { return (media.frameCount / buckets) * k + ((media.frameCount % buckets) * k) / buckets; };
        for (std::uint32_t b = 0; b < buckets; ++b) {
            float lo = std::bit_cast<float>(sampleBits(c, boundary(b))), hi = lo;
            for (auto f = boundary(b) + 1; f < boundary(b + 1); ++f) {
                const float sample = std::bit_cast<float>(sampleBits(c, f));
                lo = std::min(lo, sample); hi = std::max(hi, sample);
            }
            minima[b] = lo; maxima[b] = hi;
        }
        json.BeginObject(); json.Key("min"); json.BeginArray();
        for (float sample : minima) json.ValueRawNumber(Float(sample));
        json.EndArray(); json.Key("max"); json.BeginArray();
        for (float sample : maxima) json.ValueRawNumber(Float(sample));
        json.EndArray(); json.EndObject();
    }
    json.EndArray(); json.EndObject();
    *output = json.Finish();
    return true;
}

bool WriteAction(std::string* output, const PackGameplayCatalogActiveRow& action,
    const GameplayCatalogImage& catalog, const PackMap& packs, SourceTable& sources,
    std::size_t* budget, std::size_t* decodedBudget, std::string* error) {
    const auto owner = packs.find(action.ownerPackStableId);
    if (owner == packs.end()) { *error = "Action owner is absent from the selected publication"; return false; }
    const auto* world = owner->second->world.ProductionView();
    const ActionProgramViewV1* program = nullptr;
    if (world) for (std::uint32_t i = 0; i < world->actionCount; ++i)
        if (world->actions[i].actionStableId == action.actionStableId) program = &world->actions[i];
    if (!program) { *error = "Action program is absent from its declared owner Pack"; return false; }
    std::vector<std::pair<std::uint64_t, std::uint64_t>> providers{
        {action.ownerPackStableId, owner->second->image.requirement.packContentRevision}};
    for (const auto& edge : owner->second->image.dependencyEdges)
        providers.push_back({edge.prerequisitePackStableId, edge.prerequisitePackStableId == catalog.header.initPackStableId
            ? catalog.header.initContentRevision : 0});
    std::sort(providers.begin(), providers.end());
    providers.erase(std::unique(providers.begin(), providers.end()), providers.end());
    std::vector<std::uint8_t> providerBytes(providers.size() * 16);
    for (std::size_t i = 0; i < providers.size(); ++i) {
        StoreLE64(providerBytes.data() + i * 16, providers[i].first);
        StoreLE64(providerBytes.data() + i * 16 + 8, providers[i].second);
    }
    if (Fnv1a64(providerBytes.data(), providerBytes.size()) != action.providerClosureFingerprint) {
        *error = "Action provider closure does not match its Init catalog identity";
        return false;
    }
    std::set<std::uint64_t> closure, visiting;
    if (!Closure(action.ownerPackStableId, packs, closure, visiting)) { *error = "Invalid Pack dependency closure"; return false; }
    std::string unavailable;
    const PlayablePropertyProgramViewV1* containerProperty = nullptr;
    std::vector<Candidate> candidates;
    std::string kind = "directSound";
    if (program->stepCount != 1 || program->steps[0].opcode != ActionOpcodeV1::Play) unavailable = "composite-or-non-sound-action";
    else {
        const auto& target = program->steps[0].target;
        if (target.kind == ActionTargetKindV1::Sound) {
            Candidate candidate;
            candidate.occurrence = ObjectIdString(action.actionStableId) + ":0";
            candidate.property = FindProperty(target.stableId, closure, packs);
            candidates.push_back(candidate);
            if (candidate.property && candidate.property->programSemanticFingerprint != target.expectedProgramSemanticFingerprint) {
                *error = "Action target and Sound program identity disagree"; return false;
            }
        } else if (target.kind == ActionTargetKindV1::Container) {
            kind = "randomCandidates";
            const ParsedPack* containerOwner = nullptr;
            containerProperty = FindProperty(target.stableId, closure, packs, &containerOwner);
            const ContainerProgramViewV3* container = nullptr;
            if (containerOwner) {
                const auto* view = containerOwner->world.ProductionView();
                for (std::uint32_t i = 0; i < view->containerCount; ++i)
                    if (view->containers[i].containerStableId == target.stableId) container = &view->containers[i];
            }
            if (!container || !containerProperty) unavailable = "unresolved-container";
            else if (container->programSemanticFingerprint != target.expectedProgramSemanticFingerprint ||
                containerProperty->programSemanticFingerprint != target.expectedProgramSemanticFingerprint) {
                *error = "Action target and container identity disagree"; return false;
            } else if (container->mode != ContainerModeV1::Random || container->childCount == 0) unavailable = "unsupported-container";
            else if (container->childCount > kEditorWaveformMaxCandidates) unavailable = "metadata-budget";
            else {
                std::vector<const ContainerChildProgramViewV2*> children;
                for (std::uint32_t i = 0; i < container->childCount; ++i) children.push_back(&container->children[i]);
                std::sort(children.begin(), children.end(), [](auto a, auto b) { return a->stableChildOrdinal < b->stableChildOrdinal; });
                std::set<std::uint32_t> ordinals;
                const auto* view = containerOwner->world.ProductionView();
                for (const auto* child : children) {
                    if (!ordinals.insert(child->stableChildOrdinal).second) { *error = "Duplicate candidate occurrence"; return false; }
                    if (child->playable.kind != PlayableKindV1::Sound) { unavailable = "unsupported-candidate"; break; }
                    Candidate candidate;
                    candidate.occurrence = ObjectIdString(target.stableId) + ":" + std::to_string(child->stableChildOrdinal);
                    candidate.property = FindProperty(child->playable.stableId, closure, packs);
                    if (candidate.property && (candidate.property->programSemanticFingerprint != child->playable.expectedProgramSemanticFingerprint ||
                        candidate.property->contentRevision != child->playable.contentRevision ||
                        candidate.property->routeStableId != child->playable.routeStableId ||
                        candidate.property->compatibilityFingerprint != child->playable.compatibilityFingerprint)) {
                        *error = "Candidate and Sound identity disagree"; return false;
                    }
                    for (std::uint32_t i = 0; i < view->playableOccurrenceCount; ++i) {
                        const auto& occurrence = view->playableOccurrences[i];
                        if (occurrence.parentStableId != target.stableId || occurrence.stableOccurrenceOrdinal != child->stableChildOrdinal) continue;
                        if (candidate.overrides || occurrence.parentProgramSemanticFingerprint != containerProperty->programSemanticFingerprint ||
                            occurrence.childPlayable.stableId != child->playable.stableId ||
                            occurrence.childPlayable.expectedProgramSemanticFingerprint != child->playable.expectedProgramSemanticFingerprint) {
                            *error = "Candidate occurrence identity disagrees"; return false;
                        }
                        candidate.overrides = &occurrence;
                    }
                    if (!candidate.overrides) { unavailable = "unresolved-occurrence"; break; }
                    candidates.push_back(candidate);
                }
            }
        } else unavailable = "composite-or-non-sound-action";
    }
    SourceTable pending;
    std::size_t allocated = candidates.size() * 1024;
    std::size_t pendingDecodedBytes = 0;
    std::string durationReason;
    double minSeconds = std::numeric_limits<double>::max(), maxSeconds = 0;
    if (unavailable.empty()) for (auto& candidate : candidates) {
        if (!candidate.property || candidate.property->contentPropertyKind != playableproperties::ContentPropertyKindV1::Sound) {
            unavailable = "unresolved-source"; break;
        }
        for (auto id : closure) for (const auto& media : packs.at(id)->image.media) {
            if (media.soundId != candidate.property->stableId) continue;
            if (candidate.media) { *error = "Ambiguous media ownership"; return false; }
            candidate.media = &media; candidate.mediaOwner = packs.at(id).get();
        }
        if (!candidate.media) { unavailable = "unresolved-source"; break; }
        const auto& media = *candidate.media;
        if (media.channelCount > kEditorWaveformMaxChannels) { unavailable = "metadata-budget"; break; }
        candidate.sourceKey = SourceKey(candidate);
        if (!sources.count(candidate.sourceKey) && !pending.count(candidate.sourceKey)) {
            const auto buckets = static_cast<std::size_t>(std::min<std::uint64_t>(media.frameCount, kEditorWaveformMaxBuckets));
            const auto estimate = buckets * media.channelCount * 80 + 2048;
            // Match the SDK reader's decoded source budget, independently of JSON size.
            const auto decodedBytes = buckets * media.channelCount * 8 + 1024;
            if (allocated > *budget || estimate > *budget - allocated ||
                decodedBytes > *decodedBudget - pendingDecodedBytes) { unavailable = "metadata-budget"; break; }
            allocated += estimate;
            pendingDecodedBytes += decodedBytes;
            std::string source;
            if (!WriteSource(candidate, &source, error)) return false;
            pending.emplace(candidate.sourceKey, std::move(source));
        }
        std::string reason;
        if (!DurationKnown(program->steps[0], media, *candidate.property, containerProperty, candidate.overrides, closure, packs, &reason) && durationReason.empty())
            durationReason = reason;
        const double seconds = static_cast<double>(media.frameCount) / media.sampleRate;
        minSeconds = std::min(minSeconds, seconds); maxSeconds = std::max(maxSeconds, seconds);
    }
    if (allocated > *budget && unavailable.empty()) unavailable = "metadata-budget";
    JsonWriter json;
    json.BeginObject();
    Text(json, "name", Name(catalog, action.nameOffset, action.nameLength));
    Id(json, "actionStableId", action.actionStableId);
    Id(json, "nameBindingRevision", action.nameBindingRevision);
    Id(json, "callableFingerprint", action.callableFingerprint);
    Id(json, "ownerPackStableId", action.ownerPackStableId);
    Id(json, "providerClosureFingerprint", action.providerClosureFingerprint);
    Text(json, "kind", unavailable.empty() ? kind : "unavailable");
    if (!unavailable.empty()) Text(json, "sourceReason", unavailable);
    json.Key("duration"); json.BeginObject();
    if (!unavailable.empty() || !durationReason.empty()) {
        Text(json, "kind", "unknown");
        Text(json, "reason", unavailable.empty() ? durationReason : "source-unavailable");
    } else {
        Text(json, "kind", minSeconds == maxSeconds ? "exact" : "bounded");
        const auto seconds = [&](const char* key, double value) {
            std::array<char, 48> buffer{};
            const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, std::chars_format::general, 17);
            json.Key(key); json.ValueRawNumber(std::string(buffer.data(), converted.ptr));
        };
        seconds("minSeconds", minSeconds); seconds("maxSeconds", maxSeconds);
    }
    json.EndObject();
    json.Key("candidates"); json.BeginArray();
    if (unavailable.empty()) for (const auto& candidate : candidates) {
        json.BeginObject(); Text(json, "occurrence", candidate.occurrence); Text(json, "sourceKey", candidate.sourceKey); json.EndObject();
    }
    json.EndArray(); json.EndObject();
    *output = json.Finish();
    // Commit sources only after the whole Action is accepted. Failed Actions consume no source budget.
    if (unavailable.empty()) {
        std::size_t bytes = output->size();
        for (const auto& [key, source] : pending) bytes += source.size() + 1;
        if (bytes > *budget) { *error = "Editor metadata exceeds the companion budget"; return false; }
        *budget -= bytes;
        *decodedBudget -= pendingDecodedBytes;
        sources.merge(pending);
    }
    return true;
}
} // namespace

bool BuildEditorWaveformPublication(std::string_view selectedManifestUtf8,
    std::span<const EditorMetadataPackBytes> sources, std::string* companionJson, std::string* error) {
    if (!companionJson || !error) return false;
    companionJson->clear();
    *error = "Invalid or oversized selected manifest";
    if (selectedManifestUtf8.empty() || selectedManifestUtf8.size() > kEditorWaveformMaxBytes) return false;
    const std::string manifest(selectedManifestUtf8);
    JsonValue root;
    harkenstudiojson::PackSidecarPlatform platform;
    std::vector<harkenstudiojson::PackSidecarEntry> entries;
    if (!harkenstudiojson::ParsePackSidecarManifest(manifest, &platform, &entries, error) ||
        !harkenstudiojson::ParseJson(manifest, &root, error, true)) return false;
    *error = "Selected manifest and Pack bytes disagree";
    if (!U64(root, "schema_version", VNX::PackSchema::kPackManifestSchemaVersion, true) || sources.size() != entries.size()) return false;
    PackMap packs;
    ParsedPack* init = nullptr;
    const auto* packRows = root.Find("packs");
    for (const auto& source : sources) {
        const auto entry = std::find_if(entries.begin(), entries.end(), [&](const auto& row) { return row.packStableId == source.packStableId; });
        if (entry == entries.end() || packs.count(source.packStableId) || source.bytes.empty() ||
            Fnv1a64(source.bytes.data(), source.bytes.size()) != entry->contentHash) return false;
        auto decoded = std::make_unique<ParsedPack>();
        decoded->source = &source;
        decoded->manifest = &*entry;
        if (ParsePackCanonicalImage(source.bytes.data(), source.bytes.size(), &decoded->image) != harkenstudiobuild::kPackOk ||
            decoded->image.packStableId != source.packStableId || !decoded->image.hasPartition ||
            !decoded->world.Decode(decoded->image.partitionBody.data(), decoded->image.partitionBody.size(),
                decoded->image.partitionHeader.bodyVintage)) return false;
        const auto index = static_cast<std::size_t>(entry - entries.begin());
        const auto& row = packRows->items[index];
        if (!U64(row, "packKind", decoded->image.packKind, true) ||
            !U64(row, "contentRevision", decoded->image.requirement.packContentRevision, true)) return false;
        if (entry->isInit) {
            if (decoded->image.packKind != kPackKindInit ||
                !decoded->image.hasGameplayCatalog ||
                (decoded->image.requirement.storageModelVersion == 2 &&
                 !decoded->image.hasBusDefinitionSnapshot)) return false;
            if (decoded->image.hasCatalog) {
                *error = "Legacy Init routing catalogs are not canonical Editor metadata inputs";
                return false;
            }
            init = decoded.get();
        } else if (decoded->image.packKind != kPackKindCanonicalContent) return false;
        packs.emplace(source.packStableId, std::move(decoded));
    }
    if (!init || !CheckCatalog(root, *init) || init->image.gameplayCatalog.active.size() > kEditorWaveformMaxActions) return false;
    std::set<std::uint64_t> visited, visiting;
    for (const auto& [id, pack] : packs) {
        if (!Closure(id, packs, visited, visiting)) return false;
        std::set<std::uint64_t> expected, actual;
        for (const auto& dependency : pack->manifest->dependencies) {
            std::uint64_t prerequisite = 0;
            if (!harkenstudiojson::PackSidecarU64(dependency, &prerequisite) || !expected.insert(prerequisite).second) return false;
        }
        for (const auto& edge : pack->image.dependencyEdges) {
            const auto prerequisite = packs.find(edge.prerequisitePackStableId);
            if (prerequisite == packs.end() || !actual.insert(edge.prerequisitePackStableId).second ||
                (edge.expectedPrerequisiteContentRevision != 0 && edge.expectedPrerequisiteContentRevision !=
                    prerequisite->second->image.requirement.packContentRevision)) return false;
        }
        if (expected != actual) return false;
        if (id != init->image.packStableId && ValidateCanonicalInitDependency(pack->image.dependencyEdges,
                init->image.packStableId) != harkenstudiobuild::kPackOk) return false;
    }
    JsonWriter json;
    json.BeginObject();
    Number(json, "schemaVersion", 2);
    Text(json, "envelopeKind", "exported-source-pcm-minmax");
    Number(json, "manifestByteLength", manifest.size());
    Id(json, "manifestFnv1a64", Fnv1a64(manifest.data(), manifest.size()));
    Text(json, "projectId", Guid(init->image.gameplayCatalog.header.projectGuid));
    Text(json, "platformId", platform.id);
    Text(json, "basePlatform", platform.basePlatform);
    Text(json, "language", PackSidecarText(root, "language"));
    Id(json, "initPackStableId", init->image.packStableId);
    Id(json, "initContentHash", init->manifest->contentHash);
    Id(json, "catalogDigest", init->image.gameplayCatalog.header.digest);
    json.EndObject();
    auto result = json.Finish();
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) result.pop_back();
    result.pop_back();
    auto actions = init->image.gameplayCatalog.active;
    std::sort(actions.begin(), actions.end(), [](const auto& a, const auto& b) { return a.actionStableId < b.actionStableId; });
    std::size_t budget = kEditorWaveformMaxBytes - actions.size() * 2048 - 4096;
    std::size_t decodedBudget = kEditorWaveformMaxBytes;
    SourceTable sourceTable;
    std::vector<std::string> actionRows;
    for (const auto& action : actions) {
        std::string row;
        if (!WriteAction(&row, action, init->image.gameplayCatalog, packs, sourceTable, &budget, &decodedBudget, error)) return false;
        actionRows.push_back(std::move(row));
    }
    result += ",\"sources\":[";
    bool comma = false;
    for (const auto& [key, source] : sourceTable) { if (comma) result += ','; result += source; comma = true; }
    result += "],\"actions\":[";
    comma = false;
    for (const auto& row : actionRows) { if (comma) result += ','; result += row; comma = true; }
    result += "]}";
    if (result.size() > kEditorWaveformMaxBytes) { *error = "Editor metadata exceeds the companion budget"; return false; }
    *companionJson = std::move(result);
    error->clear();
    return true;
}

} // namespace harkenstudiobuild::tooling
