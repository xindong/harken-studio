// Shared helpers for the runtime test tree: engine session RAII, signal
// synthesis (sine / impulse / DC — spec section 16 analytic inputs), PCM
// descriptor builders, and null-backend capture.
//
// Silence guard: every buffer-comparison test must assert the compared
// signal is non-silent via AssertNonSilent (peak above kSilenceFloor); a
// comparison where both sides are silent does not count as a pass.
#ifndef HARKEN_TESTS_TESTSUPPORT_TEST_UTIL_H
#define HARKEN_TESTS_TESTSUPPORT_TEST_UTIL_H

#include <harken/Harken.h>
#include "UnitTest/SignalTestSupport.h"

#include "Engine.h"
#include "UnitTest/RawVoiceTestSupport.h"
#include "harken/core/AllocGuard.h"
#include "harken/core/LogRing.h"
#include "harken/param/VariableService.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <vector>

namespace harkentest {

// One definitions-only current world definition row for tests.
inline harken::HistoricalContainerWorldViewV1 AsHistoricalPrefix(
    const harken::ContainerWorldInstallViewV1& src) noexcept {
    harken::HistoricalContainerWorldViewV1 dst{};
    dst.identity = src.identity;
    dst.variables = src.variables;
    dst.variableCount = src.variableCount;
    dst.playableProperties = src.playableProperties;
    dst.playablePropertyCount = src.playablePropertyCount;
    dst.playableOccurrences = src.playableOccurrences;
    dst.playableOccurrenceCount = src.playableOccurrenceCount;
    dst.trackSchedules = src.trackSchedules;
    dst.trackScheduleCount = src.trackScheduleCount;
    dst.segmentSchedules = src.segmentSchedules;
    dst.segmentScheduleCount = src.segmentScheduleCount;
    dst.containers = nullptr;
    dst.containerCount = 0;
    dst.transitions = src.transitions;
    dst.transitionCount = src.transitionCount;
    dst.actions = src.actions;
    dst.actionCount = src.actionCount;
    dst.capacity = src.capacity;
    return dst;
}

inline void CopyLeafTimingToHint(
    harken::ContainerChildProgramViewV2& child,
    const harken::AuthoredCompletionLeafV1& leaf) noexcept {
    child.timingHint.kind = harken::ChildTimingHintKindV2::InlineSound;
    auto& hint = child.timingHint.payload.inlineSound;
    hint.startOffset = leaf.startOffset;
    hint.duration = leaf.duration;
    hint.loopCount = leaf.loopCount;
    hint.loopRegionPresent = leaf.loopRegionPresent;
    hint.loopStart = leaf.loopStart;
    hint.loopEnd = leaf.loopEnd;
}

inline harken::VariableDefinitionV1 MakeVariableDefinition(
    VnVariableId id, harken::VariableKindV1 kind, float rangeMin,
    float rangeMax, float defaultValue) {
    harken::VariableDefinitionV1 definition{};
    definition.variableId = id;
    definition.kind = kind;
    definition.rangeMin = rangeMin;
    definition.rangeMax = rangeMax;
    definition.defaultValue = defaultValue;
    return definition;
}

// Installs a definitions-only current world into the active engine and
// asserts the install result, so tests exercise the same production entry
// the host scenario drives.
inline harken::WorldInstallResult InstallDefinitionsWorld(
    harken::Engine* engine, const harken::VariableDefinitionV1* definitions,
    std::uint32_t count, std::uint64_t = 0x5445535457u) {
    harken::ContainerWorldInstallViewV1 world{};
    world.variables = definitions;
    world.variableCount = count;
    world.identity.capabilityMask =
        count == 0 ? 0 : harken::kWorldCapabilityVariableDefinitions;
    world.identity.runtimeSemanticFingerprint =
        harken::ComputeCurrentWorldFingerprintV4(world);
    return harken::InstallContainerWorldV1(*engine, world);
}

inline harken::WorldInstallResult InstallSmoothDefinitions(
    harken::Engine* engine, std::initializer_list<VnVariableId> ids,
    float rangeMin = -16777216.0f, float rangeMax = 16777216.0f,
    float defaultValue = 0.0f) {
    std::vector<harken::VariableDefinitionV1> definitions;
    definitions.reserve(ids.size());
    for (VnVariableId id : ids) {
        definitions.push_back(MakeVariableDefinition(
            id, harken::VariableKindV1::Smooth, rangeMin, rangeMax,
            defaultValue));
    }
    return InstallDefinitionsWorld(
        engine, definitions.data(),
        static_cast<std::uint32_t>(definitions.size()));
}

inline void AdvanceVariableWrite(harken::Engine* engine) {
    // An accepted write reaches Applied only after its effective quantum
    // (publish lead ahead of the audio clock), the slice page freezes, the
    // audio thread applies it, and the following update commits — several
    // quanta by contract, not one.
    for (std::uint32_t step = 0;
         step < 32 && engine->variables.PendingCount() != 0; ++step) {
        ASSERT_EQ(VN_OK, Vn_Update());
        engine->nullBackend.Pump(1);
    }
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_EQ(0u, engine->variables.PendingCount());
}

inline void ReachVariableWriteEffectiveBlock(harken::Engine* engine) {
    engine->nullBackend.Pump(8);
}

inline VnConfig MakeNullConfig(std::uint32_t maxVoices = 0,
                               std::uint32_t blockFrames = 0,
                               std::uint32_t sampleRate = 0) {
    VnConfig config{};
    config.structSize = sizeof(VnConfig);
    config.deviceMode = VN_DEVICE_NULL;
    config.maxVoices = maxVoices;
    config.blockFrames = blockFrames;
    config.sampleRate = sampleRate;
    return config;
}

// A null-backend config at an explicit mix / device channel layout. Leaving a
// layout as VN_CHANNEL_LAYOUT_DEFAULT keeps its documented default (mix ->
// stereo; device -> the mix layout). Used by the multichannel goldens.
inline VnConfig MakeNullConfigLayout(std::uint32_t maxVoices,
                                     std::uint32_t blockFrames,
                                     std::uint32_t sampleRate,
                                     std::uint32_t mixLayout,
                                     std::uint32_t deviceLayout) {
    VnConfig config = MakeNullConfig(maxVoices, blockFrames, sampleRate);
    config.mixChannelLayout = mixLayout;
    config.deviceChannelLayout = deviceLayout;
    return config;
}

// Init/Term pairing for one test body.
class EngineSession {
public:
    explicit EngineSession(const VnConfig& config) {
        m_initResult = Vn_Init(&config);
    }
    ~EngineSession() {
        if (m_initResult == VN_OK) {
            Vn_Term();
        }
    }
    EngineSession(const EngineSession&) = delete;
    EngineSession& operator=(const EngineSession&) = delete;

    VnResult InitResult() const { return m_initResult; }
    harken::Engine* Engine() const { return harken::detail::ActiveEngine(); }
    void Release() { m_initResult = VN_NOT_INITIALIZED; }

private:
    VnResult m_initResult;
};

// Translates a public voice handle to the internal render-slot handle the
// engine-internal maps (voice pool, parameter system, spatializer) are keyed
// by. Tests that reach past the C API into those maps must resolve the render
// handle first, because a public handle is a ledger handle whose slot need not
// equal the render slot once entries diverge (a reused slot with a lingering
// ledger entry). Returns 0 for a stale public handle.
inline VnVoiceHandle RenderHandle(harken::Engine* engine,
                                  VnVoiceHandle publicHandle) {
    return engine->ledger.ToRender(publicHandle);
}

// The snapshot voice row correlated to a public handle via the
// voicePublicHandles array (the supported public-handle-to-render-row join),
// or -1 when no row currently reports that handle.
inline int VoiceRowForHandle(const VnSnapshot& snapshot,
                             VnVoiceHandle publicHandle) {
    for (std::uint32_t i = 0; i < VN_SNAPSHOT_MAX_VOICES; ++i) {
        if (snapshot.voicePublicHandles[i] == publicHandle) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

inline bool FindLogicLog(harken::Engine* engine, std::uint32_t code,
                         harken::LogRecord* out) {
    harken::LogRecord record{};
    while (engine->logicLogRing.TryPop(record)) {
        if (record.code == code) {
            *out = record;
            return true;
        }
    }
    return false;
}

// --- analytic signals ---

// --- null-backend capture ---

struct Capture {
    std::vector<float> left;
    std::vector<float> right;
};

// Pumps `blocks` blocks one at a time, appending each rendered block's
// deinterleaved stereo output.
inline void PumpAndCapture(harken::Engine* engine, std::uint32_t blocks,
                           Capture& out) {
    const std::uint32_t frames = engine->nullBackend.BlockFrames();
    const std::uint32_t channels = engine->nullBackend.Channels();
    for (std::uint32_t b = 0; b < blocks; ++b) {
        engine->nullBackend.Pump(1);
        const float* block = engine->nullBackend.LastBlock();
        for (std::uint32_t i = 0; i < frames; ++i) {
            out.left.push_back(block[i * channels + 0]);
            out.right.push_back(block[i * channels + 1]);
        }
    }
}

// Deinterleaved capture across every device channel (not just stereo), for the
// multichannel and downmix goldens. channels[ch][frame] holds each block's
// output; channelCount is the device layout's channel count.
struct CaptureMulti {
    std::vector<std::vector<float>> channels;
    std::uint32_t channelCount = 0;
};

inline void PumpAndCaptureMulti(harken::Engine* engine, std::uint32_t blocks,
                                CaptureMulti& out) {
    const std::uint32_t frames = engine->nullBackend.BlockFrames();
    const std::uint32_t channels = engine->nullBackend.Channels();
    out.channelCount = channels;
    out.channels.assign(channels, {});
    for (std::uint32_t b = 0; b < blocks; ++b) {
        engine->nullBackend.Pump(1);
        const float* block = engine->nullBackend.LastBlock();
        for (std::uint32_t i = 0; i < frames; ++i) {
            for (std::uint32_t ch = 0; ch < channels; ++ch) {
                out.channels[ch].push_back(block[i * channels + ch]);
            }
        }
    }
}

} // namespace harkentest

#endif // HARKEN_TESTS_TESTSUPPORT_TEST_UTIL_H
