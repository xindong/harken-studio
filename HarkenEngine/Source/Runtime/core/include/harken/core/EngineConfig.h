// Internal engine configuration: the validated, default-resolved form of the
// public VnConfig. All capacity lives here; audio-thread code never consults
// the public struct.
#ifndef HARKEN_CORE_ENGINE_CONFIG_H
#define HARKEN_CORE_ENGINE_CONFIG_H

#include <harken/Harken.h>
#include <harken/abi/VoiceSlotLayout.h>

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace harken {

// Compile-time channel capacity for POD layouts (spec section 4). This is the
// single definition point; the renderer currently uses stereo within this
// capacity.
inline constexpr std::uint32_t kMaxChannels = VN_MAX_CHANNELS;
inline constexpr std::uint32_t kMaxPropertiesPerCurrentSliceDefault = 256;
inline constexpr std::uint32_t kMaxVariableSliceTargetsTotalDefault = 4096;

// The stereo channel count: the default mix/device width and the
// regression-preserving baseline. The mix and device widths are runtime config
// values (EngineConfig.mixChannelCount / deviceChannelCount); this constant is
// the value they default to and the width at which the render path is
// byte-identical to M4.
inline constexpr std::uint32_t kOutputChannels = 2;

// Compile-time upper bound on a voice's insert-effect chain. This is the
// POD-layout constant sizing the per-voice slot array and the ceiling
// ResolveConfig enforces on fxSlotsPerVoice; the active count per voice is the
// resolved config value at or below it. The value itself now lives in the
// engine-free ABI layout contract included above (kMaxVoiceFxSlots), so the
// offline program compiler that lowers a chain onto these slots resolves the same
// ceiling instead of carrying its own literal; this name is kept because the
// runtime's arrays and config validation read it everywhere.
inline constexpr std::uint32_t kMaxFxSlotsPerVoice = kMaxVoiceFxSlots;

// Compile-time upper bound on a voice's send slots (kMaxVoiceSendSlots) lives
// in the engine-free ABI layout contract included above, so the offline program
// compiler resolves the same ceiling this runtime sizes its per-voice send
// array to. It stays in namespace harken, so every consumer here reads it
// unchanged.

// Compile-time upper bound on a voice's binding instance slots (the
// per-voice ceiling of the public binding system's instance layer). This is
// the POD-layout constant sizing the pinned-attachment arrays in pending
// records and the ceiling the resolved voiceBindingInstanceSlots config
// value may not exceed. Single definition point, like kMaxVoiceSendSlots.
inline constexpr std::uint32_t kMaxVoiceBindingInstanceSlots = 8;

// Compile-time upper bound on a bus's insert-effect chain. Bus effect chains
// are pure reuse of the voice chain mechanics and share its slot maximum, so
// this equals kMaxFxSlotsPerVoice by design; it names the POD-layout constant
// sizing the per-bus slot arrays and the ceiling ResolveConfig enforces on
// fxSlotsPerBus. Kept as its own named constant (like kMaxVoiceSendSlots) so a
// per-bus array declares its own intent.
inline constexpr std::uint32_t kMaxFxSlotsPerBus = kMaxFxSlotsPerVoice;

enum class MixRateResolution : std::uint8_t {
    ProductionDefault = 0,
    ExplicitRequest = 1,
    InitialDeviceFallback = 2,
    InitialDeviceObservation = 3
};

struct EngineConfig;

// The Runtime's fixed internal time base. Resolution may use one concrete
// endpoint observation during Vn_Init for the legacy sampleRate == 0 contract,
// but the result is frozen in the Engine's construction-time configuration.
class ResolvedMixRateConfig {
public:
    constexpr ResolvedMixRateConfig() noexcept = default;

    constexpr std::uint32_t SampleRate() const noexcept {
        return m_sampleRate;
    }
    constexpr MixRateResolution Resolution() const noexcept {
        return m_resolution;
    }

private:
    friend VnResult ResolveConfigForInitialDevice(
        const VnConfig* config, std::uint32_t observedChannels,
        std::uint32_t observedSampleRate, EngineConfig* out);

    constexpr ResolvedMixRateConfig(
        std::uint32_t sampleRate,
        MixRateResolution resolution) noexcept
        : m_sampleRate(sampleRate), m_resolution(resolution) {}

    std::uint32_t m_sampleRate = 48000;
    MixRateResolution m_resolution = MixRateResolution::ProductionDefault;
};

// Resolved voice capacities use product-level terms rather than the legacy
// public field names. A Render Voice owns one full DSP slot. A Virtual Voice is
// a started, parked Logical Voice whose timeline remains alive without a render
// slot. A Logical Voice is the stable public identity across Live,
// PendingStart, and Parked states.
class VoiceCapacityModel {
public:
    constexpr VoiceCapacityModel() noexcept = default;

    // Forms the single authoritative relationship L = R + V. False means the
    // derived Logical Voice capacity cannot be represented or out is null.
    static bool TryCreate(std::uint32_t renderVoices,
                          std::uint32_t virtualVoices,
                          VoiceCapacityModel* out) noexcept;

    constexpr std::uint32_t Render() const noexcept {
        return m_renderVoices;
    }
    constexpr std::uint32_t Virtual() const noexcept {
        return m_virtualVoices;
    }
    constexpr std::uint32_t Logical() const noexcept {
        return m_logicalVoices;
    }

private:
    constexpr VoiceCapacityModel(std::uint32_t renderVoices,
                                 std::uint32_t virtualVoices) noexcept
        : m_renderVoices(renderVoices),
          m_virtualVoices(virtualVoices),
          m_logicalVoices(renderVoices + virtualVoices) {}

    std::uint32_t m_renderVoices = 256;
    std::uint32_t m_virtualVoices = 256;
    std::uint32_t m_logicalVoices = 512;
};

struct EngineConfig {
    ResolvedMixRateConfig mixRate;
    std::uint32_t blockFrames = 256;
    VoiceCapacityModel voiceCapacity;
    std::uint32_t fxSlotsPerVoice = 1;
    std::uint32_t arenaBytes = 2u << 20;
    bool nullDevice = true;
    std::uint32_t maxBuses = 32;
    std::uint32_t commandQueueDepth = 256;
    // Most control commands one MixQuantum will apply. The drain used to run
    // until the queue was empty, which tied its cost to the queue depth and to
    // how much a caller had pushed, not to the block deadline it has to meet.
    //
    // Deliberately not derived from commandQueueDepth: a deeper queue is
    // precisely the configuration that needs the ceiling, so scaling with it
    // would exempt the case the ceiling exists for.
    //
    // Commands over the ceiling apply on a later quantum rather than being
    // dropped. That is observable for a ramping setter: a ramp starts from the
    // value in effect at the boundary where it actually applies, so a deferred
    // ramp starts later and from a further-advanced value. The final value is
    // unaffected. At this default no deferral happens at the default queue
    // depth, so default-configured behaviour is unchanged block for block.
    //
    // Default pending capacity qualification: the number is a safe starting
    // ceiling, not a measured one. It is what a zero config field resolves to,
    // like every other capacity here; a caller that sets it gets its own value,
    // bounded by the same validator that bounds the queue depths.
    std::uint32_t maxControlCommandsPerQuantum = 256;
    std::uint32_t notificationQueueDepth = 256;
    VnNotificationCallback onNotification = nullptr;
    void* notificationUserData = nullptr;
    VnLogFn logCallback = nullptr;
    void* logUserData = nullptr;
    std::uint32_t delayEffectPoolCapacity = 8;
    std::uint32_t toneEffectPoolCapacity = 16;
    std::uint32_t gainEffectPoolCapacity = 16;
    std::uint32_t limiterEffectPoolCapacity = 16;
    std::uint32_t compressorEffectPoolCapacity = 16;
    std::uint32_t dynamicRouteCapacity = 64;
    std::uint32_t dynamicRoutePlanPoolCapacity = 4;
    std::uint32_t dynamicBusTapCapacity = 16;
    std::uint32_t dynamicVoiceTapCapacity = 16;
    std::uint32_t maxBusTopologyOwners = 64;
    std::uint64_t maxTopologyPlanBytes = 256ull * 1024ull * 1024ull;
    std::uint64_t topologyPlanPoolBytes = 0;
    std::uint64_t preparedTopologyPlanPoolBytes = 0;
    std::uint32_t streamRingFrames = 16384;
    std::uint32_t maxStreams = 16;
    std::uint32_t prebufferMs = 100;
    std::uint32_t starvationCooldownBlocks = 32;
    std::uint32_t maxPacks = 256;
    // Playback-management capacities. Every field defaults to
    // the value an unset (zero) config field resolves to. The container,
    // emitter, random-seed, voice-stealing, and parked-voice fields are
    // consumed by the engine; the Action/send/topology fields are validated and
    // stored for the features that own them, and their defaults keep behavior
    // identical when left unset.
    std::uint32_t maxActions = 256;
    std::uint32_t maxActionFirings = 256;
    std::uint32_t maxContainers = 64;
    std::uint32_t maxContainerChildren = 1024;
    std::uint32_t containerCursorCapacity = 256;
    std::uint32_t pendingActionCapacity = 256;
    std::uint32_t maxEmitters = 128;
    std::uint32_t emitterVariableSlots = 8;
    std::uint32_t voiceStealPolicy = 0; // 0 = refuse
    std::uint32_t audibilityParkEnabled = 0;
    float parkThresholdDb = -60.0f; // resolved from a zero config field
    std::uint32_t randomSeed = 0;   // 0 resolves to a constant where the PRNG seeds
    std::uint32_t voiceSendSlots = 2;
    std::uint32_t busSendSlotsPerBus = 4;
    std::uint32_t fxSlotsPerBus = 8;
    std::uint32_t topologyPlanPoolCapacity = 4;
    // Maximum nesting level of Action firings within one chain: a direct
    // trigger executes at depth 1, each trigger Step fires one level deeper,
    // and a nested firing beyond the ceiling is dropped at fire time.
    std::uint32_t maxTriggerDepth = 8;
    // Multichannel output (resolved from VnConfig.mixChannelLayout /
    // deviceChannelLayout). The layout fields hold a concrete VnChannelLayout
    // value (never the DEFAULT sentinel); the count fields are the matching
    // channel counts, cached so the audio path reads a plain integer. Defaults
    // are stereo, at which the whole amendment is inert and the render path is
    // byte-identical to M4. deviceChannelLayout defaults to the mix layout (the
    // null backend has no hardware to query; a real backend narrower than the
    // mix triggers the master->device output downmix).
    std::uint32_t mixChannelLayout = VN_CHANNEL_LAYOUT_STEREO;
    std::uint32_t mixChannelCount = 2;
    std::uint32_t deviceChannelLayout = VN_CHANNEL_LAYOUT_STEREO;
    std::uint32_t deviceChannelCount = 2;
    // True when deviceChannelLayout came from VN_CHANNEL_LAYOUT_DEFAULT (the
    // caller asked to "follow the device's native/default layout"). On the null
    // backend that resolves to the mix layout here; on a real backend Vn_Init
    // queries the device's native channel count and overrides
    // deviceChannelLayout/Count before the engine is built, so the
    // master->device downmix triggers on the true device width. A concrete
    // deviceChannelLayout leaves this false and is used verbatim.
    bool deviceLayoutFromDefault = true;
    // Public binding-system pools, resolved from the VnConfig tail
    // appends; zero fields keep the defaults below.
    // voiceBindingInstanceSlots is the
    // per-Render-Voice instance ceiling (the instance pool is Render capacity
    // times this) and may not exceed kMaxVoiceBindingInstanceSlots.
    std::uint32_t maxBindings = 128;
    std::uint32_t maxBindingAttachments = 256;
    std::uint32_t voiceBindingInstanceSlots = 8;
    // Meter transport ring depth in blocks, resolved from the VnConfig
    // field of the same name; zero keeps the default. Rounded up to a
    // power of two like the other
    // rings (mask indexing).
    std::uint32_t meterRingBlocks = 32;
    // Job-parallel render workers: 0 = serial in-callback rendering (default,
    // bit-identical to M4). The worker pool itself is wired in a later segment;
    // the value is validated (sanity ceiling) and stored here now so the ABI
    // surface and observability are complete.
    std::uint32_t renderWorkerCount = 0;
    // Scheduled-transaction horizon capacities (the capacity-ledger group
    // "scheduling horizon & transactions", key names used verbatim). These are
    // internal defaults: no VnConfig field maps to them yet, because adding one
    // would grow the public config image and cost a trace-format major version --
    // so they always resolve from the defaults below. The resolver rounds the
    // horizon up to a whole number of blockFrames; the scheduled-transaction pool
    // keeps the same divisibility check as a defensive Reset invariant. The values
    // are safe starting ceilings, not measured ones, in the same sense as
    // maxControlCommandsPerQuantum.
    std::uint64_t scheduleHorizonFrames = 16384; // rounded up for other block sizes
    std::uint32_t publishLeadQuanta = 4;
    std::uint32_t maxScheduledTransactions = 256;
    std::uint64_t maxTransactionSpanFrames = 8192;
    std::uint32_t maxIngressTransactionsPerQuantum = 128;
    std::uint32_t maxFrameBatchesPerQuantum = 128;
    std::uint32_t maxEventsPerQuantum = 256;
    std::uint32_t maxTransactionEvents = 128;
    std::uint32_t maxPropertiesPerCurrentSlice =
        kMaxPropertiesPerCurrentSliceDefault;
    std::uint32_t maxVariableSliceTargetsTotal =
        kMaxVariableSliceTargetsTotalDefault;
    std::uint32_t maxPlayableInstances = 256;
    std::uint32_t maxKnownScheduleLeaves = 64;
    std::uint32_t maxVariableControlActiveRows = 16;
    std::uint32_t maxVariableControlLiveRows = 32;
    std::uint32_t maxTransitionInstances = 32;
    std::uint32_t maxCompositeCompletionOwners = 256;
    std::uint32_t maxContainerInstances = 256;
    std::uint32_t maxQueueCheckpoints = 256;
    std::uint32_t maxQueueChildren = 64;
    std::uint32_t maxSelectionRowsGlobal = 256;
    std::uint32_t maxSelectionRowsPerEmitter = 16;
    std::uint32_t maxPendingSelectionDeltasTotal = 512;
    std::uint32_t maxPendingSelectionDeltasGlobalPerRow = 512;
    std::uint32_t maxPendingSelectionDeltasEmitterPerRow = 16;
    // How many deferred intents the shared coordinator may materialize per
    // quantum. No public config field maps to this internal
    // default yet. Like maxControlCommandsPerQuantum it is a pacing bound,
    // not a rejection: work beyond the budget stays deferred for a later quantum
    // rather than being dropped.
    std::uint32_t maxDeferredMaterializationsPerQuantum = 32;
    std::uint32_t maxDeferredIntents = 256;
    std::uint32_t maxActionCooldownRows = 256;
    std::uint32_t maxDecisionRows = 512;
    std::uint32_t maxParticipantsPerDecision = 128;
    std::uint32_t maxPlan03GroupHeadersPerPage = 64;
    std::uint32_t maxArmGroups = 256;
    std::uint32_t maxArmsPerGroup = 128;
    std::uint32_t maxCatalogEntries = 65536;
    std::uint64_t maxCatalogBytes = 4ull * 1024ull * 1024ull;
    std::uint64_t maxResidentDefinitionBytes = 32ull * 1024ull * 1024ull;
    std::uint64_t maxCompositeIndexBytes = 2ull * 1024ull * 1024ull;
    std::uint64_t maxPackDecodeStagingBytes = 64ull * 1024ull * 1024ull;
    std::uint64_t maxPackResourceBytes = 0;
    std::uint64_t maxPackReplacementPeakBytes = 96ull * 1024ull * 1024ull;
};

struct PreparedOwnerClaimV1 {
    std::uint32_t inUse = 0;
    std::uint32_t zero = 0;
    std::uint64_t ownerId = 0;
    std::uint64_t baseRevision = 0;
    std::uint64_t revision = 0;
    std::uint64_t digestLow = 0;
    std::uint64_t digestHigh = 0;
    std::uint64_t preparedSerial = 0;
};
static_assert(sizeof(PreparedOwnerClaimV1) == 56,
              "PreparedOwnerClaimV1 is a 56-byte prepared-owner row");
static_assert(std::is_trivially_copyable_v<PreparedOwnerClaimV1>,
              "PreparedOwnerClaimV1 is trivially copyable");

struct ProjectGenerationInstallAckIdentityV1 {
    std::uint32_t valid = 0;
    std::uint32_t zero = 0;
    std::uint64_t slotGeneration = 0;
    std::uint64_t planSequence = 0;
    std::uint64_t planFingerprint = 0;
    std::uint64_t claimSetFingerprint = 0;
};
static_assert(sizeof(ProjectGenerationInstallAckIdentityV1) == 40,
              "ack side-table identity is U32 valid/zero plus four U64");
static_assert(offsetof(ProjectGenerationInstallAckIdentityV1, slotGeneration) == 8,
              "ack identity payload starts after valid/zero");
static_assert(std::is_trivially_copyable_v<ProjectGenerationInstallAckIdentityV1>,
              "ack identity is trivially copyable");

struct RuntimeProjectGenerationTokenV1 {
    std::uint64_t projectSessionId = 0;
    std::uint64_t authoringRevision = 0;
    std::uint64_t runtimeEpoch = 0;
    std::uint64_t submissionSerial = 0;
};
static_assert(sizeof(RuntimeProjectGenerationTokenV1) == 32,
              "paired generation token is four U64");
static_assert(std::is_trivially_copyable_v<RuntimeProjectGenerationTokenV1>,
              "paired generation token is trivially copyable");

inline bool TokenIsPaired(const RuntimeProjectGenerationTokenV1& token) noexcept {
    return token.projectSessionId != 0 && token.runtimeEpoch != 0;
}

struct ProjectGenerationInstallAckV1 {
    std::uint32_t planIndex = 0;
    std::uint32_t zero = 0;
    std::uint64_t slotGeneration = 0;
    std::uint64_t planSequence = 0;
    std::uint64_t planFingerprint = 0;
    std::uint64_t claimSetFingerprint = 0;
};
static_assert(sizeof(ProjectGenerationInstallAckV1) == 40,
              "install ack ring records stay 40 bytes");
static_assert(std::is_trivially_copyable_v<ProjectGenerationInstallAckV1>,
              "install ack ring records are trivially copyable");

// Sanity ceiling on renderWorkerCount (delegated, per the spec's recommended
// 64): above it Vn_Init refuses VN_INVALID_CONFIG.
inline constexpr std::uint32_t kMaxRenderWorkerCount = 64;
inline constexpr std::uint32_t kMaxPlayableInstances = 2u * (1u << 14);
inline constexpr std::uint32_t kMaxKnownScheduleLeaves = 64;
inline constexpr std::uint32_t kMaxCompositeCompletionOwners = 16384;
inline constexpr std::uint32_t kMaxContainerInstances = 16384;
inline constexpr std::uint32_t kMaxQueueCheckpoints = 65536;
inline constexpr std::uint32_t kMaxQueueChildren = 1024;

// Validates the caller-provided config and resolves zeros to defaults.
// Returns VN_OK and fills `out`, or a -1xx code without touching `out`.
VnResult ResolveConfig(const VnConfig* config, EngineConfig* out);

// Resolves the same public config with concrete fields observed before the
// first real-device open. Zero means unknown and preserves the deterministic
// fallback. The full validation and derived-budget path is always rerun, and
// `out` is not touched on failure.
VnResult ResolveConfigForInitialDevice(const VnConfig* config,
                                       std::uint32_t observedChannels,
                                       std::uint32_t observedSampleRate,
                                       EngineConfig* out);

} // namespace harken

#endif // HARKEN_CORE_ENGINE_CONFIG_H
