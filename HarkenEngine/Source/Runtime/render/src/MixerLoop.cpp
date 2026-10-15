#include "harken/render/MixerLoop.h"

#include "MonitorCaptureHub.h"

#include "harken/core/AllocGuard.h"
#include "harken/core/ChannelLayout.h"
#include "harken/core/CpuClock.h"
#include "harken/core/FloatMode.h"

#if defined(HARKEN_RT_PROBE_MIX_PUBLICATION)
#include "harken/core/RtProbe.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>

namespace harken {

namespace {

std::size_t CurrentSliceScratchCapacity(
    const EngineConfig& config) {
    const std::uint64_t count = static_cast<std::uint64_t>(config.maxEventsPerQuantum) +
        config.maxPlayableInstances + config.voiceCapacity.Logical() + config.maxDeferredIntents;
    if (count > std::numeric_limits<std::size_t>::max() / 2 / sizeof(ConsumedCurrentSlice)) {
        throw std::bad_array_new_length{};
    }
    return static_cast<std::size_t>(count) * 2;
}

float ClampDeviceSample(float sample) {
    if (std::isnan(sample)) {
        return 0.0f;
    }
    return std::clamp(sample, -1.0f, 1.0f);
}

CompressorPreparedParameters RestoreCompressorParameters(
    const CommandRecord::Payload::SetCompressorEffectParam& payload) {
    CompressorPreparedParameters prepared{};
    prepared.thresholdDb = payload.thresholdDb;
    prepared.reciprocalRatio = payload.reciprocalRatio;
    prepared.attackCoeff = payload.attackCoeff;
    prepared.attackAdd = 1.0f - prepared.attackCoeff;
    prepared.releaseCoeff = payload.releaseCoeff;
    prepared.releaseAdd = 1.0f - prepared.releaseCoeff;
    prepared.kneeDb = payload.kneeDb;
    prepared.makeupLinear = payload.makeupLinear;
    prepared.lowerKneeLinear = payload.lowerKneeLinear;
    prepared.upperKneeLinear = payload.upperKneeLinear;
    prepared.hardKneeScale = payload.hardKneeScale;
    return prepared;
}

} // namespace

MixerLoop::MixerLoop(const EngineConfig& config, VoicePool& voices,
                     BusTree& buses, DynamicRouteSystem& dynamicRoutes,
                     CommandQueue& commands,
                     ActionSeekTable& actionSeeks,
                     StreamSeekHandoffTable& streamSeekHandoffs,
                     NotificationQueue& notifications, LogRing& logRing,
                     SourceCursorMirror& sourceCursors, MeterBlockRing& meterRing,
                     TimelineArmTable* timelineArms,
                     TimelineExecutor* timelineExecutor,
                     VoiceTimelineSink* timelineSink,
                     ScheduledTransactionPool* timelinePool,
                     VariableSliceTable* variableSlices,
                     const ProjectGenerationInstallAckIdentityV1* ackSideTable,
                     std::uint32_t ackSideTableCapacity,
                     ProjectGenerationInstallAckRingV1* installAckRing,
                     OriginalSourceSubmissionRegistry* originalSources)
    : m_maxBuses(config.maxBuses),
      m_audibilityParkEnabled(config.audibilityParkEnabled),
      m_maxCommandsPerQuantum(config.maxControlCommandsPerQuantum),
      m_voices(voices),
      m_buses(buses),
      m_dynamicRoutes(dynamicRoutes),
      m_commands(commands),
      m_actionSeeks(actionSeeks),
      m_streamSeekHandoffs(streamSeekHandoffs),
      m_seekResolvedFrames(commands.Capacity()),
      m_seekStreamHandoffs(commands.Capacity()),
      m_notifications(notifications),
      m_logRing(logRing),
      m_sourceCursors(sourceCursors),
      m_meterRing(meterRing),
      m_originalSources(originalSources),
      m_timelineArms(timelineArms),
      m_timelineExecutor(timelineExecutor),
      m_timelineSink(timelineSink),
      m_timelinePool(timelinePool),
      m_variableSlices(variableSlices),
      m_installAckSideTable(ackSideTable),
      m_installAckSideTableCapacity(ackSideTableCapacity),
      m_installAckRing(installAckRing),
      m_currentSliceScratch(timelinePool != nullptr
                                ? CurrentSliceScratchCapacity(
                                      config)
                                : 0),
      m_currentSliceScratchLimitForTest(m_currentSliceScratch.size()),
      m_meterTapScratch(static_cast<std::size_t>(kMeterTapLaneCount) *
                            config.maxBuses,
                        0.0f),
      m_meterGenerationScratch(config.maxBuses, 0),
      m_arena(config.arenaBytes),
      m_busPlanes(static_cast<std::size_t>(config.maxBuses) *
                      config.mixChannelCount,
                  nullptr),
      m_mixChannels(config.mixChannelCount),
      m_deviceChannels(config.deviceChannelCount),
      m_mixLayout(config.mixChannelLayout),
      m_deviceLayout(config.deviceChannelLayout),
      m_sampleRate(config.mixRate.SampleRate()),
      m_cpuTicksPerSecond(CpuClockTicksPerSecond()) {
    InitializeVariableSliceConsumptionStorage();
    m_voices.AttachFirstOutputCommandConsumer(this,
        [](void* context, const CommandRecord& command) noexcept {
            static_cast<MixerLoop*>(context)->ApplyCommandRecord(command, 0);
        });
    // Resolve the render-worker rendezvous budget once, here, so the audio
    // thread reads a plain integer and no division touches the block path. The
    // block width is frozen at init, so the period is a constant of the session.
    const std::uint64_t sampleRate = config.mixRate.SampleRate();
    if (m_cpuTicksPerSecond != 0 && sampleRate != 0 &&
        config.blockFrames != 0) {
        const std::uint64_t absoluteTicks =
            (kJobDeadlineMicros * m_cpuTicksPerSecond) / 1000000ull;
        const std::uint64_t blockPeriodTicks =
            (static_cast<std::uint64_t>(config.blockFrames) *
             m_cpuTicksPerSecond) /
            sampleRate;
        const std::uint64_t ceilingTicks =
            (blockPeriodTicks * kJobDeadlineBlockPeriodPercent) / 100ull;
        m_jobDeadlineTicks =
            ceilingTicks < absoluteTicks ? ceilingTicks : absoluteTicks;
        if (ceilingTicks < absoluteTicks) {
            // The ceiling bit. This configuration's blocks are short enough that
            // the budget had to be cut below the jitter floor the absolute figure
            // exists to clear, so healthy batches may now be abandoned. Computed,
            // reported, and allowed: refusing the configuration would be a
            // judgement about the host's hardware that this layer cannot make.
            LogRecord record{};
            record.level = VN_LOG_WARN;
            record.code = kLogCodeRenderDeadlineBelowJitterFloor;
            record.arg0 = (ceilingTicks * 1000000ull) /
                          (m_cpuTicksPerSecond != 0 ? m_cpuTicksPerSecond : 1);
            record.arg1 = kJobDeadlineMicros;
            m_logRing.Push(record);
        }
    }
    // These two transports are the ones this class produces into from inside a
    // block, so this class is what knows they have quanta at all. Turned on
    // after the warning above so that record is admitted under a fresh budget
    // rather than being the first thing a not-yet-opened quantum sees.
    //
    // The engine's other log ring is deliberately left alone: it is pushed from
    // ordinary API calls that belong to no block.
    m_logRing.EnableQuantumBudget();
    m_notifications.EnableQuantumBudget();
    if (m_timelineSink != nullptr) {
        m_timelineSink->SetCurrentSliceApplyCallback(this,
            [](void* context, std::uint64_t blockIndex,
               const TimelineExecutionResult& result) noexcept {
                static_cast<MixerLoop*>(context)->ApplyConsumedCurrentSlices(blockIndex, result);
            });
    }
}

namespace {

bool IsPlayLedKind(PlayLedBatchKind kind) noexcept {
    return kind == PlayLedBatchKind::TimelineArm ||
           kind == PlayLedBatchKind::DirectStart ||
           kind == PlayLedBatchKind::DeferredStart ||
           kind == PlayLedBatchKind::ParkReturn ||
           kind == PlayLedBatchKind::TimelineControlArm;
}

bool PlayLedBodyOpcodeAllowed(PlayLedBatchKind kind,
                              std::uint16_t opcode) noexcept {
    switch (kind) {
    case PlayLedBatchKind::TimelineArm:
    case PlayLedBatchKind::DirectStart:
    case PlayLedBatchKind::DeferredStart:
    case PlayLedBatchKind::ParkReturn:
        return opcode == kOpcodeSetVoiceVolume ||
               opcode == kOpcodeSetVoicePitch ||
               opcode == kOpcodeSetVoiceFilter ||
               opcode == kOpcodeSetVoicePosition ||
               opcode == kOpcodeSetVoicePan ||
               opcode == kOpcodeSetVoiceSpatialScalar ||
               opcode == kOpcodeSetVoiceLowShelf ||
               opcode == kOpcodeSetVoiceEffectBypass ||
               opcode == kOpcodeSetVoiceEffect ||
               opcode == kOpcodeClearVoiceEffect ||
               opcode == kOpcodeSetVoiceEffectParam ||
               opcode == kOpcodeSetVoiceSend ||
               opcode == kOpcodeClearVoiceSend ||
               opcode == kOpcodeSetVoiceSendLevel;
    default:
        return false;
    }
}

} // namespace

void MixerLoop::ApplyCommandRecord(const CommandRecord& command,
                                   std::uint64_t blockIndex) noexcept {
    switch (command.opcode) {
    case kOpcodeSetBusVolume:
        m_buses.ApplyVolume(command.payload.setBusVolume.busIndex,
                            command.payload.setBusVolume.volumeDb,
                            command.payload.setBusVolume.rampMs,
                            command.payload.setBusVolume.busGeneration,
                            command.payload.setBusVolume.outputOnly != 0);
        break;
    case kOpcodeSetVoiceVolume:
        if (command.flags == kCommandFlagVariableWeightSegment) {
            WeightSegmentV1 segment{};
            segment.startFrame =
                command.payload.setVoiceVariableWeight.startFrame;
            segment.endFrame =
                command.payload.setVoiceVariableWeight.endFrame;
            segment.startWeight =
                command.payload.setVoiceVariableWeight.startWeight;
            segment.endWeight =
                command.payload.setVoiceVariableWeight.endWeight;
            segment.segmentGeneration = command.payload.
                setVoiceVariableWeight.segmentGeneration;
            segment.decision = {
                command.payload.setVoiceVariableWeight.decisionSlot,
                command.payload.setVoiceVariableWeight.
                    decisionGeneration};
            segment.bank = {
                command.payload.setVoiceVariableWeight.bankSlot,
                command.payload.setVoiceVariableWeight.bankGeneration};
            (void)m_voices.ApplyVariableWeightSegment(
                command.payload.setVoiceVariableWeight.handle,
                segment);
        } else if (command.flags == 0) {
            m_voices.ApplySetVolume(
                command.payload.setVoiceVolume.handle,
                command.payload.setVoiceVolume.volumeDb,
                command.payload.setVoiceVolume.rampMs,
                command.payload.setVoiceVolume.outputOnly != 0);
        }
        break;
    case kOpcodeSetVoicePitch:
        m_voices.ApplySetPitch(command.payload.setVoicePitch.handle,
                               command.payload.setVoicePitch.ratio,
                               command.payload.setVoicePitch.rampMs);
        break;
    case kOpcodeSetVoiceFilter:
        m_voices.ApplySetFilter(
            command.payload.setVoiceFilter.handle,
            command.payload.setVoiceFilter.filterKind,
            command.payload.setVoiceFilter.enable != 0,
            command.payload.setVoiceFilter.cutoffHz,
            command.payload.setVoiceFilter.q,
            RestoreBiquadCoeffs(command.payload.setVoiceFilter.coeffs),
            command.payload.setVoiceFilter.tailFrames,
            command.payload.setVoiceFilter.transitionFrames);
        break;
    case kOpcodeSetVoicePosition:
        m_voices.ApplySetPosition(
            command.payload.setVoicePosition.handle,
            command.payload.setVoicePosition.x,
            command.payload.setVoicePosition.y,
            command.payload.setVoicePosition.z);
        break;
    case kOpcodeSetVoicePan:
        m_voices.ApplySetPan(command.payload.setVoicePan.handle,
                             command.payload.setVoicePan.pan,
                             command.payload.setVoicePan.gain0,
                             command.payload.setVoicePan.gain1,
                             command.payload.setVoicePan.channel0,
                             command.payload.setVoicePan.channel1,
                             command.payload.setVoicePan.pan2d);
        if (command.payload.setVoicePan.hasWidth != 0) {
            m_voices.ApplySetWidth(command.payload.setVoicePan.handle,
                                   command.payload.setVoicePan.width);
        }
        break;
    case kOpcodeSetVoiceSpatialScalar:
        if (command.payload.setVoiceSpatialScalar.kind ==
            kSpatialScalarKindWidth) {
            m_voices.ApplySetWidth(
                command.payload.setVoiceSpatialScalar.handle,
                command.payload.setVoiceSpatialScalar.value);
        } else if (command.payload.setVoiceSpatialScalar.kind ==
                   kSpatialScalarKindSpatializationMix) {
            m_voices.ApplySetSpatializationMix(
                command.payload.setVoiceSpatialScalar.handle,
                command.payload.setVoiceSpatialScalar.value);
        }
        break;
    case kOpcodeSetVoiceLowShelf:
        m_voices.ApplySetShelf(
            command.payload.setVoiceShelf.handle,
            command.payload.setVoiceShelf.shelfKind,
            command.payload.setVoiceShelf.enable != 0,
            command.payload.setVoiceShelf.frequencyHz,
            command.payload.setVoiceShelf.q,
            command.payload.setVoiceShelf.gainDb,
            RestoreBiquadCoeffs(command.payload.setVoiceShelf.coeffs),
            command.payload.setVoiceShelf.tailFrames,
            command.payload.setVoiceShelf.transitionFrames);
        break;
    case kOpcodeSetVoiceEffectBypass:
        m_voices.ApplySetVoiceEffectBypass(
            command.payload.setVoiceEffectBypass.handle,
            command.payload.setVoiceEffectBypass.slot,
            command.payload.setVoiceEffectBypass.bypass,
            command.payload.setVoiceEffectBypass.action,
            command.payload.setVoiceEffectBypass.expectedEffectType);
        break;
    case kOpcodeSetVoiceEffect:
        m_voices.ApplySetVoiceEffect(
            command.payload.setVoiceEffect.handle,
            command.payload.setVoiceEffect.slot,
            command.payload.setVoiceEffect.effectType,
            command.payload.setVoiceEffect.poolIndex,
            command.payload.setVoiceEffect.poolGeneration);
        break;
    case kOpcodeClearVoiceEffect:
        m_voices.ApplyClearVoiceEffect(
            command.payload.clearVoiceEffect.handle,
            command.payload.clearVoiceEffect.slot);
        break;
    case kOpcodeSetVoiceEffectParam:
        if (command.flags == kCommandFlagPreparedCompressor) {
            const auto& payload =
                command.payload.setCompressorEffectParam;
            if (payload.effectType == VN_EFFECT_COMPRESSOR) {
                m_voices.ApplySetVoiceCompressorParameters(
                    static_cast<VnVoiceHandle>(payload.host),
                    payload.slot, payload.effectType,
                    RestoreCompressorParameters(payload),
                    payload.transitionFrames);
            }
        } else if (command.flags == 0 &&
                   command.payload.setVoiceEffectParam.effectType !=
                       VN_EFFECT_COMPRESSOR) {
            m_voices.ApplySetVoiceEffectParam(
                command.payload.setVoiceEffectParam.handle,
                command.payload.setVoiceEffectParam.slot,
                command.payload.setVoiceEffectParam.paramIndex,
                command.payload.setVoiceEffectParam.value,
                command.payload.setVoiceEffectParam.effectType,
                RestoreBiquadCoeffs(
                    command.payload.setVoiceEffectParam.coeffs),
                command.payload.setVoiceEffectParam.tailFrames,
                command.payload.setVoiceEffectParam.transitionFrames);
        }
        break;
    case kOpcodeSetVoiceSend:
        m_voices.ApplySetVoiceSend(
            command.payload.setVoiceSend.handle,
            command.payload.setVoiceSend.sendSlot,
            command.payload.setVoiceSend.targetBusIndex,
            command.payload.setVoiceSend.levelDb,
            command.payload.setVoiceSend.targetBusGeneration);
        break;
    case kOpcodeClearVoiceSend:
        m_voices.ApplyClearVoiceSend(
            command.payload.clearVoiceSend.handle,
            command.payload.clearVoiceSend.sendSlot);
        break;
    case kOpcodeSetVoiceSendLevel:
        m_voices.ApplySetVoiceSendLevel(
            command.payload.setVoiceSendLevel.handle,
            command.payload.setVoiceSendLevel.sendSlot,
            command.payload.setVoiceSendLevel.levelDb,
            command.payload.setVoiceSendLevel.rampMs);
        break;
    case kOpcodeSetBusEffectParam:
        if (command.flags == kCommandFlagPreparedCompressor) {
            const auto& payload = command.payload.setCompressorEffectParam;
            const BusSlotRef busRef = BusSlotRef::FromPacked(payload.host);
            if (payload.effectType == VN_EFFECT_COMPRESSOR && busRef.Valid()) {
                m_buses.ApplySetBusCompressorParameters(
                    busRef.index, payload.slot, payload.effectType,
                    RestoreCompressorParameters(payload),
                    payload.transitionFrames, busRef.generation);
            }
        } else if (command.flags == 0 &&
                   command.payload.setBusEffectParam.effectType !=
                       VN_EFFECT_COMPRESSOR) {
            m_buses.ApplySetBusEffectParam(
                command.payload.setBusEffectParam.busIndex,
                command.payload.setBusEffectParam.slot,
                command.payload.setBusEffectParam.paramIndex,
                command.payload.setBusEffectParam.value,
                command.payload.setBusEffectParam.effectType,
                RestoreBiquadCoeffs(
                    command.payload.setBusEffectParam.coeffs),
                command.payload.setBusEffectParam.tailFrames,
                command.payload.setBusEffectParam.transitionFrames,
                command.payload.setBusEffectParam.busGeneration);
        }
        break;
    case kOpcodeSetBusFilter:
        m_buses.ApplySetBusFilter(
            command.payload.setBusFilter.busIndex,
            command.payload.setBusFilter.filterKind,
            command.payload.setBusFilter.enable != 0,
            command.payload.setBusFilter.cutoffHz,
            command.payload.setBusFilter.q,
            RestoreBiquadCoeffs(command.payload.setBusFilter.coeffs),
            command.payload.setBusFilter.tailFrames,
            command.payload.setBusFilter.transitionFrames,
            command.payload.setBusFilter.outputOnly != 0,
            command.payload.setBusFilter.busGeneration);
        break;
    default:
        (void)blockIndex;
        break;
    }
}

MixerLoop::CurrentSliceStagePlan MixerLoop::StageCurrentSlices(
    std::uint64_t blockIndex) noexcept {
    CurrentSliceStagePlan plan{};
    m_stagedCurrentSliceCount = 0;
    if (m_timelinePool == nullptr) {
        return plan;
    }
    const auto emptyFrontier =
        m_timelinePool->ConsumerFrontier().load(std::memory_order_acquire);
    (void)m_timelinePool->TryAdvancePublishedEmptyPrefixRt(emptyFrontier, blockIndex);
    plan.expectedFrontier =
        m_timelinePool->ConsumerFrontier().load(std::memory_order_acquire);
    const std::size_t capacity = EffectiveCurrentSliceCapacity();
    std::uint64_t page = plan.expectedFrontier;
    while (plan.stagedPageCount < 2 && page <= blockIndex) {
        if (m_timelinePool->PageStamp(page) != page) {
            plan.stopReason = TimelinePagePrefixStopReason::Unpublished;
            return plan;
        }
        const std::size_t remaining = capacity - m_stagedCurrentSliceCount;
        ConsumedCurrentSlice* destination =
            remaining == 0
                ? nullptr
                : m_currentSliceScratch.data() + m_stagedCurrentSliceCount;
        const std::size_t count = m_timelinePool->ConsumeCurrentSlicesRt(
            page, destination, remaining);
        if (count > remaining ||
            (count != 0 && m_currentSliceScratch.data() == nullptr)) {
            plan.stopReason = TimelinePagePrefixStopReason::ScratchCapacity;
            return plan;
        }
        m_stagedCurrentSliceCount += count;
        plan.sliceEndAfterPage[plan.stagedPageCount++] =
            m_stagedCurrentSliceCount;
        if (page == UINT64_MAX) {
            plan.stopReason =
                TimelinePagePrefixStopReason::ReachedCurrentQuantum;
            return plan;
        }
        ++page;
    }
    plan.stopReason =
        plan.stagedPageCount == 2 && page <= blockIndex
            ? TimelinePagePrefixStopReason::PageLimit
            : TimelinePagePrefixStopReason::ReachedCurrentQuantum;
    return plan;
}

void MixerLoop::ApplyConsumedCurrentSlices(std::uint64_t blockIndex,
                                           const TimelineExecutionResult& result) noexcept {
#if !defined(HARKEN_RT_QUALIFICATION)
    if (m_currentSliceFrontierHookForTest != nullptr) {
        m_currentSliceFrontierHookForTest(m_currentSliceFrontierHookContextForTest,
            CurrentSliceHookPhaseForTest::AfterExecuteBeforeApply, blockIndex,
            m_stagedCurrentSliceCount, m_activeCurrentSlicePlan.stagedPageCount,
            m_activeCurrentSlicePlan.stopReason);
    }
#endif
    ApplyStagedCurrentSlices(blockIndex, m_activeCurrentSlicePlan, result);
    m_currentSlicesAppliedInExecutor = true;
}

void MixerLoop::InitializeVariableSliceConsumptionStorage() {
    const auto capacity = m_variableSlices == nullptr ? 0u : m_variableSlices->Capacity();
    m_pendingSliceConsumptions.resize(capacity);
    m_pendingSliceOrder.resize(capacity);
}

std::size_t MixerLoop::VariableSliceConsumptionStorageBytes() const noexcept {
    return m_pendingSliceConsumptions.capacity() * sizeof(PendingSliceConsumption) +
        m_pendingSliceOrder.capacity() * sizeof(std::uint32_t);
}

void MixerLoop::ApplyStagedCurrentSlices(
    std::uint64_t blockIndex, const CurrentSliceStagePlan& plan,
    const TimelineExecutionResult& result) noexcept {
    if (m_variableSlices == nullptr && m_nativeStartCalendarConsumer == nullptr) {
        m_stagedCurrentSliceCount = 0;
        return;
    }
    const std::uint32_t appliedPages =
        std::min(result.consumedPageCount, plan.stagedPageCount);
    const std::size_t appliedSliceCount =
        appliedPages == 0 ? 0 : plan.sliceEndAfterPage[appliedPages - 1];
    for (std::size_t i = 0; i < appliedSliceCount; ++i) {
        const ConsumedCurrentSlice& consumed = m_currentSliceScratch[i];
        if (consumed.kind == CurrentSliceKind::NativeStartCalendar) {
            if (m_nativeStartCalendarConsumer != nullptr)
                (void)m_nativeStartCalendarConsumer(m_nativeStartCalendarContext, consumed);
            continue;
        }
        if (m_variableSlices == nullptr) continue;
        if (consumed.kind != CurrentSliceKind::VariableBankCut &&
            consumed.kind != CurrentSliceKind::MeterProbeWithdrawal) continue;
        if (consumed.slice.slot >= m_pendingSliceConsumptions.size()) continue;
        auto& pending = m_pendingSliceConsumptions[consumed.slice.slot];
        assert(!pending.active || pending.consumed.slice.generation == consumed.slice.generation ||
            m_variableSlices->State({pending.consumed.slice.slot, pending.consumed.slice.generation}) ==
                VariableSliceState::Free);
        pending = {true, consumed};
    }
    std::size_t pendingCount = 0;
    for (std::uint32_t i = 0; i < m_pendingSliceConsumptions.size(); ++i) {
        if (m_pendingSliceConsumptions[i].active) m_pendingSliceOrder[pendingCount++] = i;
    }
    std::sort(m_pendingSliceOrder.begin(), m_pendingSliceOrder.begin() + pendingCount,
        [&](std::uint32_t a, std::uint32_t b) {
            const auto& lhs = m_pendingSliceConsumptions[a].consumed;
            const auto& rhs = m_pendingSliceConsumptions[b].consumed;
            if (lhs.executeFrame != rhs.executeFrame) return lhs.executeFrame < rhs.executeFrame;
            if (lhs.logicCommitSequence != rhs.logicCommitSequence)
                return lhs.logicCommitSequence < rhs.logicCommitSequence;
            return lhs.executionOrdinal < rhs.executionOrdinal;
        });
    for (std::size_t i = 0; i < pendingCount; ++i) {
        auto& pending = m_pendingSliceConsumptions[m_pendingSliceOrder[i]];
        const auto& consumed = pending.consumed;
        const VariableSliceRef ref{consumed.slice.slot, consumed.slice.generation};
        struct ApplyContext {
            MixerLoop* mixer;
            const ConsumedCurrentSlice* consumed;
        } applyContext{this, &consumed};
        (void)m_variableSlices->Apply(
            ref, blockIndex + 1, &applyContext,
            [](void* context, const VariableSliceTarget& target) noexcept {
                MixerLoop* mixer = static_cast<ApplyContext*>(context)->mixer;
                switch (target.locator.scope) {
                case VariableTargetScope::Voice:
                case VariableTargetScope::VoiceEffect:
                case VariableTargetScope::Bus:
                case VariableTargetScope::BusEffect:
                    (void)mixer;
                    return true;
                }
                return false;
            },
            [](void* context, const VariableSliceTarget& target) noexcept {
                const auto& apply = *static_cast<ApplyContext*>(context);
                MixerLoop* mixer = apply.mixer;
                bool live = false;
                switch (target.locator.scope) {
                case VariableTargetScope::Voice:
                case VariableTargetScope::VoiceEffect:
                    live = mixer->m_voices.Resolve(
                               static_cast<VnVoiceHandle>(
                                   target.locator.object)) != nullptr;
                    break;
                case VariableTargetScope::Bus:
                case VariableTargetScope::BusEffect:
                    live = mixer->m_buses.AudioGenerationAt(
                               static_cast<std::uint32_t>(
                                   target.locator.object)) ==
                           target.locator.objectGeneration;
                    break;
                }
                if (live) {
                    if (apply.consumed->kind == CurrentSliceKind::VariableBankCut &&
                        mixer->m_voices.DeferVariableCommandUntilFirstOutput(target.command,
                            {apply.consumed->executeFrame, apply.consumed->logicCommitSequence,
                             apply.consumed->executionOrdinal}, target.targetRevision, target.nativeEffect,
                            target.toneBand)) return;
                    mixer->ApplyCommandRecord(target.command, 0);
                }
            }, m_commands.SettledFrontier());
        const auto state = m_variableSlices->State(ref);
        if (state != VariableSliceState::PageCommitted && state != VariableSliceState::AudioClaimed)
            pending.active = false;
    }
    m_stagedCurrentSliceCount = 0;
}

bool MixerLoop::RetryPendingControlAck(std::uint64_t blockIndex) noexcept {
    if (m_pendingControlAck.owner == nullptr) return true;
    if (blockIndex == UINT64_MAX) return false;
    using Status = TimelineArmTable::ControlCommandAckStatus;
    const auto status = m_pendingControlAck.owner != m_timelineArms ||
        m_timelineArms->Incarnation() != m_pendingControlAck.incarnation ? Status::Invalid :
        m_timelineArms->AcknowledgeControlCommand(m_pendingControlAck.token,
            m_pendingControlAck.originalCompletionSequence, blockIndex + 1u);
    if (status == Status::DeferredReservation) return false;
    if (status == Status::Invalid) ++m_commandProtocolViolations;
    auto* originalRange = m_pendingControlAck.originalRange;
    m_pendingControlAck = {};
    m_pendingControlOriginActor.Release();
    if (originalRange != nullptr) {
        if (status == Status::Stale) ++m_commandProtocolViolations;
        using Disposition = CommandQueue::OriginalControlDisposition;
        const auto disposition = status == Status::Cancelled ? Disposition::Cancelled :
            (status == Status::Invalid || status == Status::Stale ? Disposition::Invalid :
                Disposition::Settled);
        const bool completed = m_commands.CompleteOriginalControlHeaderRange(
            *originalRange, disposition);
        assert(completed);
        if (!completed) ++m_commandProtocolViolations;
    }
    return true;
}

bool MixerLoop::DrainOriginalControlHeader(std::uint64_t blockIndex,
    std::uint64_t& owed, std::uint32_t& applied) noexcept {
    if (blockIndex == UINT64_MAX || owed == 0) return false;
    auto* range = m_commands.OriginalRangeAtFront();
    if (range == nullptr || range->Kind() != CommandQueue::OriginalRangeKind::ControlHeader)
        return false;
    assert(!m_pendingControlOriginActor.Valid());
    const auto acquired = m_commands.BorrowOriginalFrontControlActor(*range,
        &m_pendingControlOriginActor);
    if (acquired == CommandQueue::NativeControlActorStatus::Refused) return false;
    if (acquired == CommandQueue::NativeControlActorStatus::Issued &&
        !m_pendingControlOriginActor.BeforeFirstWrite()) {
        m_pendingControlOriginActor.Release();
        return false;
    }
    const auto* owner = range->ControlOwner();
    CommandRecord actual{};
    const bool intact = m_commands.Peek(actual) && range->MatchesOriginalControlHeader(actual);
    const bool validOwner = owner != nullptr && owner->Valid() && owner->Owner() == m_timelineArms;
    const auto token = validOwner ? owner->Token() : TimelineArmToken{};
    if (!m_commands.TryPop(actual)) {
        m_pendingControlOriginActor.Release();
        return false;
    }
    --owed;
    ++applied;
    if (!intact || !validOwner) {
        ++m_commandProtocolViolations;
        m_pendingControlOriginActor.Release();
        const bool completed = m_commands.CompleteOriginalControlHeaderRange(
            *range, CommandQueue::OriginalControlDisposition::Invalid);
        assert(completed);
        return completed;
    }
    m_pendingControlAck = {m_timelineArms, m_timelineArms->Incarnation(), token,
        blockIndex + 1u, range};
    return RetryPendingControlAck(blockIndex);
}

void MixerLoop::FinishControlHeaderAckAfterQuiescence() noexcept {
    auto* originalRange = m_pendingControlAck.originalRange;
    m_pendingControlAck = {};
    m_pendingControlOriginActor.Release();
    if (originalRange != nullptr) {
        const bool completed = m_commands.CompleteOriginalControlHeaderRange(
            *originalRange, CommandQueue::OriginalControlDisposition::QuiescedDiscard);
        assert(completed);
        (void)completed;
    }
}

bool MixerLoop::DrainOriginalSource(std::uint64_t blockIndex, std::uint64_t& owed,
    std::uint32_t& applied, std::uint32_t remaining) noexcept {
    using Registry = OriginalSourceSubmissionRegistry;
    auto& registry = *m_originalSources;
    auto* row = registry.pending_;
    if (row == nullptr) {
        row = registry.BorrowFront();
        if (row == nullptr || blockIndex == UINT64_MAX || row->count_ > remaining ||
            row->count_ > owed) return false;
        // This is the actual frozen front range. Acquire before disposition,
        // row proof, installation proof, metadata, queue pop or Begin writes.
        if (!registry.IssueFrontRangeActor(*row)) return false;
        if (row->rangeActor_.Valid() && !row->rangeActor_.BeforeFirstWrite()) return false;
        bool intact = true;
        for (std::uint32_t i = 0; i < row->count_; ++i) {
            CommandRecord wire{};
            if (!m_commands.PeekAt(i, wire) ||
                std::memcmp(&wire, &row->body_[i], sizeof(wire)) != 0) intact = false;
        }
        row->disposition_ = intact ? Registry::Disposition::Apply : Registry::Disposition::Invalid;
        if (intact && !m_voices.OriginalVoiceReleased(row->voice_)) {
            const auto status = registry.arms_.ProvePendingSourceCommand(
                row->token_, &row->rowProof_, row);
            if (status == TimelineArmTable::SourceCommandRowStatus::DeferredReservation) return false;
            if (status == TimelineArmTable::SourceCommandRowStatus::Ready) {
                if ((row->installPublication_.Valid() &&
                     !TimelineArmTable::MatchesPublicationAfterInstallation(row->rowProof_, row->installPublication_)) ||
                    (row->groupInstallPublication_.Valid() &&
                     !TimelineArmTable::MatchesGroupedPublicationAfterInstallation(row->rowProof_, row->groupInstallPublication_))) {
                    row->rowProof_.Release();
                    row->disposition_ = Registry::Disposition::Cancel;
                } else {
                    const auto install = m_voices.ProveSourceCommandInstallation(
                        row->rowProof_, &row->installProof_, &row->voice_);
                    if (install == VoicePool::SourceCommandInstallStatus::DeferredInstallation) {
                        row->rowProof_.Release();
                        return false;
                    }
                    if (install != VoicePool::SourceCommandInstallStatus::Ready) {
                        row->rowProof_.Release();
                        row->disposition_ = Registry::Disposition::Cancel;
                    }
                }
            } else row->disposition_ = Registry::Disposition::Cancel;
        } else if (intact) row->disposition_ = Registry::Disposition::Cancel;
        if (row->disposition_ != Registry::Disposition::Apply) {
            const auto status = registry.arms_.ProveOriginalSourceCancellation(
                row->arm_, row, &row->rowProof_);
            if (status != TimelineArmTable::SourceCommandRowStatus::Ready) return false;
        }
        row->completion_ = blockIndex + 1u;
        row->progress_ = 0;
        row->phase_.store(Registry::Phase::ConsumerOwned, std::memory_order_release);
        registry.pending_ = row;
        m_commandBatchDiscardRemaining = 0;
        if (!intact && !row->protocolReported_) {
            ++m_commandProtocolViolations;
            row->protocolReported_ = true;
        }
    }
    if (row->count_ - row->progress_ > remaining ||
        row->count_ - row->progress_ > owed) return false;
    const auto defer = [&]() noexcept {
        row->phase_.store(Registry::Phase::CleanupPending, std::memory_order_release);
        return false;
    };
    // Cancel must finish actual physical disposal before Core tombstones its row.
    if (row->disposition_ != Registry::Disposition::Apply && !row->rowProof_.Valid()) {
        const auto status = registry.arms_.ProveOriginalSourceCancellation(row->arm_, row, &row->rowProof_);
        if (status != TimelineArmTable::SourceCommandRowStatus::Ready) return defer();
    }
    if (row->disposition_ != Registry::Disposition::Apply &&
        !m_voices.OriginalVoiceReleased(row->voice_)) {
        const auto disposed = m_voices.TryDisposeOriginalTimelineVoice(row->voice_);
        if (disposed != VoicePool::OriginalVoiceDisposal::Released &&
            disposed != VoicePool::OriginalVoiceDisposal::AlreadyReleased) return defer();
    }
    while (row->progress_ < row->count_) {
        const auto& command = row->body_[row->progress_];
        const bool applying = row->disposition_ == Registry::Disposition::Apply;
        // Every supported metadata payload begins with its full voice handle.
        const auto target = command.flags == kCommandFlagPreparedCompressor
            ? static_cast<VnVoiceHandle>(command.payload.setCompressorEffectParam.host)
            : command.payload.setVoiceVolume.handle;
        const bool applyMetadata = row->progress_ != 0 &&
            (applying || (row->disposition_ == Registry::Disposition::Cancel &&
                          target != row->voice_.RenderHandle()));
        VoicePool::SourceCommandMetadataAccess access(row->installProof_, applyMetadata);
        if (applyMetadata) m_voices.SupersedeDeferredFirstOutput(command);
        if (row->progress_ == 0) {
            if (applying) VoicePool::BeginSourceCommandInstallation(row->installProof_,
                command.payload.play.sourceIdentity, command.payload.play.sourceStartFrame, blockIndex);
        } else if (command.opcode == kOpcodeSetVoiceEffect) {
            const auto& effect = command.payload.setVoiceEffect;
            const EffectInstanceRef incoming{effect.effectType, effect.poolIndex, effect.poolGeneration};
            std::uint32_t owned = 0;
            while (owned < row->incomingCount_ && !Registry::SameEffect(incoming, row->incoming_[owned])) ++owned;
            assert(owned < row->incomingCount_);
            const bool handedOff = applyMetadata
                ? m_voices.ApplyOwnedVoiceEffect(effect.handle, effect.slot, incoming)
                : registry.effects_.RetireQueue().TryPush(incoming);
            if (!handedOff) return defer();
            row->incomingDone_[owned] = true;
        } else if (applyMetadata && command.opcode == kOpcodeClearVoiceEffect) {
            if (!m_voices.ApplyOwnedClearVoiceEffect(command.payload.clearVoiceEffect.handle,
                    command.payload.clearVoiceEffect.slot)) return defer();
        } else if (applyMetadata) ApplyCommandRecord(command, blockIndex);
        access.Release();
        CommandRecord discarded{};
        const bool popped = m_commands.TryPop(discarded);
        assert(popped);
        if (!popped) return defer();
        ++row->progress_;
        --owed;
        ++applied;
    }
    for (std::uint32_t i = 0; i < row->incomingCount_; ++i) {
        if (row->incomingDone_[i]) continue;
        if (!registry.effects_.RetireQueue().TryPush(row->incoming_[i])) return defer();
        row->incomingDone_[i] = true;
    }
    if (row->disposition_ == Registry::Disposition::Apply) {
        TimelineArmTable::AcknowledgeSourceCommand(row->rowProof_, row->completion_);
        VoicePool::FinishSourceCommandInstallation(row->installProof_);
        row->originalInstallationCompleted_.store(true, std::memory_order_release);
        if (row->groupInstallPublication_.Valid()) {
            using Result = TimelineArmTable::GroupedInstallationPublicationResult;
            const auto result = TimelineArmTable::ConsumeGroupedPublicationAfterInstallation(
                row->rowProof_, row->groupInstallPublication_);
            if (result != Result::Published) {
                if (result == Result::Refused && !row->protocolReported_) {
                    ++m_commandProtocolViolations;
                    row->protocolReported_ = true;
                }
                // RequestCancel adjudicates any live slice claim after this
                // Source reservation returns; physical cleanup may need another callback.
                row->disposition_ = Registry::Disposition::Cancel;
                row->rowProof_.Release();
                return defer();
            }
        }
        if (row->installPublication_.Valid()) {
            const bool published = TimelineArmTable::ConsumePublicationAfterInstallation(
                row->rowProof_, row->installPublication_);
            assert(published);
            (void)published;
        }
    } else {
        if (blockIndex == UINT64_MAX) return defer();
        TimelineArmTable::TombstoneOriginalSourceCommand(row->rowProof_, blockIndex + 1u);
    }
    row->rowProof_.Release();
    const bool complete = m_commands.CompleteOriginalRange(row->range_);
    assert(complete);
    (void)complete;
    registry.pending_ = nullptr;
    row->phase_.store(Registry::Phase::Disposed, std::memory_order_release);
    registry.CompleteOriginalDisposition(*row);
    // All effects, metadata, ACK/tombstone and the real range are complete.
    // A native callback actor can already hold the installed self-loan port.
    row->rangeActor_.Release();
    return true;
}

std::uint32_t MixerLoop::DrainCommands(std::uint64_t blockIndex,
                                       std::uint64_t publishedHead) {
    CommandRecord command;
    std::uint64_t pendingBatchRevision = 0;
    std::uint32_t pendingBatchCommands = 0;
    std::uint64_t pendingArmToken = 0;
    bool pendingControlArm = false;
    VnVoiceHandle pendingControlRender = 0;
    VnVoiceHandle pendingNativeStart = 0;
    const std::uint64_t sourceCompletion = blockIndex == UINT64_MAX ? 0 : blockIndex + 1u;
    TimelineArmTable::SourceCommandRowProof pendingSourceRow;
    VoicePool::SourceCommandInstallProof pendingSourceInstall;
    // Work this pass owes: fixed at entry, so records pushed while it runs
    // belong to a later quantum and a producer cannot extend this one.
    std::uint64_t owed = publishedHead - m_commands.ConsumedCount();
    const auto deferredCount = [&]() noexcept {
        const auto total = owed + (m_pendingControlAck.owner != nullptr ? 1u : 0u) +
            (m_originalSources != nullptr && m_originalSources->pending_ != nullptr ? 1u : 0u);
        return static_cast<std::uint32_t>(total > 0xffffffffull ? 0xffffffffull : total);
    };
    if (!RetryPendingControlAck(blockIndex)) return deferredCount();
    std::uint32_t applied = 0;
    if (m_originalSources != nullptr && m_originalSources->pending_ != nullptr &&
        !DrainOriginalSource(blockIndex, owed, applied, m_maxCommandsPerQuantum)) return deferredCount();
    while (owed != 0) {
        const std::uint32_t remaining =
            m_maxCommandsPerQuantum > applied
                ? m_maxCommandsPerQuantum - applied
                : 0;
        if (remaining == 0) {
            break;
        }
        if (const auto* range = m_commands.OriginalRangeAtFront()) {
            if (range->Kind() == CommandQueue::OriginalRangeKind::ControlHeader) {
                if (!DrainOriginalControlHeader(blockIndex, owed, applied)) break;
            } else if (m_originalSources == nullptr ||
                !DrainOriginalSource(blockIndex, owed, applied, remaining)) break;
            continue;
        }
        CommandRecord seekHeader{};
        if (pendingBatchCommands == 0 &&
            m_commandBatchDiscardRemaining == 0 &&
            m_commands.Peek(seekHeader) &&
            seekHeader.opcode == kOpcodeSeekVoice) {
            const auto& header = seekHeader.payload.seekVoice;
            const std::uint32_t batchCount = header.batchCommandCount;
            const bool countValid = batchCount != 0 &&
                                    batchCount <= m_seekResolvedFrames.size() &&
                                    batchCount <= m_maxCommandsPerQuantum &&
                                    header.batchOrdinal == 0;
            if (countValid &&
                (batchCount > remaining ||
                 static_cast<std::uint64_t>(batchCount) > owed)) {
                break;
            }

            VnResult refusal = VN_OK;
            bool protocolValid = countValid && header.ticket != 0 &&
                                 (header.mode == VN_ACTION_SEEK_ABSOLUTE ||
                                  header.mode == VN_ACTION_SEEK_RELATIVE);
            for (std::uint32_t i = 0;
                 protocolValid && i < batchCount; ++i) {
                CommandRecord item{};
                if (!m_commands.PeekAt(i, item)) {
                    protocolValid = false;
                    break;
                }
                const auto& seek = item.payload.seekVoice;
                protocolValid = item.opcode == kOpcodeSeekVoice &&
                                seek.ticket == header.ticket &&
                                seek.batchCommandCount == batchCount &&
                                seek.batchOrdinal == i &&
                                (seek.mode == VN_ACTION_SEEK_ABSOLUTE ||
                                 seek.mode == VN_ACTION_SEEK_RELATIVE);
                for (std::uint32_t earlier = 0;
                     protocolValid && earlier < i; ++earlier) {
                    CommandRecord previous{};
                    protocolValid = m_commands.PeekAt(earlier, previous) &&
                                    previous.payload.seekVoice.handle !=
                                        seek.handle;
                }
                if (protocolValid) {
                    m_seekStreamHandoffs[i] = {};
                    const StreamSeekHandoffSnapshot* stream = nullptr;
                    if (seek.streamHandoffToken != 0) {
                        if (seek.mode != VN_ACTION_SEEK_ABSOLUTE ||
                            !m_streamSeekHandoffs.CanApply(
                                seek.streamHandoffToken, seek.ticket,
                                seek.handle, &m_seekStreamHandoffs[i])) {
                            refusal = VN_CAPABILITY_UNAVAILABLE;
                            protocolValid = false;
                            break;
                        }
                        stream = &m_seekStreamHandoffs[i];
                    }
                    refusal = m_voices.CanApplySeek(
                        seek.handle, seek.mode, seek.frameValue,
                        &m_seekResolvedFrames[i], stream);
                    protocolValid = refusal == VN_OK;
                }
            }
            if (!protocolValid && refusal == VN_OK) {
                refusal = VN_INVALID_ARG;
                ++m_commandProtocolViolations;
            }

            bool apply = false;
            if (protocolValid) {
                apply = m_actionSeeks.TryBeginApply(header.ticket);
                if (!apply) {
                    (void)m_actionSeeks.FinishRequestedCancellation(
                        header.ticket);
                }
            } else if (!m_actionSeeks.CancelAcceptedFromAudio(
                           header.ticket, refusal)) {
                (void)m_actionSeeks.FinishRequestedCancellation(
                    header.ticket);
            }

            const std::uint32_t discardCount =
                countValid ? batchCount : 1u;
            std::uint64_t minimumFrame = UINT64_MAX;
            std::uint64_t maximumFrame = 0;
            for (std::uint32_t i = 0; i < discardCount; ++i) {
                CommandRecord item{};
                if (!m_commands.TryPop(item)) break;
                --owed;
                ++applied;
                if (apply) {
                    const std::uint64_t frame = m_seekResolvedFrames[i];
                    const auto token =
                        item.payload.seekVoice.streamHandoffToken;
                    m_voices.CommitSeek(
                        item.payload.seekVoice.handle, frame,
                        token == 0 ? nullptr : &m_seekStreamHandoffs[i]);
                    if (token != 0) {
                        const bool transferred =
                            m_streamSeekHandoffs.MarkApplied(
                                token, header.ticket);
                        assert(transferred);
                        (void)transferred;
                    }
                    minimumFrame = std::min(minimumFrame, frame);
                    maximumFrame = std::max(maximumFrame, frame);
                } else if (item.payload.seekVoice.streamHandoffToken != 0) {
                    (void)m_streamSeekHandoffs.CancelFromAudio(
                        item.payload.seekVoice.streamHandoffToken,
                        header.ticket);
                }
            }
            if (apply) {
                (void)m_actionSeeks.FinishApplied(
                    header.ticket, blockIndex + 1u, minimumFrame,
                    maximumFrame);
            }
            continue;
        }
        // Atomic batches are a consumer-side state machine: the header carries
        // the count and the acknowledgement fires when it reaches zero. Cutting
        // inside one would apply half a topology transaction, so the whole batch
        // waits for a quantum that can take it. Admission refuses a batch larger
        // than this same ceiling, so waiting always terminates.
        //
        // The owed clause is defensive and unreachable: a batch becomes visible
        // to this thread through one release store of the producer's head, so a
        // visible header implies every record of its batch is visible too --
        // there is no publisher that makes a batch half-visible. It stays
        // because the cost is one comparison and the failure it would absorb is
        // applying a header without its body; deferring silently is the right
        // response on this thread either way, so it carries no assertion.
        std::uint32_t skipBatchCommands = 0;
        if (m_commandBatchDiscardRemaining != 0) {
            const std::uint64_t visible = std::min<std::uint64_t>(
                owed, static_cast<std::uint64_t>(remaining));
            skipBatchCommands = static_cast<std::uint32_t>(
                std::min<std::uint64_t>(m_commandBatchDiscardRemaining,
                                        visible));
        } else if (pendingBatchCommands == 0) {
            CommandRecord ahead;
            if (m_commands.Peek(ahead)) {
                std::uint32_t batchCount = 1;
                bool playLed = false;
                bool stopLed = false;
                bool invalid = false;
                if (ahead.opcode == kOpcodeSetBusTopologyPlan) {
                    batchCount =
                        ahead.payload.setBusTopologyPlan.batchCommandCount;
                } else if (ahead.opcode == kOpcodePlay) {
                    const PlayLedBatchHeader header = ahead.payload.play.batch;
                    if (header.batchKind == PlayLedBatchKind::TimelineControlArm &&
                        blockIndex == UINT64_MAX) break;
                    if (header.batchKind == PlayLedBatchKind::None) {
                        invalid = header.batchVersion != kPlayLedBatchVersion ||
                                  header.reserved != 0 ||
                                  header.batchCommandCount != 1 ||
                                  header.armToken != 0;
                    } else {
                        playLed = true;
                        batchCount = header.batchCommandCount;
                        invalid =
                            header.batchVersion != kPlayLedBatchVersion ||
                            !IsPlayLedKind(header.batchKind) ||
                            header.reserved != 0 || batchCount == 0 ||
                            batchCount > m_maxCommandsPerQuantum ||
                            batchCount > m_commands.Capacity() ||
                            ((header.batchKind ==
                                  PlayLedBatchKind::TimelineArm ||
                              header.batchKind ==
                                  PlayLedBatchKind::TimelineControlArm)
                                 ? header.armToken == 0
                                 : header.armToken != 0) ||
                            (header.batchKind ==
                                 PlayLedBatchKind::TimelineControlArm &&
                             batchCount != 1);
                    }
                    if (m_voices.HasPendingNativeVariableStart(ahead.payload.play.handle) &&
                        header.batchKind != PlayLedBatchKind::DirectStart &&
                        header.batchKind != PlayLedBatchKind::DeferredStart &&
                        header.batchKind != PlayLedBatchKind::ParkReturn) invalid = true;
                } else if (ahead.opcode == kOpcodeStop) {
                    const PlayLedBatchHeader header = ahead.payload.stop.batch;
                    if (header.batchKind == PlayLedBatchKind::None) {
                        invalid = header.batchVersion != 0 ||
                                  header.reserved != 0 ||
                                  header.batchCommandCount != 0 ||
                                  header.armToken != 0;
                    } else {
                        stopLed = true;
                        batchCount = header.batchCommandCount;
                        invalid =
                            header.batchVersion != kPlayLedBatchVersion ||
                            header.batchKind != PlayLedBatchKind::EmitterStop ||
                            header.reserved != 0 || batchCount == 0 ||
                            batchCount > m_maxCommandsPerQuantum ||
                            batchCount > m_commands.Capacity() ||
                            header.armToken != 0;
                    }
                }
                const bool batched =
                    ahead.opcode == kOpcodeSetBusTopologyPlan || playLed ||
                    stopLed;
                if (batched && !invalid && batchCount > 1 &&
                    (batchCount > remaining ||
                     static_cast<std::uint64_t>(batchCount) > owed)) {
                    break;
                }
                if (playLed || stopLed) {
                    for (std::uint32_t i = 1; !invalid && i < batchCount; ++i) {
                        CommandRecord body;
                        if (!m_commands.PeekAt(i, body)) {
                            invalid = true;
                            break;
                        }
                        if (playLed &&
                            (!PlayLedBodyOpcodeAllowed(
                                 ahead.payload.play.batch.batchKind,
                                 body.opcode) ||
                             (body.opcode == kOpcodePlay ||
                              body.opcode == kOpcodeStop))) {
                            invalid = true;
                        }
                        if (stopLed &&
                            (body.opcode != kOpcodeStop ||
                             body.payload.stop.batch.batchVersion != 0 ||
                             body.payload.stop.batch.batchKind !=
                                 PlayLedBatchKind::None ||
                             body.payload.stop.batch.reserved != 0 ||
                             body.payload.stop.batch.batchCommandCount != 0 ||
                             body.payload.stop.batch.armToken != 0)) {
                            invalid = true;
                        }
                    }
                }
                if (!invalid && playLed && m_timelineArms != nullptr &&
                    ahead.payload.play.batch.batchKind == PlayLedBatchKind::TimelineArm) {
                    // A zero completion cannot acknowledge a real Source command.
                    if (sourceCompletion == 0) break;
                    const auto packed = ahead.payload.play.batch.armToken;
                    const auto render = ahead.payload.play.handle;
                    const TimelineArmToken token{static_cast<std::uint32_t>(packed),
                        static_cast<std::uint32_t>(packed >> 32),
                        static_cast<std::uint32_t>(render),
                        static_cast<std::uint32_t>(render >> 32)};
                    const auto rowStatus = m_timelineArms->ProvePendingSourceCommand(token, &pendingSourceRow);
                    if (rowStatus == TimelineArmTable::SourceCommandRowStatus::DeferredReservation) break;
                    if (rowStatus == TimelineArmTable::SourceCommandRowStatus::RequiresOriginalSubmission) {
                        // Registered sources require their actual ring association.
                        // Never fall through to packed-token ACK or generation-only disposal.
                        break;
                    }
                    if (rowStatus == TimelineArmTable::SourceCommandRowStatus::Ready) {
                        const auto installStatus = m_voices.ProveSourceCommandInstallation(
                            pendingSourceRow, &pendingSourceInstall);
                        if (installStatus == VoicePool::SourceCommandInstallStatus::DeferredInstallation) break;
                        if (installStatus != VoicePool::SourceCommandInstallStatus::Ready)
                            pendingSourceRow.Release();
                    }
                }
                if (invalid) {
                    if (ahead.opcode == kOpcodePlay)
                        m_voices.RejectNativeVariableStart(ahead.payload.play.handle, m_notifications, blockIndex);
#ifndef NDEBUG
                    assert(m_suppressCommandProtocolAssertForTest &&
                           "invalid atomic command batch");
#endif
                    ++m_commandProtocolViolations;
                    m_commandBatchDiscardRemaining = static_cast<std::uint32_t>(
                        batchCount == 0
                            ? 1
                            : std::min<std::uint64_t>(batchCount, owed));
                    const std::uint64_t visible = std::min<std::uint64_t>(
                        owed, static_cast<std::uint64_t>(remaining));
                    skipBatchCommands = static_cast<std::uint32_t>(
                        std::min<std::uint64_t>(m_commandBatchDiscardRemaining,
                                                visible));
                } else if (playLed || stopLed) {
                    pendingBatchCommands = batchCount;
                    pendingArmToken =
                        playLed ? ahead.payload.play.batch.armToken : 0;
                    pendingControlArm = playLed && ahead.payload.play.batch.batchKind ==
                        PlayLedBatchKind::TimelineControlArm;
                    pendingControlRender = pendingControlArm ? ahead.payload.play.handle : 0;
                    pendingNativeStart = playLed && !pendingControlArm &&
                        ahead.payload.play.batch.batchKind != PlayLedBatchKind::TimelineArm
                        ? ahead.payload.play.handle : 0;
                }
            }
        }
        if (skipBatchCommands != 0) {
            for (std::uint32_t i = 0; i < skipBatchCommands; ++i) {
                if (m_commands.OriginalRangeAtFront() != nullptr) {
                    m_commandBatchDiscardRemaining = 0;
                    break;
                }
                if (!m_commands.TryPop(command)) {
                    break;
                }
                --owed;
                ++applied;
                if (m_commandBatchDiscardRemaining != 0) {
                    --m_commandBatchDiscardRemaining;
                }
            }
            pendingBatchCommands = 0;
            continue;
        }
        if (!m_commands.TryPop(command)) {
            break;
        }
        --owed;
        ++applied;
        std::uint64_t topologyApplyRevision = 0;
        if (command.opcode == kOpcodeSetBusTopologyPlan) {
            topologyApplyRevision =
                command.payload.setBusTopologyPlan.acceptedGraphRevision;
            const std::uint32_t batchCount =
                command.payload.setBusTopologyPlan.batchCommandCount;
            if (batchCount > 1) {
                pendingBatchRevision = topologyApplyRevision;
                pendingBatchCommands = batchCount;
                topologyApplyRevision = 0;
            }
        }
        // Only this bounded metadata dispatch can access the held source.
        VoicePool::SourceCommandMetadataAccess sourceMetadataAccess(
            pendingSourceInstall, command.opcode != kOpcodePlay);
        // A later ordinary command supersedes the deferred value for its physical endpoint.
        m_voices.SupersedeDeferredFirstOutput(command, pendingNativeStart);
        switch (command.opcode) {
        case kOpcodePlay:
            if (pendingSourceInstall.Valid()) {
                VoicePool::BeginSourceCommandInstallation(pendingSourceInstall,
                    command.payload.play.sourceIdentity,
                    command.payload.play.sourceStartFrame, blockIndex);
            } else if (command.payload.play.batch.batchKind ==
                PlayLedBatchKind::TimelineArm) {
                m_voices.ApplyTimelineGatedPlay(
                    command.payload.play.handle,
                    command.payload.play.sourceIdentity,
                    command.payload.play.sourceStartFrame,
                    command.payload.play.batch.armToken,
                    blockIndex);
            } else if (command.payload.play.batch.batchKind ==
                       PlayLedBatchKind::TimelineControlArm) {
            } else {
                m_voices.ApplyPlay(command.payload.play.handle,
                                   command.payload.play.sourceIdentity,
                                   command.payload.play.sourceStartFrame,
                                   blockIndex);
            }
            break;
        case kOpcodeStop:
            m_voices.ApplyStop(command.payload.stop.handle,
                               command.payload.stop.fadeMs,
                               command.payload.stop.fadeFrames, m_notifications,
                               blockIndex, command.payload.stop.mode,
                               command.payload.stop.publicHandle,
                               command.payload.stop.parkRequestSequence);
            break;
        case kOpcodeSetBusVolume:
            m_buses.ApplyVolume(command.payload.setBusVolume.busIndex,
                                command.payload.setBusVolume.volumeDb,
                                command.payload.setBusVolume.rampMs,
                                command.payload.setBusVolume.busGeneration,
                                command.payload.setBusVolume.outputOnly != 0);
            break;
        case kOpcodeSetVoiceVolume:
            if (command.flags == kCommandFlagVariableWeightSegment) {
                WeightSegmentV1 segment{};
                segment.startFrame =
                    command.payload.setVoiceVariableWeight.startFrame;
                segment.endFrame =
                    command.payload.setVoiceVariableWeight.endFrame;
                segment.startWeight =
                    command.payload.setVoiceVariableWeight.startWeight;
                segment.endWeight =
                    command.payload.setVoiceVariableWeight.endWeight;
                segment.segmentGeneration = command.payload.
                    setVoiceVariableWeight.segmentGeneration;
                segment.decision = {
                    command.payload.setVoiceVariableWeight.decisionSlot,
                    command.payload.setVoiceVariableWeight.
                        decisionGeneration};
                segment.bank = {
                    command.payload.setVoiceVariableWeight.bankSlot,
                    command.payload.setVoiceVariableWeight.bankGeneration};
                (void)m_voices.ApplyVariableWeightSegment(
                    command.payload.setVoiceVariableWeight.handle,
                    segment);
            } else if (command.flags == 0) {
                m_voices.ApplySetVolume(
                    command.payload.setVoiceVolume.handle,
                    command.payload.setVoiceVolume.volumeDb,
                    command.payload.setVoiceVolume.rampMs,
                    command.payload.setVoiceVolume.outputOnly != 0);
            }
            break;
        case kOpcodeSetVoicePitch:
            m_voices.ApplySetPitch(command.payload.setVoicePitch.handle,
                                   command.payload.setVoicePitch.ratio,
                                   command.payload.setVoicePitch.rampMs);
            break;
        case kOpcodeSetVoiceFilter:
            m_voices.ApplySetFilter(
                command.payload.setVoiceFilter.handle,
                command.payload.setVoiceFilter.filterKind,
                command.payload.setVoiceFilter.enable != 0,
                command.payload.setVoiceFilter.cutoffHz,
                command.payload.setVoiceFilter.q,
                RestoreBiquadCoeffs(command.payload.setVoiceFilter.coeffs),
                command.payload.setVoiceFilter.tailFrames,
                command.payload.setVoiceFilter.transitionFrames);
            break;
        case kOpcodeSetVoicePosition:
            m_voices.ApplySetPosition(
                command.payload.setVoicePosition.handle,
                command.payload.setVoicePosition.x,
                command.payload.setVoicePosition.y,
                command.payload.setVoicePosition.z);
            break;
        case kOpcodeSetVoicePan:
            m_voices.ApplySetPan(command.payload.setVoicePan.handle,
                                 command.payload.setVoicePan.pan,
                                 command.payload.setVoicePan.gain0,
                                 command.payload.setVoicePan.gain1,
                                 command.payload.setVoicePan.channel0,
                                 command.payload.setVoicePan.channel1,
                                 command.payload.setVoicePan.pan2d);
            if (command.payload.setVoicePan.hasWidth != 0) {
                m_voices.ApplySetWidth(command.payload.setVoicePan.handle,
                                       command.payload.setVoicePan.width);
            }
            break;
        case kOpcodeSetVoiceSpatialScalar:
            if (command.payload.setVoiceSpatialScalar.kind ==
                kSpatialScalarKindWidth) {
                m_voices.ApplySetWidth(
                    command.payload.setVoiceSpatialScalar.handle,
                    command.payload.setVoiceSpatialScalar.value);
            } else if (command.payload.setVoiceSpatialScalar.kind ==
                       kSpatialScalarKindSpatializationMix) {
                m_voices.ApplySetSpatializationMix(
                    command.payload.setVoiceSpatialScalar.handle,
                    command.payload.setVoiceSpatialScalar.value);
            }
            break;
        case kOpcodeSetVoiceLowShelf:
            m_voices.ApplySetShelf(
                command.payload.setVoiceShelf.handle,
                command.payload.setVoiceShelf.shelfKind,
                command.payload.setVoiceShelf.enable != 0,
                command.payload.setVoiceShelf.frequencyHz,
                command.payload.setVoiceShelf.q,
                command.payload.setVoiceShelf.gainDb,
                RestoreBiquadCoeffs(command.payload.setVoiceShelf.coeffs),
                command.payload.setVoiceShelf.tailFrames,
                command.payload.setVoiceShelf.transitionFrames);
            break;
        case kOpcodeSetVoiceEffectBypass:
            m_voices.ApplySetVoiceEffectBypass(
                command.payload.setVoiceEffectBypass.handle,
                command.payload.setVoiceEffectBypass.slot,
                command.payload.setVoiceEffectBypass.bypass,
                command.payload.setVoiceEffectBypass.action,
                command.payload.setVoiceEffectBypass.expectedEffectType);
            break;
        case kOpcodeSetVoiceEffect:
            m_voices.ApplySetVoiceEffect(
                command.payload.setVoiceEffect.handle,
                command.payload.setVoiceEffect.slot,
                command.payload.setVoiceEffect.effectType,
                command.payload.setVoiceEffect.poolIndex,
                command.payload.setVoiceEffect.poolGeneration);
            break;
        case kOpcodeClearVoiceEffect:
            m_voices.ApplyClearVoiceEffect(
                command.payload.clearVoiceEffect.handle,
                command.payload.clearVoiceEffect.slot);
            break;
        case kOpcodeSetVoiceEffectParam:
            if (command.flags == kCommandFlagPreparedCompressor) {
                const auto& payload =
                    command.payload.setCompressorEffectParam;
                if (payload.effectType == VN_EFFECT_COMPRESSOR) {
                    m_voices.ApplySetVoiceCompressorParameters(
                        static_cast<VnVoiceHandle>(payload.host),
                        payload.slot, payload.effectType,
                        RestoreCompressorParameters(payload),
                        payload.transitionFrames);
                }
            } else if (command.flags == 0 &&
                       command.payload.setVoiceEffectParam.effectType !=
                           VN_EFFECT_COMPRESSOR) {
                m_voices.ApplySetVoiceEffectParam(
                    command.payload.setVoiceEffectParam.handle,
                    command.payload.setVoiceEffectParam.slot,
                    command.payload.setVoiceEffectParam.paramIndex,
                    command.payload.setVoiceEffectParam.value,
                    command.payload.setVoiceEffectParam.effectType,
                    RestoreBiquadCoeffs(
                        command.payload.setVoiceEffectParam.coeffs),
                    command.payload.setVoiceEffectParam.tailFrames,
                    command.payload.setVoiceEffectParam.transitionFrames);
            }
            break;
        case kOpcodeSetBusTopologyPlan:
            m_buses.ApplyTopologyPlan(
                command.payload.setBusTopologyPlan.planIndex,
                command.payload.setBusTopologyPlan.connectFrom,
                command.payload.setBusTopologyPlan.connectTo,
                command.payload.setBusTopologyPlan.connectLevelDb,
                topologyApplyRevision);
            m_dynamicRoutes.InstallPlan(
                command.payload.setBusTopologyPlan.dynamicRoutePlanIndex);
            // The same swap that tombstones destroyed slots migrates every
            // voice off them (dry bus to the nearest alive ancestor, dead
            // send targets deactivated), so no voice ever folds into a dead
            // slot's planes — the destroy cascade's audio-side half.
            m_voices.ApplyBusTombstones(m_buses.InstalledAlive(),
                                        m_buses.InstalledParents(),
                                        m_buses.AudioBusCount(),
                                        m_buses.InstalledGenerations(),
                                        m_buses.InstalledMigrationTargets());
            {
                const std::uint32_t planIndex =
                    command.payload.setBusTopologyPlan.planIndex;
                if (m_installAckRing != nullptr &&
                    m_installAckSideTable != nullptr &&
                    planIndex < m_installAckSideTableCapacity &&
                    m_installAckSideTable[planIndex].valid == 1u) {
                    const auto& identity = m_installAckSideTable[planIndex];
                    ProjectGenerationInstallAckV1 record{};
                    record.planIndex = planIndex;
                    record.zero = 0;
                    record.slotGeneration = identity.slotGeneration;
                    record.planSequence = identity.planSequence;
                    record.planFingerprint = identity.planFingerprint;
                    record.claimSetFingerprint = identity.claimSetFingerprint;
                    m_installAckRing->Push(record);
                }
            }
            break;
        case kOpcodeSetBusSendLevel:
            m_buses.ApplySetBusSendLevel(
                command.payload.setBusSendLevel.fromBus,
                command.payload.setBusSendLevel.toBus,
                command.payload.setBusSendLevel.levelDb,
                command.payload.setBusSendLevel.rampMs,
                command.payload.setBusSendLevel.fromGeneration,
                command.payload.setBusSendLevel.toGeneration);
            break;
        case kOpcodeSetVoiceSend:
            m_voices.ApplySetVoiceSend(
                command.payload.setVoiceSend.handle,
                command.payload.setVoiceSend.sendSlot,
                command.payload.setVoiceSend.targetBusIndex,
                command.payload.setVoiceSend.levelDb,
                command.payload.setVoiceSend.targetBusGeneration);
            break;
        case kOpcodeClearVoiceSend:
            m_voices.ApplyClearVoiceSend(
                command.payload.clearVoiceSend.handle,
                command.payload.clearVoiceSend.sendSlot);
            break;
        case kOpcodeSetVoiceSendLevel:
            m_voices.ApplySetVoiceSendLevel(
                command.payload.setVoiceSendLevel.handle,
                command.payload.setVoiceSendLevel.sendSlot,
                command.payload.setVoiceSendLevel.levelDb,
                command.payload.setVoiceSendLevel.rampMs);
            break;
        case kOpcodeSetBusEffect:
            m_buses.ApplySetBusEffect(command.payload.setBusEffect.busIndex,
                                      command.payload.setBusEffect.slot,
                                      command.payload.setBusEffect.effectType,
                                      command.payload.setBusEffect.poolIndex,
                                      command.payload.setBusEffect.poolGeneration,
                                      command.payload.setBusEffect.busGeneration);
            if (command.payload.setBusEffect.topologyPlanIndex !=
                kInvalidTopologyPlanIndex) {
                m_buses.ApplyTopologyPlan(
                    command.payload.setBusEffect.topologyPlanIndex,
                    kInvalidBusIndex, kInvalidBusIndex, 0.0f);
            }
            m_dynamicRoutes.InstallPlan(
                command.payload.setBusEffect.dynamicRoutePlanIndex);
            break;
        case kOpcodeClearBusEffect:
            m_buses.ApplyClearBusEffect(command.payload.clearBusEffect.busIndex,
                                        command.payload.clearBusEffect.slot,
                                        command.payload.clearBusEffect.busGeneration);
            if (command.payload.clearBusEffect.topologyPlanIndex !=
                kInvalidTopologyPlanIndex) {
                m_buses.ApplyTopologyPlan(
                    command.payload.clearBusEffect.topologyPlanIndex,
                    kInvalidBusIndex, kInvalidBusIndex, 0.0f);
            }
            m_dynamicRoutes.InstallPlan(
                command.payload.clearBusEffect.dynamicRoutePlanIndex);
            break;
        case kOpcodeSetBusEffectParam:
            if (command.flags == kCommandFlagPreparedCompressor) {
                const auto& payload =
                    command.payload.setCompressorEffectParam;
                const BusSlotRef busRef = BusSlotRef::FromPacked(payload.host);
                if (payload.effectType == VN_EFFECT_COMPRESSOR &&
                    busRef.Valid()) {
                    m_buses.ApplySetBusCompressorParameters(
                        busRef.index, payload.slot, payload.effectType,
                        RestoreCompressorParameters(payload),
                        payload.transitionFrames, busRef.generation);
                }
            } else if (command.flags == 0 &&
                       command.payload.setBusEffectParam.effectType !=
                           VN_EFFECT_COMPRESSOR) {
                m_buses.ApplySetBusEffectParam(
                    command.payload.setBusEffectParam.busIndex,
                    command.payload.setBusEffectParam.slot,
                    command.payload.setBusEffectParam.paramIndex,
                    command.payload.setBusEffectParam.value,
                    command.payload.setBusEffectParam.effectType,
                    RestoreBiquadCoeffs(
                        command.payload.setBusEffectParam.coeffs),
                    command.payload.setBusEffectParam.tailFrames,
                    command.payload.setBusEffectParam.transitionFrames,
                    command.payload.setBusEffectParam.busGeneration);
            }
            break;
        case kOpcodeSetBusEffectBypass:
            m_buses.ApplySetBusEffectBypass(
                command.payload.setBusEffectBypass.busIndex,
                command.payload.setBusEffectBypass.slot,
                command.payload.setBusEffectBypass.bypass,
                command.payload.setBusEffectBypass.action,
                command.payload.setBusEffectBypass.expectedEffectType,
                command.payload.setBusEffectBypass.busGeneration);
            break;
        case kOpcodeSetBusFaderPoint:
            m_buses.ApplySetBusFaderPoint(
                command.payload.setBusFaderPoint.busIndex,
                command.payload.setBusFaderPoint.faderAfterSlot,
                command.payload.setBusFaderPoint.busGeneration);
            break;
        case kOpcodeSetBusFilter:
            m_buses.ApplySetBusFilter(
                command.payload.setBusFilter.busIndex,
                command.payload.setBusFilter.filterKind,
                command.payload.setBusFilter.enable != 0,
                command.payload.setBusFilter.cutoffHz,
                command.payload.setBusFilter.q,
                RestoreBiquadCoeffs(command.payload.setBusFilter.coeffs),
                command.payload.setBusFilter.tailFrames,
                command.payload.setBusFilter.transitionFrames,
                command.payload.setBusFilter.outputOnly != 0,
                command.payload.setBusFilter.busGeneration);
            break;
        default:
            // Unknown opcodes cannot be produced by this API vintage;
            // skipping keeps a corrupt record from becoming undefined
            // behavior.
            break;
        }
        sourceMetadataAccess.Release();
        if (pendingBatchCommands != 0) {
            --pendingBatchCommands;
            if (pendingBatchCommands == 0) {
                if (pendingNativeStart != 0) {
                    if (!m_voices.FinishNativeVariableStartInstallation(pendingNativeStart))
                        m_voices.ApplyStop(pendingNativeStart, 0, 0, m_notifications, blockIndex);
                    pendingNativeStart = 0;
                }
                if (pendingBatchRevision != 0) {
                    m_buses.AcknowledgeInstalledGraphRevision(
                        pendingBatchRevision);
                    pendingBatchRevision = 0;
                }
                if (pendingArmToken != 0 && m_timelineArms != nullptr) {
                    if (pendingSourceInstall.Valid()) {
                        TimelineArmTable::AcknowledgeSourceCommand(pendingSourceRow, sourceCompletion);
                        VoicePool::FinishSourceCommandInstallation(pendingSourceInstall);
                        pendingSourceRow.Release();
                    } else if (pendingControlArm) {
                        m_pendingControlAck = {m_timelineArms, m_timelineArms->Incarnation(),
                            {static_cast<std::uint32_t>(pendingArmToken),
                                static_cast<std::uint32_t>(pendingArmToken >> 32),
                                static_cast<std::uint32_t>(pendingControlRender),
                                static_cast<std::uint32_t>(pendingControlRender >> 32)},
                            blockIndex + 1u};
                        if (!RetryPendingControlAck(blockIndex)) break;
                    } else if (!m_timelineArms->AcknowledgeApplied(pendingArmToken,
                                                            blockIndex + 1u)) {
                        TimelineArmToken token{};
                        const bool tokenResolved =
                            m_timelineArms->ResolveIdentity(pendingArmToken,
                                                            &token);
                        ArmState state = ArmState::Free;
                        if (tokenResolved &&
                            m_timelineArms->State(token, &state) &&
                            state == ArmState::CancelRequested) {
                            if (m_voices.CancelTimelineBindingFromAudio(
                                    token.renderSlot,
                                    token.renderGeneration)) {
                                (void)m_timelineArms->Tombstone(token,
                                                                blockIndex);
                            }
                        } else if (tokenResolved &&
                                   m_timelineArms->State(token, &state) &&
                                   state != ArmState::Tombstoned) {
                            ++m_commandProtocolViolations;
                        }
                    }
                    pendingArmToken = 0;
                }
            }
        }
    }
    if (m_pendingControlAck.owner == nullptr) m_commands.PublishSettledFrontier();
    return deferredCount();
}

void MixerLoop::DrainQueuedCommands(std::uint32_t quantumCount) {
    if (quantumCount == 0) {
        return;
    }
    memguard::AudioThreadScope audioScope;
    for (std::uint32_t i = 0; i < quantumCount; ++i) {
        m_logRing.BeginQuantum();
        m_notifications.BeginQuantum();
        m_voices.BeginAudioBlock(m_blocksRendered.load(std::memory_order_relaxed));
        (void)DrainCommands(m_blocksRendered.load(std::memory_order_relaxed),
                            m_commands.PublishedHead());
        // Serialized drain returns its real readers without an audio fence.
        m_voices.EndAudioBlock();
    }
}

void MixerLoop::RenderBlock(float* interleavedOut, std::uint32_t frames,
                            std::uint32_t channels) {
    floatmode::ScopedFlushDenormals floatModeScope;
    memguard::AudioThreadScope audioScope;

    if (interleavedOut == nullptr || frames == 0 || channels == 0) {
        return;
    }

    // Open the quantum for the transports this block produces into. This is the
    // only place it can go: a device callback renders as many blocks as its
    // frame count spans, so resetting at callback entry would give one budget to
    // several blocks. Resetting here gives each block its own, which is what
    // "per quantum" has to mean. Both resets are a single store and neither can
    // fail, so they sit ahead of every early return below that would otherwise
    // leave a block's production counted against its predecessor.
    m_logRing.BeginQuantum();
    m_notifications.BeginQuantum();
    harken::monitor::OnAudioQuantum();

    // Per-stage CPU timing (spec-mandated in Debug and Release alike): one
    // QPC read per stage boundary, nothing else. The stage windows share
    // their boundary reads, so the published per-stage sum can never exceed
    // the published whole. Boundary placement: the output-silence memset
    // rides in the drain window and the arena/bus-plane setup rides in the
    // voices window — each window owns the setup immediately ahead of its
    // named work, rather than spending extra clock reads on sub-microsecond
    // slices.
    const std::uint64_t ticksEntry = CpuClockTicks();

    std::memset(interleavedOut, 0,
                static_cast<std::size_t>(frames) * channels * sizeof(float));

    const std::uint64_t blockIndex =
        m_blocksRendered.load(std::memory_order_relaxed);

    // Commands apply at the block boundary, before any rendering, so a
    // queued change is either fully in effect for this block or not at all.
    m_voices.BeginAudioBlock(blockIndex);
    // Fix the producer's position before draining: anything pushed from here on
    // is this block's business no longer, so the pass cannot be extended by a
    // producer that keeps writing while it runs.
    const std::uint64_t publishedHead = m_commands.PublishedHead();
    m_commandsDeferred = DrainCommands(blockIndex, publishedHead);
    if (m_commandsDeferred > m_commandsDeferredHighWater) {
        m_commandsDeferredHighWater = m_commandsDeferred;
    }
    if (m_commandsDeferred == 0) {
        // Caught up: the episode is over, so a later one is reportable again.
        m_consecutiveDeferredBlocks = 0;
        m_deferralEpisodeReported = false;
    } else {
        ++m_consecutiveDeferredBlocks;
        if (m_consecutiveDeferredBlocks >= kDeferralEpisodeBlocks &&
            !m_deferralEpisodeReported) {
            // Once per episode. Commands still apply, just later, so a record
            // per block would bury the fact under its own repetition.
            LogRecord record{};
            record.level = VN_LOG_WARN;
            record.code = kLogCodeCommandBudgetBehind;
            record.arg0 = m_commandsDeferred;
            record.arg1 = m_consecutiveDeferredBlocks;
            m_logRing.Push(record);
            m_deferralEpisodeReported = true;
        }
    }
    m_dynamicRoutes.BeginBlock();

    const std::uint64_t ticksDrained = CpuClockTicks();

    m_arena.Reset();
    const std::uint64_t overflowsBefore = m_arena.OverflowCount();

    // Planar accumulators for every live bus. If they do not all fit the
    // arena the block stays silent — the overflow counter and a degradation
    // notification record it; nothing allocates and nothing crashes. The
    // audio-visible count is read here (after DrainCommands, so a create's
    // plan swap this block is already in effect): a bus not yet swapped in has
    // no plane and is not folded, and no voice can be routed to it because its
    // routing command necessarily drains after that swap.
    const std::uint32_t busCount = m_buses.AudioBusCount();
    bool planesOk = true;
    for (std::uint32_t i = 0; i < busCount * m_mixChannels; ++i) {
        m_busPlanes[i] = m_arena.AllocFloats(frames);
        if (m_busPlanes[i] == nullptr) {
            planesOk = false;
            break;
        }
    }

    const CurrentSliceStagePlan currentSlicePlan =
        StageCurrentSlices(blockIndex);
    m_activeCurrentSlicePlan = currentSlicePlan;
    m_currentSlicesAppliedInExecutor = false;
#if !defined(HARKEN_RT_QUALIFICATION)
    if (m_currentSliceFrontierHookForTest != nullptr) {
        m_currentSliceFrontierHookForTest(
            m_currentSliceFrontierHookContextForTest,
            CurrentSliceHookPhaseForTest::AfterStageBeforeExecute,
            blockIndex, m_stagedCurrentSliceCount,
            currentSlicePlan.stagedPageCount, currentSlicePlan.stopReason);
    }
#endif
    TimelineExecutionResult timelineResult{};
    timelineResult.frontier = currentSlicePlan.expectedFrontier;
    timelineResult.planStopReason = currentSlicePlan.stopReason;
    if (planesOk && m_timelineExecutor != nullptr &&
        frames == m_voices.BlockFrames()) {
        if (m_timelineSink != nullptr) {
            m_timelineSink->SetArenaAdmission(
                m_arena.CapacityBytes() >= m_arena.UsedBytes()
                    ? m_arena.CapacityBytes() - m_arena.UsedBytes()
                    : 0,
                frames);
        }
        const TimelineExecutionPlan executionPlan{
            currentSlicePlan.expectedFrontier,
            currentSlicePlan.stagedPageCount,
            currentSlicePlan.stopReason};
        timelineResult =
            m_timelineExecutor->ExecuteQuantum(blockIndex, executionPlan);
    }
    if (!m_currentSlicesAppliedInExecutor)
        ApplyConsumedCurrentSlices(blockIndex, timelineResult);
#if !defined(HARKEN_RT_QUALIFICATION)
    if (m_timelineFinalizedHookForTest != nullptr)
        m_timelineFinalizedHookForTest(m_timelineFinalizedHookContextForTest, blockIndex);
#endif
    m_voices.ActivatePendingNativeVariableStarts(m_notifications, blockIndex);

    std::uint64_t ticksVoices = ticksDrained;
    std::uint64_t ticksBuses = ticksDrained;
    if (planesOk) {
        const VoicePool::RenderAllResult voicesOutcome =
            m_voices.RenderAll(m_arena, m_busPlanes.data(), frames,
                               m_notifications, blockIndex,
                               m_jobDeadlineTicks,
                               static_cast<std::uint64_t>(blockIndex) * frames);
        if (voicesOutcome.batchAbandoned) {
            // The arena is this object's, so retiring the abandoned block's
            // descending span is this object's job. It happens inside the block
            // that abandoned, before the next Reset, so the next block's bus
            // planes and voice scratch are already allocated clear of anything a
            // stranded worker holds a pointer into.
            m_arena.QuarantineHighEnd();
            LogRecord record{};
            record.level = VN_LOG_WARN;
            record.code = kLogCodeRenderBatchAbandoned;
            record.arg0 = CpuTicksToMicros(0, voicesOutcome.overrunTicks,
                                           m_cpuTicksPerSecond);
            record.arg1 = voicesOutcome.quarantinedVoices;
            m_logRing.Push(record);
        }

        ticksVoices = CpuClockTicks();

        // Bottom-up bus fold; afterwards the master planes (m_busPlanes[0..
        // m_mixChannels)) hold the final mix at the mix layout.
        m_buses.MixDown(m_busPlanes.data(), frames, blockIndex,
                        static_cast<std::uint64_t>(blockIndex) * frames);
        // Lay the master's mix-width output into the device buffer at the
        // device boundary. Three cases; the first is the M4 path (stereo mix,
        // stereo device) and stays byte-identical.
        const std::uint32_t mixCh = m_mixChannels;
        const std::uint32_t devCh = m_deviceChannels;
        if (devCh == mixCh) {
            // Same layout: direct interleave.
            const std::uint32_t outChannels = std::min(channels, mixCh);
            for (std::uint32_t i = 0; i < frames; ++i) {
                for (std::uint32_t ch = 0; ch < outChannels; ++ch) {
                    interleavedOut[i * channels + ch] = m_busPlanes[ch][i];
                }
            }
        } else if (devCh < mixCh) {
            // Device narrower than the mix: the master->device output downmix,
            // through the master bus's matrix (the Appendix A default until
            // Vn_SetBusDownmixMatrix overrides it). Coefficients are applied as
            // given (no normalization); the device boundary is the clip point.
            const float* coeffs = m_buses.BusDownmixCoeffs(0);
            const std::uint32_t outChannels = std::min(channels, devCh);
            if (coeffs != nullptr) {
                for (std::uint32_t i = 0; i < frames; ++i) {
                    for (std::uint32_t r = 0; r < outChannels; ++r) {
                        float acc = 0.0f;
                        for (std::uint32_t c = 0; c < mixCh; ++c) {
                            acc += coeffs[r * 8 + c] * m_busPlanes[c][i];
                        }
                        interleavedOut[i * channels + r] = acc;
                    }
                }
            }
        } else {
            // Device wider than the mix: name-place each mix channel into the
            // device channel of matching role; unmatched device channels stay
            // silent (already zeroed by the output memset).
            ChannelRole mixRoles[kMaxChannels];
            LayoutRoles(m_mixLayout, mixRoles);
            int place[kMaxChannels];
            for (std::uint32_t c = 0; c < mixCh; ++c) {
                place[c] = ChannelOfRole(m_deviceLayout, mixRoles[c]);
            }
            for (std::uint32_t i = 0; i < frames; ++i) {
                for (std::uint32_t c = 0; c < mixCh; ++c) {
                    const int d = place[c];
                    if (d >= 0 && static_cast<std::uint32_t>(d) < channels) {
                        interleavedOut[i * channels + d] = m_busPlanes[c][i];
                    }
                }
            }
        }

        // Enforce the device-boundary postcondition after all layout paths;
        // no later stage may write to interleavedOut.
        const std::size_t outputSamples =
            static_cast<std::size_t>(frames) * channels;
        for (std::size_t i = 0; i < outputSamples; ++i) {
            interleavedOut[i] = ClampDeviceSample(interleavedOut[i]);
        }

        ticksBuses = CpuClockTicks();
    } else {
        if (m_timelineExecutor != nullptr) {
            m_timelineExecutor->NoteRenderUnavailable();
        }
        m_voices.AdvanceArenaSilentStreamParks(frames);
        m_voices.StageCompletionProgressForSilentBlock(blockIndex);
        // A whole-mixer arena shortfall skips RenderAll, but live Voice taps
        // still publish a valid zero block. Recovery therefore resumes as
        // ACTIVE|SOURCE_SILENT rather than fabricating a new FIRST_BLOCK.
        m_voices.PublishLiveRouteSilence();
    }
    // Voice tap banks advance even on an arena-degraded silent block, so a
    // missed render publishes silence rather than replaying an older block.
#if defined(HARKEN_RT_PROBE_MIX_PUBLICATION)
    rtprobe::TimelineRecord* publicationRecord =
        rtprobe::ReserveTimeline(rtprobe::TimelineKind::MixPublication);
    if (publicationRecord != nullptr) {
        rtprobe::ValidateCoordinator(*publicationRecord);
        publicationRecord->logicalIndex = blockIndex;
        publicationRecord->frames = frames;
        publicationRecord->channels = m_mixChannels;
        publicationRecord->beginTicks = CpuClockTicks();
    }
#endif
    m_dynamicRoutes.EndBlock(blockIndex);

    if (m_arena.OverflowCount() != overflowsBefore) {
        // One notification per degraded block, not one per failed allocation: the
        // block is the degradation unit the host can reason about.
        VnNotification notification{};
        notification.type = VN_NOTIFY_DEGRADATION_OCCURRED;
        notification.voiceHandle = 0;
        notification.blockTimestamp = blockIndex;
        m_notifications.Push(notification);
        // The matching log record. Every render-path scratch allocation
        // requests one block-length float plane (bus accumulators and
        // voice scratch alike), so the failed request size is uniform and
        // reportable from here without coupling the arena to the ring.
        LogRecord record{};
        record.level = VN_LOG_WARN;
        record.code = kLogCodeArenaOverflow;
        record.arg0 = static_cast<std::uint64_t>(frames) * sizeof(float);
        record.arg1 = blockIndex;
        m_logRing.Push(record);
    }

    // The whole-callback window closes here, ahead of the publication write:
    // a same-block publication cannot contain its own cost, and every stage
    // window lies inside [entry, wholeEnd], which is what keeps the stage-sum
    // invariant structural rather than statistical.
    const std::uint64_t ticksWholeEnd = CpuClockTicks();
    BlockTiming timing;
    timing.drainUs =
        CpuTicksToMicros(ticksEntry, ticksDrained, m_cpuTicksPerSecond);
    timing.voicesUs =
        CpuTicksToMicros(ticksDrained, ticksVoices, m_cpuTicksPerSecond);
    timing.busesUs =
        CpuTicksToMicros(ticksVoices, ticksBuses, m_cpuTicksPerSecond);
    timing.wholeUs =
        CpuTicksToMicros(ticksEntry, ticksWholeEnd, m_cpuTicksPerSecond);
    if (timing.wholeUs > m_audioCpuPeakUs) {
        m_audioCpuPeakUs = timing.wholeUs;
    }

    if (harken::monitor::TimeSeriesLiveArmed()) {
        harken::monitor::PublishTimeSeriesQuantum(
            timing.wholeUs, timing.drainUs, timing.voicesUs, timing.busesUs,
            m_arena.HighWaterBytes());
    }

    if (harken::monitor::AudioLiveArmed()) {
        harken::monitor::PublishMasterAudioChunk(
            interleavedOut, frames, channels, m_deviceLayout, m_sampleRate,
            m_buses.AudioGenerationAt(0),
            static_cast<std::uint64_t>(blockIndex) * frames);
    }

    PublishSnapshot(blockIndex, planesOk, timing);

    // Meter block ring append: one record carrying all three bus tap rows per
    // rendered block once any meter Variable exists. The existing busPeaks
    // and meter Variable view remains the Post Effects row. The armed gate is
    // one relaxed load, with zero work otherwise and bit-identical unarmed
    // behavior.
    // A degraded block never reached MixDown, so its record carries honest
    // silence, matching the snapshot's honesty rule. Lock-free,
    // allocation-free: the scratch row is preallocated at init.
#if defined(HARKEN_RT_PROBE_MIX_PUBLICATION)
    const bool meterArmed = m_meterRing.Armed();
    if (meterArmed) {
        if (publicationRecord != nullptr) {
            publicationRecord->flags |= rtprobe::kTimelineFlagMeterArmed;
        }
#else
    if (m_meterRing.Armed()) {
#endif
        const std::uint32_t meterBuses = m_buses.AudioBusCount();
        const std::uint32_t span =
            meterBuses < m_maxBuses ? meterBuses : m_maxBuses;
        float* const postEffectsPeaks =
            m_meterTapScratch.data() +
            MeterTapOffset(VN_BUS_METER_TAP_POST_EFFECTS,
                           m_maxBuses);
        float* const preFaderPeaks =
            m_meterTapScratch.data() +
            MeterTapOffset(VN_BUS_METER_TAP_PRE_FADER, m_maxBuses);
        float* const postFaderPeaks =
            m_meterTapScratch.data() +
            MeterTapOffset(VN_BUS_METER_TAP_POST_FADER, m_maxBuses);
        for (std::uint32_t i = 0; i < m_maxBuses; ++i) {
            const bool live = planesOk && i < span;
            preFaderPeaks[i] = live ? m_buses.PreFaderPeak(i) : 0.0f;
            postFaderPeaks[i] = live ? m_buses.PostFaderPeak(i) : 0.0f;
            postEffectsPeaks[i] = live ? m_buses.OutputPeak(i) : 0.0f;
            m_meterGenerationScratch[i] =
                i < span ? m_buses.AudioGenerationAt(i) : 0;
        }
        m_meterRing.PushTaps(blockIndex, preFaderPeaks, postFaderPeaks,
                             postEffectsPeaks, m_maxBuses,
                             m_meterGenerationScratch.data());
    }

    // Publish the per-slot source-cursor mirror for audibility parking's
    // timeline authority — one row per configured voice slot, stamped with this
    // block index, through the mirror's own seqlock (the fill writes every row,
    // so a slot not rendering this block reports inactive). Gated on the parking
    // config so a build with parking off does no extra audio-thread work and
    // stays bit-identical to the prior behavior.
    if (m_audibilityParkEnabled != 0) {
#if defined(HARKEN_RT_PROBE_MIX_PUBLICATION)
        if (publicationRecord != nullptr) {
            publicationRecord->flags |=
                rtprobe::kTimelineFlagSourceCursorPublished;
        }
#endif
        SourceCursorMirror::Row* cursorRows = m_sourceCursors.BeginWrite();
        m_voices.FillSourceCursors(cursorRows, m_sourceCursors.SlotCount());
        m_sourceCursors.EndWrite(blockIndex);
    }

    (void)m_voices.PublishCompletionProgress(blockIndex);
    // Source cursor and progress publication are the final payload users.
    m_voices.EndAudioBlock();
    const std::uint64_t renderedSequence = blockIndex + 1;
    m_blocksRendered.store(renderedSequence, std::memory_order_release);
#if defined(HARKEN_RT_PROBE_MIX_PUBLICATION)
    if (publicationRecord != nullptr) {
        publicationRecord->relatedIndex = renderedSequence;
        publicationRecord->endTicks = CpuClockTicks();
    }
#endif
}

void MixerLoop::PublishSnapshot(std::uint64_t blockIndex, bool blockRendered,
                                const BlockTiming& timing) {
    // Runs at the end of every rendered block, on the audio thread, under
    // the same no-heap/no-lock rules as the rest of the block: the writer
    // side of the double buffer is wait-free. Canary discipline: the
    // payload's blockIndex is written first and blockIndexEcho last, so a
    // capture whose canaries agree was not torn across the payload.
    SnapshotPayload& payload = m_snapshots.BeginWrite();
    payload.blockIndex = blockIndex;
    m_voices.GatherSnapshot(payload);
    // The rendered structure is the audio-visible one: a bus created this block
    // but whose swap has not applied is not yet part of the mix, so it is not
    // yet counted here.
    const std::uint32_t busCount = m_buses.AudioBusCount();
    payload.busCount = busCount;
    for (std::uint32_t i = 0; i < VN_SNAPSHOT_MAX_BUSES; ++i) {
        // A degraded block never reached MixDown, so its bus meters are
        // honest silence rather than the previous block's peaks.
        payload.busPeaks[i] =
            blockRendered ? m_buses.OutputPeak(i) : 0.0f;
        payload.busPeaksPreFader[i] =
            blockRendered ? m_buses.PreFaderPeak(i) : 0.0f;
        payload.busPeaksPostFader[i] =
            blockRendered ? m_buses.PostFaderPeak(i) : 0.0f;
        for (std::uint32_t channel = 0; channel < VN_MAX_CHANNELS; ++channel)
            payload.busChannelPeaks[i * VN_MAX_CHANNELS + channel] =
                blockRendered ? m_buses.OutputChannelPeak(i, channel) : 0.0f;
    }
    if (!blockRendered) {
        // Same honesty for the per-voice taps: nothing rendered, so the
        // published taps read silent even where slot contexts still hold
        // the previous block's measurements.
        for (VnVoiceSnapshot& entry : payload.voices) {
            entry.tapPostResample = 0.0f;
            entry.tapPostFx = 0.0f;
            entry.tapPostFilter = 0.0f;
            entry.tapPostGain = 0.0f;
        }
    }
    payload.arenaHighWaterBytes = m_arena.HighWaterBytes();
    payload.arenaOverflowCount = m_arena.OverflowCount();
    payload.voiceCompletedCount = m_voices.CompletedCount();
    payload.blocksRendered = blockIndex + 1;
    payload.audioCpuLastUs = timing.wholeUs;
    payload.audioCpuPeakUs = m_audioCpuPeakUs;
    payload.audioDrainLastUs = timing.drainUs;
    payload.audioVoicesLastUs = timing.voicesUs;
    payload.audioBusesLastUs = timing.busesUs;
    // Job-parallel worker busy peak (0 when serial). The pool tracks the peak
    // in performance-counter ticks; convert here with the cached tick rate,
    // the same passive clock read the timing septet uses.
    payload.renderWorkerBusyPeakUs = CpuTicksToMicros(
        0, m_voices.RenderWorkerBusyPeakTicks(), m_cpuTicksPerSecond);
    payload.dynamicTapGenerationMismatchCount =
        m_dynamicRoutes.AudioTapGenerationMismatchCount();
    payload.dynamicPlanPublicationCount =
        m_dynamicRoutes.AudioPlanPublicationCount();
    payload.dynamicPlanRetireOverflowCount =
        m_dynamicRoutes.AudioPlanRetireOverflowCount();
    payload.compressorDetectorNonFiniteCount =
        m_voices.AudioCompressorDetectorNonFiniteCount();
    payload.compressorDetectorSaturationCount =
        m_voices.AudioCompressorDetectorSaturationCount();
    payload.commandDeferredHighWater = m_commandsDeferredHighWater;
    payload.logQuantumBudgetDropCount =
        m_logRing.QuantumBudgetDropCount();
    payload.notificationQuantumBudgetDropCount =
        m_notifications.QuantumBudgetDropCount();
    payload.logQuantumProducedHighWater =
        m_logRing.QuantumProducedHighWater();
    payload.notificationQuantumProducedHighWater =
        m_notifications.QuantumProducedHighWater();
    if (m_timelineExecutor != nullptr) {
        const TimelineExecutor::Counters& timeline =
            m_timelineExecutor->Stats();
        payload.deadlineMissQuanta = timeline.deadlineMissQuanta;
        payload.lateConsumedEvents = timeline.lateConsumedEvents;
    }
    payload.blockIndexEcho = blockIndex;
    m_snapshots.EndWrite();
}

} // namespace harken
