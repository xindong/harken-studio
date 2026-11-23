#include <harken/Harken.h>
#include <harken/HarkenDevelopment.h>

#include "Engine.h"
#include "HarkenApiInternal.h"
#include "harken/core/SnapshotBuffer.h"
#include "harken/render/EffectRegistry.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

static_assert(sizeof(VnVoiceSnapshot) == sizeof(harken::VoiceSnapshotRow));
static_assert(offsetof(VnVoiceSnapshot, state) == offsetof(harken::VoiceSnapshotRow, state));
static_assert(offsetof(VnVoiceSnapshot, tapPostGain) == offsetof(harken::VoiceSnapshotRow, tapPostGain));
static_assert(VN_SNAPSHOT_MAX_VOICES == harken::kSnapshotMaxVoices);
static_assert(VN_SNAPSHOT_MAX_BUSES == harken::kSnapshotMaxBuses);
static_assert(VN_VOICE_STATE_INACTIVE == harken::kVoiceSnapshotInactive);
static_assert(VN_VOICE_STATE_ACTIVE == harken::kVoiceSnapshotActive);
static_assert(VN_VOICE_STATE_DRAINING == harken::kVoiceSnapshotDraining);
static_assert(VN_VOICE_STATE_PREPARING == harken::kVoiceSnapshotPreparing);
static_assert(VN_MAX_EFFECT_TYPE_STATS == harken::kMaxEffectTypeStats);
static_assert(std::is_trivially_copyable_v<VnSnapshot>);
static_assert(sizeof(VnSnapshot) == 5408);

using harken::detail::BusEffectivelyAudible;
using harken::detail::PinnedGenerationCount;
using harken::detail::TraceQuery;

extern "C" {

#if HARKEN_ENGINE_DEV
VnResult Vn_CaptureBusStates(uint32_t firstBusIndex, VnBusState* states,
                           uint32_t capacity, uint32_t* outCount,
                           uint32_t* outNextBusIndex) {
    return harken::detail::InvokeApi([&]() -> VnResult {
    auto* engine = harken::detail::g_engine;
    if (engine == nullptr || engine->callbackFatalSticky) return VN_NOT_INITIALIZED;
    if (outCount == nullptr || outNextBusIndex == nullptr || (capacity != 0 && states == nullptr))
        return VN_INVALID_ARG;
    *outCount = 0;
    *outNextBusIndex = UINT32_MAX;
    harken::SnapshotPayload receipt{};
    const bool captured = engine->mixer.Snapshots().Capture(receipt);
    const auto* activity = captured ? engine->buses.ObservedActivity(receipt.busActivityPublication) : nullptr;
    const auto end = engine->buses.BusCount();
    auto index = std::min(firstBusIndex, end);
    for (; index < end; ++index) {
        if (!engine->buses.AliveAt(index)) continue;
        if (*outCount == capacity) { *outNextBusIndex = index; break; }
        auto& state = states[(*outCount)++];
        state = {};
        state.busId = engine->buses.IdAt(index);
        state.busIndex = index;
        state.busGeneration = engine->buses.GenerationAt(index);
        if (captured) engine->buses.CaptureObservedBusState(
            {index, state.busGeneration}, receipt, activity, state);
    }
    return VN_OK;
    });
}

#endif

VnResult Vn_CaptureSnapshot(VnSnapshot* outSnapshot) {
    return harken::detail::InvokeApi([&]() -> VnResult {
    harken::Engine* engine = harken::detail::g_engine;
    if (engine == nullptr) {
        return VN_NOT_INITIALIZED;
    }
    if (engine->callbackFatalSticky) {
        return VN_NOT_INITIALIZED;
    }
    TraceQuery(engine, harken::kTraceCallCaptureSnapshot)
        .StructSizeOf(outSnapshot);
    // structSize versions the snapshot the same way VnConfig does across
    // twelve
    // vintages, each ending at the offset of the next vintage's first appended
    // field: the pre-playback struct (ends before voicePublicHandles), the
    // playback-management struct (ends before logicLogDropCount), the
    // logic-log struct (ends before nestedTriggerDroppedCount), the
    // action/hot-reload/tracing struct (ends before mixChannelCount), the
    // multichannel/job-parallel struct (ends before
    // bindingDefinitionHighWater), the property/binding observability struct
    // (ends before effectTypeStatCount), the effect-stats struct (ends
    // before busMuteMask), the bus-audibility struct (ends before
    // actionEmitterMultichannelFallbackCount), the Action fallback struct
    // (ends before busPeaksPreFader), the bus-meter-tap struct (ends before
    // dynamicRouteCurrent), the dynamic-aggregate struct (ends before
    // activeBusCount), the bus-topology struct (ends before
    // commandDeferredCount), and the current full struct. An older caller gets
    // the common prefix filled and the appended fields left as they were in
    // its shorter struct.
    const std::uint32_t snapshotVintage1Size =
        static_cast<std::uint32_t>(offsetof(VnSnapshot, voicePublicHandles));
    const std::uint32_t snapshotVintage2Size =
        static_cast<std::uint32_t>(offsetof(VnSnapshot, logicLogDropCount));
    const std::uint32_t snapshotVintage3Size = static_cast<std::uint32_t>(
        offsetof(VnSnapshot, nestedTriggerDroppedCount));
    const std::uint32_t snapshotVintage4Size =
        static_cast<std::uint32_t>(offsetof(VnSnapshot, mixChannelCount));
    const std::uint32_t snapshotVintage5Size = static_cast<std::uint32_t>(
        offsetof(VnSnapshot, bindingDefinitionHighWater));
    const std::uint32_t snapshotVintage6Size = static_cast<std::uint32_t>(
        offsetof(VnSnapshot, effectTypeStatCount));
    const std::uint32_t snapshotVintage7Size =
        static_cast<std::uint32_t>(offsetof(VnSnapshot, busMuteMask));
    const std::uint32_t snapshotVintage8Size = static_cast<std::uint32_t>(
        offsetof(VnSnapshot, actionEmitterMultichannelFallbackCount));
    const std::uint32_t snapshotVintage9Size = static_cast<std::uint32_t>(
        offsetof(VnSnapshot, busPeaksPreFader));
    const std::uint32_t snapshotVintage10Size = static_cast<std::uint32_t>(
        offsetof(VnSnapshot, dynamicRouteCurrent));
    const std::uint32_t snapshotVintage11Size = static_cast<std::uint32_t>(
        offsetof(VnSnapshot, activeBusCount));
    const std::uint32_t snapshotVintage12Size = static_cast<std::uint32_t>(
        offsetof(VnSnapshot, commandDeferredCount));
    // The newest boundary is expressed as "past the end of the last appended
    // field", not as sizeof: a gate written against sizeof silently withdraws
    // its own fields from callers compiled at this size the moment anything is
    // appended after them, which is the whole failure this ladder exists to
    // prevent. Every gate below is >= a boundary of this form.
    const std::uint32_t snapshotVintage13Size = static_cast<std::uint32_t>(
        offsetof(VnSnapshot, commandDeferredCount) +
        sizeof(VnSnapshot::commandDeferredCount));
    const std::uint32_t snapshotVintage14Size = static_cast<std::uint32_t>(
        offsetof(VnSnapshot, notificationQueueHighWater) +
        sizeof(VnSnapshot::notificationQueueHighWater));
    const std::uint32_t snapshotVintage15Size = static_cast<std::uint32_t>(
        offsetof(VnSnapshot, notificationQuantumProducedHighWater) +
        sizeof(VnSnapshot::notificationQuantumProducedHighWater));
    const std::uint32_t snapshotVintage16Size = static_cast<std::uint32_t>(
        offsetof(VnSnapshot, lateConsumedEvents) +
        sizeof(VnSnapshot::lateConsumedEvents));
    const std::uint32_t snapshotVintage17Size = static_cast<std::uint32_t>(
        offsetof(VnSnapshot, variableTerminationCount) +
        sizeof(VnSnapshot::variableTerminationCount));
    const std::uint32_t snapshotVintage18Size = static_cast<std::uint32_t>(
        offsetof(VnSnapshot, busChannelPeaks) +
        sizeof(VnSnapshot::busChannelPeaks));
    const std::uint32_t snapshotVintage19Size = static_cast<std::uint32_t>(
        offsetof(VnSnapshot, vorbisDecoderCount) + sizeof(VnSnapshot::vorbisDecoderCount));
    if (outSnapshot == nullptr ||
        (outSnapshot->structSize != sizeof(VnSnapshot) &&
         outSnapshot->structSize != snapshotVintage1Size &&
         outSnapshot->structSize != snapshotVintage2Size &&
         outSnapshot->structSize != snapshotVintage3Size &&
         outSnapshot->structSize != snapshotVintage4Size &&
         outSnapshot->structSize != snapshotVintage5Size &&
         outSnapshot->structSize != snapshotVintage6Size &&
         outSnapshot->structSize != snapshotVintage7Size &&
         outSnapshot->structSize != snapshotVintage8Size &&
         outSnapshot->structSize != snapshotVintage9Size &&
         outSnapshot->structSize != snapshotVintage10Size &&
         outSnapshot->structSize != snapshotVintage11Size &&
         outSnapshot->structSize != snapshotVintage12Size &&
         outSnapshot->structSize != snapshotVintage13Size &&
         outSnapshot->structSize != snapshotVintage14Size &&
         outSnapshot->structSize != snapshotVintage15Size &&
         outSnapshot->structSize != snapshotVintage16Size &&
         outSnapshot->structSize != snapshotVintage17Size &&
         outSnapshot->structSize != snapshotVintage18Size &&
         outSnapshot->structSize != snapshotVintage19Size)) {
        return VN_INVALID_ARG;
    }
    const bool hasPlaybackFields =
        (outSnapshot->structSize >= snapshotVintage2Size);
    const bool hasLogicLogField =
        (outSnapshot->structSize >= snapshotVintage3Size);
    const bool hasTriggerFields =
        (outSnapshot->structSize >= snapshotVintage4Size);
    const bool hasMultichannelFields =
        (outSnapshot->structSize >= snapshotVintage5Size);
    const bool hasBindingObservabilityFields =
        (outSnapshot->structSize >= snapshotVintage6Size);
    const bool hasEffectTypeStatsFields =
        (outSnapshot->structSize >= snapshotVintage7Size);
    const bool hasBusAudibilityFields =
        (outSnapshot->structSize >= snapshotVintage8Size);
    const bool hasActionEmitterMultichannelFallbackFields =
        (outSnapshot->structSize >= snapshotVintage9Size);
    const bool hasBusMeterTapFields =
        (outSnapshot->structSize >= snapshotVintage10Size);
    const bool hasDynamicAggregateFields =
        (outSnapshot->structSize >= snapshotVintage11Size);
    // Gated on the vintage that introduced them, not on the newest size:
    // pinning either of these to sizeof would withdraw its own fields from
    // callers compiled against the struct that introduced them the moment
    // anything was appended after them.
    const bool hasTopologySnapshotFields =
        (outSnapshot->structSize >= snapshotVintage12Size);
    const bool hasCommandBudgetFields =
        (outSnapshot->structSize >= snapshotVintage13Size);
    const bool hasDiagnosticOccupancyFields =
        (outSnapshot->structSize >= snapshotVintage14Size);
    const bool hasQuantumBudgetFields =
        (outSnapshot->structSize >= snapshotVintage15Size);
    const bool hasTimelineCounterFields =
        (outSnapshot->structSize >= snapshotVintage16Size);
    const bool hasVariableLifecycleFields =
        (outSnapshot->structSize >= snapshotVintage17Size);
    const bool hasBusChannelPeakFields =
        (outSnapshot->structSize >= snapshotVintage18Size);

    /* Render-side facts come only from the published double buffer —
     * never from the live render state the audio thread mutates. A
     * capture that collides with the writer through every bounded retry
     * falls back to the previous coherent copy (the writer publishes at
     * block cadence, so the fallback is at most one block stale). */
    harken::SnapshotPayload fresh;
    if (engine->mixer.Snapshots().Capture(fresh)) {
        /* Only a validated copy replaces the fallback: Capture() writes
         * its scratch argument even on a failed attempt. */
        engine->lastSnapshotPayload = fresh;
    }
    const harken::SnapshotPayload& payload = engine->lastSnapshotPayload;

    outSnapshot->activeVoiceCount = payload.activeVoiceCount;
    outSnapshot->blockIndex = payload.blockIndex;
    std::memcpy(outSnapshot->voices, payload.voices,
                sizeof(outSnapshot->voices));
    engine->ledger.ForEachPlayableOwnedLive(
        [engine, outSnapshot](
            VnVoiceHandle renderHandle,
            const harken::VoiceLedger::PlayableParentRef&) {
            if (!engine->voices.IsAlive(renderHandle)) {
                return;
            }
            const std::uint32_t slot =
                static_cast<std::uint32_t>(renderHandle);
            if (slot < VN_SNAPSHOT_MAX_VOICES) {
                outSnapshot->voices[slot] = {};
            }
        });
    outSnapshot->busCount = payload.busCount;
    std::memcpy(outSnapshot->busPeaks, payload.busPeaks,
                sizeof(outSnapshot->busPeaks));
    outSnapshot->reserved0 = 0;
    outSnapshot->arenaHighWaterBytes = payload.arenaHighWaterBytes;
    outSnapshot->arenaOverflowCount = payload.arenaOverflowCount;
    outSnapshot->voiceCompletedCount = payload.voiceCompletedCount;

    /* Logic-owned counters merge here, on the calling thread — no
     * cross-thread reads, no publication needed. */
    outSnapshot->voiceHighWater = engine->voices.HighWaterVoices();
    outSnapshot->staleHandleCount = engine->voices.StaleHandleCount();
    outSnapshot->noVoiceCount = engine->voices.NoVoiceCount();
    outSnapshot->backpressureCount = engine->commands.BackpressureCount();
    outSnapshot->notificationDropCount = engine->notifications.DroppedCount();
    outSnapshot->logDropCount = engine->logRing.DroppedCount();
    outSnapshot->paramCommitCount = engine->params.CommitCount();
    outSnapshot->paramDropCount = engine->paramDropCount;
    /* Effect pools live on the logic side (the audio thread never allocates
     * or frees an effect), so their high-water marks merge here directly,
     * like the other logic-owned counters. */
    outSnapshot->delayEffectHighWater = engine->effectBank.DelayHighWater();
    outSnapshot->toneEffectHighWater = engine->effectBank.ToneHighWater();
    /* Pack-table and stream-manager facts are logic-owned (the stream
     * high-waters are claimed/measured off the audio thread), merging here
     * like the effect pools. The starvation counter is audio-owned and
     * rides the published payload with the other render-side counters. */
    outSnapshot->packHighWater = engine->packs.LoadedHighWater();
    outSnapshot->noPackCount = engine->packs.NoPackCount();
    outSnapshot->streamHighWater = engine->streams.StreamHighWater();
    outSnapshot->streamRingHighWaterFrames =
        engine->streams.RingHighWaterFrames();
    outSnapshot->voiceStarvedCount = payload.voiceStarvedCount;

    if (hasPlaybackFields) {
        /* Correlation array, index-aligned with voices[]: for each render slot
         * that a live ledger entry currently owns, publish the public handle
         * the caller holds. The live predicate reads the voice pool as a
         * liveness check only (the same advisory use as the flush above), so an
         * ended-but-unreaped ledger entry — whose render slot a newer voice may
         * already hold — never clobbers the true owner's row. Logic thread
         * only; no cross-thread reads. */
        engine->ledger.FillPublicHandles(
            outSnapshot->voicePublicHandles, VN_SNAPSHOT_MAX_VOICES,
            [engine](VnVoiceHandle renderHandle) {
                return engine->voices.IsAlive(renderHandle);
            });
        /* Logic-owned degradation counter for the logic-side notification queue;
         * merges here like the other logic-owned counters. */
        outSnapshot->logicNotificationDropCount =
            engine->logicNotifications.DroppedCount();
        /* Below, the Action, container, emitter, steal, pending-start,
         * audibility-parking, and bus-graph counters all carry live engine
         * values. */
        outSnapshot->parkedVoiceCount = engine->ledger.ParkedCount();
        outSnapshot->voiceStolenCount = engine->voiceStolenCount;
        outSnapshot->voiceParkedCount = engine->voiceParkedCount;
        outSnapshot->voiceReturnedCount = engine->voiceReturnedCount;
        outSnapshot->returnRefusedCount = engine->returnRefusedCount;
        outSnapshot->stepSkippedCount = 0;
        outSnapshot->actionHighWater = 0;
        outSnapshot->containerHighWater = 0;
        outSnapshot->emitterHighWater = engine->emitters.HighWater();
        outSnapshot->pendingActionHighWater = 0;
        outSnapshot->parkedHighWater = engine->parkedHighWater;
        outSnapshot->sendSlotHighWater = engine->sendSlotHighWater;
        outSnapshot->containerChildHighWater = 0;
        outSnapshot->containerCursorHighWater = 0;
        outSnapshot->topologyPlanHighWater =
            engine->topologyPlans.HighWater();
        outSnapshot->containerResolutionFailCount = 0;
        outSnapshot->topologyPlanCount = engine->topologyPlanCount;
        outSnapshot->droppedDeadEmitterStepCount = 0;
        outSnapshot->pendingStartCount = engine->ledger.PendingStartCount();
        outSnapshot->parkDeferredCount = engine->parkDeferredCount;
        outSnapshot->noActionCount = 0;
        outSnapshot->noContainerCount = 0;
        outSnapshot->noEmitterCount = engine->noEmitterCount;
        outSnapshot->noSendCount = engine->noSendCount;
        outSnapshot->triggerRefusedCount = 0;
        outSnapshot->topologyRefusedCount = engine->topologyRefusedCount;
    }
    if (hasLogicLogField) {
        /* Drop accounting for the logic-side log ring (container resolution
         * outcomes), the counterpart to logDropCount's audio-side ring. */
        outSnapshot->logicLogDropCount =
            static_cast<std::uint32_t>(engine->logicLogRing.DroppedCount());
        /* reserved1 sits inside this vintage's declared size, so it is cleared
         * under this gate. Clearing it with the group that follows it left a
         * caller declaring exactly this size reading four bytes the capture
         * never touched. */
        outSnapshot->reserved1 = 0;
    }
    if (hasTriggerFields) {
        /* Nested-trigger accounting carries live engine values. */
        outSnapshot->nestedTriggerDroppedCount = 0;
        outSnapshot->triggerDepthHighWater = 0;
        /* Hot-reload accounting: the cumulative outcome counters are
         * logic-owned; the live pinned total is derived from the three
         * generation owners at capture time (the high water is maintained
         * at each successful replace, the only moment the total rises). */
        outSnapshot->hotReloadCount = engine->hotReloadCount;
        outSnapshot->hotReloadRefusedCount = engine->hotReloadRefusedCount;
        outSnapshot->pinnedGenerationCount = PinnedGenerationCount(*engine);
        outSnapshot->pinnedGenerationHighWater =
            engine->pinnedGenerationHighWater;
        /* Trace status is logic-owned; the audio timing quintet rides the
         * published payload with the other render-side facts, and the
         * update pair is the logic thread's own self-measurement. */
        outSnapshot->traceActive =
            (engine->trace != nullptr && harken::tracing::Active(engine->trace.get())) ? 1u : 0u;
        outSnapshot->traceFailed = engine->traceFailedSticky ? 1u : 0u;
        outSnapshot->audioCpuLastUs = payload.audioCpuLastUs;
        outSnapshot->audioCpuPeakUs = payload.audioCpuPeakUs;
        outSnapshot->audioDrainLastUs = payload.audioDrainLastUs;
        outSnapshot->audioVoicesLastUs = payload.audioVoicesLastUs;
        outSnapshot->audioBusesLastUs = payload.audioBusesLastUs;
        outSnapshot->updateCpuLastUs = engine->updateCpuLastUs;
        outSnapshot->updateCpuPeakUs = engine->updateCpuPeakUs;
        outSnapshot->reserved2 = 0;
    }
    if (hasMultichannelFields) {
        /* Multichannel/job-parallel facts. The mix and device channel counts
         * and output-downmix flag are session-fixed config values. Worker count
         * is the pool's active admission state: zero while disabled, suspended,
         * or permanently faulted (merged here on the logic thread). The
         * output downmix runs exactly when the device layout is narrower than
         * the mix. The worker busy peak is the one audio-measured figure — it
         * rides the published payload and reads 0 while rendering serially. */
        outSnapshot->mixChannelCount = engine->config.mixChannelCount;
        outSnapshot->deviceChannelCount = engine->config.deviceChannelCount;
        outSnapshot->outputDownmixActive =
            (engine->config.deviceChannelCount <
             engine->config.mixChannelCount)
                ? 1u
                : 0u;
        const bool callbackActive =
            engine->config.nullDevice ||
            (!engine->gameOutputStartup.Opening() && engine->device.CallbackRealtimeDomainReady());
        outSnapshot->renderWorkerCount =
            callbackActive ? engine->voices.RenderWorkerCount() : 0u;
        // Worker busy peak rides the published payload (audio-thread-measured),
        // unlike the config-fixed counts merged above: it is a per-block figure
        // the job-parallel render path fills. Serial rendering leaves it 0.
        outSnapshot->renderWorkerBusyPeakUs = payload.renderWorkerBusyPeakUs;
        outSnapshot->reserved3 = 0;
    }
    if (hasBindingObservabilityFields) {
        /* Property/binding observability (section 12), all logic-owned.
         * The pool marks and refusal counters read straight off their
         * owners; the meter overflow total is the logic-observed drain
         * figure (at most one pump behind the audio-side producer — no
         * cross-thread read). */
        outSnapshot->bindingDefinitionHighWater =
            engine->bindings.DefinitionHighWater();
        outSnapshot->bindingDefinitionRefusedCount =
            engine->bindings.DefinitionRefusedCount();
        outSnapshot->bindingAttachmentHighWater =
            engine->bindings.AttachmentHighWater();
        outSnapshot->bindingAttachmentRefusedCount =
            engine->bindings.AttachmentRefusedCount();
        outSnapshot->bindingInstanceHighWater =
            engine->params.InstanceHighWater();
        outSnapshot->bindingInstanceRefusedCount =
            engine->bindingInstanceRefusedCount;
        outSnapshot->paramPublicationDeferredCount =
            engine->propertyPublisher.DeferredCount();
        outSnapshot->meterRingOverflowCount =
            engine->meterRing.OverflowCount();
        outSnapshot->meterLoopCount = 0;
    }
    if (hasEffectTypeStatsFields) {
        /* Per-type effect-pool stats, one row per generated registry row in
         * effect-type order. The pools are logic-owned, so the per-type
         * capacity / live / high-water / refusal figures read straight off the
         * bank here. The legacy delayEffectHighWater / toneEffectHighWater
         * scalars above are filled from the same source and stay equal to their
         * rows' highWater. Rows beyond the registered count and the reserved
         * fields are zeroed so the table is deterministic. */
        outSnapshot->effectTypeStatCount = harken::kEffectRegistryRowCount;
        outSnapshot->reserved4 = 0;
        for (std::uint32_t i = 0; i < harken::kEffectRegistryRowCount; ++i) {
            const std::uint32_t type = harken::kEffectRegistry[i].effectType;
            VnEffectTypeStats& row = outSnapshot->effectTypeStats[i];
            row.effectType = type;
            row.capacity = engine->effectBank.CapacityOf(type);
            row.activeCount = engine->effectBank.InUseOf(type);
            row.highWater = engine->effectBank.HighWaterOf(type);
            row.refusalCount = engine->effectBank.RefusalCountOf(type);
            row.reserved0 = 0;
        }
        for (std::uint32_t i = harken::kEffectRegistryRowCount;
             i < VN_MAX_EFFECT_TYPE_STATS; ++i) {
            outSnapshot->effectTypeStats[i] = VnEffectTypeStats{};
        }
    }
    if (hasBusAudibilityFields) {
        /* Bus audibility masks, logic-owned: bit i describes dense bus index
         * i. busMuteMask is the authored mute flags; busSoloEffectiveMask is
         * post-solo effective audibility (set = audible). Buses beyond the
         * mask width report through busCount only. */
        std::uint32_t muteMask = 0;
        std::uint32_t audibleMask = 0;
        const std::uint32_t busTotal = engine->buses.BusCount();
        const std::uint32_t maskCount = busTotal < 32u ? busTotal : 32u;
        for (std::uint32_t i = 0; i < maskCount; ++i) {
            if (!engine->buses.AliveAt(i)) {
                continue; // a tombstoned slot is neither muted nor audible
            }
            if (engine->busMuted[i] != 0) {
                muteMask |= (1u << i);
            }
            if (BusEffectivelyAudible(*engine, i)) {
                audibleMask |= (1u << i);
            }
        }
        outSnapshot->busMuteMask = muteMask;
        outSnapshot->busSoloEffectiveMask = audibleMask;
    }
    if (hasActionEmitterMultichannelFallbackFields) {
        outSnapshot->actionEmitterMultichannelFallbackCount = 0;
    }
    if (hasBusMeterTapFields) {
        std::memcpy(outSnapshot->busPeaksPreFader,
                    payload.busPeaksPreFader,
                    sizeof(outSnapshot->busPeaksPreFader));
        std::memcpy(outSnapshot->busPeaksPostFader,
                    payload.busPeaksPostFader,
                    sizeof(outSnapshot->busPeaksPostFader));
    }
    if (hasDynamicAggregateFields) {
        /* Drain only the logic-owned route acknowledgement/retirement
         * channels before reporting pool use. Audio-owned diagnostics come
         * exclusively from the coherent payload captured above. */
        engine->dynamicRoutes.DrainAudioMessages();
        outSnapshot->dynamicRouteCurrent =
            engine->dynamicRoutes.ActiveRouteCount();
        outSnapshot->dynamicRouteCapacity =
            engine->dynamicRoutes.RouteCapacity();
        outSnapshot->dynamicRouteHighWater =
            engine->dynamicRoutes.RouteHighWater();
        outSnapshot->dynamicRoutePlanCurrent =
            engine->dynamicRoutes.PlanInUse();
        outSnapshot->dynamicRoutePlanCapacity =
            engine->dynamicRoutes.PlanCapacity();
        outSnapshot->dynamicRoutePlanHighWater =
            engine->dynamicRoutes.PlanHighWater();
        outSnapshot->dynamicBusTapCurrent =
            engine->dynamicRoutes.BusTapInUse();
        outSnapshot->dynamicBusTapCapacity =
            engine->dynamicRoutes.BusTapCapacity();
        outSnapshot->dynamicBusTapHighWater =
            engine->dynamicRoutes.BusTapHighWater();
        outSnapshot->dynamicVoiceTapCurrent =
            engine->dynamicRoutes.VoiceTapInUse();
        outSnapshot->dynamicVoiceTapCapacity =
            engine->dynamicRoutes.VoiceTapCapacity();
        outSnapshot->dynamicVoiceTapHighWater =
            engine->dynamicRoutes.VoiceTapHighWater();
        outSnapshot->dynamicRouteRefusalCount =
            engine->dynamicRoutes.RouteRefusalCount();
        outSnapshot->dynamicRouteCycleCount =
            engine->dynamicRoutes.RouteCycleCount();
        outSnapshot->dynamicRouteOrderRefusalCount =
            engine->dynamicRoutes.RouteOrderRefusalCount();
        outSnapshot->dynamicRouteStaleReadCount =
            engine->dynamicRoutes.StaleReadCount();
        outSnapshot->dynamicTapGenerationMismatchCount =
            payload.dynamicTapGenerationMismatchCount;
        outSnapshot->dynamicPlanPublicationCount =
            payload.dynamicPlanPublicationCount;
        outSnapshot->dynamicPlanRetireOverflowCount =
            payload.dynamicPlanRetireOverflowCount;
        outSnapshot->compressorDetectorNonFiniteCount =
            payload.compressorDetectorNonFiniteCount;
        outSnapshot->compressorDetectorSaturationCount =
            payload.compressorDetectorSaturationCount;
    }
    if (hasTopologySnapshotFields) {
        outSnapshot->activeBusCount = engine->buses.ActiveBusCount();
        outSnapshot->freeBusSlotCount =
            engine->buses.FreeBusSlotCount();
        outSnapshot->quarantinedBusSlotCount =
            engine->buses.QuarantinedBusSlotCount();
        outSnapshot->reserved5 = 0;
        outSnapshot->busSlotReuseCount =
            engine->buses.SlotReuseCount();
        outSnapshot->acceptedBusTopologyRevision =
            engine->acceptedBusTopologyRevision;
        outSnapshot->installedBusTopologyRevision =
            engine->buses.InstalledGraphRevision();
        outSnapshot->topologyPlanByteBudget =
            engine->config.maxTopologyPlanBytes;
        outSnapshot->topologyPlanPoolBytes =
            engine->config.topologyPlanPoolBytes;
        std::fill_n(outSnapshot->busIds, VN_SNAPSHOT_MAX_BUSES,
                    VnBusId{});
        std::fill_n(outSnapshot->busGenerations,
                    VN_SNAPSHOT_MAX_BUSES, 0u);
        const std::uint32_t busRows =
            (std::min)(engine->buses.BusCount(),
                       static_cast<std::uint32_t>(
                           VN_SNAPSHOT_MAX_BUSES));
        for (std::uint32_t i = 0; i < busRows; ++i) {
            if (!engine->buses.AliveAt(i)) {
                continue;
            }
            outSnapshot->busIds[i] = engine->buses.IdAt(i);
            outSnapshot->busGenerations[i] =
                engine->buses.GenerationAt(i);
        }
        outSnapshot->lastTopologyRefusalResult =
            engine->lastTopologyRefusalResult;
        outSnapshot->lastTopologyRefusalCategory =
            engine->lastTopologyRefusalCategory;
        outSnapshot->lastTopologyRefusalOwner =
            engine->lastTopologyRefusalOwner;
        outSnapshot->lastTopologyRefusalBusId =
            engine->lastTopologyRefusalBusId;
        outSnapshot->lastTopologyRefusalRelatedBusId =
            engine->lastTopologyRefusalRelatedBusId;
    }
    if (hasCommandBudgetFields) {
        // The mixer owns this counter on the audio thread; consume its
        // coherent published copy with the rest of the render snapshot.
        outSnapshot->commandDeferredCount = payload.commandDeferredHighWater;
    }
    if (hasDiagnosticOccupancyFields) {
        // Both are consumer-side state, accumulated where the rings are
        // drained, so reading them here costs nothing and races nothing.
        outSnapshot->logRingHighWater =
            engine->logRing.OccupancyHighWater();
        outSnapshot->notificationQueueHighWater =
            engine->notifications.OccupancyHighWater();
    }
    if (hasQuantumBudgetFields) {
        // Producer-side state is audio-owned and arrives with the coherent
        // published payload; never read the live transports on this thread.
        outSnapshot->logQuantumBudgetDropCount =
            payload.logQuantumBudgetDropCount;
        outSnapshot->notificationQuantumBudgetDropCount =
            payload.notificationQuantumBudgetDropCount;
        outSnapshot->logQuantumProducedHighWater =
            payload.logQuantumProducedHighWater;
        outSnapshot->notificationQuantumProducedHighWater =
            payload.notificationQuantumProducedHighWater;
    }
    if (hasTimelineCounterFields) {
        outSnapshot->deadlineMissQuanta = payload.deadlineMissQuanta;
        outSnapshot->lateConsumedEvents = payload.lateConsumedEvents;
    }
    if (hasVariableLifecycleFields) {
        outSnapshot->variableAcceptedCount = engine->variables.AcceptedCount();
        outSnapshot->variableAppliedCount = engine->variables.AppliedCount();
        outSnapshot->variableSupersededCount =
            engine->variables.SupersededCount();
        outSnapshot->variablePendingCount = engine->variables.PendingCount();
        outSnapshot->variableTerminationCount =
            engine->variables.TerminationCount();
    }
    if (hasBusChannelPeakFields) {
        std::memcpy(outSnapshot->busChannelPeaks, payload.busChannelPeaks,
                    sizeof(outSnapshot->busChannelPeaks));
    }
    if (outSnapshot->structSize >= snapshotVintage19Size) {
#if HARKEN_ENGINE_DEV
        outSnapshot->vorbisDecodeLastUs = payload.vorbisDecodeLastUs;
        outSnapshot->vorbisDecoderCount = engine->vorbis.states.ActiveCount();
#else
        outSnapshot->vorbisDecodeLastUs = 0;
        outSnapshot->vorbisDecoderCount = 0;
#endif
    }
    return VN_OK;
    });
}

} // extern "C"
