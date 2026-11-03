#include "Common/ObjectIdTestSupport.h"
#include "Common/RawVoiceTestSupport.h"
#include "Engine.h"
#include "PlayableRuntime.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {
constexpr unsigned kFrames = 256, kRate = 48000, kWarmup = 128, kMeasured = 400;
struct Scene { unsigned endpoints, voices, layout; bool content; };
struct Result {
    bool ok = false;
    double median = 0, p99 = 0;
    std::uint64_t hash = 14695981039346656037ull, arenaBytes = 0;
};

Result Run(const Scene& scene, bool enabled) {
    Result result;
    VnConfig config{};
    config.structSize = sizeof(config);
    config.deviceMode = VN_DEVICE_NULL;
    config.sampleRate = kRate;
    config.blockFrames = kFrames;
    config.maxVoices = scene.voices;
    config.maxBuses = scene.content ? 1 : scene.endpoints + 1;
    config.arenaBytes = 1u << 24;
    config.commandQueueDepth = 1024;
    config.mixChannelLayout = config.deviceChannelLayout = scene.layout;
    if (Vn_Init(&config) != VN_OK) return result;
    struct Term { ~Term() { Vn_Term(); } } term;
    auto* engine = harken::detail::ActiveEngine();
    if (engine == nullptr) return result;
    std::vector<float> samples(kFrames * (kWarmup + kMeasured + 64), .1f / scene.voices);
    std::vector<harken::PlayablePropertyProgramViewV1> properties(scene.content ? scene.endpoints : 0);
    std::vector<harken::SignalReceiverV1> receivers(scene.endpoints);
    std::vector<harken::SignalRelationV1> relations(scene.endpoints);
    for (unsigned i = 0; i < scene.endpoints; ++i) {
        const auto id = harkentest::ObjectId(100 + i);
        if (!scene.content) {
            if (Vn_CreateBus(id, VN_BUS_MASTER) != VN_OK) return result;
            engine->nullBackend.Pump(1);
            if (Vn_Update() != VN_OK) return result;
        }
        if (scene.content || i == 0) {
            VnPCMDesc pcm{};
            pcm.structSize = sizeof(pcm);
            pcm.sampleRate = kRate;
            pcm.channels = 1;
            pcm.layout = VN_PCM_LAYOUT_INTERLEAVED;
            pcm.policy = VN_PCM_POLICY_COPY;
            pcm.frames = samples.size();
            pcm.samplesInterleaved = samples.data();
            if (Vn_RegisterPCM(id, &pcm) != VN_OK) return result;
        }
        auto& endpoint = receivers[i].target;
        endpoint.stableId = id;
        if (scene.content) {
            auto& property = properties[i];
            property.contentPropertyKind = harken::playableproperties::ContentPropertyKindV1::Sound;
            property.stableId = id;
            property.authoredStableOrder = i;
            property.contentRevision = engine->registry.Lookup(id).contentRevision;
            property.routeStableId = VN_BUS_MASTER;
            property.compatibilityFingerprint = i + 1;
            property.payload.presentMask = harken::kResolvedSectionGroupPropertyMaskV1;
            property.payload.values[3].bits = 1;
            property.payload.values[6].objectId = VN_BUS_MASTER;
            property.payload.values[11].bits = std::bit_cast<std::uint32_t>(1.0f);
            property.payload.values[16].bits = std::bit_cast<std::uint32_t>(1.0f);
            property.programSemanticFingerprint = harken::ComputePlayablePropertyFingerprintV1(property);
            endpoint.kind = harken::SignalEndpointKindV1::Sound;
            endpoint.expectedPropertyFingerprint = property.programSemanticFingerprint;
        }
    }
    for (unsigned i = 0; i < scene.endpoints; ++i) {
        relations[i].stableId = harkentest::ObjectId(10000 + i);
        relations[i].source = receivers[i].target;
        relations[i].target = receivers[(i + 1) % scene.endpoints].target;
    }
    harken::ContainerWorldInstallViewV1 world;
    world.sourceBodyVintage = harken::kCurrentWorldBodyVintageV20;
    world.playableProperties = properties.data();
    world.playablePropertyCount = static_cast<unsigned>(properties.size());
    world.identity.capabilityMask = scene.content ? harken::kWorldCapabilityPlayableProperties : 0;
    if (enabled) {
        world.signalRelations = relations.data(); world.signalRelationCount = scene.endpoints;
        world.signalReceivers = receivers.data(); world.signalReceiverCount = scene.endpoints;
        world.identity.capabilityMask |= harken::kWorldCapabilitySignalRelations;
    }
    world.identity.runtimeSemanticFingerprint = harken::ComputeCurrentWorldFingerprintV7(world);
    if (harken::InstallContainerWorldV1(*engine, world).status != harken::WorldInstallStatus::Ok) return result;
    for (unsigned i = 0; i < scene.voices; ++i) {
        if (scene.content) {
            const auto& property = properties[i % scene.endpoints];
            harken::PlayableStartRequest request;
            request.root.kind = harken::PlayableKindV1::Sound;
            request.root.stableId = property.stableId;
            request.root.expectedProgramSemanticFingerprint = property.programSemanticFingerprint;
            request.root.contentRevision = property.contentRevision;
            request.root.routeStableId = VN_BUS_MASTER;
            request.root.compatibilityFingerprint = property.compatibilityFingerprint;
            if (harken::StartPlayable(*engine, request).status != harken::PlayableStartStatus::Accepted) return result;
        } else {
            VnPlayDesc play{};
            play.structSize = sizeof(play);
            play.soundId = harkentest::ObjectId(100);
            play.busId = harkentest::ObjectId(100 + i % scene.endpoints);
            play.pitchRatio = 1;
            play.loopCount = 1;
            VnVoiceHandle voice = 0;
            if (harkentest::PlayRaw(&play, &voice) != VN_OK) return result;
        }
    }
    if (Vn_Update() != VN_OK) return result;
    for (unsigned i = 0; i < kWarmup; ++i) {
        engine->nullBackend.Pump(1);
        if (Vn_Update() != VN_OK) return result;
    }
    if (engine->voices.ActiveCount() != scene.voices) return result;
    std::vector<double> timings;
    timings.reserve(kMeasured);
    for (unsigned i = 0; i < kMeasured; ++i) {
        const auto begin = std::chrono::steady_clock::now();
        engine->nullBackend.Pump(1);
        const auto end = std::chrono::steady_clock::now();
        timings.push_back(std::chrono::duration<double, std::micro>(end - begin).count());
        const auto* output = engine->nullBackend.LastBlock();
        for (unsigned j = 0; j < kFrames * engine->nullBackend.Channels(); ++j) {
            if (!std::isfinite(output[j])) return result;
            const auto bits = std::bit_cast<std::uint32_t>(output[j]);
            for (unsigned shift = 0; shift < 32; shift += 8) {
                result.hash ^= (bits >> shift) & 255;
                result.hash *= 1099511628211ull;
            }
        }
        if (Vn_Update() != VN_OK) return result;
    }
    if (engine->voices.ActiveCount() != scene.voices || engine->mixer.Arena().OverflowCount() != 0) return result;
    std::sort(timings.begin(), timings.end());
    result.median = timings[kMeasured / 2];
    result.p99 = timings[kMeasured * 99 / 100 - 1];
    result.arenaBytes = engine->mixer.Arena().HighWaterBytes();
    result.ok = true;
    return result;
}
}

int RunSignalRoutingBenchmark(int argc) {
    if (argc != 0) return 2;
    const Scene scenes[] = {{2, 16, VN_CHANNEL_LAYOUT_STEREO, false},
        {8, 64, VN_CHANNEL_LAYOUT_STEREO, false},
        {8, 64, VN_CHANNEL_LAYOUT_STEREO, true},
        {8, 64, VN_CHANNEL_LAYOUT_7_1, true}};
    for (const auto& scene : scenes) {
        std::uint64_t hashes[2]{};
        for (unsigned repetition = 0; repetition < 3; ++repetition) {
            for (unsigned order = 0; order < 2; ++order) {
                const unsigned enabled = order ^ (repetition & 1u);
                const auto result = Run(scene, enabled != 0);
                std::printf("{\"kind\":\"%s\",\"endpoints\":%u,\"voices\":%u,\"layout\":%u,"
                    "\"enabled\":%u,\"repetition\":%u,\"warmup_blocks\":%u,\"measured_blocks\":%u,"
                    "\"rate\":%u,\"frames\":%u,\"workers\":0,\"render_median_us\":%.3f,\"render_p99_us\":%.3f,"
                    "\"arena_high_water_bytes\":%llu,\"pcm_hash\":\"%016llx\",\"ok\":%s}\n",
                    scene.content ? "sound" : "bus", scene.endpoints, scene.voices, scene.layout,
                    enabled, repetition, kWarmup, kMeasured, kRate, kFrames, result.median, result.p99,
                    static_cast<unsigned long long>(result.arenaBytes), static_cast<unsigned long long>(result.hash),
                    result.ok ? "true" : "false");
                if (!result.ok || (repetition != 0 && result.hash != hashes[enabled])) return 1;
                hashes[enabled] = result.hash;
            }
        }
    }
    return 0;
}
