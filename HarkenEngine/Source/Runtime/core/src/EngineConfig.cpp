#include "harken/core/EngineConfig.h"
#include "TimelineCapacityOverrideForTest.h"
#include "harken/core/ActionCooldownTable.h"
#if defined(HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE)
#include "harken/core/ScheduledTransactionPool.h"
#endif
#include "harken/core/StreamParkToken.h"

// The delay-line preallocation this validator bounds is sized from the
// effect-side maximum delay time; ResolveConfig reads that constant to reject a
// pool footprint the engine would refuse to allocate. Including it here (from a
// .cpp, not the public core header) keeps EngineConfig.h free of the render
// dependency while letting the byte ceiling below track the real line length.
#include "harken/render/BuiltinEffects.h"
#include "harken/render/DynamicRoute.h"
#include "harken/param/VariableService.h"
#include "harken/param/EmitterTable.h"
#include "harken/core/CurrentSelection.h"

#include "harken/core/ChannelLayout.h"
#include "harken/media/StreamFormat.h"

#include <cmath>
#include <cstddef>
#include <limits>

namespace harken {

bool VoiceCapacityModel::TryCreate(std::uint32_t renderVoices,
                                   std::uint32_t virtualVoices,
                                   VoiceCapacityModel* out) noexcept {
    if (out == nullptr ||
        renderVoices >
            std::numeric_limits<std::uint32_t>::max() - virtualVoices) {
        return false;
    }
    *out = VoiceCapacityModel(renderVoices, virtualVoices);
    return true;
}

namespace {

// Sanity ceilings: reject configs that are plainly nonsensical rather than
// letting absurd capacities exhaust memory at init. These are validation
// bounds, not tuning parameters.
constexpr std::uint32_t kMaxSampleRate = 384000;
constexpr std::uint32_t kMaxBlockFrames = 1 << 16;
constexpr std::uint32_t kMaxVoicesCeiling = 1 << 14;
constexpr std::uint32_t kMaxArenaBytes = 1 << 28;
constexpr std::uint32_t kMaxBusesCeiling = 1 << 12;
constexpr std::uint32_t kMaxQueueDepthCeiling = 1 << 20;
// The per-block control-command ceiling is counted in the same unit as the
// queue that feeds it, so the largest queue this validator will admit is also
// the largest per-block allowance that can ever bind: no block can apply more
// commands than the queue is able to hold. Sharing the bound is not the same as
// deriving the value -- the default deliberately does not scale with
// commandQueueDepth, because a deeper queue is exactly the configuration the
// ceiling exists for.
constexpr std::uint32_t kMaxControlCommandsPerQuantumCeiling =
    kMaxQueueDepthCeiling;
// Effect instance pools preallocate all their storage at Init (the delay
// pool's delay lines are large), so this sanity ceiling rejects clearly
// nonsensical capacities before construction attempts the allocation.
constexpr std::uint32_t kMaxEffectPoolCeiling = 1 << 12;
constexpr std::uint32_t kMaxDynamicRouteCapacityCeiling = 1 << 14;
constexpr std::uint32_t kMaxDynamicRoutePlanPoolCeiling = 1 << 12;
constexpr std::uint32_t kMaxDynamicTapCapacityCeiling = 1 << 12;

// Stream capacity ceilings. Ring frames is bounded per ring; the byte
// ceiling below bounds the whole preallocation.
constexpr std::uint32_t kMaxStreamRingFramesCeiling = 1 << 22;
constexpr std::uint32_t kMaxStreamsCeiling = 1 << 12;
// Prebuffer is a start-latency knob, not a capacity; a minute of prebuffer
// is already far past any sane figure, so anything above is a typo.
constexpr std::uint32_t kMaxPrebufferMsCeiling = 60000;
constexpr std::uint32_t kMaxStarvationCooldownCeiling = 1 << 20;
constexpr std::uint32_t kMaxPacksCeiling = 1 << 12;
constexpr std::uint32_t kMaxCatalogEntriesCeiling = 1048576;
constexpr std::uint64_t kMaxCatalogBytesCeiling = 64ull * 1024ull * 1024ull;
constexpr std::uint64_t kMaxResidentDefinitionBytesCeiling =
    256ull * 1024ull * 1024ull;
constexpr std::uint64_t kMaxCompositeIndexBytesCeiling =
    32ull * 1024ull * 1024ull;
constexpr std::uint64_t kMaxPackDecodeStagingBytesCeiling =
    256ull * 1024ull * 1024ull;
constexpr std::uint64_t kMaxPackReplacementPeakBytesCeiling =
    384ull * 1024ull * 1024ull;

// Playback-management sanity ceilings, same discipline as above: reject
// plainly nonsensical capacities before construction attempts an allocation.
// These bound logic-side pools; none of them approach the delay-line or ring
// footprints that need a byte-level ceiling.
constexpr std::uint32_t kMaxActionsCeiling = 1 << 16;
constexpr std::uint32_t kMaxActionFiringsCeiling = 4096;
constexpr std::uint32_t kMaxContainersCeiling = 1 << 16;
constexpr std::uint32_t kMaxContainerChildrenCeiling = 1 << 20;
constexpr std::uint32_t kMaxContainerCursorCeiling = 1 << 20;
constexpr std::uint32_t kMaxPendingActionCeiling = 1 << 20;
constexpr std::uint32_t kMaxEmittersCeiling = 1 << 16;
constexpr std::uint32_t kMaxEmitterVariableSlotsCeiling = 1 << 12;
constexpr std::uint32_t kMaxParkedVoicesCeiling = 1 << 14;
constexpr std::uint32_t kMaxBusSendSlotsCeiling = 1 << 8;
constexpr std::uint32_t kMaxTopologyPlanPoolCeiling = 1 << 12;
constexpr std::uint32_t kMaxBusTopologyOwnersCeiling = 1 << 16;
constexpr std::uint64_t kMaxTopologyPlanBytesCeiling =
    1ull * 1024ull * 1024ull * 1024ull;
// The steal policy is a small enum; anything above steal-oldest is a header
// vintage this engine does not know.
constexpr std::uint32_t kMaxStealPolicy = 2;

// Binding-pool sanity ceilings, same discipline as the playback-management
// ceilings: logic-side pools, preallocated at Init, refused when plainly
// nonsensical. The per-voice instance-slot field is bounded by its
// POD-layout constant (kMaxVoiceBindingInstanceSlots) instead, below.
constexpr std::uint32_t kMaxBindingsCeiling = 1 << 16;
constexpr std::uint32_t kMaxBindingAttachmentsCeiling = 1 << 16;

// Meter transport ring depth ceiling. 1024 blocks is already several
// seconds of un-drained peaks at the default block size — far past any
// sane pump gap — so anything above is a typo, not a tuning.
constexpr std::uint32_t kMaxMeterRingBlocksCeiling = 1 << 10;

// Meter transport ring depth floor. The compressed overflow channel
// needs ordinary-record headroom for its validated take: a near-zero
// ring would overflow on almost every block and live permanently inside
// the bounded degradation window meant for stalled pumps. Eight blocks
// (~43 ms at the 256/48k default) keeps overflow an exceptional state.
constexpr std::uint32_t kMinMeterRingBlocksFloor = 8;

// Trigger-depth ceiling. Unlike the capacity ceilings above this bounds a
// per-chain nesting level, not a pool, and the Action-extension contract pins
// its over-ceiling refusal to VN_INVALID_ARG (checked apart from the
// VN_INVALID_CONFIG capacity chain below).
constexpr std::uint32_t kMaxTriggerDepthCeiling = 64;

// Scheduled-transaction horizon ceilings, same discipline as the other capacity
// ceilings: reject a plainly nonsensical size before the pool tries to allocate
// it. These bound the logic-side scheduled-transaction pool; none approach a
// byte-level footprint that needs the aggregate check the delay/stream pools use.
constexpr std::uint64_t kMaxScheduleHorizonFramesCeiling = 1ull << 24;
constexpr std::uint32_t kMaxPublishLeadQuantaCeiling = 1 << 16;
constexpr std::uint32_t kMaxScheduledTransactionsCeiling = 1 << 16;
constexpr std::uint64_t kMaxTransactionSpanFramesCeiling = 1ull << 24;
constexpr std::uint32_t kMaxIngressTransactionsPerQuantumCeiling = 1 << 16;
constexpr std::uint32_t kMaxFrameBatchesPerQuantumCeiling = 1 << 16;
constexpr std::uint32_t kMaxEventsPerQuantumCeiling = 1 << 20;
constexpr std::uint32_t kMaxTransactionEventsCeiling = 1 << 16;
constexpr std::uint32_t kMaxDeferredMaterializationsPerQuantumCeiling = 1 << 16;

// Byte ceiling for the feedback-delay pool's total preallocated line storage.
// Each pooled delay instance holds a full kDelayEffectMaxSeconds line across
// kMaxChannels at the engine sample rate, so at high rates and large pools this
// is by far the biggest single Init allocation. The instance-count ceiling
// alone does not bound it: many instances at a very high rate could still ask
// for tens of gigabytes and throw bad_alloc out of Vn_Init. 256 MiB caps the
// worst case at a small fraction of a typical game's audio budget while still
// admitting generous pools at normal rates (over 160 one-second lines at
// 48 kHz across kMaxChannels).
constexpr std::uint64_t kMaxDelayLineBytes = 256ull * 1024ull * 1024ull;

// Byte ceiling for total streaming memory governed by the stream slot count:
// every slot owns a preallocated ring and can grow one whole-chunk staging
// buffer when active. The two count ceilings alone would still admit a
// multi-gigabyte request.
constexpr std::uint64_t kMaxStreamMemoryBytes = 256ull * 1024ull * 1024ull;
constexpr std::uint64_t kMaxDynamicRouteMemoryBytes =
    256ull * 1024ull * 1024ull;

bool CheckedMul(std::uint64_t a, std::uint64_t b, std::uint64_t* out) {
    if (out == nullptr ||
        (a != 0 &&
         b > (std::numeric_limits<std::uint64_t>::max)() / a)) {
        return false;
    }
    *out = a * b;
    return true;
}

bool CheckedAdd(std::uint64_t a, std::uint64_t b, std::uint64_t* out) {
    if (out == nullptr ||
        b > (std::numeric_limits<std::uint64_t>::max)() - a) {
        return false;
    }
    *out = a + b;
    return true;
}

// Rings index with a power-of-two mask, so capacity rounds up before the
// byte ceiling is applied — the ceiling must see what will actually be
// allocated, not the raw request.
std::uint64_t RoundUpPow2(std::uint64_t v) {
    std::uint64_t p = 1;
    while (p < v) {
        p <<= 1;
    }
    return p;
}

} // namespace

VnResult ResolveConfigForInitialDevice(const VnConfig* config,
                                       std::uint32_t observedChannels,
                                       std::uint32_t observedSampleRate,
                                       EngineConfig* out) {
    if (config == nullptr || out == nullptr) {
        return VN_INVALID_ARG;
    }
    // structSize versions the struct. Thirteen vintages exist, each ending at the
    // offset of the next vintage's first appended field: the original struct
    // (ends before the callback fields), the transport vintage (ends before
    // the log fields), the log vintage (ends before the effect-pool fields),
    // the effect-pool vintage (ends before the stream fields), the stream
    // vintage (ends before the playback-management fields), the
    // playback-management vintage (ends before the trigger-depth field), the
    // trigger-depth vintage (ends before the multichannel fields), the
    // multichannel vintage (ends before the binding-pool fields), the
    // binding-pool vintage (ends before the gain-pool field), the gain-pool
    // vintage (ends before the limiter-pool field), the limiter-pool vintage
    // (ends before the Compressor/dynamic fields), the Compressor/dynamic
    // vintage (ends before the Bus-topology snapshot fields), and the current
    // full struct.
    // Appended fields of an older caller read as engine defaults.
    const std::uint32_t vintage1Size =
        static_cast<std::uint32_t>(offsetof(VnConfig, onNotification));
    const std::uint32_t vintage2Size =
        static_cast<std::uint32_t>(offsetof(VnConfig, logCallback));
    const std::uint32_t vintage3Size =
        static_cast<std::uint32_t>(offsetof(VnConfig, delayEffectPoolCapacity));
    const std::uint32_t vintage4Size =
        static_cast<std::uint32_t>(offsetof(VnConfig, streamRingFrames));
    const std::uint32_t vintage5Size =
        static_cast<std::uint32_t>(offsetof(VnConfig, maxActions));
    const std::uint32_t vintage6Size =
        static_cast<std::uint32_t>(offsetof(VnConfig, maxTriggerDepth));
    const std::uint32_t vintage7Size =
        static_cast<std::uint32_t>(offsetof(VnConfig, mixChannelLayout));
    const std::uint32_t vintage8Size =
        static_cast<std::uint32_t>(offsetof(VnConfig, maxBindings));
    const std::uint32_t vintage9Size =
        static_cast<std::uint32_t>(offsetof(VnConfig, gainEffectPoolCapacity));
    const std::uint32_t vintage10Size =
        static_cast<std::uint32_t>(offsetof(VnConfig, limiterEffectPoolCapacity));
    const std::uint32_t vintage11Size =
        static_cast<std::uint32_t>(offsetof(VnConfig, compressorEffectPoolCapacity));
    const std::uint32_t vintage12Size =
        static_cast<std::uint32_t>(offsetof(VnConfig, maxBusTopologyOwners));
    // Past the end of the newest vintage's last field rather than sizeof: a
    // gate written against sizeof stops recognising its own fields the moment
    // anything is appended after them, which for an input struct means
    // silently ignoring capacities the caller did set.
    const std::uint32_t vintage13Size = static_cast<std::uint32_t>(
        offsetof(VnConfig, maxTopologyPlanBytes) +
        sizeof(VnConfig::maxTopologyPlanBytes));
    const std::uint32_t vintage14Size = static_cast<std::uint32_t>(
        offsetof(VnConfig, reserved10) + sizeof(VnConfig::reserved10));
    const std::uint32_t vintage15Size = static_cast<std::uint32_t>(
        offsetof(VnConfig, maxPackReplacementPeakBytes) +
        sizeof(VnConfig::maxPackReplacementPeakBytes));
    const std::uint32_t vintage16Size = static_cast<std::uint32_t>(
        offsetof(VnConfig, reserved12) + sizeof(VnConfig::reserved12));
    if (config->structSize != sizeof(VnConfig) &&
        config->structSize != vintage1Size &&
        config->structSize != vintage2Size &&
        config->structSize != vintage3Size &&
        config->structSize != vintage4Size &&
        config->structSize != vintage5Size &&
        config->structSize != vintage6Size &&
        config->structSize != vintage7Size &&
        config->structSize != vintage8Size &&
        config->structSize != vintage9Size &&
        config->structSize != vintage10Size &&
        config->structSize != vintage11Size &&
        config->structSize != vintage12Size &&
        config->structSize != vintage13Size &&
        config->structSize != vintage14Size &&
        config->structSize != vintage15Size &&
        config->structSize != vintage16Size) {
        return VN_INVALID_ARG;
    }
    const bool hasTransportFields = (config->structSize >= vintage2Size);
    const bool hasLogFields = (config->structSize >= vintage3Size);
    const bool hasEffectPoolFields = (config->structSize >= vintage4Size);
    const bool hasStreamFields = (config->structSize >= vintage5Size);
    const bool hasPlaybackFields = (config->structSize >= vintage6Size);
    const bool hasTriggerFields = (config->structSize >= vintage7Size);
    const bool hasMultichannelFields = (config->structSize >= vintage8Size);
    const bool hasBindingFields = (config->structSize >= vintage9Size);
    const bool hasGainField = (config->structSize >= vintage10Size);
    const bool hasLimiterField = (config->structSize >= vintage11Size);
    const bool hasCompressorAndDynamicFields =
        (config->structSize >= vintage12Size);
    const bool hasTopologySnapshotFields =
        (config->structSize >= vintage13Size);
    const bool hasActionFiringFields = (config->structSize >= vintage14Size);
    const bool hasPackBudgetFields = (config->structSize >= vintage15Size);
    const bool hasActionCooldownFields =
        (config->structSize >= vintage16Size);
    if (hasActionFiringFields &&
        (config->pendingActionCapacity != 0 || config->reserved10 != 0)) {
        return VN_INVALID_ARG;
    }
    if (hasPackBudgetFields && config->reserved11 != 0) {
        return VN_INVALID_ARG;
    }
    if (hasActionCooldownFields && config->reserved12 != 0) {
        return VN_INVALID_ARG;
    }
    if (hasCompressorAndDynamicFields && config->reserved9 != 0) {
        return VN_INVALID_ARG;
    }
    if (config->deviceMode != VN_DEVICE_REAL &&
        config->deviceMode != VN_DEVICE_NULL) {
        return VN_INVALID_CONFIG;
    }

    EngineConfig resolved;
    std::uint32_t renderVoiceCapacity = resolved.voiceCapacity.Render();
    std::uint32_t virtualVoiceCapacity = resolved.voiceCapacity.Virtual();
    resolved.nullDevice = (config->deviceMode == VN_DEVICE_NULL);
    if (config->sampleRate != 0) {
        resolved.mixRate = ResolvedMixRateConfig(
            config->sampleRate, MixRateResolution::ExplicitRequest);
    } else if (!resolved.nullDevice && observedSampleRate != 0) {
        resolved.mixRate = ResolvedMixRateConfig(
            observedSampleRate,
            MixRateResolution::InitialDeviceObservation);
    } else {
        resolved.mixRate = ResolvedMixRateConfig(
            48000,
            resolved.nullDevice
                ? MixRateResolution::ProductionDefault
                : MixRateResolution::InitialDeviceFallback);
    }
    if (config->blockFrames != 0) resolved.blockFrames = config->blockFrames;
    if (config->maxVoices != 0)
        renderVoiceCapacity = config->maxVoices;
    if (config->fxSlotsPerVoice != 0) resolved.fxSlotsPerVoice = config->fxSlotsPerVoice;
    if (config->arenaBytes != 0) resolved.arenaBytes = config->arenaBytes;
    if (hasTransportFields) {
        if (config->maxBuses != 0) resolved.maxBuses = config->maxBuses;
        if (config->commandQueueDepth != 0)
            resolved.commandQueueDepth = config->commandQueueDepth;
        if (config->notificationQueueDepth != 0)
            resolved.notificationQueueDepth = config->notificationQueueDepth;
        resolved.onNotification = config->onNotification;
        resolved.notificationUserData = config->notificationUserData;
    }
    if (hasLogFields) {
        resolved.logCallback = config->logCallback;
        resolved.logUserData = config->logUserData;
    }
    if (hasEffectPoolFields) {
        if (config->delayEffectPoolCapacity != 0)
            resolved.delayEffectPoolCapacity = config->delayEffectPoolCapacity;
        if (config->toneEffectPoolCapacity != 0)
            resolved.toneEffectPoolCapacity = config->toneEffectPoolCapacity;
    }
    if (hasStreamFields) {
        if (config->streamRingFrames != 0)
            resolved.streamRingFrames = config->streamRingFrames;
        if (config->maxStreams != 0) resolved.maxStreams = config->maxStreams;
        if (config->prebufferMs != 0) resolved.prebufferMs = config->prebufferMs;
        if (config->starvationCooldownBlocks != 0)
            resolved.starvationCooldownBlocks =
                config->starvationCooldownBlocks;
        if (config->maxPacks != 0)
            resolved.maxPacks = config->maxPacks;
    }
    if (hasPlaybackFields) {
        if (config->maxActions != 0) resolved.maxActions = config->maxActions;
        if (config->maxContainers != 0)
            resolved.maxContainers = config->maxContainers;
        if (config->maxContainerChildren != 0)
            resolved.maxContainerChildren = config->maxContainerChildren;
        if (config->containerCursorCapacity != 0)
            resolved.containerCursorCapacity = config->containerCursorCapacity;
        if (config->pendingActionCapacity != 0)
            resolved.pendingActionCapacity = config->pendingActionCapacity;
        if (config->maxEmitters != 0)
            resolved.maxEmitters = config->maxEmitters;
        if (config->emitterVariableSlots != 0)
            resolved.emitterVariableSlots = config->emitterVariableSlots;
        if (config->maxParkedVoices != 0)
            virtualVoiceCapacity = config->maxParkedVoices;
        // voiceStealPolicy 0 is a real value (refuse), so it is copied
        // verbatim rather than resolved from zero; range is checked below.
        resolved.voiceStealPolicy = config->voiceStealPolicy;
        resolved.audibilityParkEnabled = config->audibilityParkEnabled;
        // parkThresholdDb 0 selects the -60 dB default; any nonzero finite
        // value is used as given (finiteness is checked below).
        if (config->parkThresholdDb != 0.0f)
            resolved.parkThresholdDb = config->parkThresholdDb;
        // randomSeed 0 is the documented default constant, handled where the
        // PRNG seeds; the raw value is carried through.
        resolved.randomSeed = config->randomSeed;
        if (config->voiceSendSlots != 0)
            resolved.voiceSendSlots = config->voiceSendSlots;
        if (config->busSendSlotsPerBus != 0)
            resolved.busSendSlotsPerBus = config->busSendSlotsPerBus;
        if (config->fxSlotsPerBus != 0)
            resolved.fxSlotsPerBus = config->fxSlotsPerBus;
        if (config->topologyPlanPoolCapacity != 0)
            resolved.topologyPlanPoolCapacity =
                config->topologyPlanPoolCapacity;
    }
    if (hasTriggerFields) {
        if (config->maxTriggerDepth != 0)
            resolved.maxTriggerDepth = config->maxTriggerDepth;
    }
    if (hasMultichannelFields) {
        // Both layout fields resolve VN_CHANNEL_LAYOUT_DEFAULT (0) to a
        // documented default: the mix layout to stereo (the regression-
        // preserving default, at which the whole amendment is inert), and the
        // device layout to the resolved mix layout (the null backend has no
        // hardware to query; a real backend narrower than the mix triggers the
        // master->device downmix). A concrete value is used as given.
        if (config->mixChannelLayout != VN_CHANNEL_LAYOUT_DEFAULT) {
            resolved.mixChannelLayout = config->mixChannelLayout;
        }
        if (config->deviceChannelLayout != VN_CHANNEL_LAYOUT_DEFAULT) {
            resolved.deviceChannelLayout = config->deviceChannelLayout;
            resolved.deviceLayoutFromDefault = false;
        } else if (!resolved.nullDevice && observedChannels != 0) {
            resolved.deviceChannelLayout =
                DeviceLayoutFromNativeChannels(observedChannels);
            resolved.deviceLayoutFromDefault = true;
        } else {
            // Follow the mix layout for the null backend and for a real-device
            // query that reports no concrete channel count.
            resolved.deviceChannelLayout = resolved.mixChannelLayout;
            resolved.deviceLayoutFromDefault = true;
        }
        resolved.renderWorkerCount = config->renderWorkerCount;
    } else {
        // A caller whose struct predates the multichannel fields specified no
        // device layout: DEFAULT semantics apply (follow mix on null, native on
        // real).
        if (!resolved.nullDevice && observedChannels != 0) {
            resolved.deviceChannelLayout =
                DeviceLayoutFromNativeChannels(observedChannels);
        }
        resolved.deviceLayoutFromDefault = true;
    }
    if (hasBindingFields) {
        if (config->maxBindings != 0)
            resolved.maxBindings = config->maxBindings;
        if (config->maxBindingAttachments != 0)
            resolved.maxBindingAttachments = config->maxBindingAttachments;
        if (config->voiceBindingInstanceSlots != 0)
            resolved.voiceBindingInstanceSlots =
                config->voiceBindingInstanceSlots;
        if (config->meterRingBlocks != 0)
            resolved.meterRingBlocks = config->meterRingBlocks;
    }
    if (hasGainField) {
        if (config->gainEffectPoolCapacity != 0)
            resolved.gainEffectPoolCapacity = config->gainEffectPoolCapacity;
    }
    if (hasLimiterField) {
        if (config->limiterEffectPoolCapacity != 0)
            resolved.limiterEffectPoolCapacity =
                config->limiterEffectPoolCapacity;
    }
    if (hasCompressorAndDynamicFields) {
        if (config->compressorEffectPoolCapacity != 0)
            resolved.compressorEffectPoolCapacity =
                config->compressorEffectPoolCapacity;
        if (config->dynamicRouteCapacity != 0)
            resolved.dynamicRouteCapacity = config->dynamicRouteCapacity;
        if (config->dynamicRoutePlanPoolCapacity != 0)
            resolved.dynamicRoutePlanPoolCapacity =
                config->dynamicRoutePlanPoolCapacity;
        if (config->dynamicBusTapCapacity != 0)
            resolved.dynamicBusTapCapacity = config->dynamicBusTapCapacity;
        if (config->dynamicVoiceTapCapacity != 0)
            resolved.dynamicVoiceTapCapacity = config->dynamicVoiceTapCapacity;
    }
    if (hasTopologySnapshotFields) {
        if (config->maxBusTopologyOwners != 0) {
            resolved.maxBusTopologyOwners = config->maxBusTopologyOwners;
        }
        if (config->maxTopologyPlanBytes != 0) {
            resolved.maxTopologyPlanBytes = config->maxTopologyPlanBytes;
        }
        if (config->maxControlCommandsPerQuantum != 0) {
            resolved.maxControlCommandsPerQuantum =
                config->maxControlCommandsPerQuantum;
        }
    }
    if (hasActionFiringFields && config->maxActionFirings != 0) {
        resolved.maxActionFirings = config->maxActionFirings;
    }
    if (hasPackBudgetFields) {
        if (config->maxCatalogEntries != 0) {
            resolved.maxCatalogEntries = config->maxCatalogEntries;
        }
        if (config->maxCatalogBytes != 0) {
            resolved.maxCatalogBytes = config->maxCatalogBytes;
        }
        if (config->maxResidentDefinitionBytes != 0) {
            resolved.maxResidentDefinitionBytes =
                config->maxResidentDefinitionBytes;
        }
        if (config->maxCompositeIndexBytes != 0) {
            resolved.maxCompositeIndexBytes = config->maxCompositeIndexBytes;
        }
        if (config->maxPackDecodeStagingBytes != 0) {
            resolved.maxPackDecodeStagingBytes =
                config->maxPackDecodeStagingBytes;
        }
        resolved.maxPackResourceBytes = config->maxPackResourceBytes;
        if (config->maxPackReplacementPeakBytes != 0) {
            resolved.maxPackReplacementPeakBytes =
                config->maxPackReplacementPeakBytes;
        }
    }
    if (hasActionCooldownFields && config->maxActionCooldownRows != 0) {
        resolved.maxActionCooldownRows = config->maxActionCooldownRows;
    }

#if defined(HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE)
    detail::TimelineCapacityOverrideForTest testCapacity{};
    const auto testCapacityRead =
        detail::ReadTimelineCapacityOverrideForTest(&testCapacity);
    if (testCapacityRead ==
        detail::TimelineCapacityOverrideReadForTest::OtherThread) {
        return VN_INVALID_CONFIG;
    }
    if (testCapacityRead ==
        detail::TimelineCapacityOverrideReadForTest::CurrentThread) {
        if (!resolved.nullDevice || testCapacity.events == 0 ||
            testCapacity.events > resolved.maxEventsPerQuantum ||
            testCapacity.groups == 0 ||
            testCapacity.groups > resolved.maxPlan03GroupHeadersPerPage ||
            testCapacity.groups > kMaxPlan03GroupHeadersPerPageHard ||
            testCapacity.frameBatches == 0 ||
            testCapacity.frameBatches > resolved.maxFrameBatchesPerQuantum ||
            testCapacity.frameBatches > testCapacity.events) {
            return VN_INVALID_CONFIG;
        }
        resolved.maxEventsPerQuantum = testCapacity.events;
        resolved.maxPlan03GroupHeadersPerPage = testCapacity.groups;
        resolved.maxFrameBatchesPerQuantum = testCapacity.frameBatches;
    }
#endif

    // Trigger-depth ceiling: refused VN_INVALID_ARG (the public contract for
    // this field), checked apart from the capacity chain below.
    if (resolved.maxTriggerDepth > kMaxTriggerDepthCeiling) {
        return VN_INVALID_ARG;
    }

    // Channel layouts: a non-concrete resolved layout (an out-of-range enum
    // value the build cannot honor) is VN_UNSUPPORTED_LAYOUT, apart from the
    // capacity chain. After this both layouts are concrete, so the channel
    // counts are well-defined. The device layout may be wider, equal, or
    // narrower than the mix — only narrower triggers the output downmix.
    if (!IsConcreteLayout(resolved.mixChannelLayout) ||
        !IsConcreteLayout(resolved.deviceChannelLayout)) {
        return VN_UNSUPPORTED_LAYOUT;
    }
    resolved.mixChannelCount = LayoutChannelCount(resolved.mixChannelLayout);
    resolved.deviceChannelCount =
        LayoutChannelCount(resolved.deviceChannelLayout);

    // Render-worker sanity ceiling (delegated): above it the config is
    // refused, matching the other capacity ceilings' VN_INVALID_CONFIG.
    if (resolved.renderWorkerCount > kMaxRenderWorkerCount) {
        return VN_INVALID_CONFIG;
    }

    if (!VoiceCapacityModel::TryCreate(
            renderVoiceCapacity, virtualVoiceCapacity,
            &resolved.voiceCapacity)) {
        return VN_INVALID_CONFIG;
    }

    // fxSlotsPerVoice is bounded by the POD-layout constant that sizes the
    // per-voice slot array (kMaxFxSlotsPerVoice), not an arbitrary sanity
    // ceiling: a request past the array capacity cannot be honored, so it is
    // refused here on the same over-ceiling path as every other field.
    if (resolved.mixRate.SampleRate() > kMaxSampleRate ||
        resolved.blockFrames > kMaxBlockFrames ||
        resolved.voiceCapacity.Render() > kMaxVoicesCeiling ||
        resolved.fxSlotsPerVoice > kMaxFxSlotsPerVoice ||
        resolved.arenaBytes > kMaxArenaBytes ||
        resolved.maxBuses > kMaxBusesCeiling ||
        resolved.commandQueueDepth > kMaxQueueDepthCeiling ||
        resolved.maxControlCommandsPerQuantum >
            kMaxControlCommandsPerQuantumCeiling ||
        resolved.notificationQueueDepth > kMaxQueueDepthCeiling ||
        resolved.delayEffectPoolCapacity > kMaxEffectPoolCeiling ||
        resolved.toneEffectPoolCapacity > kMaxEffectPoolCeiling ||
        resolved.gainEffectPoolCapacity > kMaxEffectPoolCeiling ||
        resolved.limiterEffectPoolCapacity > kMaxEffectPoolCeiling ||
        resolved.compressorEffectPoolCapacity > kMaxEffectPoolCeiling ||
        resolved.dynamicRouteCapacity > kMaxDynamicRouteCapacityCeiling ||
        resolved.dynamicRoutePlanPoolCapacity >
            kMaxDynamicRoutePlanPoolCeiling ||
        resolved.dynamicRoutePlanPoolCapacity < 2 ||
        resolved.dynamicBusTapCapacity > kMaxDynamicTapCapacityCeiling ||
        resolved.dynamicVoiceTapCapacity > kMaxDynamicTapCapacityCeiling ||
        resolved.streamRingFrames > kMaxStreamRingFramesCeiling ||
        resolved.maxStreams > kMaxStreamsCeiling ||
        resolved.prebufferMs > kMaxPrebufferMsCeiling ||
        resolved.starvationCooldownBlocks > kMaxStarvationCooldownCeiling ||
        resolved.maxPacks > kMaxPacksCeiling ||
        resolved.maxCatalogEntries > kMaxCatalogEntriesCeiling ||
        resolved.maxCatalogBytes > kMaxCatalogBytesCeiling ||
        resolved.maxResidentDefinitionBytes >
            kMaxResidentDefinitionBytesCeiling ||
        resolved.maxCompositeIndexBytes > kMaxCompositeIndexBytesCeiling ||
        resolved.maxPackDecodeStagingBytes >
            kMaxPackDecodeStagingBytesCeiling ||
        resolved.maxPackReplacementPeakBytes >
            kMaxPackReplacementPeakBytesCeiling ||
        resolved.maxActions > kMaxActionsCeiling ||
        resolved.maxActionFirings > kMaxActionFiringsCeiling ||
        resolved.maxActionCooldownRows > kMaxActionCooldownRowsHard ||
        resolved.maxContainers > kMaxContainersCeiling ||
        resolved.maxContainerChildren > kMaxContainerChildrenCeiling ||
        resolved.containerCursorCapacity > kMaxContainerCursorCeiling ||
        resolved.pendingActionCapacity > kMaxPendingActionCeiling ||
        resolved.maxEmitters > kMaxEmittersCeiling ||
        resolved.emitterVariableSlots > kMaxEmitterVariableSlotsCeiling ||
        resolved.voiceCapacity.Virtual() > kMaxParkedVoicesCeiling ||
        resolved.voiceStealPolicy > kMaxStealPolicy ||
        resolved.audibilityParkEnabled > 1u ||
        !std::isfinite(resolved.parkThresholdDb) ||
        // voiceSendSlots is bounded by the POD-layout constant that sizes the
        // per-voice send array (kMaxVoiceSendSlots), like fxSlotsPerVoice.
        resolved.voiceSendSlots > kMaxVoiceSendSlots ||
        resolved.busSendSlotsPerBus > kMaxBusSendSlotsCeiling ||
        // Bus effect chains share the compile-time slot maximum with voices.
        resolved.fxSlotsPerBus > kMaxFxSlotsPerVoice ||
        resolved.topologyPlanPoolCapacity > kMaxTopologyPlanPoolCeiling ||
        resolved.maxBusTopologyOwners > kMaxBusTopologyOwnersCeiling ||
        resolved.maxTopologyPlanBytes > kMaxTopologyPlanBytesCeiling ||
        resolved.maxBindings > kMaxBindingsCeiling ||
        resolved.maxBindingAttachments > kMaxBindingAttachmentsCeiling ||
        // voiceBindingInstanceSlots is bounded by the POD-layout constant
        // that sizes the pinned-attachment arrays, like fxSlotsPerVoice.
        resolved.voiceBindingInstanceSlots > kMaxVoiceBindingInstanceSlots ||
        resolved.meterRingBlocks > kMaxMeterRingBlocksCeiling) {
        return VN_INVALID_CONFIG;
    }

    std::uint64_t overrideBytesPerEmitter = 0;
    std::uint64_t bytesPerEmitter = 0;
    std::uint64_t emitterVariableBankBytes = 0;
    if (!CheckedMul(resolved.emitterVariableSlots,
                    kEmitterOverrideBudgetBytes,
                    &overrideBytesPerEmitter) ||
        !CheckedAdd(kEmitterBankBudgetBytes, overrideBytesPerEmitter,
                    &bytesPerEmitter) ||
        !CheckedMul(resolved.maxEmitters, bytesPerEmitter,
                    &emitterVariableBankBytes) ||
        emitterVariableBankBytes > kMaxEmitterVariableBankBytes) {
        return VN_INVALID_CONFIG;
    }

    SelectionMemoryConfigV1 defaultSelectionConfig{};
    defaultSelectionConfig.maxEmitterRowsPerEmitter = resolved.maxSelectionRowsPerEmitter;
    std::uint64_t defaultContextBytes = 0;
    std::uint64_t parkedStateBytes = 0;
    if (!CheckedMul(resolved.voiceCapacity.Logical(), sizeof(VnEmitterHandle),
                    &parkedStateBytes) ||
        !CheckedAdd(bytesPerEmitter,
                    EmitterTable::StorageBytesPerEmitter(resolved.emitterVariableSlots),
                    &defaultContextBytes) ||
        !CheckedAdd(defaultContextBytes,
                    SelectionMemoryTableV1::EmitterStorageBytes(defaultSelectionConfig),
                    &defaultContextBytes) ||
        !CheckedAdd(defaultContextBytes, sizeof(std::uint32_t), &defaultContextBytes) ||
        !CheckedAdd(defaultContextBytes, parkedStateBytes, &defaultContextBytes) ||
        !CheckedAdd(defaultContextBytes,
                    64ull * (4ull + 2ull * resolved.maxSelectionRowsPerEmitter),
                    &defaultContextBytes) || defaultContextBytes > (16ull << 20)) {
        return VN_INVALID_CONFIG;
    }

    // Scheduled-transaction horizon capacities: over-ceiling or zero is refused
    // on the same VN_INVALID_CONFIG path as the other capacity fields. These are
    // internal defaults with no VnConfig field, so this guards the declared bounds
    // and any future caller mapping rather than a value a caller can currently
    // set. A batch ceiling above the event ceiling is a mistake: a
    // page cannot hold more distinct-frame batches than it holds events.
    if (resolved.scheduleHorizonFrames == 0 ||
        resolved.scheduleHorizonFrames > kMaxScheduleHorizonFramesCeiling ||
        resolved.publishLeadQuanta > kMaxPublishLeadQuantaCeiling ||
        resolved.maxScheduledTransactions == 0 ||
        resolved.maxScheduledTransactions > kMaxScheduledTransactionsCeiling ||
        resolved.maxTransactionSpanFrames == 0 ||
        resolved.maxTransactionSpanFrames > kMaxTransactionSpanFramesCeiling ||
        resolved.maxIngressTransactionsPerQuantum == 0 ||
        resolved.maxIngressTransactionsPerQuantum >
            kMaxIngressTransactionsPerQuantumCeiling ||
        resolved.maxFrameBatchesPerQuantum == 0 ||
        resolved.maxFrameBatchesPerQuantum > kMaxFrameBatchesPerQuantumCeiling ||
        resolved.maxEventsPerQuantum == 0 ||
        resolved.maxEventsPerQuantum > kMaxEventsPerQuantumCeiling ||
        resolved.maxFrameBatchesPerQuantum > resolved.maxEventsPerQuantum ||
        resolved.maxTransactionEvents == 0 ||
        resolved.maxTransactionEvents > kMaxTransactionEventsCeiling ||
        resolved.maxDeferredMaterializationsPerQuantum == 0 ||
        resolved.maxDeferredMaterializationsPerQuantum >
            kMaxDeferredMaterializationsPerQuantumCeiling) {
        return VN_INVALID_CONFIG;
    }

    const std::uint64_t horizonRemainder =
        resolved.scheduleHorizonFrames % resolved.blockFrames;
    if (horizonRemainder != 0) {
        const std::uint64_t padding =
            resolved.blockFrames - horizonRemainder;
        if (resolved.scheduleHorizonFrames >
            kMaxScheduleHorizonFramesCeiling - padding) {
            return VN_INVALID_CONFIG;
        }
        resolved.scheduleHorizonFrames += padding;
    }
    if (resolved.scheduleHorizonFrames >
        kMaxScheduleHorizonFramesCeiling) {
        return VN_INVALID_CONFIG;
    }

    // Floor first (a zero request already took the default above): asks
    // below eight blocks resolve to eight, then rings index with a
    // power-of-two mask, so the rounded capacity resolves here and
    // config observability and the constructed ring agree.
    if (resolved.meterRingBlocks < kMinMeterRingBlocksFloor) {
        resolved.meterRingBlocks = kMinMeterRingBlocksFloor;
    }
    resolved.meterRingBlocks = static_cast<std::uint32_t>(
        RoundUpPow2(resolved.meterRingBlocks));

    // TopologyPlanPool owns several capacity-shaped arrays. Account for the
    // exact aggregate before construction so no legal-looking count set can
    // overflow size_t arithmetic or bypass the caller's byte budget.
    std::uint64_t planBus = 0;
    std::uint64_t planBusSend = 0;
    std::uint64_t planBusCoefficients = 0;
    std::uint64_t bytes = 0;
    const auto addBytes = [&bytes](std::uint64_t count,
                                   std::uint64_t elementBytes) {
        std::uint64_t part = 0;
        std::uint64_t next = 0;
        if (!CheckedMul(count, elementBytes, &part) ||
            !CheckedAdd(bytes, part, &next)) {
            return false;
        }
        bytes = next;
        return true;
    };
    if (!CheckedMul(resolved.topologyPlanPoolCapacity, resolved.maxBuses,
                    &planBus) ||
        !CheckedMul(planBus, resolved.busSendSlotsPerBus, &planBusSend) ||
        !CheckedMul(planBus, 64u, &planBusCoefficients) ||
        !addBytes(planBus, sizeof(std::uint32_t)) ||       // order
        !addBytes(resolved.topologyPlanPoolCapacity,
                  sizeof(std::uint32_t)) ||                // order count
        !addBytes(planBusSend, sizeof(std::uint32_t)) ||   // send targets
        !addBytes(planBusSend, sizeof(float)) ||           // send levels
        !addBytes(planBusSend, sizeof(std::uint8_t)) ||    // level apply
        !addBytes(planBusSend, sizeof(std::uint32_t)) ||   // level ramps
        !addBytes(planBus, sizeof(std::uint32_t)) ||       // layouts
        !addBytes(planBusCoefficients, sizeof(float)) ||   // fold matrices
        !addBytes(planBus, sizeof(std::uint32_t)) ||       // parents
        !addBytes(planBus, sizeof(std::uint8_t)) ||        // liveness
        !addBytes(planBus, sizeof(std::uint64_t)) ||       // public Bus IDs
        !addBytes(planBus, sizeof(std::uint32_t)) ||       // generations
        !addBytes(planBus, sizeof(std::uint32_t)) ||       // migrations
        !addBytes(planBus, sizeof(float)) ||               // Bus volumes
        !addBytes(planBus, sizeof(std::uint32_t)) ||       // Bus volume ramps
        !addBytes(planBus, sizeof(std::uint8_t)) ||        // Bus volume apply
        !addBytes(resolved.topologyPlanPoolCapacity,
                  sizeof(std::uint32_t)) ||                // slot counts
        !addBytes(resolved.topologyPlanPoolCapacity,
                  sizeof(std::uint32_t)) ||                // free list
        !addBytes(RoundUpPow2(
                      static_cast<std::uint64_t>(
                          resolved.topologyPlanPoolCapacity) +
                      1u),
                  sizeof(std::uint32_t))) {                // retire ring
        return VN_INVALID_CONFIG;
    }
    resolved.topologyPlanPoolBytes = bytes;
    if (resolved.topologyPlanPoolBytes > resolved.maxTopologyPlanBytes) {
        return VN_INVALID_CONFIG;
    }
    resolved.preparedTopologyPlanPoolBytes = bytes;
    std::uint64_t preparedBudget = 0;
    if (!CheckedAdd(resolved.topologyPlanPoolBytes,
                    resolved.preparedTopologyPlanPoolBytes, &preparedBudget) ||
        preparedBudget > resolved.maxTopologyPlanBytes) {
        return VN_INVALID_CONFIG;
    }
    std::uint64_t claimOwnerBytes = 0;
    std::uint64_t ackTableBytes = 0;
    if (!CheckedMul(resolved.maxBusTopologyOwners,
                    static_cast<std::uint64_t>(sizeof(PreparedOwnerClaimV1)),
                    &claimOwnerBytes) ||
        !CheckedMul(resolved.topologyPlanPoolCapacity,
                    static_cast<std::uint64_t>(
                        sizeof(ProjectGenerationInstallAckIdentityV1)),
                    &ackTableBytes) ||
        !CheckedAdd(preparedBudget, claimOwnerBytes, &preparedBudget) ||
        !CheckedAdd(preparedBudget, ackTableBytes, &preparedBudget)) {
        return VN_INVALID_CONFIG;
    }
    std::uint64_t ackRingBytes = 0;
    std::uint32_t ackRingCapacity = 1;
    const std::uint32_t ackRingWant = resolved.topologyPlanPoolCapacity + 1u;
    while (ackRingCapacity < ackRingWant) {
        ackRingCapacity <<= 1;
    }
    if (ackRingCapacity < 2u) {
        ackRingCapacity = 2u;
    }
    if (!CheckedMul(static_cast<std::uint64_t>(ackRingCapacity),
                    static_cast<std::uint64_t>(
                        sizeof(ProjectGenerationInstallAckV1)),
                    &ackRingBytes) ||
        !CheckedAdd(preparedBudget, ackRingBytes, &preparedBudget) ||
        preparedBudget > resolved.maxTopologyPlanBytes) {
        return VN_INVALID_CONFIG;
    }

    // Byte-level ceiling on the delay pool's preallocation. The instance-count
    // and sample-rate ceilings above bound every factor (delayCap <= 4096,
    // rate <= 384000, kMaxChannels fixed), so this product cannot overflow a
    // 64-bit accumulator — the multiply is wraparound-safe by construction. A
    // config that passes the count ceilings but would still allocate gigabytes
    // of delay line is a capacity the engine will not attempt.
    const std::uint64_t delayLineFrames = static_cast<std::uint64_t>(std::ceil(
        kDelayEffectMaxSeconds *
        static_cast<double>(resolved.mixRate.SampleRate())));
    const std::uint64_t delayLineBytes =
        static_cast<std::uint64_t>(resolved.delayEffectPoolCapacity) *
        delayLineFrames * static_cast<std::uint64_t>(kMaxChannels) *
        static_cast<std::uint64_t>(sizeof(float));
    if (delayLineBytes > kMaxDelayLineBytes) {
        return VN_INVALID_CONFIG;
    }

    // Dynamic waveform routing owns independent immutable plans, Bus tap
    // planes, double-buffered Voice tap planes, and one detector scratch.
    // Every product is checked before construction; the aggregate ceiling is
    // applied to the storage this implementation actually preallocates.
    std::uint64_t planRowsPerPlan = 0;
    std::uint64_t planPinsPerPlan = 0;
    std::uint64_t planBytesPerPlan = 0;
    std::uint64_t dynamicPlanBytes = 0;
    std::uint64_t dynamicPlanSlots = 0;
    std::uint64_t busTapBytes = 0;
    std::uint64_t voiceTapBytes = 0;
    std::uint64_t detectorBytes = 0;
    std::uint64_t dynamicBytes = 0;
    const std::uint64_t sampleBytes =
        static_cast<std::uint64_t>(kMaxChannels) *
        resolved.blockFrames * sizeof(float);
    if (!CheckedMul(resolved.dynamicRouteCapacity,
                    sizeof(DynamicRoutePlanRow), &planRowsPerPlan) ||
        !CheckedMul(static_cast<std::uint64_t>(
                        resolved.dynamicBusTapCapacity) +
                        resolved.dynamicVoiceTapCapacity,
                    sizeof(std::uint32_t), &planPinsPerPlan) ||
        !CheckedAdd(planRowsPerPlan, planPinsPerPlan,
                    &planBytesPerPlan) ||
        !CheckedAdd(resolved.dynamicRoutePlanPoolCapacity, 1,
                    &dynamicPlanSlots) ||
        !CheckedMul(dynamicPlanSlots,
                    planBytesPerPlan, &dynamicPlanBytes) ||
        !CheckedMul(resolved.dynamicBusTapCapacity, sampleBytes,
                    &busTapBytes) ||
        !CheckedMul(resolved.dynamicVoiceTapCapacity, sampleBytes,
                    &voiceTapBytes) ||
        !CheckedMul(voiceTapBytes, 2, &voiceTapBytes) ||
        !CheckedMul(resolved.blockFrames, sizeof(float),
                    &detectorBytes) ||
        !CheckedAdd(dynamicPlanBytes, busTapBytes, &dynamicBytes) ||
        !CheckedAdd(dynamicBytes, voiceTapBytes, &dynamicBytes) ||
        !CheckedAdd(dynamicBytes, detectorBytes, &dynamicBytes) ||
        dynamicBytes > kMaxDynamicRouteMemoryBytes) {
        return VN_INVALID_CONFIG;
    }

    // Each stream slot owns a power-of-two ring and may stage one whole chunk.
    // Budget both at their allocated/worst accepted capacities. Comparing the
    // per-slot sum against ceiling / slot count avoids multiplication wrap.
    const std::uint64_t ringFramesPow2 = RoundUpPow2(resolved.streamRingFrames);
    const std::uint64_t bytesPerStreamFrame =
        static_cast<std::uint64_t>(kMaxChannels) * sizeof(float);
    const std::uint64_t ringBytesPerStream =
        ringFramesPow2 * bytesPerStreamFrame;
    const std::uint64_t stagingBytesPerStream =
        static_cast<std::uint64_t>(kMaxStreamChunkFrames) *
        bytesPerStreamFrame;
    const std::uint64_t bytesPerStream =
        ringBytesPerStream + stagingBytesPerStream;
    std::uint64_t streamBytes = 0;
    std::uint64_t parkTokenBytes = 0;
    std::uint64_t streamingAggregateBytes = 0;
    if (!CheckedMul(resolved.maxStreams, bytesPerStream,
                    &streamBytes) ||
        !CheckedMul(resolved.voiceCapacity.Virtual(),
                    kStreamParkTokenBytes, &parkTokenBytes) ||
        !CheckedAdd(streamBytes, parkTokenBytes,
                    &streamingAggregateBytes) ||
        streamingAggregateBytes > kMaxStreamMemoryBytes) {
        return VN_INVALID_CONFIG;
    }

    *out = resolved;
    return VN_OK;
}

VnResult ResolveConfig(const VnConfig* config, EngineConfig* out) {
    return ResolveConfigForInitialDevice(config, 0, 0, out);
}

} // namespace harken
