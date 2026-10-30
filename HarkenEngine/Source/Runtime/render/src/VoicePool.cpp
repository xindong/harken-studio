#include "harken/render/VoicePool.h"
#include "harken/render/OriginalSourceSubmission.h"

#include "MonitorCaptureHub.h"

#include "harken/core/CommandQueue.h"
#include "harken/core/DecisionTable.h"
#include "harken/core/ResamplerAdvance.h"
#include "harken/render/BuiltinEffects.h"
#include "harken/render/VoiceTimelineEligibility.h"
#include "harken/dsp/GainRamp.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <utility>

namespace harken {
void VoicePool::SlotStorageRegistration::Detach() noexcept {
    if (owner_ == nullptr) return;
    if (previous_ != nullptr) previous_->next_ = next_;
    else owner_->m_slotStorageRegistrations = next_;
    if (next_ != nullptr) next_->previous_ = previous_;
    owner_ = nullptr;
    previous_ = next_ = nullptr;
    context_ = nullptr;
    prepare_ = nullptr;
}

bool VoicePool::AttachSlotStoragePreparation(SlotStorageRegistration& registration,
    void* context, SlotStorageRegistration::Prepare prepare) noexcept {
    if (memguard::IsAudioTagged() || prepare == nullptr || registration.owner_ != nullptr ||
        !prepare(context, m_slots.ConstructedCount())) return false;
    registration.owner_ = this;
    registration.context_ = context;
    registration.prepare_ = prepare;
    registration.next_ = m_slotStorageRegistrations;
    if (m_slotStorageRegistrations != nullptr) m_slotStorageRegistrations->previous_ = &registration;
    m_slotStorageRegistrations = &registration;
    return true;
}

bool VoicePool::FirstOutputBackingReady(std::uint32_t count) const noexcept {
    return count <= m_slots.Capacity() &&
        m_firstOutputEndpoints.ConstructedCount() >= count * m_firstOutputStride &&
        m_firstOutputWeights.ConstructedCount() >= count * kSelectionPathNodeCapacity &&
        m_pendingTimelineVariableEvents.ConstructedCount() >= count;
}

bool VoicePool::PrepareFirstOutputBacking(std::uint32_t count) noexcept {
    if (FirstOutputBackingReady(count)) return true;
    if (count > m_slots.Capacity() || memguard::IsAudioTagged() || m_slots.PreparedUseActive()) return false;
    return m_firstOutputEndpoints.ConstructThrough(count * m_firstOutputStride) &&
        m_firstOutputWeights.ConstructThrough(count * kSelectionPathNodeCapacity) &&
        m_pendingTimelineVariableEvents.ConstructThrough(count);
}

bool VoicePool::PrepareRenderSlotBacking(std::uint32_t count, std::uint32_t channelCount,
    bool firstOutput, std::uint32_t highRatioChannels) noexcept {
    if (count > m_slots.Capacity() || channelCount > kMaxChannels ||
        highRatioChannels > kMaxChannels) return false;
    const bool monitorPrepared = m_monitorPreChainPrepared.load(std::memory_order_acquire);
    for (std::uint32_t ch = 0; ch < channelCount; ++ch) {
        if (!m_ringStorage[ch].ConstructThrough(count) ||
            (monitorPrepared &&
             !m_monitorPreChainStorage[ch].ConstructThrough(count * m_blockFrames))) return false;
    }
    highRatioChannels = std::max(channelCount, highRatioChannels);
    for (std::uint32_t ch = 0; ch < highRatioChannels; ++ch)
        if (!m_highRatioStorage[ch].ConstructThrough(count)) return false;
    if (firstOutput) {
        if (!PrepareFirstOutputBacking(count)) return false;
        for (std::uint32_t slot = 0; slot < count; ++slot)
            if (!PrepareVariableWeightBacking(slot)) return false;
    }
    for (auto* registration = m_slotStorageRegistrations; registration != nullptr; registration = registration->next_)
        if (!registration->prepare_(registration->context_, count)) return false;
    return true;
}

bool VoicePool::PrepareMonitorPreChainBacking() noexcept {
    if (m_monitorPreChainPrepared.load(std::memory_order_acquire)) return true;
    if (memguard::IsAudioTagged() || m_slots.PreparedUseActive()) return false;
    // Include every held source prefix before capture can become live.
    for (std::uint32_t ch = 0; ch < kMaxChannels; ++ch) {
        if (!m_monitorPreChainStorage[ch].ConstructThrough(
                m_ringStorage[ch].ConstructedCount() * m_blockFrames)) return false;
    }
    m_monitorPreChainPrepared.store(true, std::memory_order_release);
    return true;
}

bool VoicePool::RenderSlotBackingReady(std::uint32_t count, std::uint32_t channelCount,
    std::uint32_t highRatioChannels) const noexcept {
    if (count > m_slots.Capacity() || channelCount > kMaxChannels ||
        highRatioChannels > kMaxChannels) return false;
    const bool monitorPrepared = m_monitorPreChainPrepared.load(std::memory_order_acquire);
    for (std::uint32_t ch = 0; ch < channelCount; ++ch) {
        if (m_ringStorage[ch].ConstructedCount() < count ||
            (monitorPrepared &&
             m_monitorPreChainStorage[ch].ConstructedCount() < count * m_blockFrames)) return false;
    }
    highRatioChannels = std::max(channelCount, highRatioChannels);
    for (std::uint32_t ch = 0; ch < highRatioChannels; ++ch)
        if (m_highRatioStorage[ch].ConstructedCount() < count) return false;
    return true;
}

void VoicePool::BindHighRatioBacking(std::uint32_t slot) noexcept {
    // Construction always extends a channel prefix and never shrinks it.
    std::uint32_t channels = 0;
    while (channels < kMaxChannels && m_highRatioStorage[channels].ConstructedCount() > slot)
        ++channels;
    m_slots[slot].render.resampler.BindHighRatioColumns(m_highRatioColumns, slot, channels);
}

void VoicePool::BindMonitorPreChainBacking(std::uint32_t slot) noexcept {
    auto& render = m_slots[slot].render;
    // Reserved addresses and source width stay fixed for this voice generation.
    // A later capture plan constructs the backing before publishing live audio.
    for (std::uint32_t ch = 0; ch < kMaxChannels; ++ch) {
        render.monitorPreChain[ch] = ch < render.pcm.channelCount
            ? m_monitorPreChainStorage[ch].data() + static_cast<std::size_t>(slot) * m_blockFrames
            : nullptr;
    }
}

bool VoicePool::PrepareVariableWeightBacking(std::uint32_t slot) noexcept {
    if (m_variableWeightStorage.IsConstructed(slot)) return true;
    if (memguard::IsAudioTagged() || m_slots.PreparedUseActive()) return false;
    return m_variableWeightStorage.Emplace(slot);
}

void VoicePool::BindVariableWeightBacking(std::uint32_t slot) noexcept {
    if (!m_variableWeightStorage.IsConstructed(slot)) return;
    auto& weights = m_variableWeightStorage[slot];
    weights = {};
    m_slots[slot].render.variableWeightControls = weights;
}

bool VoicePool::PrepareSlotWorkingSet(std::uint32_t renderSlot, std::uint32_t channelCount,
    ResampleProfile profile, bool prepareOwnedWeights, std::uint32_t highRatioChannels) noexcept {
    if (renderSlot >= m_slots.Capacity() || channelCount == 0 || channelCount > kMaxChannels ||
        highRatioChannels > kMaxChannels) return false;
    if (profile != ResampleProfile::LegacyCompatibility &&
        profile != ResampleProfile::Professional44148) return false;
    highRatioChannels = std::max(channelCount, highRatioChannels);
    if (!m_slots.ConstructThrough(renderSlot + 1u, [this, channelCount, highRatioChannels](std::uint32_t required) noexcept {
        const bool held = m_slots.HasPreparedBacking();
        return PrepareRenderSlotBacking(held ? std::max(required, m_slots.ConstructedCount()) : required,
            held ? kMaxChannels : channelCount, held, highRatioChannels);
    })) return false;
    return RenderSlotBackingReady(renderSlot + 1u, channelCount, highRatioChannels) &&
        (!prepareOwnedWeights || PrepareVariableWeightBacking(renderSlot)) &&
        (profile != ResampleProfile::Professional44148 || m_professionalStorage.Emplace(renderSlot));
}

bool VoicePool::PrepareSlotBacking(std::uint32_t count, StableStoragePreparation* out) noexcept {
    auto occupied = m_slots.ConstructedCount();
    while (occupied != 0 && CanClaimSlot(m_slots[occupied - 1u])) --occupied;
    return m_slots.PrepareBacking(std::min(count, m_slots.Capacity()), occupied, out,
        [this](std::uint32_t required) noexcept {
            return PrepareRenderSlotBacking(std::max(required, m_slots.ConstructedCount()), kMaxChannels, true);
        });
}

namespace {

VnVoiceHandle MakeHandle(std::uint32_t slot, std::uint32_t generation) {
    return (static_cast<std::uint64_t>(generation) << 32) | slot;
}

// The constant-gain stereo balance pair for a pan scalar. Unlike the equal-power
// pair carried on the pan command line — which lands the center at 1/sqrt(2) on
// both sides, a 3 dB dip — this keeps the center at unity: a full-left pan mutes
// the right output and leaves the left untouched, and vice versa. Derived on the
// audio thread from the already-resolved pan; two mins, no trigonometry.
struct StereoBalance {
    float left = 1.0f;
    float right = 1.0f;
};

StereoBalance DeriveStereoBalance(float pan) {
    const float p = (pan < -1.0f) ? -1.0f : (pan > 1.0f) ? 1.0f : pan;
    return StereoBalance{std::min(1.0f, 1.0f - p), std::min(1.0f, 1.0f + p)};
}


float DbToLinear(float db) {
    return std::pow(10.0f, db / 20.0f);
}

VnNotification MakeVoiceNotification(std::uint32_t type, VnVoiceHandle handle,
                       std::uint64_t blockIndex) {
    VnNotification notification{};
    notification.type = type;
    notification.voiceHandle = handle;
    notification.blockTimestamp = blockIndex;
    return notification;
}

} // namespace

VoicePool::VoicePool(const EngineConfig& config, const ResampleTables* tables,
                     const MultirateResampleTables* multirateTables,
                     const BiquadTrigTable* biquadTrig,
                     EffectPoolBank& effectBank,
                     DynamicRouteSystem& dynamicRoutes,
                     TimelineArmTable* timelineArms)
    : m_sampleRate(config.mixRate.SampleRate()),
      m_pitchRampFrames(BiquadSmoothingFrames(config.mixRate.SampleRate())),
      m_blockFrames(config.blockFrames),
      m_fxSlotsPerVoice(config.fxSlotsPerVoice),
      m_starvationCooldownBlocks(config.starvationCooldownBlocks),
      m_mixChannels(config.mixChannelCount),
      m_mixLayout(config.mixChannelLayout),
      m_monoStereoMix(output_gain_detail::SelectMonoStereoMixKernel()),
      m_tables(tables),
      m_multirateTables(multirateTables),
      m_biquadTrig(biquadTrig),
      m_effectBank(effectBank),
      m_dynamicRoutes(dynamicRoutes),
      m_timelineArms(timelineArms),
      m_originalCallbackStorage(config.voiceCapacity.Render()),
      m_streamParkMailboxes(config.voiceCapacity.Render()),
      m_jobSlots(config.voiceCapacity.Render(), 0),
      m_jobGenerations(config.voiceCapacity.Render(), 0) {
    if (!m_slots.Reserve(config.voiceCapacity.Render()) ||
        !m_professionalStorage.Reserve(config.voiceCapacity.Render()) ||
        !m_variableWeightStorage.Reserve(config.voiceCapacity.Render()) ||
        !m_streamParkTokens.Reserve(config.voiceCapacity.Virtual())) throw std::bad_alloc{};
    m_firstOutputStride = kFirstOutputBuiltinEndpoints + config.fxSlotsPerVoice * kMaxEffectParamCount;
    const auto monitorCapacity = static_cast<std::uint64_t>(config.voiceCapacity.Render()) * m_blockFrames;
    const auto endpointCapacity = static_cast<std::uint64_t>(config.voiceCapacity.Render()) * m_firstOutputStride;
    const auto weightCapacity = static_cast<std::uint64_t>(config.voiceCapacity.Render()) * kSelectionPathNodeCapacity;
    if (monitorCapacity > UINT32_MAX || endpointCapacity > UINT32_MAX || weightCapacity > UINT32_MAX ||
        !m_firstOutputEndpoints.Reserve(static_cast<std::uint32_t>(endpointCapacity)) ||
        !m_firstOutputWeights.Reserve(static_cast<std::uint32_t>(weightCapacity)) ||
        !m_pendingTimelineVariableEvents.Reserve(config.voiceCapacity.Render())) throw std::bad_alloc{};
    for (std::uint32_t ch = 0; ch < kMaxChannels; ++ch) {
        if (!m_ringStorage[ch].Reserve(config.voiceCapacity.Render()) ||
            !m_highRatioStorage[ch].Reserve(config.voiceCapacity.Render()) ||
            !m_monitorPreChainStorage[ch].Reserve(static_cast<std::uint32_t>(monitorCapacity)))
            throw std::bad_alloc{};
        m_highRatioColumns.channels[ch] = m_highRatioStorage[ch].data();
    }
    // Create the fixed worker set on the logic thread. Real-device workers stay
    // disabled until the backend binds them to its callback scheduling domain
    // and validates that callback. The Null backend explicitly selects offline
    // mode; it makes no real-time scheduling claim. Any creation failure must
    // abort engine construction instead of silently changing N to zero.
    // maxJobs is the render-voice count: a batch dispatches at most one job per
    // rendering voice, which is the same bound m_jobSlots is sized from.
    if (!m_workers.Start(config.renderWorkerCount,
                         config.voiceCapacity.Render())) {
        throw std::bad_alloc{};
    }
    if (config.nullDevice && !m_workers.EnableOffline()) {
        throw std::bad_alloc{};
    }
}

VoicePool::~VoicePool() {
    while (m_slotStorageRegistrations != nullptr) m_slotStorageRegistrations->Detach();
    assert(m_liveOriginalVoiceOwners.load(std::memory_order_relaxed) == 0 &&
        "original physical owners must retire before the pool");
    for (const auto& slot : m_slots) {
        assert((slot.originalBodyLifetime.load(std::memory_order_relaxed) &
            (kOriginalBodyPinMask | kOriginalBodyCallbackReader)) == 0);
        assert(!slot.pendingOriginalSource.Valid() && !slot.installedOriginalSource.Valid());
    }
}

VnResult VoicePool::Play(const PCMView& view, float volumeDb,
                         double pitchRatio, std::uint32_t busIndex,
                         bool spatialized, float posX, float posY,
                         float posZ, const ResolvedPanGains& initialPan,
                         std::uint64_t loopStartFrame,
                         std::uint64_t loopEndFrame, std::uint32_t loopCount,
                         ResampleProfile resampleProfile,
                         VnVoiceHandle* outHandle,
                         std::uint32_t busGeneration,
                         std::uint32_t requestedSlot,
                         std::uint32_t expectedGeneration,
                         bool prepared, float outputBusVolumeDb,
                         float spatializationMix, bool prepareOwnedWeights) {
    float linearGain = 0.0f;
    float outputLinearGain = 0.0f;
    if (!TryVolumeGainFromDb(volumeDb, &linearGain) ||
        !TryVolumeGainFromDb(outputBusVolumeDb, &outputLinearGain) ||
        !TryVolumeGainFromDb(volumeDb + outputBusVolumeDb) ||
        !std::isfinite(spatializationMix) || spatializationMix < 0.0f ||
        spatializationMix > 1.0f || (spatialized && view.channelCount > 2)) {
        return VN_INVALID_ARG;
    }
    std::uint32_t slotIndex = requestedSlot;
    if (requestedSlot == ~std::uint32_t{0}) {
        slotIndex = 0;
        for (; slotIndex < m_slots.size(); ++slotIndex) {
            // Acquire pairs with ReleaseSlot's release store: once Free is
            // visible, the audio thread's final writes into this slot's
            // render state happened-before our re-initialization below.
            if (CanClaimSlot(m_slots[slotIndex])) {
                break;
            }
        }
    }
    if (slotIndex >= m_slots.Capacity() ||
        (slotIndex >= m_slots.ConstructedCount() && expectedGeneration > 1u) ||
        !PrepareSlotWorkingSet(slotIndex, view.channelCount, resampleProfile, prepareOwnedWeights) ||
        !CanClaimSlot(m_slots[slotIndex]) ||
        (expectedGeneration != 0 &&
         m_slots[slotIndex].generation.load(std::memory_order_relaxed) !=
             expectedGeneration)) {
        // Over-capacity refusal is a countable degradation, like stale
        // handles: the host can observe how often the pool ran out.
        ++m_noVoiceRefusals;
        return VN_NO_VOICE;
    }

    Slot& slot = m_slots[slotIndex];
    ResetOriginalSourceBodyForReuse(slot);
    // Relaxed is enough for the claim: the audio thread acts on this slot
    // only after popping the play command, and the queue's release/acquire
    // publishes the fully built render context along with this state.
    slot.state.store(prepared ? SlotState::Prepared : SlotState::Claimed,
                     std::memory_order_relaxed);
    slot.stagedCompletionProgress = nullptr;
    slot.stagedCompletionUpdate = {};
    slot.completionProgressStaged = false;
    slot.completionProgressTerminal = false;
    slot.monitorControl = {};
    slot.monitorSourceTerminalPublished = false;
    slot.render = VoiceRenderContext{};
    BindHighRatioBacking(slotIndex);
    BindVariableWeightBacking(slotIndex);
    ClearFirstOutput(slot);
    slot.render.biquadTrig = m_biquadTrig;
    slot.render.engineSampleRate = m_sampleRate;
    slot.render.pcm = view;
    BindMonitorPreChainBacking(slotIndex);
    slot.render.busIndex = busIndex;
    slot.render.busGeneration = busGeneration;
    slot.render.phase = SourcePhase::Active;
    // The chain starts with every slot empty (an empty slot is skipped at
    // zero cost, bit-identical to a passthrough); fxSlotCount is how many of
    // the fixed array are active for this voice. No default effect is
    // installed — the logic side wires effects into slots at Set time.
    slot.render.fxSlotCount = m_fxSlotsPerVoice;
    // Loop state, resolved on the logic side. loopEndFrame is clamped to the
    // real length and a degenerate region collapses to whole-file, so the
    // audio side can trust [loopStartFrame, loopEndFrame) without checks.
    slot.render.loopStartFrame = loopStartFrame;
    slot.render.loopEndFrame = loopEndFrame;
    slot.render.loopInfinite = (loopCount == 0);
    slot.render.loopPassesRemaining = loopCount;
    slot.render.gain.Set(linearGain, 0);
    slot.render.outputGain.Set(outputLinearGain);
    slot.render.spatialized = spatialized;
    slot.render.pan = initialPan.pan;
    slot.render.pan2d = initialPan.pan;
    slot.render.panGain0 = initialPan.gain0;
    slot.render.panGain1 = initialPan.gain1;
    slot.render.panChannel0 = initialPan.channel0;
    slot.render.panChannel1 = initialPan.channel1;
    // The 2D balance lanes start snapped to the initial pan so a non-spatialized
    // voice folds correctly from its first block; a centered start lands at
    // unity and takes the plain frozen mapping.
    {
        const StereoBalance balance = DeriveStereoBalance(initialPan.pan);
        slot.render.balanceLeftTransition.SnapTo(balance.left);
        slot.render.balanceRightTransition.SnapTo(balance.right);
    }
    slot.render.spatializationMix = spatializationMix;
    slot.render.positionX = posX;
    slot.render.positionY = posY;
    slot.render.positionZ = posZ;

    // One combined ratio: caller pitch times source-rate / engine-rate.
    // The rate factor is kept on the context so a runtime pitch update
    // rebuilds the combined ratio without re-deriving it from the view.
    slot.render.rateScale = static_cast<double>(view.sampleRate) /
                            static_cast<double>(m_sampleRate);
    const double combined = pitchRatio * slot.render.rateScale;

    float* rings[kMaxChannels] = {};
    for (std::uint32_t ch = 0; ch < view.channelCount; ++ch) {
        rings[ch] = m_ringStorage[ch][slotIndex].data();
    }
    slot.render.resampler.Reset(rings, view.channelCount,
                                ResampleQuality::Linear, combined, m_tables,
                                resampleProfile, m_multirateTables,
                                resampleProfile == ResampleProfile::Professional44148
                                    ? &m_professionalStorage[slotIndex] : nullptr);

    if (view.useCount != nullptr) {
        view.useCount->fetch_add(1, std::memory_order_relaxed);
    }

    if (!prepared) {
        const std::uint32_t active = ActiveCount();
        if (active > m_highWater) {
            m_highWater = active;
        }
    }

    // The generation is stable here: only ReleaseSlot writes it, and no
    // release can target a slot we just claimed (its stop/end commands
    // cannot precede the play command in the queue).
    *outHandle =
        MakeHandle(slotIndex, slot.generation.load(std::memory_order_relaxed));
    return VN_OK;
}

VnResult VoicePool::PlayStream(StreamRing* ring, std::uint32_t channelCount,
                               std::uint32_t sourceSampleRate,
                               std::uint32_t prebufferTargetFrames,
                               float volumeDb, double pitchRatio,
                               std::uint32_t busIndex, bool spatialized,
                               float posX, float posY, float posZ,
                               const ResolvedPanGains& initialPan,
                               std::uint64_t activationStartFrame,
                               std::uint64_t frameCount,
                               std::uint64_t loopStartFrame,
                               std::uint64_t loopEndFrame,
                               std::uint32_t loopsRemaining,
                               ResampleProfile resampleProfile,
                               VnVoiceHandle* outHandle,
                               std::uint32_t busGeneration,
                               std::uint32_t requestedSlot,
                               std::uint32_t expectedGeneration,
                               const StreamParkTokenRef* resumeToken,
                               float outputBusVolumeDb,
                               bool prepared,
                               float spatializationMix) {
    float linearGain = 0.0f;
    float outputLinearGain = 0.0f;
    if (!TryVolumeGainFromDb(volumeDb, &linearGain) ||
        !TryVolumeGainFromDb(outputBusVolumeDb, &outputLinearGain) ||
        !TryVolumeGainFromDb(volumeDb + outputBusVolumeDb) ||
        !std::isfinite(spatializationMix) || spatializationMix < 0.0f ||
        spatializationMix > 1.0f || (spatialized && channelCount > 2)) {
        return VN_INVALID_ARG;
    }
    const StreamParkToken* saved = nullptr;
    if (resumeToken != nullptr) {
        saved = ResolveParkOwnedToken(*resumeToken);
        if (saved == nullptr ||
            saved->formatVersion != StreamParkToken::kFormatVersion ||
            saved->usedBytes > sizeof(StreamParkToken) ||
            saved->usedBytes !=
                sizeof(StreamParkToken) -
                    sizeof(ResamplerParkState) +
                    saved->resampler.usedBytes ||
            saved->ringGeneration == 0 ||
            saved->profile != resampleProfile ||
            saved->resampler.profile != saved->profile ||
            saved->channelCount != channelCount ||
            !Resampler::ValidateParkState(saved->resampler)) {
            ++m_noVoiceRefusals;
            return VN_NO_VOICE;
        }
    }
    std::uint32_t slotIndex = requestedSlot;
    if (requestedSlot == ~std::uint32_t{0}) {
        slotIndex = 0;
        for (; slotIndex < m_slots.size(); ++slotIndex) {
            // Same acquire pairing as Play(): a Free observation publishes the
            // audio thread's final writes into this slot's render state.
            if (CanClaimSlot(m_slots[slotIndex])) {
                break;
            }
        }
    }
    if (slotIndex >= m_slots.Capacity() ||
        (slotIndex >= m_slots.ConstructedCount() && expectedGeneration > 1u) ||
        !PrepareSlotWorkingSet(slotIndex, channelCount, resampleProfile, true,
            saved != nullptr ? ResamplerView::RequiredHighRatioChannels(saved->resampler) : channelCount) ||
        !CanClaimSlot(m_slots[slotIndex]) ||
        (expectedGeneration != 0 &&
         m_slots[slotIndex].generation.load(std::memory_order_relaxed) !=
             expectedGeneration)) {
        ++m_noVoiceRefusals;
        return VN_NO_VOICE;
    }

    Slot& slot = m_slots[slotIndex];
    ResetOriginalSourceBodyForReuse(slot);
    slot.state.store(prepared ? SlotState::Prepared : SlotState::Claimed,
                     std::memory_order_relaxed);
    slot.streamRingToken.store(0, std::memory_order_relaxed);
    slot.monitorControl = {};
    slot.monitorSourceTerminalPublished = false;
    slot.render = VoiceRenderContext{};
    BindHighRatioBacking(slotIndex);
    BindVariableWeightBacking(slotIndex);
    ClearFirstOutput(slot);
    slot.render.biquadTrig = m_biquadTrig;
    slot.render.engineSampleRate = m_sampleRate;
    // Synthetic view: the cascade's channel and rate plumbing reads these
    // two facts; the data planes stay null because the stream feed path
    // never dereferences pcm (frames == 0 keeps every pcm read unreachable).
    slot.render.pcm.channelCount = channelCount;
    BindMonitorPreChainBacking(slotIndex);
    slot.render.pcm.sampleRate = sourceSampleRate;
    slot.render.busIndex = busIndex;
    slot.render.busGeneration = busGeneration;
    slot.render.phase = SourcePhase::Active;
    slot.render.fxSlotCount = m_fxSlotsPerVoice;
    slot.render.streamRing = ring;
    slot.render.streamActivationStartFrame =
        saved != nullptr ? saved->activationFrame : activationStartFrame;
    slot.render.streamFeedActivationStartFrame =
        saved != nullptr ? saved->feedFrame : activationStartFrame;
    slot.render.streamFrameCount = frameCount;
    slot.render.streamLoopStartFrame = loopStartFrame;
    slot.render.streamLoopEndFrame = loopEndFrame;
    slot.render.streamActivationLoopsRemaining =
        saved != nullptr ? saved->activationLoopsRemaining : loopsRemaining;
    slot.render.streamFeedActivationLoopsRemaining =
        saved != nullptr ? saved->feedLoopsRemaining : loopsRemaining;
    // A target above the ring's capacity could never be reached — a full
    // ring is as prebuffered as it can get, so the gate clamps there.
    slot.render.prebufferTargetFrames =
        prebufferTargetFrames < ring->capacity ? prebufferTargetFrames
                                               : ring->capacity;
    slot.render.streamGateOpen =
        saved != nullptr && saved->gateOpen;
    slot.render.gain.Set(linearGain, 0);
    slot.render.outputGain.Set(outputLinearGain);
    slot.render.spatialized = spatialized;
    slot.render.pan = initialPan.pan;
    slot.render.pan2d = initialPan.pan;
    slot.render.panGain0 = initialPan.gain0;
    slot.render.panGain1 = initialPan.gain1;
    slot.render.panChannel0 = initialPan.channel0;
    slot.render.panChannel1 = initialPan.channel1;
    {
        const StereoBalance balance = DeriveStereoBalance(initialPan.pan);
        slot.render.balanceLeftTransition.SnapTo(balance.left);
        slot.render.balanceRightTransition.SnapTo(balance.right);
    }
    slot.render.spatializationMix = spatializationMix;
    slot.render.positionX = posX;
    slot.render.positionY = posY;
    slot.render.positionZ = posZ;

    slot.render.rateScale = static_cast<double>(sourceSampleRate) /
                            static_cast<double>(m_sampleRate);
    const double combined = pitchRatio * slot.render.rateScale;

    float* rings[kMaxChannels] = {};
    for (std::uint32_t ch = 0; ch < channelCount; ++ch) {
        rings[ch] = m_ringStorage[ch][slotIndex].data();
    }
    if (saved != nullptr) {
        if (!slot.render.resampler.RestoreParkState(
                saved->resampler, rings, channelCount, m_tables,
                m_multirateTables, resampleProfile == ResampleProfile::Professional44148
                    ? &m_professionalStorage[slotIndex] : nullptr)) {
            slot.state.store(SlotState::Free, std::memory_order_release);
            ++m_noVoiceRefusals;
            return VN_NO_VOICE;
        }
        if (combined != saved->resampler.targetRatio) {
            slot.render.resampler.UpdateRatio(
                combined, saved->resampler.rampRemaining);
        }
    } else {
        slot.render.resampler.Reset(
            rings, channelCount, ResampleQuality::Linear,
            combined, m_tables, resampleProfile, m_multirateTables,
            resampleProfile == ResampleProfile::Professional44148
                ? &m_professionalStorage[slotIndex] : nullptr);
    }

    if (!prepared) {
        const std::uint32_t active = ActiveCount();
        if (active > m_highWater) {
            m_highWater = active;
        }
    }

    *outHandle =
        MakeHandle(slotIndex, slot.generation.load(std::memory_order_relaxed));
    return VN_OK;
}

bool VoicePool::BindStreamRingToken(VnVoiceHandle handle, StreamRing* ring,
                                    std::uint64_t ringToken) noexcept {
    Slot* slot = SlotFor(handle);
    if (slot == nullptr || ring == nullptr || ringToken == 0 ||
        slot->render.streamRing != ring ||
        static_cast<std::uint32_t>(ringToken >> 32u) !=
            ring->generation.load(std::memory_order_acquire)) {
        return false;
    }
    const SlotState state = slot->state.load(std::memory_order_acquire);
    if (state != SlotState::Claimed && state != SlotState::Prepared) {
        return false;
    }
    slot->render.streamRingToken = ringToken;
    slot->streamRingToken.store(ringToken, std::memory_order_release);
    return true;
}

bool VoicePool::CaptureStreamRingToken(
    VnVoiceHandle handle, std::uint64_t* outToken) const noexcept {
    if (outToken == nullptr || handle == 0) return false;
    const std::uint32_t index = static_cast<std::uint32_t>(handle);
    if (index >= m_slots.size()) return false;
    const Slot& slot = m_slots[index];
    if (slot.generation.load(std::memory_order_acquire) !=
        static_cast<std::uint32_t>(handle >> 32u)) {
        return false;
    }
    const SlotState state = slot.state.load(std::memory_order_acquire);
    if (state != SlotState::Claimed && state != SlotState::Prepared &&
        state != SlotState::PreparedCommitHeld &&
        !IsAudioRenderingState(state)) {
        return false;
    }
    const std::uint64_t token =
        slot.streamRingToken.load(std::memory_order_acquire);
    if (token == 0) return false;
    *outToken = token;
    return true;
}

bool VoicePool::CanClaimSlot(const Slot& slot) noexcept {
    if (slot.state.load(std::memory_order_acquire) != SlotState::Free ||
        slot.releaseSequence.load(std::memory_order_acquire) == UINT64_MAX) return false;
    const auto lifetime = slot.originalBodyLifetime.load(std::memory_order_acquire);
    return (lifetime & (kOriginalBodyPinMask | kOriginalBodyCallbackReader)) == 0 &&
        (slot.originalBodyMarker == 0 || (lifetime & kOriginalBodyLoanReturned) != 0);
}

void VoicePool::PublishRelease(Slot& slot) noexcept {
    const auto sequence = slot.releaseSequence.load(std::memory_order_relaxed);
    if (sequence != UINT64_MAX) {
        slot.releaseSequence.store(sequence + 1u, std::memory_order_release);
    }
}

bool VoicePool::CaptureReleaseObserver(
    VnVoiceHandle handle, VoiceReleaseObserver* out) const noexcept {
    const auto index = static_cast<std::uint32_t>(handle);
    if (out == nullptr || handle == 0 || index >= m_slots.size()) return false;
    const auto& slot = m_slots[index];
    const auto state = slot.state.load(std::memory_order_acquire);
    if ((state != SlotState::Prepared && state != SlotState::PreparedCommitHeld &&
         state != SlotState::Claimed) ||
        slot.generation.load(std::memory_order_relaxed) !=
            static_cast<std::uint32_t>(handle >> 32)) return false;
    const auto sequence = slot.releaseSequence.load(std::memory_order_acquire);
    if (sequence == UINT64_MAX) return false;
    *out = {index, sequence + 1u};
    return true;
}

bool VoicePool::ReleaseObserved(const VoiceReleaseObserver& observer) const noexcept {
    return observer.Valid() && observer.slot < m_slots.size() &&
        m_slots[observer.slot].releaseSequence.load(std::memory_order_acquire) >=
            observer.targetSequence;
}

bool VoicePool::HasFreeSlot() const {
    if (m_slots.ConstructedCount() < m_slots.Capacity()) return true;
    for (const Slot& slot : m_slots) {
        if (CanClaimSlot(slot)) {
            return true;
        }
    }
    return false;
}

std::uint32_t VoicePool::AvailableSlotCount() const {
    std::uint32_t count = m_slots.Capacity() - m_slots.ConstructedCount();
    for (const Slot& slot : m_slots) {
        if (CanClaimSlot(slot)) {
            ++count;
        }
    }
    return count;
}

bool VoicePool::FreeSlotGeneration(std::uint32_t slot,
                                   std::uint32_t* outGeneration) const {
    if (slot >= m_slots.Capacity() || outGeneration == nullptr) return false;
    if (slot >= m_slots.ConstructedCount()) {
        *outGeneration = 1;
        return true;
    }
    if (!CanClaimSlot(m_slots[slot])) return false;
    *outGeneration =
        m_slots[slot].generation.load(std::memory_order_relaxed);
    return true;
}

VnResult VoicePool::CheckCapacityForPlay() {
    if (m_slots.ConstructedCount() < m_slots.Capacity()) return VN_OK;
    for (const Slot& slot : m_slots) {
        if (CanClaimSlot(slot)) {
            return VN_OK;
        }
    }
    // Count the refusal exactly as Play() would: the probe stands in for
    // the claim in the logic side's refusal ordering.
    ++m_noVoiceRefusals;
    return VN_NO_VOICE;
}

bool VoicePool::IsAlive(VnVoiceHandle handle) const noexcept {
    const auto index = static_cast<std::uint32_t>(handle);
    if (handle == 0 || index >= m_slots.size()) return false;
    const auto& slot = m_slots[index];
    const auto state = slot.state.load(std::memory_order_acquire);
    return state != SlotState::Free && state != SlotState::Quarantined &&
        slot.generation.load(std::memory_order_acquire) ==
            static_cast<std::uint32_t>(handle >> 32);
}

VnResult VoicePool::CheckLive(VnVoiceHandle handle) {
    if (!IsAlive(handle)) {
        ++m_staleHandleHits;
        return VN_STALE_HANDLE;
    }
    return VN_OK;
}

void VoicePool::ApplyTimelineGatedPlay(
    VnVoiceHandle handle, VnVoiceHandle sourceIdentity,
    std::uint64_t sourceStartFrame, std::uint64_t packedArmToken,
    std::uint64_t blockIndex) {
    Slot* slot = SlotFor(handle);
    if (slot != nullptr &&
        slot->state.load(std::memory_order_acquire) == SlotState::Claimed) {
        slot->render.timelineGated = true;
        slot->render.timelineArmSlot =
            static_cast<std::uint32_t>(packedArmToken);
        slot->render.timelineArmGeneration =
            static_cast<std::uint32_t>(packedArmToken >> 32);
        if (m_timelineArms != nullptr) {
            const TimelineArmTable::Row* row = m_timelineArms->RowAt(
                slot->render.timelineArmSlot);
            if (row != nullptr &&
                row->identity.load(std::memory_order_acquire) ==
                    packedArmToken) {
                slot->render.timelineParentGroup = row->parentGroup;
                slot->render.timelineGroupMemberIndex = row->memberIndex;
            }
        }
    }
    ApplyPlay(handle, sourceIdentity, sourceStartFrame, blockIndex);
}

std::uint64_t VoicePool::ScratchBytesBeforeTimeline(
    std::uint32_t blockFrames) const noexcept {
    std::uint64_t requiredBytes = 0;
    std::uint64_t requiredThroughLastTimeline = 0;
    for (const Slot& slot : m_slots) {
        if (!IsAudioRenderingState(slot.state.load(std::memory_order_relaxed))) {
            continue;
        }
        const VoiceRenderContext& render = slot.render;
        requiredBytes +=
            VoiceCascadeScratchBytes(render, blockFrames, m_mixChannels);
        if (render.timelineArmSlot != UINT32_MAX &&
            render.timelineArmGeneration != 0) {
            requiredThroughLastTimeline = requiredBytes;
        }
    }
    return requiredThroughLastTimeline;
}

std::uint64_t VoicePool::TimelineScratchBytes(
    std::uint32_t blockFrames) const noexcept {
    std::uint64_t requiredBytes = 0;
    for (const Slot& slot : m_slots) {
        if (!IsAudioRenderingState(slot.state.load(std::memory_order_relaxed))) {
            continue;
        }
        const VoiceRenderContext& render = slot.render;
        if (render.timelineArmSlot == UINT32_MAX ||
            render.timelineArmGeneration == 0) {
            continue;
        }
        requiredBytes +=
            VoiceCascadeScratchBytes(render, blockFrames, m_mixChannels);
    }
    return requiredBytes;
}

bool VoicePool::AdmitTimelineQuantum(
    std::uint32_t arenaAvailableBytes,
    std::uint32_t blockFrames) const noexcept {
    return ScratchBytesBeforeTimeline(blockFrames) <= arenaAvailableBytes;
}

bool VoicePool::AdmitTimelineQuantum(
    std::uint32_t arenaAvailableBytes, std::uint32_t blockFrames,
    const DecisionTable* decisions,
    const Plan03ScratchDecisionState* currentSlices,
    std::uint32_t currentSliceCount,
    const std::uint32_t* controlRenderGenerations,
    std::uint32_t controlRenderCount) const noexcept {
    const bool decisionScratch = decisions != nullptr && currentSlices != nullptr &&
        currentSliceCount != 0;
    if (!decisionScratch && (controlRenderGenerations == nullptr || controlRenderCount == 0)) {
        return AdmitTimelineQuantum(arenaAvailableBytes, blockFrames);
    }
    std::uint64_t requiredBytes = 0;
    std::uint64_t requiredThroughLastTimeline = 0;
    for (std::uint32_t renderSlot = 0; renderSlot < m_slots.size();
         ++renderSlot) {
        const Slot& slot = m_slots[renderSlot];
        if (!IsAudioRenderingState(slot.state.load(std::memory_order_relaxed))) {
            continue;
        }
        const VoiceRenderContext& render = slot.render;
        const bool controlTarget = controlRenderGenerations != nullptr &&
            renderSlot < controlRenderCount && controlRenderGenerations[renderSlot] != 0 &&
            controlRenderGenerations[renderSlot] == slot.generation.load(std::memory_order_relaxed);
        bool exclude = false;
        if (decisionScratch && !controlTarget && render.timelineGated && render.timelineParentGroup.Valid() &&
            render.timelineArmSlot != UINT32_MAX &&
            render.timelineArmGeneration != 0 && m_timelineArms != nullptr) {
            const std::uint32_t renderGeneration =
                slot.generation.load(std::memory_order_relaxed);
            const TimelineArmToken token{
                render.timelineArmSlot, render.timelineArmGeneration,
                renderSlot, renderGeneration};
            TimelineArmObservation arm{};
            DecisionCommittedSnapshot decision{};
            const DecisionToken parent =
                render.timelineParentGroup.decision;
            if (m_timelineArms->Observe(token, &arm) &&
                arm.state == ArmState::CancelRequested &&
                arm.memberIndex == render.timelineGroupMemberIndex &&
                arm.parentGroup.decision.slot == parent.slot &&
                arm.parentGroup.decision.generation == parent.generation &&
                decisions->CaptureCommitted(parent, &decision) &&
                (decision.audioState == AudioGroupState::Invalidated ||
                 decision.audioState == AudioGroupState::DeadlineInvalidated ||
                 decision.audioState ==
                     AudioGroupState::PostStartInvalidated ||
                 decision.audioState ==
                     AudioGroupState::PostStartDeadlineInvalidated)) {
                const bool sliceClaimed =
                    parent.slot < currentSliceCount &&
                    currentSlices[parent.slot].generation ==
                        parent.generation &&
                    currentSlices[parent.slot].currentSliceClaimed;
                exclude = !sliceClaimed;
            }
        }
        if (exclude) {
            continue;
        }
        requiredBytes +=
            VoiceCascadeScratchBytes(render, blockFrames, m_mixChannels);
        if (controlTarget || (render.timelineArmSlot != UINT32_MAX &&
            render.timelineArmGeneration != 0)) {
            requiredThroughLastTimeline = requiredBytes;
        }
    }
    return requiredThroughLastTimeline <= arenaAvailableBytes;
}

void VoicePool::BeginTimelineQuantum(std::uint64_t quantumIndex) noexcept {
    assert(!m_timelineQuantumOpen);
    m_timelineQuantumOpen = true;
    for (Slot& slot : m_slots) {
        auto state = slot.state.load(std::memory_order_acquire);
        if (state == SlotState::Rendering) {
            auto expected = state;
            if (!slot.state.compare_exchange_strong(expected, SlotState::TimelineCallbackHeld,
                std::memory_order_acq_rel, std::memory_order_acquire)) continue;
            state = SlotState::TimelineCallbackHeld;
        }
        if (!IsAudioRenderingState(state)) continue;
        VoiceEventSpan& span = slot.render.timelineEvents;
        assert(slot.timelineProvenMask == 0);
        span.quantumIndex = quantumIndex;
        span.renderGeneration = slot.generation.load(std::memory_order_relaxed);
        span.eventCount = 0;
        slot.timelinePageEventCount = 0;
        slot.timelineLaneEventCount = 0;
        ++slot.timelineReservationEpoch;
        slot.timelineReservedMask = 0;
        slot.timelineFilledMask = 0;
        std::fill(std::begin(slot.timelineReservationCounts),
                  std::end(slot.timelineReservationCounts), 0);
    }
}

bool VoicePool::TimelineBindingValid(
    std::uint32_t renderSlot, std::uint32_t renderGeneration) const noexcept {
    if (renderSlot >= m_slots.size()) {
        return false;
    }
    const Slot& slot = m_slots[renderSlot];
    const SlotState state = slot.state.load(std::memory_order_relaxed);
    return (IsAudioRenderingState(state) || state == SlotState::Claimed) &&
           slot.generation.load(std::memory_order_relaxed) == renderGeneration;
}

void VoicePool::FinalizeTimelineQuantum(std::uint64_t quantumIndex) noexcept {
    for (Slot& slot : m_slots) {
        if (!IsAudioRenderingState(slot.state.load(std::memory_order_relaxed))) continue;
        assert(slot.timelineProvenMask == 0);
        auto& span = slot.render.timelineEvents;
        if (span.quantumIndex != quantumIndex) continue;
        // Fixed Page and lane slots share a stable sort without allocating.
        for (std::uint32_t current = 1; current < span.eventCount; ++current) {
            const auto event = span.events[current];
            auto position = current;
            while (position != 0) {
                const auto& prior = span.events[position - 1];
                if (prior.offset < event.offset ||
                    (prior.offset == event.offset && !(event.order < prior.order))) break;
                span.events[position] = prior;
                --position;
            }
            span.events[position] = event;
        }
        // Every serial claim is complete here. Merge only actual eligible stops,
        // including stops from another Page/lane, before any worker reads guards.
        const auto cutoff = SelectAuthoredRootStop(span, m_blockFrames,
            slot.render.authoredRootStopFrame,
            slot.render.authoredRootStopControl.Stop().authoredStopOperation);
        if (cutoff.frame != UINT64_MAX) {
            for (std::uint32_t index = 0; index < span.eventCount; ++index) {
                auto& guard = span.events[index].frontierProof;
                if (guard.state != FrontierEventState::Legacy &&
                    guard.Eligible(quantumIndex, span.renderGeneration) &&
                    cutoff.frame < guard.cutoffFrame) {
                    guard.cutoffFrame = cutoff.frame;
                    guard.cutoffOperation = cutoff.operation;
                }
            }
        }
        ++slot.timelineReservationEpoch;
    }
    m_timelineQuantumOpen = false;
    if (!m_originalSourceCallbackOpen) ReturnTimelineCallbackStorage();
}

void VoicePool::ReturnTimelineCallbackStorage() noexcept {
    for (auto& slot : m_slots) {
        auto expected = SlotState::TimelineCallbackHeld;
        (void)slot.state.compare_exchange_strong(expected, SlotState::Rendering,
            std::memory_order_acq_rel, std::memory_order_acquire);
    }
}

bool VoicePool::CanAppendTimelineEvents(
    std::uint32_t renderSlot, std::uint32_t renderGeneration,
    std::uint64_t quantumIndex, std::uint32_t eventCount) const noexcept {
    if (renderSlot >= m_slots.size() ||
        eventCount > kTimelineMaxEventsPerVoiceQuantum) {
        return false;
    }
    const Slot& slot = m_slots[renderSlot];
    if (!IsAudioRenderingState(slot.state.load(std::memory_order_relaxed)) ||
        slot.generation.load(std::memory_order_relaxed) != renderGeneration) {
        return false;
    }
    const VoiceEventSpan& span = slot.render.timelineEvents;
    return span.quantumIndex == quantumIndex &&
           span.renderGeneration == renderGeneration &&
           span.eventCount <= kTimelineMaxCombinedEventsPerVoiceQuantum &&
           slot.timelinePageEventCount <= kTimelineMaxEventsPerVoiceQuantum &&
           eventCount <= kTimelineMaxEventsPerVoiceQuantum - slot.timelinePageEventCount &&
           eventCount <= kTimelineMaxCombinedEventsPerVoiceQuantum - span.eventCount;
}

bool VoicePool::AppendTimelineEvent(
    std::uint32_t renderSlot, std::uint32_t renderGeneration,
    std::uint64_t quantumIndex, const VoiceTimelineEvent& event) noexcept {
    if (renderSlot >= m_slots.size()) {
        return false;
    }
    Slot& slot = m_slots[renderSlot];
    if (!IsAudioRenderingState(slot.state.load(std::memory_order_relaxed)) ||
        slot.generation.load(std::memory_order_relaxed) != renderGeneration) {
        return false;
    }
    VoiceEventSpan& span = slot.render.timelineEvents;
    if (span.quantumIndex != quantumIndex ||
        span.renderGeneration != renderGeneration ||
        span.eventCount >= kTimelineMaxCombinedEventsPerVoiceQuantum ||
        slot.timelinePageEventCount >= kTimelineMaxEventsPerVoiceQuantum) {
        return false;
    }
    span.events[span.eventCount++] = event;
    ++slot.timelinePageEventCount;
    return true;
}

bool VoicePool::ReserveTimelineEvents(
    std::uint32_t renderSlot, std::uint32_t renderGeneration,
    std::uint64_t quantumIndex, std::uint32_t count,
    VoiceTimelineReservation* out) noexcept {
    static_assert(kTimelineMaxCombinedEventsPerVoiceQuantum < 32);
    if (out == nullptr || count == 0 ||
        !CanAppendTimelineEvents(renderSlot, renderGeneration, quantumIndex, count)) return false;
    auto& slot = m_slots[renderSlot];
    auto& span = slot.render.timelineEvents;
    const auto first = span.eventCount;
    for (std::uint32_t i = 0; i < count; ++i) {
        auto& event = span.events[first + i];
        event = {};
        event.frontierProof.state = FrontierEventState::Pending;
        event.frontierProof.quantum = quantumIndex;
        event.frontierProof.renderGeneration = renderGeneration;
    }
    slot.timelineReservedMask |= ((1u << count) - 1u) << first;
    slot.timelineReservationCounts[first] = static_cast<std::uint8_t>(count);
    span.eventCount += count;
    slot.timelinePageEventCount += count;
    *out = {renderSlot, renderGeneration, quantumIndex,
        slot.timelineReservationEpoch, first, count};
    return true;
}

bool VoicePool::ReserveLaneTimelineEvents(
    std::uint32_t renderSlot, std::uint32_t renderGeneration,
    std::uint64_t quantumIndex, std::uint32_t count,
    VoiceTimelineReservation* out) noexcept {
    if (out == nullptr || count == 0 || renderSlot >= m_slots.size() ||
        count > kTimelineMaxLaneEventsPerVoiceQuantum) return false;
    auto& slot = m_slots[renderSlot];
    auto& span = slot.render.timelineEvents;
    if (!IsAudioRenderingState(slot.state.load(std::memory_order_relaxed)) ||
        slot.generation.load(std::memory_order_relaxed) != renderGeneration ||
        span.quantumIndex != quantumIndex || span.renderGeneration != renderGeneration ||
        slot.timelineLaneEventCount > kTimelineMaxLaneEventsPerVoiceQuantum ||
        count > kTimelineMaxLaneEventsPerVoiceQuantum - slot.timelineLaneEventCount ||
        span.eventCount > kTimelineMaxCombinedEventsPerVoiceQuantum ||
        count > kTimelineMaxCombinedEventsPerVoiceQuantum - span.eventCount) return false;
    const auto first = span.eventCount;
    for (std::uint32_t i = 0; i < count; ++i) {
        auto& event = span.events[first + i];
        event = {};
        event.frontierProof.state = FrontierEventState::Pending;
        event.frontierProof.quantum = quantumIndex;
        event.frontierProof.renderGeneration = renderGeneration;
    }
    slot.timelineReservedMask |= ((1u << count) - 1u) << first;
    slot.timelineReservationCounts[first] = static_cast<std::uint8_t>(count);
    span.eventCount += count;
    slot.timelineLaneEventCount += count;
    *out = {renderSlot, renderGeneration, quantumIndex,
        slot.timelineReservationEpoch, first, count};
    return true;
}

VoicePool::Slot* VoicePool::ResolveTimelineReservation(
    const VoiceTimelineReservation& reservation) noexcept {
    if (reservation.renderSlot >= m_slots.size() || reservation.count == 0 ||
        reservation.first >= kTimelineMaxCombinedEventsPerVoiceQuantum ||
        reservation.count > kTimelineMaxCombinedEventsPerVoiceQuantum - reservation.first) return nullptr;
    auto& slot = m_slots[reservation.renderSlot];
    const auto& span = slot.render.timelineEvents;
    const auto mask = ((1u << reservation.count) - 1u) << reservation.first;
    if (!IsAudioRenderingState(slot.state.load(std::memory_order_relaxed)) ||
        slot.generation.load(std::memory_order_relaxed) != reservation.renderGeneration ||
        slot.timelineReservationEpoch != reservation.epoch ||
        slot.timelineReservationCounts[reservation.first] != reservation.count ||
        span.quantumIndex != reservation.quantum ||
        span.renderGeneration != reservation.renderGeneration ||
        span.eventCount < reservation.first + reservation.count ||
        (slot.timelineReservedMask & mask) != mask) return nullptr;
    return &slot;
}

bool VoicePool::StageReservedTimelineEvent(const VoiceTimelineReservation& reservation,
    std::uint32_t index, const VoiceTimelineEvent& event,
    const FrontierEntryReceiptRef& entry, const FrontierDueOperationRef& operation) noexcept {
    auto* slot = ResolveTimelineReservation(reservation);
    const auto& guard = event.frontierProof;
    if (slot == nullptr || index >= reservation.count ||
        guard.state != FrontierEventState::Pending || !entry.Valid() ||
        !operation.Valid() || guard.quantum != reservation.quantum ||
        guard.renderGeneration != reservation.renderGeneration) return false;
    const auto position = reservation.first + index;
    const auto mask = 1u << position;
    auto& destination = slot->render.timelineEvents.events[position];
    if ((slot->timelineFilledMask & mask) != 0 ||
        destination.frontierProof.state != FrontierEventState::Pending) return false;
    destination = event;
    destination.frontierProof.origin_ = FrozenFrontierEventProof::Origin::Page;
    slot->timelineFilledMask |= mask;
    return true;
}

bool VoicePool::ProveTimelineReservation(const VoiceTimelineReservation& reservation,
    VoiceTimelineReservationProof* out) noexcept {
    auto* slot = ResolveTimelineReservation(reservation);
    if (slot == nullptr || out == nullptr || out->events_ != nullptr) return false;
    const auto mask = ((1u << reservation.count) - 1u) << reservation.first;
    if ((slot->timelineFilledMask & mask) != mask ||
        (slot->timelineProvenMask & mask) != 0) return false;
    auto* events = slot->render.timelineEvents.events + reservation.first;
    for (std::uint32_t i = 0; i < reservation.count; ++i) {
        if (events[i].frontierProof.state != FrontierEventState::Pending) return false;
    }
    out->events_ = events;
    out->count_ = reservation.count;
    out->claims_ = &slot->timelineProvenMask;
    out->mask_ = mask;
    slot->timelineProvenMask |= mask;
    return true;
}

bool VoicePool::MatchesHeldLaneStart(std::uint32_t renderSlot,
    std::uint32_t renderGeneration, std::uint64_t quantum,
    const VoiceTimelineEvent& expected, const VoiceTimelineReservationProof& proof,
    const TimelineArmTable::LaneAudioProof& arm,
    const VariableTimelineInitializerProof& initializer) noexcept {
    if (renderSlot >= m_slots.size() || !proof.Valid() || proof.count_ != 1u ||
        expected.Kind() != TimelineEventKind::Start || proof.mask_ == 0u ||
        (proof.mask_ & (proof.mask_ - 1u)) != 0u) return false;
    const auto& slot = m_slots[renderSlot];
    const auto& span = slot.render.timelineEvents;
    if (!IsAudioRenderingState(slot.state.load(std::memory_order_acquire)) ||
        slot.generation.load(std::memory_order_acquire) != renderGeneration ||
        span.renderGeneration != renderGeneration || span.quantumIndex != quantum ||
        proof.claims_ != &slot.timelineProvenMask ||
        (slot.timelineProvenMask & proof.mask_) != proof.mask_ ||
        (slot.timelineFilledMask & proof.mask_) != proof.mask_ ||
        (slot.timelineReservedMask & proof.mask_) != proof.mask_) return false;
    std::uint32_t index = 0;
    while (index < kTimelineMaxCombinedEventsPerVoiceQuantum &&
        (proof.mask_ & (1u << index)) == 0u) ++index;
    if (index >= span.eventCount || proof.events_ != &span.events[index] ||
        slot.timelineReservationCounts[index] != 1u) return false;
    const auto& actual = *proof.events_;
    if (actual.Kind() != expected.Kind() || actual.offset != expected.offset ||
        !(actual.order == expected.order) || actual.frame != 0u || actual.endFrame != 0u ||
        actual.authoredRootStop || actual.authoredFadeOutCandidate ||
        actual.envelopeCurve != nullptr || actual.value != 0u || actual.firstOutputDeferred ||
        actual.frontierProof.state != FrontierEventState::Pending ||
        actual.frontierProof.origin_ != FrozenFrontierEventProof::Origin::Lane ||
        actual.frontierProof.quantum != quantum ||
        actual.frontierProof.renderGeneration != renderGeneration) return false;
    VariableTimelineInitializerProof fresh;
    if (!ProveTimelineVariableInitializer(renderSlot, renderGeneration, arm, &fresh)) return false;
    return fresh.Valid() == initializer.Valid() &&
        (!fresh.Valid() || (fresh.row_ == initializer.row_ &&
            fresh.nativeHandle_ == initializer.nativeHandle_ &&
            fresh.commands_ == initializer.commands_ && fresh.commandCount_ == initializer.commandCount_ &&
            fresh.order_.frame == initializer.order_.frame &&
            fresh.order_.sequence == initializer.order_.sequence &&
            fresh.order_.ordinal == initializer.order_.ordinal));
}

bool VoicePool::StageReservedLaneTimelineEvent(
    const VoiceTimelineReservation& reservation, std::uint32_t index,
    const VoiceTimelineEvent& event) noexcept {
    auto* slot = ResolveTimelineReservation(reservation);
    const auto& guard = event.frontierProof;
    if (slot == nullptr || index >= reservation.count ||
        guard.state != FrontierEventState::Pending ||
        guard.origin_ == FrozenFrontierEventProof::Origin::Page || guard.quantum != reservation.quantum ||
        guard.renderGeneration != reservation.renderGeneration) return false;
    const auto position = reservation.first + index;
    const auto mask = 1u << position;
    auto& destination = slot->render.timelineEvents.events[position];
    if ((slot->timelineFilledMask & mask) != 0 ||
        destination.frontierProof.state != FrontierEventState::Pending) return false;
    destination = event;
    destination.frontierProof.origin_ = FrozenFrontierEventProof::Origin::Lane;
    slot->timelineFilledMask |= mask;
    return true;
}

bool VoicePool::RejectTimelineReservation(const VoiceTimelineReservation& reservation) noexcept {
    auto* slot = ResolveTimelineReservation(reservation);
    if (slot == nullptr) return false;
    const auto mask = ((1u << reservation.count) - 1u) << reservation.first;
    if ((slot->timelineProvenMask & mask) != 0) return false;
    auto* events = slot->render.timelineEvents.events + reservation.first;
    for (std::uint32_t i = 0; i < reservation.count; ++i) {
        if (events[i].frontierProof.state != FrontierEventState::Pending) return false;
    }
    for (std::uint32_t i = 0; i < reservation.count; ++i)
        events[i].frontierProof.state = FrontierEventState::Rejected;
    return true;
}

void VoicePool::ApplyProvenTimelineReservation(VoiceTimelineReservationProof& proof,
    std::uint64_t cutoffFrame, AuthoredStopOperationIdentity cutoff) noexcept {
    for (std::uint32_t i = 0; i < proof.count_; ++i) {
        auto& guard = proof.events_[i].frontierProof;
        guard.cutoffFrame = cutoffFrame;
        guard.cutoffOperation = cutoff;
        guard.state = FrontierEventState::Applied;
    }
    proof.Release();
}

void VoicePool::RejectProvenTimelineReservation(VoiceTimelineReservationProof& proof) noexcept {
    assert(proof.Valid());
    for (std::uint32_t index = 0; index < proof.count_; ++index)
        proof.events_[index].frontierProof.state = FrontierEventState::Rejected;
    proof.Release();
}

void VoicePool::ApplyProvenLaneTimelineReservation(
    VoiceTimelineReservationProof& proof) noexcept {
    for (std::uint32_t i = 0; i < proof.count_; ++i)
        proof.events_[i].frontierProof.state = FrontierEventState::LaneApplied;
    proof.Release();
}

bool VoicePool::CancelTimelineBinding(
    std::uint32_t renderSlot, std::uint32_t renderGeneration) noexcept {
    return CancelTimelineBindingImpl(renderSlot, renderGeneration, ReleaseThread::Logic);
}

bool VoicePool::CancelTimelineBindingImpl(std::uint32_t renderSlot,
    std::uint32_t renderGeneration, ReleaseThread thread) noexcept {
    if (renderSlot >= m_slots.size()) {
        return false;
    }
    Slot& slot = m_slots[renderSlot];
    if (slot.state.load(std::memory_order_acquire) == SlotState::OriginalCallbackHeld) return false;
    if (slot.generation.load(std::memory_order_relaxed) != renderGeneration) {
        return false;
    }
    const SlotState state = slot.state.load(std::memory_order_acquire);
    if (state == SlotState::Claimed) {
        SlotState expected = SlotState::Claimed;
        if (!slot.state.compare_exchange_strong(expected, SlotState::ReleaseHeld,
            std::memory_order_acq_rel, std::memory_order_acquire)) return false;
        if (slot.generation.load(std::memory_order_acquire) != renderGeneration ||
            slot.timelineProvenMask != 0) {
            slot.state.store(SlotState::Claimed, std::memory_order_release);
            return false;
        }
        ReleaseSlot(slot, thread);
        return true;
    }
    if (state == SlotState::Rendering) {
        if (const auto hook = m_cancelTimelineReadHook.exchange(nullptr, std::memory_order_acq_rel))
            hook(m_cancelTimelineReadContext.load(std::memory_order_acquire));
        SlotState expected = SlotState::Rendering;
        if (!slot.state.compare_exchange_strong(expected, SlotState::ReleaseHeld,
            std::memory_order_acq_rel, std::memory_order_acquire)) return false;
        if (slot.generation.load(std::memory_order_acquire) != renderGeneration ||
            slot.timelineProvenMask != 0 || !slot.render.timelineGated) {
            slot.state.store(SlotState::Rendering, std::memory_order_release);
            return false;
        }
        ReleaseSlot(slot, thread);
        return true;
    }
    return state == SlotState::Free;
}

bool VoicePool::CancelTimelineBindingFromAudio(
    std::uint32_t renderSlot, std::uint32_t renderGeneration) noexcept {
    if (renderSlot >= m_slots.size()) return false;
    auto& slot = m_slots[renderSlot];
    if (slot.state.load(std::memory_order_acquire) == SlotState::TimelineCallbackHeld) {
        if ((!m_originalSourceCallbackOpen && !m_timelineQuantumOpen) ||
            slot.generation.load(std::memory_order_acquire) != renderGeneration ||
            slot.timelineProvenMask != 0 || !slot.render.timelineGated) return false;
        auto expected = SlotState::TimelineCallbackHeld;
        if (!slot.state.compare_exchange_strong(expected, SlotState::ReleaseHeld,
            std::memory_order_acq_rel, std::memory_order_acquire)) return false;
        ReleaseSlot(slot, ReleaseThread::Audio);
        return true;
    }
    if (slot.state.load(std::memory_order_acquire) != SlotState::OriginalCallbackHeld)
        return CancelTimelineBindingImpl(renderSlot, renderGeneration, ReleaseThread::Audio);
    // This native callback owns the installed body and its pre-existing actor.
    // Logic callers retain the generic refusal; no callback loan returns here.
    const auto& lease = m_originalCallbackStorage[renderSlot];
    if (!m_originalSourceCallbackOpen ||
        slot.generation.load(std::memory_order_acquire) != renderGeneration ||
        (slot.originalBodyLifetime.load(std::memory_order_acquire) & kOriginalBodyCallbackReader) == 0 ||
        !lease.storageHeld || !lease.actor.Valid() || lease.bodyMarker == 0 ||
        lease.bodyMarker != slot.originalBodyMarker ||
        !OriginalSourceSubmissionRegistry::SameToken(lease.arm, slot.originalBodyArm) ||
        lease.arm.renderSlot != renderSlot || lease.arm.renderGeneration != renderGeneration ||
        !slot.installedOriginalSource.Valid() ||
        !slot.originalBodyInstallationCompleted.load(std::memory_order_acquire) ||
        (slot.originalBodyLifetime.load(std::memory_order_acquire) & kOriginalBodyDisposed) != 0 ||
        slot.timelineProvenMask != 0 || !slot.render.timelineGated ||
        slot.render.timelineArmSlot != lease.arm.slot ||
        slot.render.timelineArmGeneration != lease.arm.generation) return false;
    auto expected = SlotState::OriginalCallbackHeld;
    if (!slot.state.compare_exchange_strong(expected, SlotState::ReleaseHeld,
        std::memory_order_acq_rel, std::memory_order_acquire)) return false;
    ReleaseSlot(slot, ReleaseThread::Audio);
    return true;
}

OriginalSourceLifetimePin::OriginalSourceLifetimePin(
    OriginalSourceLifetimePin&& other) noexcept { *this = std::move(other); }
OriginalSourceLifetimePin& OriginalSourceLifetimePin::operator=(
    OriginalSourceLifetimePin&& other) noexcept {
    if (this != &other) {
        Release();
        owner_ = std::exchange(other.owner_, nullptr);
        row_ = std::exchange(other.row_, nullptr);
        token_ = std::exchange(other.token_, {});
        marker_ = std::exchange(other.marker_, 0);
    }
    return *this;
}
bool OriginalSourceLifetimePin::Borrow(OriginalSourceLifetimePin* out) const noexcept {
    return owner_ != nullptr && owner_->BorrowOriginal(*this, out);
}
void OriginalSourceLifetimePin::Release() noexcept {
    if (owner_ != nullptr) owner_->ReleaseOriginal(*this);
}
OriginalSourceMutationActor::OriginalSourceMutationActor(
    OriginalSourceMutationActor&& other) noexcept { *this = std::move(other); }
OriginalSourceMutationActor& OriginalSourceMutationActor::operator=(
    OriginalSourceMutationActor&& other) noexcept {
    if (this != &other) {
        Release();
        lifetime_ = std::move(other.lifetime_);
        catalogSibling_ = std::move(other.catalogSibling_);
        catalogFallback_ = std::exchange(other.catalogFallback_, false);
        borrowedMode_ = std::exchange(other.borrowedMode_, 0);
        wrote_ = std::exchange(other.wrote_, false);
        callbackStanding_ = std::exchange(other.callbackStanding_, false);
    }
    return *this;
}
bool OriginalSourceMutationActor::Borrow(OriginalSourceMutationActor* out) const noexcept {
    return lifetime_.owner_ != nullptr && lifetime_.owner_->BorrowActor(*this, out);
}
bool OriginalSourceMutationActor::BeforeFirstWrite() noexcept {
    return lifetime_.owner_ != nullptr && lifetime_.owner_->ActorBeforeFirstWrite(*this);
}
void OriginalSourceMutationActor::Release() noexcept {
    if (lifetime_.owner_ != nullptr) lifetime_.owner_->ReturnActor(*this);
}

OriginalSourceInstallationLoan::OriginalSourceInstallationLoan(
    OriginalSourceInstallationLoan&& other) noexcept { *this = std::move(other); }

OriginalSourceInstallationLoan& OriginalSourceInstallationLoan::operator=(
    OriginalSourceInstallationLoan&& other) noexcept {
    if (this != &other) {
        Release();
        owner_ = std::exchange(other.owner_, nullptr);
        token_ = std::exchange(other.token_, {});
        marker_ = std::exchange(other.marker_, 0);
    }
    return *this;
}
bool OriginalSourceInstallationLoan::Matches(const void* actualSubmission,
    TimelineArmToken token, VnVoiceHandle sourceIdentity) const noexcept {
    return owner_ != nullptr && owner_->MatchesInstallationForSource(*this, actualSubmission,
        token, sourceIdentity);
}
void OriginalSourceInstallationLoan::Release() noexcept {
    if (owner_ != nullptr) owner_->ReleaseInstallationLoan(*this);
}
bool OriginalSourceInstallationLoan::BorrowMutationForFirstWrite(OriginalSourceMutationActor* out) const noexcept {
    return owner_ != nullptr && owner_->BorrowInstallationMutation(*this, out, true);
}

VoicePool::OriginalSourceBodyLifetimePin::OriginalSourceBodyLifetimePin(
    OriginalSourceBodyLifetimePin&& other) noexcept { *this = std::move(other); }
VoicePool::OriginalSourceBodyLifetimePin& VoicePool::OriginalSourceBodyLifetimePin::operator=(
    OriginalSourceBodyLifetimePin&& other) noexcept {
    if (this != &other) {
        Release();
        pool_ = std::exchange(other.pool_, nullptr);
        slot_ = std::exchange(other.slot_, UINT32_MAX);
        arm_ = std::exchange(other.arm_, {});
        marker_ = std::exchange(other.marker_, 0);
    }
    return *this;
}
bool VoicePool::OriginalSourceBodyLifetimePin::Borrow(OriginalSourceBodyLifetimePin* out) const noexcept {
    return pool_ != nullptr && pool_->BorrowOriginalSourceBody(*this, out);
}
void VoicePool::OriginalSourceBodyLifetimePin::Release() noexcept {
    if (pool_ != nullptr) pool_->ReleaseOriginalSourceBody(*this);
}

bool VoicePool::CanIssueOriginalSourceBody(const PreparedCommitProof& proof,
    const OriginalSourceBodyLifetimePin* out) const noexcept {
    if (out == nullptr || out->Valid() || proof.pool_ != this || !proof.Valid() ||
        m_originalBodyShutdown || m_originalBodyMarkerExhausted || m_nextOriginalBodyMarker == 0)
        return false;
    const auto& slot = *proof.slot_;
    return slot.state.load(std::memory_order_acquire) == SlotState::PreparedCommitHeld &&
        slot.originalBodyMarker == 0 &&
        slot.originalBodyLifetime.load(std::memory_order_acquire) == 0 &&
        !slot.pendingOriginalSource.Valid() && !slot.installedOriginalSource.Valid() &&
        slot.releaseSequence.load(std::memory_order_acquire) != UINT64_MAX;
}
void VoicePool::IssueOriginalSourceBody(const PreparedCommitProof& proof,
    TimelineArmToken arm, VnVoiceHandle sourceIdentity,
    OriginalSourceInstallationLoan&& installation, OriginalSourceBodyLifetimePin* out) noexcept {
    assert(CanIssueOriginalSourceBody(proof, out) && installation.Valid() && sourceIdentity != 0 &&
        proof.OriginalPreparedHandle() == OriginalSourceSubmissionRegistry::RenderOf(arm));
    auto& slot = *proof.slot_;
    const auto issued = TryIssue(&m_nextOriginalBodyMarker, &m_originalBodyMarkerExhausted);
    assert(issued.status == IssueStatus::Ok);
    slot.originalBodyMarker = issued.value;
    slot.originalBodyArm = arm;
    slot.originalBodySourceIdentity = sourceIdentity;
    slot.originalBodyReleaseTarget = slot.releaseSequence.load(std::memory_order_acquire) + 1u;
    slot.originalBodyInstallationCompleted.store(false, std::memory_order_relaxed);
    slot.pendingOriginalSource = std::move(installation);
    slot.originalBodyLifetime.store(1u, std::memory_order_release);
    out->pool_ = this;
    out->slot_ = arm.renderSlot;
    out->arm_ = arm;
    out->marker_ = issued.value;
}
bool VoicePool::MatchesPreparedOriginalSourceBody(const OriginalSourceBodyLifetimePin& original,
    const PreparedCommitProof& preparation, const PreparedCommitInput& expected,
    const OriginalSourceInstallationLoan** actualPending) const noexcept {
    return expected.completionProgress != nullptr &&
        MatchesPreparedOriginalSourceStorage(original, preparation, expected, actualPending);
}
bool VoicePool::MatchesPreparedOriginalSourceStorage(const OriginalSourceBodyLifetimePin& original,
    const PreparedCommitProof& preparation, const PreparedCommitInput& expected,
    const OriginalSourceInstallationLoan** actualPending) const noexcept {
    if (actualPending == nullptr || !MatchesOriginalSourceBody(original) || preparation.pool_ != this ||
        preparation.slot_ != &m_slots[original.slot_] || !preparation.Valid() ||
        expected.renderHandle != OriginalSourceSubmissionRegistry::RenderOf(original.arm_) ||
        preparation.OriginalPreparedHandle() != expected.renderHandle ||
        (expected.sourceTerminal == nullptr
            ? expected.sourceTerminalToken.slot != UINT32_MAX || expected.sourceTerminalToken.generation != 0
            : !expected.sourceTerminalToken.Valid()) ||
        (expected.streamRing == nullptr
            ? expected.streamRingToken != 0 || expected.streamGeneration != 0
            : expected.streamRingToken == 0 || expected.streamGeneration == 0)) return false;
    const auto& slot = *preparation.slot_;
    const auto& render = slot.render;
    PreparedRoutingV1 routing{};
    const auto release = slot.releaseSequence.load(std::memory_order_acquire);
    if (slot.state.load(std::memory_order_acquire) != SlotState::PreparedCommitHeld ||
        slot.generation.load(std::memory_order_acquire) != original.arm_.renderGeneration ||
        !slot.pendingOriginalSource.Valid() || slot.installedOriginalSource.Valid() ||
        (slot.originalBodyLifetime.load(std::memory_order_acquire) & kOriginalBodyDisposed) != 0 ||
        release == UINT64_MAX || slot.originalBodyReleaseTarget != release + 1u ||
        !expected.originalRelease.Valid() || expected.originalRelease.slot != original.slot_ ||
        expected.originalRelease.targetSequence != slot.originalBodyReleaseTarget ||
        render.sourceTerminal != expected.sourceTerminal ||
        render.sourceTerminalToken.slot != expected.sourceTerminalToken.slot ||
        render.sourceTerminalToken.generation != expected.sourceTerminalToken.generation ||
        render.completionProgress != expected.completionProgress ||
        render.completionOccurrence.slot != expected.completionOccurrence.slot ||
        render.completionOccurrence.generation != expected.completionOccurrence.generation ||
        render.completionRenderGeneration != expected.completionRenderGeneration ||
        render.playbackFactsRevision != expected.playbackFactsRevision ||
        (expected.completionProgress == nullptr
            ? expected.completionOccurrence.slot != 0 || expected.completionOccurrence.generation != 0 ||
                expected.completionRenderGeneration != 0 || expected.playbackFactsRevision != 0
            : expected.completionOccurrence.generation == 0 ||
                expected.completionRenderGeneration != original.arm_.renderGeneration || expected.playbackFactsRevision == 0) ||
        render.streamRing != expected.streamRing ||
        slot.streamRingToken.load(std::memory_order_acquire) != expected.streamRingToken ||
        (expected.streamRing != nullptr && expected.streamRing->generation.load(std::memory_order_acquire) != expected.streamGeneration) ||
        !CapturePreparedRoutingV1(expected.renderHandle, &routing) || routing != expected.routing) return false;
    *actualPending = &slot.pendingOriginalSource;
    return true;
}

bool VoicePool::MatchesOriginalSourceBody(const OriginalSourceBodyLifetimePin& original) const noexcept {
    if (original.pool_ != this || original.slot_ >= m_slots.size() || original.marker_ == 0)
        return false;
    const auto& slot = m_slots[original.slot_];
    return slot.originalBodyMarker == original.marker_ &&
        OriginalSourceSubmissionRegistry::SameToken(slot.originalBodyArm, original.arm_) &&
        original.arm_.renderSlot == original.slot_ &&
        (slot.originalBodyLifetime.load(std::memory_order_acquire) & kOriginalBodyPinMask) != 0;
}
bool VoicePool::IsOriginalSourceBodyLifetimePinFor(const OriginalSourceBodyLifetimePin& original,
    TimelineArmToken arm, VnVoiceHandle sourceIdentity) const noexcept {
    return MatchesOriginalSourceBody(original) &&
        OriginalSourceSubmissionRegistry::SameToken(original.arm_, arm) && sourceIdentity != 0 &&
        m_slots[original.slot_].originalBodySourceIdentity == sourceIdentity;
}
bool VoicePool::IsOriginalSourceBodyCurrent(const OriginalSourceBodyLifetimePin& original) const noexcept {
    if (!MatchesOriginalSourceBody(original)) return false;
    const auto& slot = m_slots[original.slot_];
    return slot.originalBodyInstallationCompleted.load(std::memory_order_acquire) &&
        (slot.originalBodyLifetime.load(std::memory_order_acquire) & kOriginalBodyDisposed) == 0 &&
        IsAudioRenderingState(slot.state.load(std::memory_order_acquire)) &&
        slot.generation.load(std::memory_order_acquire) == original.arm_.renderGeneration &&
        slot.releaseSequence.load(std::memory_order_acquire) < slot.originalBodyReleaseTarget;
}
bool VoicePool::MatchesOriginalSourceRetirementPair(const OriginalVoiceOwner& owner,
    const OriginalSourceBodyLifetimePin& body) const noexcept {
    return owner.pool_ == this && owner.Valid() && owner.releaseTarget_ != 0 &&
        MatchesOriginalSourceBody(body) && body.slot_ == static_cast<std::uint32_t>(owner.handle_) &&
        body.arm_.renderGeneration == static_cast<std::uint32_t>(owner.handle_ >> 32) &&
        m_slots[body.slot_].originalBodyReleaseTarget == owner.releaseTarget_;
}
void VoicePool::RequestOriginalSourceBodyRetirement(const OriginalSourceBodyLifetimePin& body) noexcept {
    assert(MatchesOriginalSourceBody(body));
    m_slots[body.slot_].originalBodyRetirementRequest.store(body.marker_, std::memory_order_release);
}

bool VoicePool::CaptureOriginalSourceRetirementIdentity(const OriginalVoiceOwner& owner,
    const OriginalSourceBodyLifetimePin& body, VoiceReleaseObserver* release,
    SourceTerminalToken* terminal) const noexcept {
    if (release == nullptr || terminal == nullptr ||
        !MatchesOriginalSourceRetirementPair(owner, body) || !IsOriginalSourceBodyCurrent(body)) return false;
    const auto& render = m_slots[body.slot_].render;
    const auto* source = render.sourceTerminal;
    if (source == nullptr && !render.sourceTerminalToken.Valid()) {
        *release = {body.slot_, owner.releaseTarget_};
        *terminal = {};
        return true;
    }
    if (source == nullptr || !render.sourceTerminalToken.Valid() || !source->occupied ||
        source->generation != render.sourceTerminalToken.generation || source->render != owner.handle_ ||
        source->release.slot != body.slot_ || source->release.targetSequence != owner.releaseTarget_) return false;
    *release = source->release;
    *terminal = render.sourceTerminalToken;
    return true;
}

bool VoicePool::CanReturnUnpublishedOriginalSourceBodyStorage(const OriginalSourceBodyLifetimePin& body,
    const OriginalSourceInstallationLoan** installation) const noexcept {
    if (installation == nullptr || !MatchesOriginalSourceBody(body)) return false;
    const auto& slot = m_slots[body.slot_];
    const auto lifetime = slot.originalBodyLifetime.load(std::memory_order_acquire);
    if ((lifetime & kOriginalBodyPinMask) != 1u || (lifetime & kOriginalBodyDisposed) == 0 ||
        (lifetime & (kOriginalBodyLoanReturned | kOriginalBodyReturnClaimed |
            kOriginalBodyCallbackReader)) != 0 ||
        !slot.pendingOriginalSource.Valid() || slot.installedOriginalSource.Valid()) return false;
    *installation = &slot.pendingOriginalSource;
    return true;
}
bool VoicePool::CanReturnPublishedOriginalSourceBodyStorage(const OriginalVoiceOwner& owner,
    const OriginalSourceBodyLifetimePin& body, VoiceReleaseObserver release,
    const OriginalSourceInstallationLoan** installation) const noexcept {
    if (installation == nullptr || !MatchesOriginalSourceRetirementPair(owner, body) ||
        !release.Valid() || release.slot != body.slot_ || release.targetSequence != owner.releaseTarget_ ||
        !OriginalVoiceReleased(owner) || !ReleaseObserved(release)) return false;
    const auto& slot = m_slots[body.slot_];
    const auto lifetime = slot.originalBodyLifetime.load(std::memory_order_acquire);
    if ((lifetime & kOriginalBodyPinMask) == 0 || (lifetime & kOriginalBodyDisposed) == 0 ||
        (lifetime & (kOriginalBodyLoanReturned | kOriginalBodyReturnClaimed |
            kOriginalBodyCallbackReader)) != 0 ||
        slot.pendingOriginalSource.Valid() == slot.installedOriginalSource.Valid()) return false;
    // Cancellation before installation keeps the pending association.
    // Extra storage loans retain their own episode after this holder returns.
    *installation = slot.installedOriginalSource.Valid()
        ? &slot.installedOriginalSource : &slot.pendingOriginalSource;
    return true;
}

bool VoicePool::ConsumeOriginalSourceBodyRetirement(Slot& slot, std::uint64_t blockIndex) noexcept {
    if (slot.state.load(std::memory_order_acquire) != SlotState::OriginalCallbackHeld) return false;
    const auto marker = slot.originalBodyRetirementRequest.load(std::memory_order_acquire);
    if (marker == 0 || marker != slot.originalBodyMarker) return false;
    const auto index = static_cast<std::size_t>(&slot - m_slots.data());
    const auto& lease = m_originalCallbackStorage[index];
    if (!lease.storageHeld || !lease.actor.Valid() || lease.bodyMarker != marker ||
        (slot.originalBodyLifetime.load(std::memory_order_acquire) & kOriginalBodyCallbackReader) == 0 ||
        !OriginalSourceSubmissionRegistry::SameToken(lease.arm, slot.originalBodyArm) ||
        !slot.installedOriginalSource.Valid()) return false;
    return slot.installedOriginalSource.owner_->RetireRequestedInstalledSource(
        slot.installedOriginalSource, blockIndex);
}
bool VoicePool::BorrowOriginalSourceBody(const OriginalSourceBodyLifetimePin& original,
    OriginalSourceBodyLifetimePin* out) noexcept {
    if (out == nullptr || out->Valid() || !MatchesOriginalSourceBody(original)) return false;
    auto& slot = m_slots[original.slot_];
    auto lifetime = slot.originalBodyLifetime.load(std::memory_order_acquire);
    for (;;) {
        const auto count = lifetime & kOriginalBodyPinMask;
        if (count == 0 || count == kOriginalBodyPinMask || (lifetime & kOriginalBodyLoanReturned) != 0)
            return false;
        if (slot.originalBodyLifetime.compare_exchange_weak(lifetime, lifetime + 1u,
            std::memory_order_acq_rel, std::memory_order_acquire)) break;
    }
    out->pool_ = this;
    out->slot_ = original.slot_;
    out->arm_ = original.arm_;
    out->marker_ = original.marker_;
    return true;
}
void VoicePool::ReleaseOriginalSourceBody(OriginalSourceBodyLifetimePin& original) noexcept {
    const bool matches = MatchesOriginalSourceBody(original);
    assert(matches);
    if (!matches) return;
    auto& slot = m_slots[original.slot_];
    const auto previous = slot.originalBodyLifetime.fetch_sub(1u, std::memory_order_acq_rel);
    assert((previous & kOriginalBodyPinMask) != 0 && (previous & kOriginalBodyLoanReturned) == 0);
    if ((previous & kOriginalBodyPinMask) == 1u &&
        (previous & (kOriginalBodyDisposed | kOriginalBodyCallbackReader)) == kOriginalBodyDisposed)
        ReturnOriginalSourceInstallation(slot);
    original.pool_ = nullptr;
    original.slot_ = UINT32_MAX;
    original.arm_ = {};
    original.marker_ = 0;
}
void VoicePool::AcquireOriginalCallbackStorage(Slot& slot,
    const SourceCommandInstallProof* installation) noexcept {
    auto state = slot.state.load(std::memory_order_acquire);
    if (installation != nullptr) {
        // Only the actual exclusive native installation proof can hand off.
        assert(installation->pool_ == this && installation->slot_ == &slot &&
            installation->Valid() && installation->begun_ &&
            state == SlotState::CommandInstallHeld);
    } else {
        auto expected = SlotState::Rendering;
        if (!slot.state.compare_exchange_strong(expected, SlotState::OriginalCallbackHeld,
                std::memory_order_acq_rel, std::memory_order_acquire)) return;
        state = SlotState::OriginalCallbackHeld;
    }
    // No body marker, installed loan or payload was read before native ownership.
    if (slot.originalBodyMarker == 0) {
        if (state == SlotState::OriginalCallbackHeld)
            slot.state.store(slot.render.timelineGated
                ? SlotState::TimelineCallbackHeld : SlotState::Rendering, std::memory_order_release);
        return;
    }
    const auto index = static_cast<std::size_t>(&slot - m_slots.data());
    auto& lease = m_originalCallbackStorage[index];
    assert(!lease.storageHeld &&
        (slot.originalBodyLifetime.load(std::memory_order_acquire) & kOriginalBodyCallbackReader) == 0);
    assert(slot.installedOriginalSource.Valid() &&
        (slot.originalBodyLifetime.load(std::memory_order_acquire) & kOriginalBodyDisposed) == 0);
    slot.originalBodyLifetime.fetch_or(kOriginalBodyCallbackReader, std::memory_order_acq_rel);
    lease.bodyMarker = slot.originalBodyMarker;
    lease.arm = slot.originalBodyArm;
    lease.storageHeld = true;
    const bool borrowed = slot.installedOriginalSource.BorrowMutationForFirstWrite(&lease.actor);
    assert(borrowed);
    if (borrowed) {
        const bool wrote = lease.actor.BeforeFirstWrite();
        assert(wrote);
        (void)wrote;
    }
}

void VoicePool::ReturnOriginalCallbackStorage(Slot& slot,
    OriginalCallbackStorageLease& lease) noexcept {
    assert(lease.storageHeld &&
        (slot.originalBodyLifetime.load(std::memory_order_acquire) & kOriginalBodyCallbackReader) != 0);
    // Clear the reusable holder before any last native Return can publish reuse.
    auto actor = std::move(lease.actor);
    lease.arm = {};
    lease.bodyMarker = 0;
    lease.storageHeld = false;
    // All native payload users have finished. Restore a surviving render slot
    // while the real storage reader still excludes any physical reuse.
    auto expected = SlotState::OriginalCallbackHeld;
    (void)slot.state.compare_exchange_strong(expected, SlotState::Rendering,
        std::memory_order_acq_rel, std::memory_order_acquire);
    actor.Release();
    // Body returns and callback exit share one modification order, so the last
    // user observes all preceding exits before claiming the installation.
    const auto previous = slot.originalBodyLifetime.fetch_and(~kOriginalBodyCallbackReader,
        std::memory_order_acq_rel);
    assert((previous & kOriginalBodyCallbackReader) != 0);
    if ((previous & (kOriginalBodyDisposed | kOriginalBodyPinMask)) == kOriginalBodyDisposed)
        ReturnOriginalSourceInstallation(slot);
    // No Slot or Source payload access follows the final installation Return.
}

void VoicePool::ReturnOriginalSourceInstallation(Slot& slot) noexcept {
    auto lifetime = slot.originalBodyLifetime.load(std::memory_order_acquire);
    for (;;) {
        if ((lifetime & kOriginalBodyDisposed) == 0 ||
            (lifetime & (kOriginalBodyPinMask | kOriginalBodyCallbackReader)) != 0 ||
            (lifetime & (kOriginalBodyReturnClaimed | kOriginalBodyLoanReturned)) != 0) return;
        if (slot.originalBodyLifetime.compare_exchange_weak(lifetime,
                lifetime | kOriginalBodyReturnClaimed, std::memory_order_acq_rel,
                std::memory_order_acquire)) break;
    }
    // Last foreign Body Return may race callback last use. Only this actual
    // native claim winner returns the installation association.
    assert(!(slot.pendingOriginalSource.Valid() && slot.installedOriginalSource.Valid()));
    slot.pendingOriginalSource.Release();
    slot.installedOriginalSource.Release();
    slot.originalBodyLifetime.fetch_or(kOriginalBodyLoanReturned, std::memory_order_release);
}
void VoicePool::CompleteOriginalSourceBodyDisposition(Slot& slot) noexcept {
    if (slot.originalBodyMarker == 0) return;
    slot.originalBodyInstallationCompleted.store(false, std::memory_order_release);
    const auto previous = slot.originalBodyLifetime.fetch_or(kOriginalBodyDisposed,
        std::memory_order_acq_rel);
    if ((previous & (kOriginalBodyDisposed | kOriginalBodyPinMask | kOriginalBodyCallbackReader)) == 0)
        ReturnOriginalSourceInstallation(slot);
}
void VoicePool::ResetOriginalSourceBodyForReuse(Slot& slot) noexcept {
    const auto lifetime = slot.originalBodyLifetime.load(std::memory_order_acquire);
    assert((lifetime & (kOriginalBodyPinMask | kOriginalBodyCallbackReader)) == 0 &&
        (slot.originalBodyMarker == 0 || (lifetime & kOriginalBodyLoanReturned) != 0) &&
        !slot.pendingOriginalSource.Valid() && !slot.installedOriginalSource.Valid());
    slot.originalBodyMarker = 0;
    slot.originalBodyRetirementRequest.store(0, std::memory_order_relaxed);
    slot.originalBodyArm = {};
    slot.originalBodySourceIdentity = 0;
    slot.originalBodyReleaseTarget = 0;
    slot.originalBodyInstallationCompleted.store(false, std::memory_order_relaxed);
    slot.originalBodyLifetime.store(0, std::memory_order_relaxed);
}
void VoicePool::ShutdownOriginalSourceBodiesAfterQuiescence() noexcept {
    // Unbind can return without joining an offline thread set. Stop is the
    // actual thread join, and the lifecycle owner has already stopped callback.
    m_workers.Stop();
    m_originalBodyShutdown = true;
    for (auto& slot : m_slots) CompleteOriginalSourceBodyDisposition(slot);
    EndAudioBlock();
    for (auto& slot : m_slots) slot.nativeVariableStart = {};
    m_nativeVariableStartAuthority = {};
    m_timelineVariableStartAuthority = {};
    for (auto& slot : m_slots) ClearFirstOutput(slot);
    m_firstOutputCommandConsumer = nullptr;
    m_firstOutputCommandContext = nullptr;
}

VoicePool::SourceCommandInstallProof::SourceCommandInstallProof(
    SourceCommandInstallProof&& other) noexcept { *this = std::move(other); }

VoicePool::SourceCommandInstallProof& VoicePool::SourceCommandInstallProof::operator=(
    SourceCommandInstallProof&& other) noexcept {
    if (this != &other) {
        Release();
        assert(other.pool_ == nullptr || other.pool_->m_commandMetadataProof != &other);
        pool_ = std::exchange(other.pool_, nullptr);
        slot_ = std::exchange(other.slot_, nullptr);
        source_ = std::exchange(other.source_, nullptr);
        release_ = other.release_;
        block_ = other.block_;
        originalBodyMarker_ = std::exchange(other.originalBodyMarker_, 0);
        begun_ = std::exchange(other.begun_, false);
    }
    return *this;
}

void VoicePool::SourceCommandInstallProof::Release() noexcept {
    if (slot_ == nullptr) return;
    assert(pool_ != nullptr && pool_->m_commandMetadataProof != this &&
        slot_->state.load(std::memory_order_acquire) == SlotState::CommandInstallHeld);
    // Only an untouched pre-Pop proof may roll back. An interrupted installed
    // source is physically disposed by this actual exclusive owner.
    if (begun_) pool_->ReleaseSlot(*slot_, ReleaseThread::Audio);
    else slot_->state.store(SlotState::Claimed, std::memory_order_release);
    slot_ = nullptr;
    pool_ = nullptr;
    source_ = nullptr;
    originalBodyMarker_ = 0;
    begun_ = false;
}

VoicePool::SourceCommandMetadataAccess::SourceCommandMetadataAccess(
    SourceCommandInstallProof& proof, bool enabled) noexcept {
    if (!enabled || !proof.Valid()) return;
    assert(proof.begun_ && proof.source_->Valid() && proof.pool_->m_commandMetadataProof == nullptr);
    pool_ = proof.pool_;
    pool_->m_commandMetadataProof = &proof;
}

void VoicePool::SourceCommandMetadataAccess::Release() noexcept {
    if (pool_ != nullptr) pool_->m_commandMetadataProof = nullptr;
    pool_ = nullptr;
}

VoicePool::SourceCommandInstallStatus VoicePool::ProveSourceCommandInstallation(
    const TimelineArmTable::SourceCommandRowProof& source,
    SourceCommandInstallProof* out, const OriginalVoiceOwner* original) noexcept {
    using Status = SourceCommandInstallStatus;
    if (!source.Valid() || source.IsCancellation() || source.CompletionSequence() != 0 ||
        source.Owner() != m_timelineArms || out == nullptr || out->Valid())
        return Status::NotClaimed;
    const auto token = source.Token();
    if (token.renderSlot >= m_slots.size()) return Status::NotClaimed;
    auto& slot = m_slots[token.renderSlot];
    SlotState expected = SlotState::Claimed;
    if (!slot.state.compare_exchange_strong(expected, SlotState::CommandInstallHeld,
        std::memory_order_acq_rel, std::memory_order_acquire))
        return expected == SlotState::CommandInstallHeld || expected == SlotState::ReleaseHeld
            ? Status::DeferredInstallation : Status::NotClaimed;
    const auto release = slot.releaseSequence.load(std::memory_order_acquire);
    if (slot.generation.load(std::memory_order_acquire) != token.renderGeneration ||
        release == UINT64_MAX ||
        (source.OriginalSourceSubmission() != nullptr && original == nullptr) ||
        (original != nullptr && (original->pool_ != this || !original->Valid() ||
            original->handle_ != MakeHandle(token.renderSlot, token.renderGeneration) ||
            original->releaseTarget_ == 0 || release != original->releaseTarget_ - 1u)) ||
        (slot.originalBodyMarker != 0 &&
            (!slot.pendingOriginalSource.Valid() || slot.installedOriginalSource.Valid() ||
                !slot.pendingOriginalSource.Matches(source.OriginalSourceSubmission(), token,
                    slot.originalBodySourceIdentity) ||
                !OriginalSourceSubmissionRegistry::SameToken(slot.originalBodyArm, token) ||
                slot.originalBodyReleaseTarget != release + 1u ||
                (slot.originalBodyLifetime.load(std::memory_order_acquire) & kOriginalBodyDisposed) != 0))) {
        slot.state.store(SlotState::Claimed, std::memory_order_release);
        return Status::NotClaimed;
    }
    out->pool_ = this;
    out->slot_ = &slot;
    out->source_ = &source;
    out->release_ = {token.renderSlot, release + 1u};
    out->originalBodyMarker_ = slot.originalBodyMarker;
    out->begun_ = false;
    return Status::Ready;
}

void VoicePool::BeginSourceCommandInstallation(SourceCommandInstallProof& proof,
    VnVoiceHandle sourceIdentity, std::uint64_t sourceStartFrame,
    std::uint64_t blockIndex) noexcept {
    assert(proof.Valid() && proof.source_->Valid() && !proof.begun_);
    auto& render = proof.slot_->render;
    const auto token = proof.source_->Token();
    if (proof.originalBodyMarker_ != 0) {
        assert(proof.slot_->originalBodyMarker == proof.originalBodyMarker_ &&
            proof.slot_->originalBodySourceIdentity == sourceIdentity &&
            proof.slot_->pendingOriginalSource.Matches(proof.source_->OriginalSourceSubmission(), token,
                proof.slot_->originalBodySourceIdentity) &&
            !proof.slot_->installedOriginalSource.Valid());
        // Pre-owned factory association moves; this cannot allocate or Borrow.
        proof.slot_->installedOriginalSource = std::move(proof.slot_->pendingOriginalSource);
    }
    render.timelineGated = true;
    render.timelineArmSlot = token.slot;
    render.timelineArmGeneration = token.generation;
    render.timelineParentGroup = proof.source_->Parent();
    render.timelineGroupMemberIndex = proof.source_->MemberIndex();
    proof.pool_->InitializePlay(*proof.slot_, sourceIdentity, sourceStartFrame, blockIndex);
    proof.block_ = blockIndex;
    proof.begun_ = true;
}

void VoicePool::FinishSourceCommandInstallation(SourceCommandInstallProof& proof) noexcept {
    assert(proof.Valid() && proof.begun_ && proof.source_->Valid() &&
        proof.source_->CompletionSequence() == proof.block_ + 1u &&
        proof.pool_->m_commandMetadataProof == nullptr);
    if (proof.originalBodyMarker_ != 0) {
        assert(proof.slot_->originalBodyMarker == proof.originalBodyMarker_ &&
            proof.slot_->installedOriginalSource.Matches(proof.source_->OriginalSourceSubmission(),
                proof.source_->Token(), proof.slot_->originalBodySourceIdentity));
        proof.slot_->originalBodyInstallationCompleted.store(true, std::memory_order_release);
        if (proof.pool_->m_originalSourceCallbackOpen)
            proof.pool_->AcquireOriginalCallbackStorage(*proof.slot_, &proof);
    }
    const auto installedState =
        (proof.slot_->originalBodyLifetime.load(std::memory_order_acquire) & kOriginalBodyCallbackReader) != 0
        ? SlotState::OriginalCallbackHeld : SlotState::Rendering;
    proof.slot_->state.store(installedState, std::memory_order_release);
    proof.originalBodyMarker_ = 0;
    proof.slot_ = nullptr;
    proof.pool_ = nullptr;
    proof.source_ = nullptr;
    proof.begun_ = false;
}

bool VoicePool::AbortClaimedVoice(VnVoiceHandle handle) noexcept {
    Slot* slot = SlotFor(handle);
    if (slot == nullptr) return false;
    SlotState expected = SlotState::Claimed;
    if (!slot->state.compare_exchange_strong(expected, SlotState::ReleaseHeld,
        std::memory_order_acq_rel, std::memory_order_acquire)) return false;
    if (slot->generation.load(std::memory_order_acquire) !=
        static_cast<std::uint32_t>(handle >> 32)) {
        slot->state.store(SlotState::Claimed, std::memory_order_release);
        return false;
    }
    ReleaseSlot(*slot, ReleaseThread::Logic);
    return true;
}

bool VoicePool::CommitPreparedVoice(VnVoiceHandle handle) noexcept {
    Slot* slot = SlotFor(handle);
    if (slot == nullptr || slot->state.load(std::memory_order_acquire) != SlotState::Prepared) {
        return false;
    }
    SlotState expected = SlotState::Prepared;
    const auto committedState = slot->nativeVariableStart.cut.Valid()
        ? SlotState::VariableStartClaimed : SlotState::Claimed;
    if (!slot->state.compare_exchange_strong(
            expected, committedState, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return false;
    }
    const std::uint32_t active = ActiveCount();
    if (active > m_highWater) {
        m_highWater = active;
    }
    return true;
}

bool VoicePool::BindPreparedNativeReadCut(VnVoiceHandle handle,
    const VariableNativeStartIdentity& identity) noexcept {
    auto* slot = SlotFor(handle);
    if (slot == nullptr || slot->state.load(std::memory_order_acquire) != SlotState::Prepared ||
        !m_nativeVariableStartAuthority.Valid() || !identity.cut.Valid() ||
        identity.nativeHandle != handle || identity.transport == 0 || slot->originalBodyMarker != 0 ||
        slot->nativeVariableStart.cut.Valid() || slot->timelineVariableReadUse.Valid() || identity.sequence == 0 ||
        !BindPreparedFirstOutputOrder(handle,
            {identity.startFrame, identity.sequence, identity.executionOrdinalExclusive})) return false;
    slot->nativeVariableStart = identity;
    return true;
}

bool VoicePool::BindPreparedTimelineReadUse(VnVoiceHandle handle, VariableTimelineReadUseRef use,
    VariablePlaybackOrder order, bool pendingPlacement) noexcept {
    auto* slot = SlotFor(handle);
    if (slot == nullptr || !use.Valid() || !m_timelineVariableStartAuthority.Valid() ||
        slot->timelineVariableReadUse.Valid() || slot->nativeVariableStart.cut.Valid() ||
        !BindPreparedFirstOutputOrder(handle, order)) return false;
    slot->timelineVariableReadUse = use;
    slot->firstOutputOrderFinal = !pendingPlacement;
    return true;
}

bool VoicePool::ProveTimelineVariableInitializer(std::uint32_t renderSlot, std::uint32_t renderGeneration,
    const TimelineArmTable::PlainAudioProof& arm, VariableTimelineInitializerProof* out) noexcept {
    if (!HasTimelineVariableSourceRead(renderSlot, renderGeneration)) return true;
    const auto& slot = m_slots[renderSlot];
    if (m_timelineVariableStartAuthority.provePlain == nullptr ||
        m_timelineVariableStartAuthority.apply == nullptr || m_firstOutputCommandConsumer == nullptr) return false;
    const auto nativeHandle = (std::uint64_t{renderGeneration} << 32) | renderSlot;
    return m_timelineVariableStartAuthority.provePlain(m_timelineVariableStartAuthority.context,
        slot.timelineVariableReadUse, nativeHandle, arm, out);
}

bool VoicePool::ProveTimelineVariableInitializer(std::uint32_t renderSlot, std::uint32_t renderGeneration,
    const TimelineArmTable::LaneAudioProof& arm, VariableTimelineInitializerProof* out) noexcept {
    if (!HasTimelineVariableSourceRead(renderSlot, renderGeneration)) return true;
    const auto& slot = m_slots[renderSlot];
    if (m_timelineVariableStartAuthority.proveLane == nullptr ||
        m_timelineVariableStartAuthority.apply == nullptr || m_firstOutputCommandConsumer == nullptr) return false;
    const auto nativeHandle = (std::uint64_t{renderGeneration} << 32) | renderSlot;
    return m_timelineVariableStartAuthority.proveLane(m_timelineVariableStartAuthority.context,
        slot.timelineVariableReadUse, nativeHandle, arm, out);
}

bool VoicePool::ProveTimelineVariableInitializer(std::uint32_t renderSlot, std::uint32_t renderGeneration,
    const TimelineArmTable::BoundAudioProof& arm, VariableTimelineInitializerProof* out) noexcept {
    if (!HasTimelineVariableSourceRead(renderSlot, renderGeneration)) return true;
    const auto& slot = m_slots[renderSlot];
    if (m_timelineVariableStartAuthority.proveBound == nullptr ||
        m_timelineVariableStartAuthority.apply == nullptr || m_firstOutputCommandConsumer == nullptr) return false;
    const auto nativeHandle = (std::uint64_t{renderGeneration} << 32) | renderSlot;
    return m_timelineVariableStartAuthority.proveBound(m_timelineVariableStartAuthority.context,
        slot.timelineVariableReadUse, nativeHandle, arm, out);
}

void VoicePool::ApplyTimelineVariableInitializer(VariableTimelineInitializerProof& proof) noexcept {
    if (!proof.Valid()) return;
    const auto renderSlot = static_cast<std::uint32_t>(proof.nativeHandle_);
    const auto renderGeneration = static_cast<std::uint32_t>(proof.nativeHandle_ >> 32);
    assert(HasTimelineVariableSourceRead(renderSlot, renderGeneration));
    auto& slot = m_slots[renderSlot];
    slot.firstOutputOrder = proof.order_;
    m_timelineVariableStartAuthority.apply(proof, m_firstOutputCommandContext, m_firstOutputCommandConsumer);
    slot.firstOutputOrderFinal = true;
    const auto base = static_cast<std::size_t>(renderSlot) * m_firstOutputStride;
    for (std::uint32_t index = 0; index < m_firstOutputStride; ++index) {
        auto& endpoint = m_firstOutputEndpoints[base + index];
        if (endpoint.valid && endpoint.order.frame <= slot.firstOutputOrder.frame &&
            (endpoint.order.sequence < slot.firstOutputOrder.sequence ||
             (endpoint.order.sequence == slot.firstOutputOrder.sequence &&
                endpoint.order.ordinal < slot.firstOutputOrder.ordinal))) endpoint.valid = false;
    }
    const auto weightBase = static_cast<std::size_t>(renderSlot) * kSelectionPathNodeCapacity;
    for (std::uint32_t index = 0; index < kSelectionPathNodeCapacity; ++index) {
        auto& endpoint = m_firstOutputWeights[weightBase + index];
        if (endpoint.valid && endpoint.order.frame <= slot.firstOutputOrder.frame &&
            (endpoint.order.sequence < slot.firstOutputOrder.sequence ||
             (endpoint.order.sequence == slot.firstOutputOrder.sequence &&
                endpoint.order.ordinal < slot.firstOutputOrder.ordinal))) endpoint.valid = false;
    }
    FinishTimelineVariableSourceRead(renderSlot, renderGeneration);
    proof.Reset();
}

bool VoicePool::ProveTimelineVariableInitializer(std::uint32_t renderSlot, std::uint32_t renderGeneration,
    const TimelineArmTable::AudioOperationClaim& arm, VariableTimelineInitializerProof* out) noexcept {
    if (!HasTimelineVariableSourceRead(renderSlot, renderGeneration)) return true;
    const auto& slot = m_slots[renderSlot];
    if (m_timelineVariableStartAuthority.proveOperation == nullptr ||
        m_timelineVariableStartAuthority.apply == nullptr || m_firstOutputCommandConsumer == nullptr) return false;
    const auto nativeHandle = (std::uint64_t{renderGeneration} << 32) | renderSlot;
    return m_timelineVariableStartAuthority.proveOperation(m_timelineVariableStartAuthority.context,
        slot.timelineVariableReadUse, nativeHandle, arm, out);
}

VariableReadCutStatus VoicePool::TimelineEntryVariableSourcesStatus(
    const FrontierActivationDescriptor& descriptor) const noexcept {
    if (!descriptor.initialVariables.Valid()) return VariableReadCutStatus::Applied;
    if (m_timelineVariableStartAuthority.entryReady == nullptr) return VariableReadCutStatus::Failed;
    return m_timelineVariableStartAuthority.entryReady(m_timelineVariableStartAuthority.context, descriptor);
}

VariableReadCutStatus VoicePool::TimelineVariableSourcesStatus(std::uint32_t renderSlot,
    std::uint32_t renderGeneration) const noexcept {
    if (renderSlot >= m_slots.size()) return VariableReadCutStatus::Failed;
    const auto& slot = m_slots[renderSlot];
    if (slot.generation.load(std::memory_order_acquire) != renderGeneration ||
        slot.state.load(std::memory_order_acquire) == SlotState::Free) return VariableReadCutStatus::Failed;
    if (!slot.timelineVariableReadUse.Valid()) return VariableReadCutStatus::Applied;
    if (!m_timelineVariableStartAuthority.Valid()) return VariableReadCutStatus::Failed;
    const auto nativeHandle = (std::uint64_t{renderGeneration} << 32) | renderSlot;
    return m_timelineVariableStartAuthority.ready(m_timelineVariableStartAuthority.context,
        slot.timelineVariableReadUse, nativeHandle);
}

VariableReadCutStatus VoicePool::TimelineStartSourcesStatus(const TimelineArmTable& arms,
    TimelineArmToken arm, bool* waitingPublication) const noexcept {
    if (waitingPublication != nullptr) *waitingPublication = false;
    const auto variables = TimelineVariableSourcesStatus(arm.renderSlot, arm.renderGeneration);
    if (variables == VariableReadCutStatus::Failed) return variables;
    if (!arms.SourcePublicationAccepted(arm)) {
        if (waitingPublication != nullptr) *waitingPublication = true;
        return VariableReadCutStatus::Waiting;
    }
    return variables;
}

bool VoicePool::PendingTimelineVariableReadMatchesArm(TimelineArmToken arm) const noexcept {
    if (!arm.Valid() || arm.renderSlot >= m_slots.size() ||
        m_timelineVariableStartAuthority.pendingArm == nullptr) return false;
    const auto& slot = m_slots[arm.renderSlot];
    if (slot.generation.load(std::memory_order_acquire) != arm.renderGeneration ||
        slot.state.load(std::memory_order_acquire) == SlotState::Free ||
        !slot.timelineVariableReadUse.Valid()) return false;
    const auto nativeHandle = (std::uint64_t{arm.renderGeneration} << 32) | arm.renderSlot;
    return m_timelineVariableStartAuthority.pendingArm(m_timelineVariableStartAuthority.context,
        slot.timelineVariableReadUse, nativeHandle, arm);
}

bool VoicePool::HasTimelineVariableSourceRead(std::uint32_t renderSlot,
    std::uint32_t renderGeneration) const noexcept {
    return renderSlot < m_slots.size() &&
        m_slots[renderSlot].generation.load(std::memory_order_acquire) == renderGeneration &&
        m_slots[renderSlot].timelineVariableReadUse.Valid();
}

void VoicePool::FinishTimelineVariableSourceRead(std::uint32_t renderSlot,
    std::uint32_t renderGeneration) noexcept {
    if (renderSlot < m_slots.size() &&
        m_slots[renderSlot].generation.load(std::memory_order_relaxed) == renderGeneration)
        m_slots[renderSlot].timelineVariableReadUse = {};
}

VariableTimelineReadUseRef VoicePool::TimelineVariableSourceRead(std::uint32_t renderSlot,
    std::uint32_t renderGeneration) const noexcept {
    return HasTimelineVariableSourceRead(renderSlot, renderGeneration)
        ? m_slots[renderSlot].timelineVariableReadUse : VariableTimelineReadUseRef{};
}

bool VoicePool::FinishNativeVariableStartInstallation(VnVoiceHandle handle) noexcept {
    auto* slot = SlotFor(handle);
    if (slot == nullptr) return true;
    const auto state = slot->state.load(std::memory_order_acquire);
    if (state != SlotState::VariableStartWaiting) return state != SlotState::VariableStartClaimed;
    return m_nativeVariableStartAuthority.installed(m_nativeVariableStartAuthority.context,
                                                   slot->nativeVariableStart);
}

bool VoicePool::HasPendingNativeVariableStart(VnVoiceHandle handle) noexcept {
    auto* slot = SlotFor(handle);
    if (slot == nullptr) return false;
    const auto state = slot->state.load(std::memory_order_acquire);
    return state == SlotState::VariableStartClaimed || state == SlotState::VariableStartWaiting;
}

void VoicePool::RejectNativeVariableStart(VnVoiceHandle handle, NotificationQueue& notifications,
    std::uint64_t blockIndex) noexcept {
    if (HasPendingNativeVariableStart(handle))
        ApplyStop(handle, 0, 0, notifications, blockIndex);
}

bool VoicePool::FinishNativeVariableStart(Slot& slot, bool activated) noexcept {
    if (!slot.nativeVariableStart.cut.Valid()) return true;
    const auto identity = slot.nativeVariableStart;
    slot.nativeVariableStart = {};
    if (m_nativeVariableStartAuthority.finish(m_nativeVariableStartAuthority.context, identity, activated))
        return true;
    slot.nativeVariableStart = identity;
    return false;
}

void VoicePool::ActivatePendingNativeVariableStarts(NotificationQueue& notifications,
    std::uint64_t blockIndex) noexcept {
    for (auto& slot : m_slots) {
        if (slot.state.load(std::memory_order_acquire) != SlotState::VariableStartWaiting) continue;
        const auto identity = slot.nativeVariableStart;
        const auto status = m_nativeVariableStartAuthority.ready(m_nativeVariableStartAuthority.context, identity);
        if (status == VariableReadCutStatus::Waiting) continue;
        if (status == VariableReadCutStatus::Applied && FinishNativeVariableStart(slot, true)) {
            slot.state.store(SlotState::Rendering, std::memory_order_release);
            continue;
        }
        ApplyStop(identity.nativeHandle, 0, 0, notifications, blockIndex);
    }
}

bool VoicePool::BindPreparedFirstOutputOrder(VnVoiceHandle handle, VariablePlaybackOrder order) noexcept {
    auto* slot = SlotFor(handle);
    if (slot == nullptr || order.sequence == 0 ||
        slot->state.load(std::memory_order_acquire) != SlotState::Prepared ||
        slot->firstOutputPhase != FirstOutputPhase::Live || m_firstOutputCommandConsumer == nullptr ||
        !PrepareFirstOutputBacking(static_cast<std::uint32_t>(handle) + 1u)) return false;
    slot->firstOutputOrder = order;
    slot->firstOutputPhase = FirstOutputPhase::Prepared;
    return true;
}

bool VoicePool::FirstOutputProtected(const Slot& slot) noexcept {
    return slot.firstOutputPhase != FirstOutputPhase::Live;
}

std::size_t VoicePool::FirstOutputStorageBytes() const noexcept {
    return m_firstOutputEndpoints.RequestedBytes() + m_firstOutputWeights.RequestedBytes() +
        m_pendingTimelineVariableEvents.RequestedBytes() +
        m_slots.size() * (sizeof(VariablePlaybackOrder) + sizeof(FirstOutputPhase) +
            sizeof(VariableTimelineReadUseRef));
}

void VoicePool::ClearFirstOutput(Slot& slot) noexcept {
    slot.timelineVariableReadUse = {};
    slot.firstOutputOrder = {};
    slot.firstOutputOrderFinal = true;
    slot.firstOutputPhase = FirstOutputPhase::Live;
    if (!FirstOutputBackingReady(static_cast<std::uint32_t>(&slot - m_slots.data()) + 1u)) return;
    const auto base = static_cast<std::size_t>(&slot - m_slots.data()) * m_firstOutputStride;
    for (std::uint32_t index = 0; index < m_firstOutputStride; ++index) {
        auto& endpoint = m_firstOutputEndpoints[base + index];
        endpoint.valid = false;
    }
    const auto weightBase = static_cast<std::size_t>(&slot - m_slots.data()) * kSelectionPathNodeCapacity;
    for (std::uint32_t index = 0; index < kSelectionPathNodeCapacity; ++index) {
        auto& endpoint = m_firstOutputWeights[weightBase + index];
        endpoint.valid = false;
        endpoint.weightOwner = {};
        endpoint.weightRemoved = false;
    }
}

std::uint32_t VoicePool::FirstOutputCommandKey(const CommandRecord& command,
    VnVoiceHandle& handle) const noexcept {
    handle = 0;
    std::uint32_t key = UINT32_MAX;
    switch (command.opcode) {
    case kOpcodeSetVoiceVolume:
        if (command.flags != 0 && command.flags != kCommandFlagVariableWeightSegment) break;
        handle = command.payload.setVoiceVolume.handle;
        key = command.flags == kCommandFlagVariableWeightSegment ? 10u
            : command.payload.setVoiceVolume.outputOnly != 0 ? 1u : 0u;
        break;
    case kOpcodeSetVoicePitch:
        handle = command.payload.setVoicePitch.handle;
        key = 2;
        break;
    case kOpcodeSetVoiceFilter:
        handle = command.payload.setVoiceFilter.handle;
        if (command.payload.setVoiceFilter.filterKind < 2) key = 3 + command.payload.setVoiceFilter.filterKind;
        break;
    case kOpcodeSetVoiceLowShelf:
        handle = command.payload.setVoiceShelf.handle;
        if (command.payload.setVoiceShelf.shelfKind < 2) key = 5 + command.payload.setVoiceShelf.shelfKind;
        break;
    case kOpcodeSetVoicePan:
        handle = command.payload.setVoicePan.handle;
        key = 7;
        break;
    case kOpcodeSetVoiceSpatialScalar:
        handle = command.payload.setVoiceSpatialScalar.handle;
        if (command.payload.setVoiceSpatialScalar.kind < 2) key = 8 + command.payload.setVoiceSpatialScalar.kind;
        break;
    case kOpcodeSetVoiceEffectParam: {
        const bool compressor = command.flags == kCommandFlagPreparedCompressor;
        handle = compressor ? command.payload.setCompressorEffectParam.host : command.payload.setVoiceEffectParam.handle;
        const auto effectSlot = compressor ? command.payload.setCompressorEffectParam.slot : command.payload.setVoiceEffectParam.slot;
        const auto type = compressor ? command.payload.setCompressorEffectParam.effectType : command.payload.setVoiceEffectParam.effectType;
        if ((compressor && type != VN_EFFECT_COMPRESSOR) ||
            (!compressor && (command.flags != 0 || type == VN_EFFECT_COMPRESSOR))) break;
        const auto param = compressor ? 0u : command.payload.setVoiceEffectParam.paramIndex;
        if (effectSlot < m_fxSlotsPerVoice && param < kMaxEffectParamCount) {
            const auto physical = type == VN_EFFECT_THREE_BAND_TONE
                ? (param <= kToneParamLowShelfFreqHz ? 0u : param <= kToneParamPeakQ ? 1u : 2u) : param;
            key = kFirstOutputBuiltinEndpoints + effectSlot * kMaxEffectParamCount + physical;
        }
        break;
    }
    default:
        break;
    }
    return key;
}

void VoicePool::SupersedeDeferredFirstOutput(const CommandRecord& command,
    VnVoiceHandle initializingVoice) noexcept {
    VnVoiceHandle handle = 0;
    const auto key = FirstOutputCommandKey(command, handle);
    if (handle == initializingVoice) return;
    const auto* proof = m_commandMetadataProof;
    if (proof != nullptr && proof->Valid() && proof->pool_ == this && proof->begun_ &&
        proof->source_->Valid()) {
        const auto token = proof->source_->Token();
        if (static_cast<std::uint32_t>(handle) == token.renderSlot &&
            static_cast<std::uint32_t>(handle >> 32) == token.renderGeneration) return;
    }
    auto* slot = key == UINT32_MAX ? nullptr : SlotForCommandMetadata(handle);
    if (slot == nullptr || !FirstOutputProtected(*slot)) return;
    const auto base = static_cast<std::size_t>(slot - m_slots.data()) * m_firstOutputStride;
    m_firstOutputEndpoints[base + key].valid = false;
    if (command.opcode == kOpcodeSetVoicePan && command.payload.setVoicePan.hasWidth != 0)
        m_firstOutputEndpoints[base + 8].valid = false;
}

bool VoicePool::DeferVariableCommandUntilFirstOutput(const CommandRecord& command,
    VariablePlaybackOrder order, std::uint32_t targetRevision, VariableNativeEffectIdentity effect,
    VariableToneBandValues toneBand) noexcept {
    VnVoiceHandle handle = 0;
    const auto key = FirstOutputCommandKey(command, handle);
    auto* slot = SlotFor(handle);
    if (slot == nullptr || key == UINT32_MAX || !FirstOutputProtected(*slot)) return false;
    const auto state = slot->state.load(std::memory_order_acquire);
    if (!IsAudioRenderingState(state) && state != SlotState::Claimed &&
        state != SlotState::VariableStartClaimed && state != SlotState::VariableStartWaiting) return false;
    const auto& cut = slot->firstOutputOrder;
    if (slot->firstOutputOrderFinal && order.frame <= cut.frame && (order.sequence < cut.sequence ||
        (order.sequence == cut.sequence && order.ordinal < cut.ordinal))) return true;
    const auto base = static_cast<std::size_t>(slot - m_slots.data()) * m_firstOutputStride;
    auto& pending = m_firstOutputEndpoints[base + key];
    if (pending.valid && (order.frame < pending.order.frame ||
        (order.frame == pending.order.frame && (order.sequence < pending.order.sequence ||
            (order.sequence == pending.order.sequence && order.ordinal < pending.order.ordinal))))) return true;
    pending.valid = true;
    pending.order = order;
    pending.targetRevision = targetRevision;
    pending.nativeEffect = effect;
    pending.toneBand = toneBand;
    pending.command = command;
    if (command.opcode == kOpcodeSetVoicePan && command.payload.setVoicePan.hasWidth != 0) {
        pending.command.payload.setVoicePan.hasWidth = 0;
        CommandRecord width{};
        width.opcode = kOpcodeSetVoiceSpatialScalar;
        width.payload.setVoiceSpatialScalar.handle = handle;
        width.payload.setVoiceSpatialScalar.kind = kSpatialScalarKindWidth;
        width.payload.setVoiceSpatialScalar.value = command.payload.setVoicePan.width;
        (void)DeferVariableCommandUntilFirstOutput(width, order, targetRevision);
    }
    return true;
}

void VoicePool::ApplyDeferredFirstOutput(Slot& slot) noexcept {
    if (slot.firstOutputPhase != FirstOutputPhase::FirstFolded) return;
    slot.firstOutputPhase = FirstOutputPhase::Live;
    const auto base = static_cast<std::size_t>(&slot - m_slots.data()) * m_firstOutputStride;
    for (std::uint32_t index = 0; index < m_firstOutputStride; ++index) {
        auto& endpoint = m_firstOutputEndpoints[base + index];
        if (!endpoint.valid) continue;
        endpoint.valid = false;
        if (endpoint.nativeEffect.generation != 0) {
            const auto effectSlot = endpoint.command.flags == kCommandFlagPreparedCompressor
                ? endpoint.command.payload.setCompressorEffectParam.slot : endpoint.command.payload.setVoiceEffectParam.slot;
            if (effectSlot >= slot.render.fxSlotCount) continue;
            const auto& installed = slot.render.fxSlotEffectRef[effectSlot];
            if (installed.type != endpoint.nativeEffect.type || installed.poolIndex != endpoint.nativeEffect.poolIndex ||
                installed.generation != endpoint.nativeEffect.generation) continue;
        }
        if (endpoint.toneBand.valid) {
            const auto& command = endpoint.command.payload.setVoiceEffectParam;
            if (command.effectType != VN_EFFECT_THREE_BAND_TONE || command.slot >= slot.render.fxSlotCount ||
                slot.render.fxSlotEffectRef[command.slot].type != VN_EFFECT_THREE_BAND_TONE) continue;
            auto* tone = static_cast<ToneEffectState*>(slot.render.fxSlots[command.slot].state);
            if (tone == nullptr) continue;
            const auto& band = endpoint.toneBand;
            // The command installs one prepared section; its entire scalar image must match it.
            if (command.paramIndex <= kToneParamLowShelfFreqHz) {
                tone->lowShelfGainDb = band.gainDb;
                tone->lowShelfFreqHz = band.frequencyHz;
                tone->lowShelfEverActive = tone->lowShelfEverActive || band.gainDb != 0.0f;
            } else if (command.paramIndex <= kToneParamPeakQ) {
                tone->peakGainDb = band.gainDb;
                tone->peakFreqHz = band.frequencyHz;
                tone->peakQ = band.q;
                tone->peakEverActive = tone->peakEverActive || band.gainDb != 0.0f;
            } else {
                tone->highShelfGainDb = band.gainDb;
                tone->highShelfFreqHz = band.frequencyHz;
                tone->highShelfEverActive = tone->highShelfEverActive || band.gainDb != 0.0f;
            }
        }
        m_firstOutputCommandConsumer(m_firstOutputCommandContext, endpoint.command);
    }
    const auto weightBase = static_cast<std::size_t>(&slot - m_slots.data()) * kSelectionPathNodeCapacity;
    for (std::uint32_t index = 0; index < kSelectionPathNodeCapacity; ++index) {
        auto& endpoint = m_firstOutputWeights[weightBase + index];
        if (!endpoint.valid) continue;
        endpoint.valid = false;
        if (endpoint.weightOwner.Valid()) {
            (void)slot.render.ApplyVariableWeightControl(endpoint.weightOwner, endpoint.weightSegment);
            if (endpoint.weightRemoved) {
                for (std::uint32_t weight = 0; weight < slot.render.variableWeightControlCount; ++weight) {
                    auto& binding = slot.render.variableWeightControls[weight];
                    if (binding.owner == endpoint.weightOwner &&
                        binding.segment.segmentGeneration == endpoint.weightSegment.segmentGeneration) {
                        binding.active = false;
                        break;
                    }
                }
            }
        }
    }
}

VoicePool::DeferredFirstOutputWeight* VoicePool::FindFirstOutputWeightEndpoint(
    Slot& slot, VariableWeightControlIdentity owner, bool create) noexcept {
    const auto base = static_cast<std::size_t>(&slot - m_slots.data()) * kSelectionPathNodeCapacity;
    const auto count = slot.render.variableWeightControlCount;
    for (std::uint32_t index = 0; index < count; ++index) {
        if (slot.render.variableWeightControls[index].owner == owner)
            return &m_firstOutputWeights[base + index];
    }
    DeferredFirstOutputWeight* empty = nullptr;
    for (std::uint32_t index = count; index < kSelectionPathNodeCapacity; ++index) {
        auto& endpoint = m_firstOutputWeights[base + index];
        if (endpoint.weightOwner == owner) return &endpoint;
        if (!endpoint.weightOwner.Valid() && empty == nullptr) empty = &endpoint;
    }
    return create ? empty : nullptr;
}

void VoicePool::PrepareFirstOutputTimelineWeights(Slot& slot) noexcept {
    if (!FirstOutputProtected(slot)) return;
    auto& span = slot.render.timelineEvents;
    const auto cutoff = SelectAuthoredRootStop(span, m_blockFrames, slot.render.authoredRootStopFrame,
        slot.render.authoredRootStopControl.Stop().authoredStopOperation);
    const auto handle = MakeHandle(static_cast<std::uint32_t>(&slot - m_slots.data()),
        slot.generation.load(std::memory_order_relaxed));
    for (std::uint32_t index = 0; index < span.eventCount; ++index) {
        auto& event = span.events[index];
        if (event.Kind() != TimelineEventKind::VariableWeight || !event.Weight().variableWeightOwner.Valid() ||
            event.offset >= m_blockFrames || !TimelineEventPrecedesRootStop(span, index,
                m_blockFrames, cutoff, slot.render.authoredRootStopApplied)) continue;
        const auto& cut = slot.firstOutputOrder;
        const auto& order = event.order;
        const auto frame = event.frontierProof.state == FrontierEventState::Legacy
            ? span.quantumIndex * m_blockFrames + event.offset : order.executeFrame;
        const bool included = frame <= cut.frame && (order.logicCommitSequence < cut.sequence ||
            (order.logicCommitSequence == cut.sequence && order.executionOrdinal < cut.ordinal));
        if (!included) (void)ApplyOwnedVariableWeightSegment(handle, event.Weight().variableWeightOwner,
            event.Weight().variableWeight);
        event.firstOutputDeferred = true;
    }
}

VoicePool::PreparedCommitProof::PreparedCommitProof(PreparedCommitProof&& other) noexcept
    : pool_(std::exchange(other.pool_, nullptr)), slot_(std::exchange(other.slot_, nullptr)) {}

VoicePool::PreparedCommitProof& VoicePool::PreparedCommitProof::operator=(
    PreparedCommitProof&& other) noexcept {
    if (this != &other) {
        Release();
        pool_ = std::exchange(other.pool_, nullptr);
        slot_ = std::exchange(other.slot_, nullptr);
    }
    return *this;
}

VoicePool::PreparedCommitProof::~PreparedCommitProof() { Release(); }

void VoicePool::PreparedCommitProof::Release() noexcept {
    if (slot_ == nullptr) return;
    assert(pool_ != nullptr &&
        slot_->state.load(std::memory_order_acquire) == SlotState::PreparedCommitHeld);
    slot_->state.store(SlotState::Prepared, std::memory_order_release);
    slot_ = nullptr;
    pool_ = nullptr;
}

bool VoicePool::ProvePreparedCommit(const PreparedCommitInput& expected,
                                   PreparedCommitProof* out) noexcept {
    const auto index = static_cast<std::uint32_t>(expected.renderHandle);
    const auto generation = static_cast<std::uint32_t>(expected.renderHandle >> 32);
    if (out == nullptr || out->Valid() || expected.renderHandle == 0 || generation == 0 ||
        index >= m_slots.size() || !expected.originalRelease.Valid() ||
        expected.originalRelease.slot != index ||
        (expected.sourceTerminal == nullptr
            ? expected.sourceTerminalToken.slot != UINT32_MAX || expected.sourceTerminalToken.generation != 0
            : !expected.sourceTerminalToken.Valid()) ||
        (expected.completionProgress == nullptr
            ? expected.completionOccurrence.slot != 0 || expected.completionOccurrence.generation != 0 ||
              expected.completionRenderGeneration != 0 || expected.playbackFactsRevision != 0
            : expected.completionOccurrence.generation == 0 ||
              expected.completionRenderGeneration != generation || expected.playbackFactsRevision == 0) ||
        (expected.streamRing == nullptr
            ? expected.streamRingToken != 0 || expected.streamGeneration != 0
            : expected.streamRingToken == 0 || expected.streamGeneration == 0)) return false;
    auto& slot = m_slots[index];
    const auto release = slot.releaseSequence.load(std::memory_order_acquire);
    if (release == UINT64_MAX || release + 1u != expected.originalRelease.targetSequence ||
        slot.generation.load(std::memory_order_acquire) != generation) return false;
    SlotState prepared = SlotState::Prepared;
    if (!slot.state.compare_exchange_strong(prepared, SlotState::PreparedCommitHeld,
            std::memory_order_acq_rel, std::memory_order_acquire)) return false;
    const auto& render = slot.render;
    PreparedRoutingV1 routing{};
    const bool valid = slot.generation.load(std::memory_order_acquire) == generation &&
        slot.releaseSequence.load(std::memory_order_acquire) == release &&
        render.sourceTerminal == expected.sourceTerminal &&
        render.sourceTerminalToken.slot == expected.sourceTerminalToken.slot &&
        render.sourceTerminalToken.generation == expected.sourceTerminalToken.generation &&
        render.completionProgress == expected.completionProgress &&
        render.completionOccurrence.slot == expected.completionOccurrence.slot &&
        render.completionOccurrence.generation == expected.completionOccurrence.generation &&
        render.completionRenderGeneration == expected.completionRenderGeneration &&
        render.playbackFactsRevision == expected.playbackFactsRevision &&
        render.streamRing == expected.streamRing &&
        slot.streamRingToken.load(std::memory_order_acquire) == expected.streamRingToken &&
        (expected.streamRing == nullptr ||
            expected.streamRing->generation.load(std::memory_order_acquire) == expected.streamGeneration) &&
        CapturePreparedRoutingV1(expected.renderHandle, &routing) && routing == expected.routing;
    if (!valid) {
        slot.state.store(SlotState::Prepared, std::memory_order_release);
        return false;
    }
    out->pool_ = this;
    out->slot_ = &slot;
    return true;
}

void VoicePool::ConsumePreparedCommit(PreparedCommitProof&& proof) noexcept {
    assert(proof.Valid() && proof.pool_ != nullptr &&
        proof.slot_->state.load(std::memory_order_acquire) == SlotState::PreparedCommitHeld);
    proof.slot_->state.store(SlotState::Claimed, std::memory_order_release);
    const auto active = proof.pool_->ActiveCount();
    if (active > proof.pool_->m_highWater) proof.pool_->m_highWater = active;
    proof.slot_ = nullptr;
    proof.pool_ = nullptr;
}

VnVoiceHandle VoicePool::PreparedCommitProof::OriginalPreparedHandle() const noexcept {
    if (pool_ == nullptr || slot_ == nullptr) return 0;
    return MakeHandle(static_cast<std::uint32_t>(slot_ - pool_->m_slots.data()),
        slot_->generation.load(std::memory_order_acquire));
}

VoicePool::OriginalVoiceOwner::OriginalVoiceOwner(OriginalVoiceOwner&& other) noexcept {
    *this = std::move(other);
}

VoicePool::OriginalVoiceOwner& VoicePool::OriginalVoiceOwner::operator=(
    OriginalVoiceOwner&& other) noexcept {
    if (this != &other) {
        Release();
        pool_ = std::exchange(other.pool_, nullptr);
        handle_ = std::exchange(other.handle_, 0);
        releaseTarget_ = std::exchange(other.releaseTarget_, 0);
    }
    return *this;
}

void VoicePool::OriginalVoiceOwner::Release() noexcept {
    if (pool_ == nullptr) return;
    const auto previous = pool_->m_liveOriginalVoiceOwners.fetch_sub(1,
        std::memory_order_relaxed);
    assert(previous != 0);
    (void)previous;
    pool_ = nullptr;
    handle_ = 0;
    releaseTarget_ = 0;
}

bool VoicePool::IssueOriginalVoiceOwner(const PreparedCommitProof& preparation,
    OriginalVoiceOwner* out) noexcept {
    if (out == nullptr || out->Valid() || preparation.pool_ != this ||
        !preparation.Valid()) return false;
    auto& slot = *preparation.slot_;
    if (slot.state.load(std::memory_order_acquire) != SlotState::PreparedCommitHeld)
        return false;
    const auto released = slot.releaseSequence.load(std::memory_order_acquire);
    if (released == UINT64_MAX || slot.issuedOriginalReleaseTarget == released + 1u ||
        m_liveOriginalVoiceOwners.load(std::memory_order_relaxed) == UINT64_MAX)
        return false;
    const auto index = static_cast<std::uint32_t>(&slot - m_slots.data());
    const auto generation = slot.generation.load(std::memory_order_acquire);
    assert(index < m_slots.size() && generation != 0);
    slot.issuedOriginalReleaseTarget = released + 1u;
    out->handle_ = (static_cast<std::uint64_t>(generation) << 32) | index;
    out->releaseTarget_ = released + 1u;
    m_liveOriginalVoiceOwners.fetch_add(1, std::memory_order_relaxed);
    out->pool_ = this;
    return true;
}

VoicePool::OriginalVoiceDisposal VoicePool::TryDisposeOriginalTimelineVoice(
    const OriginalVoiceOwner& owner) noexcept {
    return TryDisposeOriginalTimelineVoiceImpl(owner, ReleaseThread::Audio);
}

VoicePool::OriginalVoiceDisposal VoicePool::TryDisposeOriginalTimelineVoiceFromLogic(
    const OriginalVoiceOwner& owner) noexcept {
    return TryDisposeOriginalTimelineVoiceImpl(owner, ReleaseThread::Logic);
}

VoicePool::OriginalVoiceDisposal VoicePool::TryDisposeOriginalTimelineVoiceImpl(
    const OriginalVoiceOwner& owner, ReleaseThread thread) noexcept {
    using Result = OriginalVoiceDisposal;
    const auto index = static_cast<std::uint32_t>(owner.handle_);
    if (owner.pool_ != this || !owner.Valid() || owner.releaseTarget_ == 0 ||
        index >= m_slots.size()) return Result::Refused;
    auto& slot = m_slots[index];
    if (slot.releaseSequence.load(std::memory_order_acquire) >= owner.releaseTarget_)
        return Result::AlreadyReleased;
    const auto state = slot.state.load(std::memory_order_acquire);
    if (state == SlotState::PreparedCommitHeld || state == SlotState::CommandInstallHeld ||
        state == SlotState::ReleaseHeld || state == SlotState::OriginalCallbackHeld ||
        state == SlotState::TimelineCallbackHeld) return Result::Deferred;
    if (state != SlotState::Claimed && state != SlotState::Rendering) return Result::Refused;
    auto expected = state;
    if (!slot.state.compare_exchange_strong(expected, SlotState::ReleaseHeld,
        std::memory_order_acq_rel, std::memory_order_acquire)) return Result::Deferred;
    const auto released = slot.releaseSequence.load(std::memory_order_acquire);
    if (slot.generation.load(std::memory_order_acquire) !=
            static_cast<std::uint32_t>(owner.handle_ >> 32) ||
        released != owner.releaseTarget_ - 1u || slot.timelineProvenMask != 0 ||
        (state == SlotState::Rendering && !slot.render.timelineGated)) {
        slot.state.store(state, std::memory_order_release);
        return released >= owner.releaseTarget_ ? Result::AlreadyReleased : Result::Refused;
    }
    ReleaseSlot(slot, thread);
    return Result::Released;
}

bool VoicePool::OriginalVoiceReleased(const OriginalVoiceOwner& owner) const noexcept {
    const auto index = static_cast<std::uint32_t>(owner.handle_);
    return owner.pool_ == this && owner.Valid() && owner.releaseTarget_ != 0 &&
        index < m_slots.size() &&
        m_slots[index].releaseSequence.load(std::memory_order_acquire) >= owner.releaseTarget_;
}

bool VoicePool::MatchesOriginalPreparedVoice(const OriginalVoiceOwner& original,
    const PreparedCommitProof& proof) const noexcept {
    if (original.pool_ != this || proof.pool_ != this || !original.Valid() ||
        !proof.Valid() || original.releaseTarget_ == 0) return false;
    const auto index = static_cast<std::uint32_t>(original.handle_);
    return index < m_slots.size() && proof.slot_ == &m_slots[index] &&
        proof.slot_->state.load(std::memory_order_acquire) == SlotState::PreparedCommitHeld &&
        proof.slot_->generation.load(std::memory_order_acquire) ==
            static_cast<std::uint32_t>(original.handle_ >> 32) &&
        proof.slot_->releaseSequence.load(std::memory_order_acquire) == original.releaseTarget_ - 1u;
}

bool VoicePool::CancelPreparedVoice(VnVoiceHandle handle) noexcept {
    Slot* slot = SlotFor(handle);
    if (slot == nullptr ||
        slot->state.load(std::memory_order_acquire) != SlotState::Prepared) {
        return false;
    }
    if (slot->render.pcm.useCount != nullptr) {
        slot->render.pcm.useCount->fetch_sub(1, std::memory_order_release);
    }
    if (slot->render.streamRing != nullptr) {
        slot->render.streamRing->state.store(StreamSlotState::Retired,
                                             std::memory_order_release);
        slot->render.streamRing = nullptr;
    }
    slot->streamRingToken.store(0, std::memory_order_release);
    slot->stagedCompletionProgress = nullptr;
    slot->stagedCompletionUpdate = {};
    slot->completionProgressStaged = false;
    slot->completionProgressTerminal = false;
    if (slot->render.sourceTerminal != nullptr)
        slot->render.sourceTerminal->bypassResult.store(
            SourceTerminalKind::Cancelled, std::memory_order_release);
    slot->render = VoiceRenderContext{};
    slot->nativeVariableStart = {};
    ClearFirstOutput(*slot);
    PublishRelease(*slot);
    CompleteOriginalSourceBodyDisposition(*slot);
    slot->state.store(SlotState::Free, std::memory_order_release);
    return true;
}

bool VoicePool::CancelOriginalPreparedVoice(const OriginalVoiceOwner& owner) noexcept {
    if (owner.pool_ != this || !owner.Valid() || owner.releaseTarget_ == 0) return false;
    if (OriginalVoiceReleased(owner)) return true;
    const auto index = static_cast<std::uint32_t>(owner.handle_);
    if (index >= m_slots.size()) return false;
    auto* slot = &m_slots[index];
    SlotState expected = SlotState::Prepared;
    if (!slot->state.compare_exchange_strong(expected, SlotState::ReleaseHeld,
            std::memory_order_acq_rel, std::memory_order_acquire)) return false;
    if (slot->generation.load(std::memory_order_acquire) !=
            static_cast<std::uint32_t>(owner.handle_ >> 32) ||
        slot->releaseSequence.load(std::memory_order_acquire) != owner.releaseTarget_ - 1u) {
        slot->state.store(SlotState::Prepared, std::memory_order_release);
        return OriginalVoiceReleased(owner);
    }
    if (slot->render.pcm.useCount != nullptr) {
        slot->render.pcm.useCount->fetch_sub(1, std::memory_order_release);
    }
    if (slot->render.streamRing != nullptr) {
        slot->render.streamRing->state.store(StreamSlotState::Retired,
                                             std::memory_order_release);
        slot->render.streamRing = nullptr;
    }
    slot->streamRingToken.store(0, std::memory_order_release);
    slot->stagedCompletionProgress = nullptr;
    slot->stagedCompletionUpdate = {};
    slot->completionProgressStaged = false;
    slot->completionProgressTerminal = false;
    if (slot->render.sourceTerminal != nullptr)
        slot->render.sourceTerminal->bypassResult.store(
            SourceTerminalKind::Cancelled, std::memory_order_release);
    slot->render = VoiceRenderContext{};
    slot->nativeVariableStart = {};
    ClearFirstOutput(*slot);
    PublishRelease(*slot);
    CompleteOriginalSourceBodyDisposition(*slot);
    slot->state.store(SlotState::Free, std::memory_order_release);
    return true;
}

bool VoicePool::BindMonitorControl(VnVoiceHandle handle,
    const MonitorVoiceControlBinding& binding) noexcept {
    Slot* slot = SlotFor(handle);
    if (slot == nullptr || !binding.Valid() || binding.render != handle) return false;
    const auto state = slot->state.load(std::memory_order_acquire);
    if (state != SlotState::Claimed && state != SlotState::Prepared) return false;
    slot->monitorControl = binding;
    return true;
}

bool VoicePool::BindSourceTerminal(VnVoiceHandle handle,
    SourceTerminalRecord* record, SourceTerminalToken token) noexcept {
    Slot* slot = SlotFor(handle);
    if (slot == nullptr || record == nullptr || !token.Valid() ||
        !record->occupied || record->generation != token.generation ||
        record->render != handle || record->leaf == 0 || record->acceptedEpoch == 0 ||
        slot->render.sourceTerminal != nullptr) return false;
    const auto state = slot->state.load(std::memory_order_acquire);
    if (state != SlotState::Claimed && state != SlotState::Prepared) return false;
    slot->render.sourceTerminal = record;
    slot->render.sourceTerminalToken = token;
    slot->render.sourceTerminalStreamGeneration = slot->render.streamRing != nullptr
        ? slot->render.streamRing->generation.load(std::memory_order_acquire) : 0;
    return true;
}

void VoicePool::PublishSourceTerminal(Slot& slot, std::uint64_t blockIndex) noexcept {
    auto& render = slot.render;
    const auto observedKind = render.monitorStoppedBeforeCascade
        ? SourceTerminalKind::Stop : render.stagedSourceTerminal;
    const auto observedOffset = render.monitorStoppedBeforeCascade ? 0u : render.stagedSourceTerminalOffset;
    if (observedKind == SourceTerminalKind::None || blockIndex == UINT64_MAX ||
        blockIndex > (UINT64_MAX - m_blockFrames) / m_blockFrames) return;
    if (!slot.monitorSourceTerminalPublished) {
        slot.monitorSourceTerminalPublished = true;
        if (slot.monitorControl.Valid()) {
            const bool timelineControl = observedKind == SourceTerminalKind::Stop &&
                render.monitorTimelineControlAdopted;
            (void)slot.monitorControl.receipt->TryPublish(slot.monitorControl,
                observedKind, blockIndex * m_blockFrames + observedOffset,
                timelineControl ? render.monitorAdoptedControlCause : MonitorControlCause{}, timelineControl);
        }
    }
    auto* record = render.sourceTerminal;
    if (record == nullptr || render.stagedSourceTerminal == SourceTerminalKind::None ||
        blockIndex == UINT64_MAX || blockIndex > (UINT64_MAX - m_blockFrames) / m_blockFrames ||
        !record->occupied || record->generation != render.sourceTerminalToken.generation ||
        record->render != MakeHandle(static_cast<std::uint32_t>(&slot - m_slots.data()),
            slot.generation.load(std::memory_order_relaxed)) ||
        record->completedSequence.load(std::memory_order_relaxed) != 0 ||
        record->bypassResult.load(std::memory_order_relaxed) != SourceTerminalKind::None)
        return;
    record->payload = {render.stagedSourceTerminal,
        blockIndex * m_blockFrames + render.stagedSourceTerminalOffset};
    record->completedSequence.store(blockIndex + 1u, std::memory_order_release);
}

void VoicePool::PublishPlaybackEnd(Slot& slot, std::uint64_t blockIndex) noexcept {
    const auto& render = slot.render;
    if (!slot.monitorControl.Valid() || render.streamParkActive ||
        render.stagedPlaybackEnd == SourceTerminalKind::None ||
        blockIndex == UINT64_MAX || blockIndex > (UINT64_MAX - m_blockFrames) / m_blockFrames) return;
    const bool timelineControl = render.stagedPlaybackEnd == SourceTerminalKind::Stop &&
        render.monitorTimelineControlAdopted;
    (void)slot.monitorControl.receipt->TryPublishEnd(slot.monitorControl, blockIndex + 1u,
        blockIndex * m_blockFrames + render.stagedPlaybackEndOffset, render.stagedPlaybackEnd,
        timelineControl ? render.monitorAdoptedControlCause : MonitorControlCause{}, timelineControl);
}

bool VoicePool::CapturePreparedCompletionProgress(const PreparedCommitProof& proof,
    const CompletionProgressRow** progress, OccurrenceToken* occurrence) const noexcept {
    if (!proof.Valid() || proof.pool_ != this || progress == nullptr || occurrence == nullptr ||
        proof.slot_->state.load(std::memory_order_acquire) != SlotState::PreparedCommitHeld) return false;
    *progress = proof.slot_->render.completionProgress;
    *occurrence = proof.slot_->render.completionOccurrence;
    return true;
}

bool VoicePool::BindCompletionProgress(
    VnVoiceHandle handle, CompletionProgressRow* row,
    OccurrenceToken occurrence,
    std::uint64_t playbackFactsRevision) noexcept {
    Slot* slot = SlotFor(handle);
    if (slot == nullptr || row == nullptr || occurrence.generation == 0 ||
        playbackFactsRevision == 0 || m_workers.WorkerCount() != 0 ||
        (slot->state.load(std::memory_order_acquire) != SlotState::Claimed &&
         slot->state.load(std::memory_order_acquire) != SlotState::Prepared)) {
        return false;
    }
    const std::uint32_t renderGeneration =
        static_cast<std::uint32_t>(handle >> 32);
    CompletionProgressSnapshot snapshot{};
    if (!CaptureCompletionProgressExact(*row, occurrence, renderGeneration,
                                        &snapshot) ||
        snapshot.playbackFactsRevision != playbackFactsRevision) {
        return false;
    }
    slot->render.completionProgress = row;
    slot->render.completionOccurrence = occurrence;
    slot->render.completionRenderGeneration = renderGeneration;
    slot->render.playbackFactsRevision = playbackFactsRevision;
    return true;
}

void VoicePool::InitializePlay(Slot& slot, VnVoiceHandle sourceIdentity,
    std::uint64_t sourceStartFrame, std::uint64_t blockIndex) {
    if (slot.firstOutputPhase == FirstOutputPhase::Prepared)
        slot.firstOutputPhase = FirstOutputPhase::AwaitingFirstFold;
    VoiceRenderContext& render = slot.render;
    if (render.completionProgress != nullptr &&
        CatchUpCompletionProgressFirstRender(
            *render.completionProgress,
            render.completionOccurrence,
            render.completionRenderGeneration,
            render.playbackFactsRevision,
            blockIndex) != CompletionProgressPublishStatus::Ok) {
        slot.completionProgressTerminal = true;
    }
    render.dynamicSourceIdentity = sourceIdentity;
    slot.monitorPublicHandle.store(sourceIdentity, std::memory_order_release);
    // Seed the PCM read cursor before rendering begins. 0 is the original
    // "start at the file" behavior; a nonzero value (a parked voice's
    // virtual offset) starts the read there — a start frame inside the loop
    // region then wraps naturally. Stream voices decode from their ring
    // (their seek happened at activation), so this only touches PCM.
    if (render.streamRing == nullptr) {
        render.srcCursor = sourceStartFrame;
        render.sourceActivationStartFrame = sourceStartFrame;
        render.sourceActivationLoopsRemaining =
            render.loopPassesRemaining;
    }
}

void VoicePool::ApplyPlay(VnVoiceHandle handle, VnVoiceHandle sourceIdentity,
    std::uint64_t sourceStartFrame, std::uint64_t blockIndex) {
    Slot* slot = SlotFor(handle);
    const auto state = slot == nullptr ? SlotState::Free : slot->state.load(std::memory_order_acquire);
    if (state == SlotState::Claimed || state == SlotState::VariableStartClaimed) {
        InitializePlay(*slot, sourceIdentity, sourceStartFrame, blockIndex);
        slot->state.store(state == SlotState::VariableStartClaimed
            ? SlotState::VariableStartWaiting : m_originalSourceCallbackOpen && slot->render.timelineGated
                ? SlotState::TimelineCallbackHeld : SlotState::Rendering, std::memory_order_release);
    }
}

void VoicePool::FillSourceCursor(const Slot& slot, SlotState state,
    SourceCursorMirror::Row& row, std::uint64_t serial, bool captureParking) noexcept {
    if (!IsAudioRenderingState(state)) return;
    const auto& render = slot.render;
    const bool timelineSource = render.timelineArmSlot != UINT32_MAX &&
        render.timelineArmGeneration != 0;
    if (!captureParking && !timelineSource) return;
    row.sourceFrame = render.srcCursor;
    row.renderGeneration = slot.generation.load(std::memory_order_relaxed);
    row.loopPassesRemaining = render.loopPassesRemaining;
    row.active = true;
    row.exactTimingValid = timelineSource && render.streamRing == nullptr &&
        render.pcm.Valid() &&
        render.resampler.Profile() == ResampleProfile::LegacyCompatibility;
    row.awaitingStart = render.timelineGated;
    row.loopInfinite = render.loopInfinite;
    row.hasExitMarker = render.hasExitMarker;
    if (row.exactTimingValid) {
        auto& progress = row.progress;
        progress.sourceReadFrame = render.resampler.SourceFrame();
        progress.sourceStartFrame = render.sourceActivationStartFrame;
        progress.sourcePhase = {render.resampler.phaseHigh, render.resampler.phaseLow};
        progress.currentRatio = render.resampler.CurrentRatio();
        progress.targetRatio = render.resampler.TargetRatio();
        progress.rampStep = render.resampler.RampStep();
        progress.rampRemaining = render.resampler.RampFramesRemaining();
        progress.activationLoopsRemaining = render.sourceActivationLoopsRemaining;
    }
    row.publicationSerial = serial;
}

void VoicePool::FillSourceCursors(SourceCursorMirror::WriteView cursors,
                                bool captureParking) const {
    const auto count = std::min(cursors.count, m_slots.ConstructedCount());
    for (std::uint32_t i = 0; i < count; ++i) {
        const Slot& slot = m_slots[i];
        FillSourceCursor(slot, slot.state.load(std::memory_order_acquire),
            cursors.rows[i], cursors.serial, captureParking);
    }
}

VnResult VoicePool::CanApplySeek(
    VnVoiceHandle handle, std::uint32_t mode, std::int64_t frameValue,
    std::uint64_t* outSourceFrame,
    const StreamSeekHandoffSnapshot* stream) noexcept {
    if (outSourceFrame == nullptr) return VN_INVALID_ARG;
    Slot* slot = SlotFor(handle);
    if (slot == nullptr ||
        !IsAudioRenderingState(slot->state.load(std::memory_order_acquire))) {
        return VN_STALE_HANDLE;
    }
    const VoiceRenderContext& render = slot->render;
    if (render.phase != SourcePhase::Active) {
        return VN_STALE_HANDLE;
    }
    if (render.completionProgress != nullptr) {
        return VN_CAPABILITY_UNAVAILABLE;
    }
    if (render.streamRing != nullptr) {
        if (stream == nullptr || mode != VN_ACTION_SEEK_ABSOLUTE ||
            frameValue < 0 ||
            static_cast<std::uint64_t>(frameValue) != stream->targetFrame ||
            stream->targetFrame > stream->frameCount ||
            render.streamRingToken != stream->expectedOldRingToken ||
            slot->streamRingToken.load(std::memory_order_acquire) !=
                stream->expectedOldRingToken ||
            render.streamRing->generation.load(std::memory_order_acquire) !=
                static_cast<std::uint32_t>(
                    stream->expectedOldRingToken >> 32u) ||
            stream->replacementRing == nullptr ||
            stream->replacementRing == render.streamRing ||
            stream->replacementRing->generation.load(
                std::memory_order_acquire) !=
                static_cast<std::uint32_t>(
                    stream->replacementRingToken >> 32u) ||
            stream->replacementRing->state.load(std::memory_order_acquire) !=
                StreamSlotState::Active ||
            render.pcm.sampleRate != stream->sourceSampleRate ||
            render.streamFrameCount != stream->frameCount) {
            return VN_CAPABILITY_UNAVAILABLE;
        }
        const StreamTerminalReason terminal =
            stream->replacementRing->TerminalReason();
        if ((terminal == StreamTerminalReason::IntegrityAbort ||
             terminal == StreamTerminalReason::ProducerFailure) ||
            (stream->replacementRing->ReadAvailable() <
                 stream->prebufferTargetFrames &&
             terminal == StreamTerminalReason::None)) {
            return VN_DEVICE_ERROR;
        }
        *outSourceFrame = stream->targetFrame;
        return VN_OK;
    }
    if (stream != nullptr) return VN_INVALID_ARG;

    std::uint64_t target = 0;
    if (mode == VN_ACTION_SEEK_ABSOLUTE) {
        if (frameValue < 0) return VN_INVALID_ARG;
        target = static_cast<std::uint64_t>(frameValue);
        if (target > render.pcm.frames) return VN_INVALID_ARG;
    } else if (mode == VN_ACTION_SEEK_RELATIVE) {
        const std::uint64_t localSourceFrame =
            render.resampler.SourceFrame();
        const std::uint64_t audibleAdvance =
            localSourceFrame > kResampleHistoryPad
                ? localSourceFrame - kResampleHistoryPad
                : 0;
        std::uint64_t current = 0;
        std::uint32_t audibleLoopsRemaining = 0;
        MapLoopedSourceFrame(
            render.sourceActivationStartFrame,
            render.sourceActivationLoopsRemaining, audibleAdvance,
            render.pcm.frames, render.loopStartFrame, render.loopEndFrame,
            &current, &audibleLoopsRemaining);
        if (frameValue < 0) {
            const std::uint64_t magnitude =
                static_cast<std::uint64_t>(-(frameValue + 1)) + 1u;
            target = magnitude >= current ? 0 : current - magnitude;
        } else {
            const std::uint64_t delta =
                static_cast<std::uint64_t>(frameValue);
            target = delta >= render.pcm.frames -
                                  std::min(current, render.pcm.frames)
                         ? render.pcm.frames
                         : current + delta;
        }
    } else {
        return VN_INVALID_ARG;
    }

    *outSourceFrame = target;
    return VN_OK;
}

void VoicePool::CommitSeek(
    VnVoiceHandle handle, std::uint64_t sourceFrame,
    const StreamSeekHandoffSnapshot* stream) noexcept {
    Slot* slot = SlotFor(handle);
    if (slot == nullptr || !IsAudioRenderingState(slot->state.load(std::memory_order_acquire))) return;
    assert(slot != nullptr);
    assert(IsAudioRenderingState(slot->state.load(std::memory_order_relaxed)));
    VoiceRenderContext& render = slot->render;
    const ResampleQuality quality = render.resampler.quality;
    if (stream != nullptr) {
        assert(render.streamRing != nullptr);
        StreamRing* oldRing = render.streamRing;
        const double currentRatio = render.resampler.CurrentRatio();
        const double targetRatio = render.resampler.TargetRatio();
        const std::uint32_t rampRemaining =
            render.resampler.RampFramesRemaining();
        const std::uint32_t slotIndex = static_cast<std::uint32_t>(handle);
        float* rings[kMaxChannels] = {};
        for (std::uint32_t ch = 0; ch < render.pcm.channelCount; ++ch) {
            rings[ch] = m_ringStorage[ch][slotIndex].data();
        }
        render.resampler.Reset(rings, render.pcm.channelCount,
                               quality, currentRatio, m_tables,
                               render.resampler.Profile(), m_multirateTables,
                               render.resampler.professional);
        if (rampRemaining != 0 && targetRatio != currentRatio) {
            render.resampler.UpdateRatio(targetRatio, rampRemaining);
        }
        render.streamRing = stream->replacementRing;
        render.streamRingToken = stream->replacementRingToken;
        render.streamActivationStartFrame = sourceFrame;
        render.streamFeedActivationStartFrame = sourceFrame;
        render.streamFrameCount = stream->frameCount;
        render.streamLoopStartFrame = 0;
        render.streamLoopEndFrame = stream->frameCount;
        render.streamActivationLoopsRemaining = 1;
        render.streamFeedActivationLoopsRemaining = 1;
        render.streamGateOpen = true;
        render.starvedThisBlock = false;
        render.starveCooldownRemaining = 0;
        render.sourceTerminalStreamGeneration =
            static_cast<std::uint32_t>(stream->replacementRingToken >> 32u);
        slot->streamRingToken.store(stream->replacementRingToken,
                                    std::memory_order_release);
        oldRing->state.store(StreamSlotState::Retired,
                             std::memory_order_release);
        return;
    }
    assert(render.streamRing == nullptr);

    // Reset discards prefetched wraps. Preserve the passes still owed at the
    // consumed position, using the same source-time mapping as relative Seek.
    const std::uint64_t localSourceFrame = render.resampler.SourceFrame();
    const std::uint64_t audibleAdvance = localSourceFrame > kResampleHistoryPad
        ? localSourceFrame - kResampleHistoryPad : 0;
    std::uint64_t audibleSourceFrame = 0;
    std::uint32_t audibleLoopsRemaining = 0;
    MapLoopedSourceFrame(render.sourceActivationStartFrame,
        render.sourceActivationLoopsRemaining, audibleAdvance,
        render.pcm.frames, render.loopStartFrame, render.loopEndFrame,
        &audibleSourceFrame, &audibleLoopsRemaining);
    if (audibleLoopsRemaining > render.loopPassesRemaining) {
        const auto discardedPasses = audibleLoopsRemaining - render.loopPassesRemaining;
        assert(render.completedLoopPasses >= discardedPasses);
        render.completedLoopPasses -= discardedPasses;
    }
    render.loopPassesRemaining = audibleLoopsRemaining;

    const double currentRatio = render.resampler.CurrentRatio();
    const double targetRatio = render.resampler.TargetRatio();
    const std::uint32_t rampRemaining =
        render.resampler.RampFramesRemaining();
    const std::uint32_t slotIndex = static_cast<std::uint32_t>(handle);
    float* rings[kMaxChannels] = {};
    for (std::uint32_t ch = 0; ch < render.pcm.channelCount; ++ch) {
        rings[ch] = m_ringStorage[ch][slotIndex].data();
    }
    render.resampler.Reset(rings, render.pcm.channelCount,
                           quality, currentRatio, m_tables,
                           render.resampler.Profile(), m_multirateTables,
                           render.resampler.professional);
    if (rampRemaining != 0 && targetRatio != currentRatio) {
        render.resampler.UpdateRatio(targetRatio, rampRemaining);
    }
    if (sourceFrame >= render.loopEndFrame &&
        (render.loopInfinite || render.loopPassesRemaining > 1)) {
        render.loopInfinite = false;
        render.loopPassesRemaining = 1;
    }
    render.srcCursor = sourceFrame;
    render.sourceActivationStartFrame = sourceFrame;
    render.sourceActivationLoopsRemaining = render.loopPassesRemaining;
}

void VoicePool::ApplyStop(VnVoiceHandle handle, std::uint32_t fadeMs,
                          std::uint32_t fadeFrames,
                          NotificationQueue& notifications,
                          std::uint64_t blockIndex, std::uint32_t mode,
                          VnVoiceHandle publicHandle,
                          std::uint64_t parkRequestSequence) {
    Slot* slot = SlotFor(handle);
    if (mode == kStopModeStreamPark) {
        const std::uint32_t slotIndex =
            static_cast<std::uint32_t>(handle);
        if (slotIndex >= m_streamParkMailboxes.size()) {
            return;
        }
        const StreamParkMailbox& mailbox =
            m_streamParkMailboxes[slotIndex];
        const StreamParkMailboxState mailboxState =
            mailbox.state.load(std::memory_order_acquire);
        if ((mailboxState != StreamParkMailboxState::Reserved &&
             mailboxState != StreamParkMailboxState::Cancelled) ||
            mailbox.outcome.renderHandle != handle ||
            mailbox.outcome.publicHandle != publicHandle ||
            mailbox.outcome.requestSequence != parkRequestSequence) {
            return;
        }
        if (slot == nullptr || slot->render.streamRing == nullptr) {
            FinishStreamPark(slotIndex, nullptr,
                             StreamParkDisposition::StaleRender);
            return;
        }
        VoiceRenderContext& render = slot->render;
        render.streamParkPublicHandle = publicHandle;
        render.streamParkRequestSequence = parkRequestSequence;
        StreamTerminalReason terminal = StreamTerminalReason::None;
        if (!render.streamRing->TryClaimPark(&terminal)) {
            const StreamParkDisposition disposition =
                ParkDispositionForTerminalReason(terminal);
            FinishStreamPark(slotIndex, slot, disposition);
            return;
        }
        if (!render.streamGateOpen) {
            StreamParkMailbox& mutableMailbox =
                m_streamParkMailboxes[slotIndex];
            if (mutableMailbox.outcome.token.IsValid()) {
                ReleaseStreamParkToken(
                    mutableMailbox.outcome.token,
                    StreamParkTokenState::TransactionOwned);
                mutableMailbox.outcome.token = {};
            }
            FinishStreamPark(slotIndex, slot,
                             StreamParkDisposition::ResumeReady);
            return;
        }
        render.streamParkActive = true;
        render.fading = true;
        render.fadeTotalFrames =
            fadeFrames == 0 ? m_blockFrames : fadeFrames;
        render.fadeDoneFrames = 0;
        return;
    }
    if (slot == nullptr) {
        return; // the voice ended between enqueue and apply: benign no-op
    }
    // An unstarted dependent voice has no audible tail to fade.
    if (slot->nativeVariableStart.cut.Valid() || FirstOutputProtected(*slot)) fadeMs = fadeFrames = 0;
    ClearFirstOutput(*slot);
    // Resolve the fade length in frames. A caller may specify it directly in
    // frames (the steal declick, bounded to one block regardless of block
    // size) or in milliseconds (the public Vn_Stop contract); frames win when
    // set. Milliseconds round up so a short positive fade is never zero-length.
    std::uint64_t frames = fadeFrames;
    if (fadeFrames == 0 && fadeMs != 0) {
        frames =
            (static_cast<std::uint64_t>(fadeMs) * m_sampleRate + 999) / 1000;
        frames = std::min<std::uint64_t>(frames, UINT32_MAX);
    }
    if (frames == 0) {
        // An API stop cannot replace a previously adopted timeline cause.
        slot->render.monitorControlAdopted = true;
        slot->render.stagedSourceTerminal = SourceTerminalKind::Stop;
        slot->render.stagedSourceTerminalOffset = 0;
        slot->render.stagedPlaybackEnd = SourceTerminalKind::Stop;
        slot->render.stagedPlaybackEndOffset = 0;
        PublishSourceTerminal(*slot, blockIndex);
        PublishPlaybackEnd(*slot, blockIndex);
        // An immediate cut has no ramp, so it reports as a plain ended
        // voice; stopped-by-fade is reserved for a ramp reaching zero.
        NoteMonitorVoiceRetired(slot->render.dynamicSourceIdentity);
        m_dynamicRoutes.InvalidateVoiceTap(
            slot->render.dynamicSourceIdentity);
        if (m_timelineArms != nullptr &&
            slot->render.timelineArmSlot != UINT32_MAX) {
            const TimelineArmTable::Row* row =
                m_timelineArms->RowAt(slot->render.timelineArmSlot);
            const std::uint64_t identity =
                row == nullptr
                    ? 0
                    : row->identity.load(std::memory_order_acquire);
            if (static_cast<std::uint32_t>(identity) ==
                    slot->render.timelineArmSlot &&
                static_cast<std::uint32_t>(identity >> 32) ==
                    slot->render.timelineArmGeneration) {
                const TimelineArmToken token{
                    slot->render.timelineArmSlot,
                    slot->render.timelineArmGeneration,
                    static_cast<std::uint32_t>(handle),
                    static_cast<std::uint32_t>(handle >> 32)};
                (void)m_timelineArms->Tombstone(token, blockIndex);
            }
        }
        ReleaseSlot(*slot, ReleaseThread::Audio);
        notifications.Push(
            MakeVoiceNotification(VN_NOTIFY_VOICE_ENDED, handle, blockIndex));
        return;
    }
    if (!slot->render.fading) {
        // An API stop cannot replace a previously adopted timeline cause.
        slot->render.monitorControlAdopted = true;
        slot->render.fading = true;
        slot->render.fadeTotalFrames =
            static_cast<std::uint32_t>(frames > 0 ? frames : 1);
        slot->render.fadeDoneFrames = 0;
    }
    // A second stop while fading keeps the first ramp: deterministic and
    // never lengthens an already-armed fade.
}

bool VoicePool::ReserveStreamPark(VnVoiceHandle renderHandle,
                                  VnVoiceHandle publicHandle,
                                  std::uint64_t requestSequence,
                                  ResampleProfile profile) {
    const std::uint32_t slotIndex =
        static_cast<std::uint32_t>(renderHandle);
    if (slotIndex >= m_streamParkMailboxes.size() ||
        SlotFor(renderHandle) == nullptr) {
        return false;
    }
    StreamParkMailbox& mailbox = m_streamParkMailboxes[slotIndex];
    if (mailbox.state.load(std::memory_order_acquire) !=
        StreamParkMailboxState::Empty) {
        return false;
    }
    mailbox.outcome = StreamParkOutcome{};
    mailbox.outcome.renderHandle = renderHandle;
    mailbox.outcome.publicHandle = publicHandle;
    mailbox.outcome.requestSequence = requestSequence;
    if (profile != ResampleProfile::LegacyCompatibility &&
        profile != ResampleProfile::Professional44148) {
        mailbox.outcome = StreamParkOutcome{};
        return false;
    }
    mailbox.outcome.token = ReserveStreamParkToken();
    if (!mailbox.outcome.token.IsValid()) {
        mailbox.outcome = StreamParkOutcome{};
        return false;
    }
    ++m_pendingStreamParkMailboxes;
    mailbox.state.store(StreamParkMailboxState::Reserved,
                        std::memory_order_release);
    return true;
}

void VoicePool::AbortStreamParkReservation(
    VnVoiceHandle renderHandle, VnVoiceHandle publicHandle,
    std::uint64_t requestSequence) {
    const std::uint32_t slotIndex =
        static_cast<std::uint32_t>(renderHandle);
    if (slotIndex >= m_streamParkMailboxes.size()) {
        return;
    }
    StreamParkMailbox& mailbox = m_streamParkMailboxes[slotIndex];
    if (mailbox.state.load(std::memory_order_acquire) ==
            StreamParkMailboxState::Reserved &&
        mailbox.outcome.renderHandle == renderHandle &&
        mailbox.outcome.publicHandle == publicHandle &&
        mailbox.outcome.requestSequence == requestSequence) {
        if (mailbox.outcome.token.IsValid()) {
            ReleaseStreamParkToken(
                mailbox.outcome.token,
                StreamParkTokenState::TransactionOwned);
        }
        mailbox.state.store(StreamParkMailboxState::Empty,
                            std::memory_order_release);
        --m_pendingStreamParkMailboxes;
    }
}

StreamParkCancelResult VoicePool::CancelStreamPark(
    VnVoiceHandle renderHandle, VnVoiceHandle publicHandle,
    std::uint64_t requestSequence) {
    const std::uint32_t slotIndex =
        static_cast<std::uint32_t>(renderHandle);
    if (slotIndex >= m_streamParkMailboxes.size()) {
        return StreamParkCancelResult::Missing;
    }
    StreamParkMailbox& mailbox = m_streamParkMailboxes[slotIndex];
    StreamParkMailboxState state =
        mailbox.state.load(std::memory_order_acquire);
    if (state == StreamParkMailboxState::Empty ||
        mailbox.outcome.renderHandle != renderHandle ||
        mailbox.outcome.publicHandle != publicHandle ||
        mailbox.outcome.requestSequence != requestSequence) {
        return StreamParkCancelResult::Missing;
    }
    if (state == StreamParkMailboxState::Published) {
        return StreamParkCancelResult::Published;
    }
    if (state == StreamParkMailboxState::Cancelled) {
        return StreamParkCancelResult::Cancelled;
    }
    if (mailbox.state.compare_exchange_strong(
            state, StreamParkMailboxState::Cancelled,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        return StreamParkCancelResult::Cancelled;
    }
    return state == StreamParkMailboxState::Published
               ? StreamParkCancelResult::Published
               : state == StreamParkMailboxState::Cancelled
                     ? StreamParkCancelResult::Cancelled
                     : StreamParkCancelResult::Missing;
}

bool VoicePool::ConsumeStreamParkOutcome(std::uint32_t renderSlot,
                                         StreamParkOutcome* out,
                                         bool* outCancelled) {
    if (renderSlot >= m_streamParkMailboxes.size() || out == nullptr ||
        outCancelled == nullptr) {
        return false;
    }
    StreamParkMailbox& mailbox = m_streamParkMailboxes[renderSlot];
    if (mailbox.state.load(std::memory_order_acquire) !=
        StreamParkMailboxState::Published) {
        return false;
    }
    *out = mailbox.outcome;
    *outCancelled = mailbox.outcome.cancelled;
    mailbox.state.store(StreamParkMailboxState::Empty,
                        std::memory_order_release);
    --m_pendingStreamParkMailboxes;
    if (out->token.IsValid()) {
        StreamParkTokenSlot* token = ResolveStreamParkToken(
            out->token, StreamParkTokenState::Published);
        if (!*outCancelled &&
            out->disposition == StreamParkDisposition::ResumeReady &&
            token != nullptr) {
            token->state.store(StreamParkTokenState::ParkOwned,
                               std::memory_order_release);
        } else {
            ReleaseStreamParkToken(
                out->token, StreamParkTokenState::Published);
            out->token = {};
        }
    }
    return true;
}

bool VoicePool::ConsumeStreamParkOutcomeExact(
    VnVoiceHandle renderHandle, VnVoiceHandle publicHandle,
    std::uint64_t requestSequence, StreamParkOutcome* out,
    bool* outCancelled) {
    const std::uint32_t slotIndex =
        static_cast<std::uint32_t>(renderHandle);
    if (slotIndex >= m_streamParkMailboxes.size() || out == nullptr ||
        outCancelled == nullptr) {
        return false;
    }
    StreamParkMailbox& mailbox = m_streamParkMailboxes[slotIndex];
    if (mailbox.state.load(std::memory_order_acquire) !=
            StreamParkMailboxState::Published ||
        mailbox.outcome.renderHandle != renderHandle ||
        mailbox.outcome.publicHandle != publicHandle ||
        mailbox.outcome.requestSequence != requestSequence) {
        return false;
    }
    *out = mailbox.outcome;
    *outCancelled = mailbox.outcome.cancelled;
    mailbox.state.store(StreamParkMailboxState::Empty,
                        std::memory_order_release);
    --m_pendingStreamParkMailboxes;
    if (out->token.IsValid()) {
        StreamParkTokenSlot* token = ResolveStreamParkToken(
            out->token, StreamParkTokenState::Published);
        if (!*outCancelled &&
            out->disposition == StreamParkDisposition::ResumeReady &&
            token != nullptr) {
            token->state.store(StreamParkTokenState::ParkOwned,
                               std::memory_order_release);
        } else {
            ReleaseStreamParkToken(
                out->token, StreamParkTokenState::Published);
            out->token = {};
        }
    }
    return true;
}

void VoicePool::DiscardAllStreamParkMailboxes() {
    for (StreamParkMailbox& mailbox : m_streamParkMailboxes) {
        const StreamParkMailboxState state =
            mailbox.state.load(std::memory_order_acquire);
        if (mailbox.outcome.token.IsValid()) {
            if (state == StreamParkMailboxState::Published) {
                ReleaseStreamParkToken(
                    mailbox.outcome.token,
                    StreamParkTokenState::Published);
            } else if (state == StreamParkMailboxState::Reserved ||
                       state == StreamParkMailboxState::Cancelled) {
                ReleaseStreamParkToken(
                    mailbox.outcome.token,
                    StreamParkTokenState::TransactionOwned);
            }
        }
        mailbox.state.store(StreamParkMailboxState::Empty,
                            std::memory_order_release);
    }
    m_pendingStreamParkMailboxes = 0;
    for (StreamParkTokenSlot& token : m_streamParkTokens) {
        token.state.store(StreamParkTokenState::Free,
                          std::memory_order_release);
    }
}

StreamParkTokenRef VoicePool::ReserveStreamParkToken() {
    if (memguard::IsAudioTagged()) return {};
    for (std::uint32_t index = 0;
         index < m_streamParkTokens.Capacity(); ++index) {
        if (!m_streamParkTokens.ConstructThrough(index + 1u)) return {};
        StreamParkTokenSlot& slot = m_streamParkTokens[index];
        StreamParkTokenState expected = StreamParkTokenState::Free;
        if (!slot.state.compare_exchange_strong(
                expected, StreamParkTokenState::TransactionOwned,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            continue;
        }
        ++slot.generation;
        if (slot.generation == 0)
            slot.generation = 1;
        slot.payload = StreamParkToken{};
        return {index, slot.generation};
    }
    return {};
}

VoicePool::StreamParkTokenSlot* VoicePool::ResolveStreamParkToken(
    StreamParkTokenRef ref, StreamParkTokenState expected) {
    if (!ref.IsValid() || ref.index >= m_streamParkTokens.size())
        return nullptr;
    StreamParkTokenSlot& slot = m_streamParkTokens[ref.index];
    if (slot.generation != ref.generation ||
        slot.state.load(std::memory_order_acquire) != expected) {
        return nullptr;
    }
    return &slot;
}

void VoicePool::ReleaseStreamParkToken(
    StreamParkTokenRef ref, StreamParkTokenState expected) {
    StreamParkTokenSlot* slot =
        ResolveStreamParkToken(ref, expected);
    if (slot == nullptr)
        return;
    slot->state.store(StreamParkTokenState::Free,
                      std::memory_order_release);
}

const StreamParkToken* VoicePool::ResolveParkOwnedToken(
    StreamParkTokenRef ref) const {
    if (!ref.IsValid() || ref.index >= m_streamParkTokens.size())
        return nullptr;
    const StreamParkTokenSlot& slot = m_streamParkTokens[ref.index];
    if (slot.generation != ref.generation ||
        slot.state.load(std::memory_order_acquire) !=
            StreamParkTokenState::ParkOwned) {
        return nullptr;
    }
    return &slot.payload;
}

void VoicePool::ReleaseParkOwnedToken(StreamParkTokenRef ref) {
    ReleaseStreamParkToken(ref, StreamParkTokenState::ParkOwned);
}

void VoicePool::BeginAudioBlock(std::uint64_t blockIndex) {
    assert(!m_originalSourceCallbackOpen);
    m_originalSourceCallbackOpen = true;
    ResetStreamParkCaptureBudget();
    for (Slot& slot : m_slots) {
        const auto state = slot.state.load(std::memory_order_acquire);
        if (state != SlotState::Rendering) continue;
        // Competing logic ReleaseHeld wins or this actual callback holds the
        // native slot. A stale sampled Rendering state grants no metadata use.
        AcquireOriginalCallbackStorage(slot);
        if (ConsumeOriginalSourceBodyRetirement(slot, blockIndex)) continue;
        if (!IsAudioRenderingState(slot.state.load(std::memory_order_acquire))) continue;
        ApplyDeferredFirstOutput(slot);
        slot.stagedCompletionProgress = nullptr;
        slot.stagedCompletionUpdate = {};
        slot.completionProgressStaged = false;
        slot.completionProgressTerminal = false;
    }
}

void VoicePool::EndAudioBlock() noexcept {
    for (std::size_t index = 0; index < m_originalCallbackStorage.size(); ++index) {
        auto& lease = m_originalCallbackStorage[index];
        if (!lease.storageHeld) continue;
        auto& slot = m_slots[index];
        // A timed-out worker still owns native render payload. Only Stop's
        // actual join permits its reader and Source actor to Return.
        if (!m_originalBodyShutdown &&
            slot.state.load(std::memory_order_acquire) == SlotState::Quarantined) continue;
        ReturnOriginalCallbackStorage(slot, lease);
    }
    ReturnTimelineCallbackStorage();
    m_originalSourceCallbackOpen = false;
}

bool VoicePool::StageCompletionProgress(
    Slot& slot, std::uint64_t blockIndex,
    std::uint32_t sourceOutputFrames) noexcept {
    VoiceRenderContext& render = slot.render;
    if (render.completionProgress == nullptr) {
        return true;
    }
    if (m_workers.WorkerCount() != 0 || blockIndex == UINT64_MAX) {
        slot.completionProgressTerminal = true;
        return false;
    }
    CompletionProgressBlockUpdate update{};
    update.occurrence = render.completionOccurrence;
    update.renderGeneration = render.completionRenderGeneration;
    update.sourceOutputFramesThisBlock = sourceOutputFrames;
    update.resamplerSourceFrame = render.resampler.SourceFrame();
    update.resamplerSourceFraction = render.resampler.SourceFraction();
    update.resamplerPhaseHigh = render.resampler.phaseHigh;
    update.resamplerPhaseLow = render.resampler.phaseLow;
    update.currentRatio = render.resampler.CurrentRatio();
    update.targetRatio = render.resampler.TargetRatio();
    update.rampStep = render.resampler.RampStep();
    update.rampRemaining = render.resampler.RampFramesRemaining();
    update.sourceFeedCursor = render.srcCursor;
    update.loopStartFrame = render.loopStartFrame;
    update.loopEndFrame = render.loopEndFrame;
    update.completedLoopPasses = render.completedLoopPasses;
    update.loopsRemaining = render.loopPassesRemaining;
    update.logicalSourceExhausted = render.resampler.Exhausted();
    update.loopInfinite = render.loopInfinite ? 1 : 0;
    update.hasExitMarker = render.hasExitMarker ? 1 : 0;
    update.playbackFactsRevision = render.playbackFactsRevision;
    update.completedThroughBlockSequence = blockIndex + 1u;
    ResamplerAdvanceState advance{};
    advance.sourcePhase = {update.resamplerPhaseHigh, update.resamplerPhaseLow};
    advance.currentRatio = update.currentRatio;
    advance.targetRatio = update.targetRatio;
    advance.rampStep = update.rampStep;
    advance.rampRemaining = update.rampRemaining;
    advance.loopStartFrame = render.loopStartFrame;
    advance.loopEndFrame = render.loopEndFrame;
    advance.pcmFrameCount = render.pcm.frames;
    advance.sourceStartFrame = render.loopStartFrame;
    SetResamplerConsumedPosition(&advance, render.sourceActivationStartFrame,
        render.sourceActivationLoopsRemaining, update.resamplerSourceFrame > kResampleHistoryPad
            ? update.resamplerSourceFrame - kResampleHistoryPad : 0);
    advance.loopInfinite = render.loopInfinite;
    advance.hasExitMarker = render.hasExitMarker;
    advance.exitMarkerSourceFrame = render.exitMarkerSourceFrame;
    CompletionProgressSnapshot current{};
    if (CaptureCompletionProgress(*render.completionProgress, &current) &&
        current.occurrence.slot == render.completionOccurrence.slot &&
        current.occurrence.generation ==
            render.completionOccurrence.generation &&
        current.renderGeneration == render.completionRenderGeneration) {
        advance.logicalOutputFramesProduced =
            current.logicalOutputFramesProduced + sourceOutputFrames;
    }
    (void)ComputeNextResamplerBoundaries(
        advance, &update.nextLoopEndOutputFrame,
        &update.nextExitMarkerOutputFrame);
    slot.stagedCompletionProgress = render.completionProgress;
    slot.stagedCompletionUpdate = update;
    slot.completionProgressStaged = true;
    return true;
}

void VoicePool::StageCompletionProgressForSilentBlock(
    std::uint64_t blockIndex) noexcept {
    for (Slot& slot : m_slots) {
        if (IsAudioRenderingState(slot.state.load(std::memory_order_relaxed))) {
            (void)StageCompletionProgress(slot, blockIndex, 0);
        }
    }
}

bool VoicePool::PublishCompletionProgress(
    std::uint64_t blockIndex, SourceCursorMirror::WriteView cursors,
    bool captureParking) noexcept {
    bool success = blockIndex != UINT64_MAX;
    std::uint32_t index = 0;
    for (Slot& slot : m_slots) {
        const auto state = slot.state.load(std::memory_order_acquire);
        if (index < cursors.count)
            FillSourceCursor(slot, state, cursors.rows[index], cursors.serial, captureParking);
        ++index;
        if (state == SlotState::Prepared || state == SlotState::PreparedCommitHeld) continue;
        if (slot.completionProgressTerminal) {
            success = false;
        }
        if (!slot.completionProgressStaged) {
            continue;
        }
        if (blockIndex == UINT64_MAX ||
            slot.stagedCompletionUpdate.completedThroughBlockSequence !=
                blockIndex + 1u ||
            PublishCompletionProgressBlock(
                *slot.stagedCompletionProgress,
                slot.stagedCompletionUpdate) !=
                CompletionProgressPublishStatus::Ok) {
            success = false;
        }
        slot.completionProgressStaged = false;
    }
    return success;
}

void VoicePool::ResetStreamParkCaptureBudget() {
    m_streamParkCaptureBytesThisBlock = 0;
}

void VoicePool::ApplySetVolume(VnVoiceHandle handle, float volumeDb,
                               std::uint32_t rampMs, bool outputOnly) {
    Slot* slot = SlotForCommandMetadata(handle);
    if (slot == nullptr) {
        return; // stale in flight: benign no-op
    }
    VoiceRenderContext& render = slot->render;
    float linear = 0.0f;
    if (!TryVolumeGainFromDb(volumeDb, &linear)) {
        ++m_invalidVolumeApplies;
        return;
    }
    const std::uint64_t rampFrames =
        (static_cast<std::uint64_t>(rampMs) * m_sampleRate + 999) / 1000;
    if (outputOnly) {
        render.outputGain.Set(linear, rampFrames);
        return;
    }
    // N = ceil(rampMs * rate / 1000) produced samples. rampMs 0 is a snap.
    // Do not round N up to a whole block. A re-set restarts from current.
    render.gain.Set(linear, rampFrames);
}

bool VoicePool::ApplyVariableWeightSegment(
    VnVoiceHandle handle,
    const WeightSegmentV1& segment) noexcept {
    Slot* slot = SlotForCommandMetadata(handle);
    float validated = 0.0f;
    if (slot == nullptr ||
        !ResolveWeightSegment(segment, segment.startFrame, &validated)) {
        return false;
    }
    VoiceRenderContext& render = slot->render;
    if (render.variableWeightControlCount != 0 || (render.variableWeightActive &&
        segment.segmentGeneration <=
            render.variableWeightSegment.segmentGeneration)) {
        return false;
    }
    render.variableWeightSegment = segment;
    render.variableWeightActive = true;
    return true;
}

bool VoicePool::ApplyOwnedVariableWeightSegment(VnVoiceHandle handle,
    VariableWeightControlIdentity owner, const WeightSegmentV1& segment) noexcept {
    auto* slot = SlotFor(handle);
    if (slot == nullptr) return false;
    if (!FirstOutputProtected(*slot)) return slot->render.ApplyVariableWeightControl(owner, segment);
    float checked = 0.0f;
    if (!owner.Valid() || !ResolveWeightSegment(segment, segment.startFrame, &checked)) return false;
    for (std::uint32_t index = 0; index < slot->render.variableWeightControlCount; ++index) {
        const auto& binding = slot->render.variableWeightControls[index];
        if (binding.owner == owner && (!binding.active ||
            segment.segmentGeneration <= binding.segment.segmentGeneration)) return false;
    }
    auto* endpoint = FindFirstOutputWeightEndpoint(*slot, owner, true);
    if (endpoint == nullptr || (endpoint->valid && (endpoint->weightRemoved ||
        segment.segmentGeneration <= endpoint->weightSegment.segmentGeneration))) return false;
    *endpoint = {};
    endpoint->valid = true;
    endpoint->weightOwner = owner;
    endpoint->weightSegment = segment;
    return true;
}

bool VoicePool::PrepareVariableWeightControls(VnVoiceHandle handle,
    const VariableWeightControlBinding* bindings, std::uint32_t count) noexcept {
    auto* slot = SlotFor(handle);
    if (slot == nullptr || slot->state.load(std::memory_order_acquire) != SlotState::Prepared ||
        (bindings == nullptr && count != 0) || count > kSelectionPathNodeCapacity ||
        slot->render.variableWeightControlCount != 0 || slot->render.variableWeightActive) return false;
    for (std::uint32_t index = 0; index < count; ++index) {
        float checked = 0.0f;
        if (!bindings[index].owner.Valid() || !ResolveWeightSegment(bindings[index].segment,
                bindings[index].segment.startFrame, &checked)) return false;
        for (std::uint32_t prior = 0; prior < index; ++prior)
            if (bindings[prior].owner == bindings[index].owner) return false;
    }
    if (count != 0 && slot->render.variableWeightControls.empty()) {
        const auto slotIndex = static_cast<std::uint32_t>(handle);
        if (!PrepareVariableWeightBacking(slotIndex)) return false;
        slot->render.variableWeightControls = m_variableWeightStorage[slotIndex];
    }
    for (std::uint32_t index = 0; index < count; ++index)
        slot->render.variableWeightControls[index] = {bindings[index].owner, bindings[index].segment, true,
            slot->render.pendingImmediateWeightRebase ? bindings[index].segment.segmentGeneration : 0u,
            1.0 / static_cast<double>(bindings[index].segment.endFrame - bindings[index].segment.startFrame - 1u)};
    slot->render.variableWeightControlCount = count;
    if (slot->render.pendingImmediateWeightRebase)
        slot->render.pendingImmediateWeightCount = count;
    return true;
}

bool VoicePool::RemoveOwnedVariableWeightSegment(VnVoiceHandle handle,
    VariableWeightControlIdentity owner, std::uint32_t expectedGeneration) noexcept {
    auto* slot = SlotFor(handle);
    if (slot == nullptr || !owner.Valid()) return false;
    auto& render = slot->render;
    if (FirstOutputProtected(*slot)) {
        auto* endpoint = FindFirstOutputWeightEndpoint(*slot, owner, false);
        if (endpoint != nullptr && endpoint->valid) {
            if (endpoint->weightRemoved || endpoint->weightSegment.segmentGeneration != expectedGeneration) return false;
            endpoint->weightRemoved = true;
            return true;
        }
        for (std::uint32_t index = 0; index < render.variableWeightControlCount; ++index) {
            const auto& binding = render.variableWeightControls[index];
            if (binding.owner != owner) continue;
            if (!binding.active || binding.segment.segmentGeneration != expectedGeneration) return false;
            assert(endpoint != nullptr);
            *endpoint = {};
            endpoint->valid = true;
            endpoint->weightOwner = owner;
            endpoint->weightSegment = binding.segment;
            endpoint->weightRemoved = true;
            return true;
        }
        return false;
    }
    for (std::uint32_t index = 0; index < render.variableWeightControlCount; ++index) {
        auto& binding = render.variableWeightControls[index];
        if (binding.owner != owner) continue;
        if (!binding.active || binding.segment.segmentGeneration != expectedGeneration) return false;
        // Retain the generation tombstone so a delayed update cannot restore it.
        binding.active = false;
        return true;
    }
    return false;
}

void VoicePool::ApplySetPitch(VnVoiceHandle handle, double ratio,
                              std::uint32_t rampMs) {
    Slot* slot = SlotForCommandMetadata(handle);
    if (slot == nullptr) {
        return;
    }
    VoiceRenderContext& render = slot->render;
    if (rampMs == 0) {
        render.resampler.UpdateRatio(ratio * render.rateScale, 0);
        return;
    }
    const std::uint64_t rampFrames =
        (static_cast<std::uint64_t>(rampMs) * m_sampleRate + 999) / 1000;
    const std::uint32_t frames =
        rampFrames > (std::numeric_limits<std::uint32_t>::max)()
            ? (std::numeric_limits<std::uint32_t>::max)()
            : static_cast<std::uint32_t>(rampFrames);
    render.resampler.UpdateRatio(ratio * render.rateScale, frames);
}

void VoicePool::ApplySetFilter(VnVoiceHandle handle,
                               std::uint32_t filterKind, bool enable,
                               float frequencyHz, float q,
                               const BiquadCoeffs& coeffs,
                               std::uint32_t tailFrames,
                               std::uint32_t transitionFrames) {
    Slot* slot = SlotForCommandMetadata(handle);
    if (slot == nullptr) {
        return;
    }
    VoiceRenderContext& render = slot->render;
    const bool lowpass = (filterKind == kFilterKindLowpass);
    if (!lowpass && filterKind != kFilterKindHighpass) {
        return; // unknown section: cannot be produced by this API vintage
    }
    bool& enabled = lowpass ? render.lpfEnabled : render.hpfEnabled;
    BiquadCoeffs& storedCoeffs =
        lowpass ? render.lpfCoeffs : render.hpfCoeffs;
    std::uint32_t& storedTailFrames =
        lowpass ? render.lpfTailFrames : render.hpfTailFrames;
    BiquadTransitionState<kMaxChannels>& transition =
        lowpass ? render.lpfTransition : render.hpfTransition;
    BiquadParameterGlide& glide =
        lowpass ? render.lpfGlide : render.hpfGlide;
    if (render.firstBlock) {
        // Only the very first install may snap: before the voice has rendered a
        // block there is no live section to zipper. A later zero-ramp retune is
        // a smoothed crossfade below, not an instantaneous coefficient swap.
        const bool wasEnabled = enabled;
        enabled = enable;
        if (enable) {
            storedCoeffs = coeffs;
            storedTailFrames = tailFrames;
            glide.Snap(frequencyHz, q, 0.0f);
        } else {
            storedTailFrames = 0;
            glide.active = false;
        }
        transition.Reset();
        BiquadState* states = lowpass ? render.lpfState : render.hpfState;
        if (!wasEnabled && enable) {
            for (std::uint32_t ch = 0; ch < kMaxChannels; ++ch) {
                states[ch].Reset();
            }
        }
        return;
    }
    const PostEffectTailContribution contribution =
        lowpass ? PostEffectTailContribution::Lowpass
                : PostEffectTailContribution::Highpass;
    QueueVoiceTailSuffixRearm(
        render, TailContributionPositionAfterEffects(render.fxSlotCount,
                                                      contribution));
    // A zero-length ramp on a live section falls back to the smoothing contract
    // (5-20 ms), never a hard swap; an explicit ramp keeps its own length.
    const std::uint32_t effectiveFrames =
        transitionFrames == 0 ? BiquadSmoothingFrames(m_sampleRate)
                              : transitionFrames;
    if (transitionFrames != 0 && !transition.transitioning && enabled &&
        enable) {
        // Steady-state wet->wet retune with an explicit ramp: morph the
        // coefficients continuously in the stable parameter domain. Guarded on
        // no crossfade being mid-flight — a glide run in parallel with a
        // transition would fight it for the section's coefficients, and a
        // disable crossfade still reads enabled==true until it completes, so an
        // unguarded fast path would silently drop the re-enable request.
        transition.targetCoeffs = coeffs;
        transition.targetTailFrames = tailFrames;
        glide.Retarget(frequencyHz, q, 0.0f, transitionFrames);
        return;
    }
    // A crossfade is mid-flight, the enabled state changes, or the ramp is
    // zero: hand the latest request to the transition machinery, which coalesces
    // to pending-latest, and keep the glide out of the way — an enable seeds its
    // parameter position for a later steady-state retune; a disable parks it.
    if (enable) {
        glide.Snap(frequencyHz, q, 0.0f);
    } else {
        glide.active = false;
    }
    RequestBiquadTransition(transition, enabled, storedCoeffs, tailFrames,
                            enable, coeffs, effectiveFrames);
}

void VoicePool::ApplySetPosition(VnVoiceHandle handle, float x, float y,
                                 float z) {
    Slot* slot = SlotForCommandMetadata(handle);
    if (slot == nullptr) {
        return;
    }
    slot->render.positionX = x;
    slot->render.positionY = y;
    slot->render.positionZ = z;
}

void VoicePool::ApplySetPan(VnVoiceHandle handle, float pan, float gain0,
                            float gain1, std::uint32_t channel0,
                            std::uint32_t channel1, float pan2d) {
    Slot* slot = SlotForCommandMetadata(handle);
    if (slot == nullptr) {
        return;
    }
    VoiceRenderContext& render = slot->render;
    render.pan = (pan < -1.0f) ? -1.0f : (pan > 1.0f) ? 1.0f : pan;
    render.pan2d = std::clamp(pan2d, -1.0f, 1.0f);
    render.spatialMatrixDirty = true;
    render.panGain0 = gain0;
    render.panGain1 = gain1;
    render.panChannel0 = channel0;
    render.panChannel1 = channel1;
    if (render.pcm.channelCount > 2) {
        const StereoBalance balance = DeriveStereoBalance(render.pan2d);
        if (render.firstBlock) {
            render.balanceLeftTransition.SnapTo(balance.left);
            render.balanceRightTransition.SnapTo(balance.right);
        } else {
            const auto frames = BiquadSmoothingFrames(m_sampleRate);
            render.balanceLeftTransition.Retarget(balance.left, frames);
            render.balanceRightTransition.Retarget(balance.right, frames);
        }
    }
}

void VoicePool::ApplySetWidth(VnVoiceHandle handle, float width) {
    Slot* slot = SlotForCommandMetadata(handle);
    if (slot == nullptr) {
        return;
    }
    VoiceRenderContext& render = slot->render;
    const float clamped =
        (width < 0.0f) ? 0.0f : (width > 1.0f) ? 1.0f : width;
    render.width = clamped;
    render.spatialMatrixDirty = true;
}

void VoicePool::ApplySetSpatializationMix(VnVoiceHandle handle, float mix) {
    Slot* slot = SlotForCommandMetadata(handle);
    if (slot == nullptr) {
        return;
    }
    VoiceRenderContext& render = slot->render;
    const float clamped =
        (mix < 0.0f) ? 0.0f : (mix > 1.0f) ? 1.0f : mix;
    render.spatializationMix = clamped;
    render.spatialMatrixDirty = true;
}

void VoicePool::ApplySetShelf(VnVoiceHandle handle,
                              std::uint32_t shelfKind, bool enable,
                              float frequencyHz, float q, float gainDb,
                              const BiquadCoeffs& coeffs,
                              std::uint32_t tailFrames,
                              std::uint32_t transitionFrames) {
    Slot* slot = SlotForCommandMetadata(handle);
    if (slot == nullptr) {
        return;
    }
    VoiceRenderContext& render = slot->render;
    const bool lowShelf = shelfKind == kShelfKindLow;
    if (!lowShelf && shelfKind != kShelfKindHigh) {
        return;
    }
    bool& enabled =
        lowShelf ? render.shelfEnabled : render.highShelfEnabled;
    BiquadCoeffs& storedCoeffs =
        lowShelf ? render.shelfCoeffs : render.highShelfCoeffs;
    std::uint32_t& storedTailFrames =
        lowShelf ? render.shelfTailFrames : render.highShelfTailFrames;
    BiquadState* states =
        lowShelf ? render.shelfState : render.highShelfState;
    BiquadTransitionState<kMaxChannels>& transition =
        lowShelf ? render.shelfTransition : render.highShelfTransition;
    BiquadParameterGlide& glide =
        lowShelf ? render.shelfGlide : render.highShelfGlide;
    if (render.firstBlock) {
        // Only the very first install may snap: before the voice has rendered a
        // block there is no live section to zipper. A later zero-ramp retune is
        // a smoothed crossfade below, not an instantaneous coefficient swap.
        const bool wasEnabled = enabled;
        enabled = enable;
        if (enable) {
            storedCoeffs = coeffs;
            storedTailFrames = tailFrames;
            glide.Snap(frequencyHz, q, gainDb);
        } else {
            storedTailFrames = 0;
            glide.active = false;
        }
        transition.Reset();
        if (!wasEnabled && enable) {
            for (std::uint32_t ch = 0; ch < kMaxChannels; ++ch) {
                states[ch].Reset();
            }
        }
        return;
    }
    QueueVoiceTailSuffixRearm(
        render, TailContributionPositionAfterEffects(
                    render.fxSlotCount,
                    lowShelf ? PostEffectTailContribution::Shelf
                             : PostEffectTailContribution::HighShelf));
    // A zero-length ramp on a live section falls back to the smoothing contract
    // (5-20 ms), never a hard swap; an explicit ramp keeps its own length.
    const std::uint32_t effectiveFrames =
        transitionFrames == 0 ? BiquadSmoothingFrames(m_sampleRate)
                              : transitionFrames;
    if (transitionFrames != 0 && !transition.transitioning && enabled &&
        enable) {
        // Steady-state wet->wet retune with an explicit ramp: morph the
        // coefficients continuously in the stable parameter domain. Guarded on
        // no crossfade being mid-flight — a glide run in parallel with a
        // transition would fight it for the section's coefficients, and a
        // disable crossfade still reads enabled==true until it completes, so an
        // unguarded fast path would silently drop the re-enable request.
        transition.targetCoeffs = coeffs;
        transition.targetTailFrames = tailFrames;
        glide.Retarget(frequencyHz, q, gainDb, transitionFrames);
        return;
    }
    // A crossfade is mid-flight, the enabled state changes, or the ramp is
    // zero: hand the latest request to the transition machinery, which coalesces
    // to pending-latest, and keep the glide out of the way — an enable seeds its
    // parameter position for a later steady-state retune; a disable parks it.
    if (enable) {
        glide.Snap(frequencyHz, q, gainDb);
    } else {
        glide.active = false;
    }
    RequestBiquadTransition(transition, enabled, storedCoeffs, tailFrames,
                            enable, coeffs, effectiveFrames);
}

void VoicePool::ApplySetVoiceEffectBypass(VnVoiceHandle handle,
                                          std::uint32_t slot,
                                          std::uint32_t bypass,
                                          std::uint32_t action,
                                          std::uint32_t expectedEffectType) {
    Slot* s = SlotForCommandMetadata(handle);
    if (s == nullptr) {
        return; // stale in flight: benign no-op
    }
    VoiceRenderContext& render = s->render;
    if (slot >= render.fxSlotCount) {
        return; // out-of-range for this voice's chain: nothing to toggle
    }
    if (action == kEffectActionReset) {
        if (render.fxSlotEffectRef[slot].type == expectedEffectType &&
            expectedEffectType == VN_EFFECT_COMPRESSOR &&
            render.fxSlots[slot].reset != nullptr) {
            render.fxSlots[slot].reset(render.fxSlots[slot].state);
        }
        return;
    }
    // Target 1 = fully dry (bypassed), 0 = fully wet (active).
    const float target = (bypass != 0) ? 1.0f : 0.0f;
    const bool enablingLimiter =
        target == 0.0f && render.fxBypassTarget[slot] == 1.0f &&
        render.fxSlotEffectRef[slot].type == VN_EFFECT_LIMITER;
    render.fxBypassTarget[slot] = target;
    if (render.fxSlots[slot].process == nullptr) {
        // An empty slot is skipped in the cascade before any ramp could
        // settle, so settle it here at the moment the command applies:
        // otherwise the ramp would linger and, once an effect is installed
        // into the slot later, leak a wet->dry crossfade on that first block
        // despite this earlier bypass. Settling at apply (not in the render
        // loop) also holds when an effect is installed before the next
        // render, since no intervening block is needed.
        render.fxBypassMix[slot] = target;
        render.fxBypassRamping[slot] = false;
        return;
    }
    if (enablingLimiter && render.fxSlots[slot].reset != nullptr) {
        // The frozen Limiter contract starts bypass-to-enable with clean gain
        // reduction while retaining its parameters.
        render.fxSlots[slot].reset(render.fxSlots[slot].state);
    }
    // Occupied slot: arm a one-block crossfade only when the target differs
    // from the settled mix, so setting the state a slot already holds is a
    // no-op and never restarts a ramp.
    render.fxBypassRamping[slot] = (target != render.fxBypassMix[slot]);
}

void VoicePool::ApplySetVoiceEffect(VnVoiceHandle handle, std::uint32_t slot,
                                    std::uint32_t effectType,
                                    std::uint32_t poolIndex,
                                    std::uint64_t poolGeneration) {
    (void)ApplyOwnedVoiceEffect(handle, slot, {effectType, poolIndex, poolGeneration});
}

bool VoicePool::ApplyOwnedVoiceEffect(VnVoiceHandle handle, std::uint32_t slot,
    const EffectInstanceRef& incoming) {
    void* incomingState = m_effectBank.LiveStateFor(incoming);
    if (incomingState == nullptr) { return false; }
    Slot* s = SlotForCommandMetadata(handle);
    if (s == nullptr) {
        // The voice ended between the logic-side Set and this apply: the
        // instance was acquired but never installed, so retire it here or it
        // would leak (the audio thread never frees; the logic side reclaims).
        return m_effectBank.RetireQueue().TryPush(incoming);
    }
    VoiceRenderContext& render = s->render;
    if (slot >= render.fxSlotCount) {
        // Out of range for this voice (validated logic-side; defensive):
        // retire the unused instance rather than drop it.
        return m_effectBank.RetireQueue().TryPush(incoming);
    }
    // In-place replace: whatever this slot held is retired for reclamation.
    if (render.fxSlotEffectRef[slot].type != kEffectTypeNone) {
        if (!m_effectBank.RetireQueue().TryPush(render.fxSlotEffectRef[slot])) return false;
    }
    render.fxSlots[slot] =
        BuildEffect(incoming.type, incomingState);
    render.fxSlotEffectRef[slot] = incoming;
    // The slot keeps its settled bypass mix: installing into a slot that was
    // bypassed (mix 1) yields a dry output from the first block, matching the
    // empty-slot settle-at-apply rule.
    return true;
}

void VoicePool::ApplyClearVoiceEffect(VnVoiceHandle handle,
                                      std::uint32_t slot) {
    (void)ApplyOwnedClearVoiceEffect(handle, slot);
}

bool VoicePool::ApplyOwnedClearVoiceEffect(VnVoiceHandle handle, std::uint32_t slot) {
    Slot* s = SlotForCommandMetadata(handle);
    if (s == nullptr) {
        return true; // Its effects already belong to the physical release tail.
    }
    VoiceRenderContext& render = s->render;
    if (slot >= render.fxSlotCount) {
        return true;
    }
    if (render.fxSlotEffectRef[slot].type != kEffectTypeNone) {
        // Retire the instance and empty the slot; its tail is cut here (not
        // drained). If a drain is already in progress, the cascade recomputes
        // the voice's aggregate tail from the still-occupied slots on its next
        // block, so removing this slot immediately drops its contribution to
        // the remaining life (down to the surviving slots, or to Done when it
        // was the only one keeping the voice alive).
        if (!m_effectBank.RetireQueue().TryPush(render.fxSlotEffectRef[slot])) return false;
        render.fxSlots[slot] = InsertEffect{};
        render.fxSlotEffectRef[slot] = EffectInstanceRef{};
    }
    // Settle any in-progress bypass ramp to its target and keep the target, so
    // the cleared slot behaves exactly like a never-occupied one (matching the
    // empty-slot settle-at-apply rule).
    render.fxBypassMix[slot] = render.fxBypassTarget[slot];
    render.fxBypassRamping[slot] = false;
    return true;
}

void VoicePool::ApplySetVoiceEffectParam(VnVoiceHandle handle,
                                         std::uint32_t slot,
                                         std::uint32_t paramIndex,
                                         float value,
                                         std::uint32_t effectType,
                                         const BiquadCoeffs& coeffs,
                                         std::uint32_t tailFrames,
                                         std::uint32_t transitionFrames) {
    Slot* s = SlotForCommandMetadata(handle);
    if (s == nullptr) {
        return;
    }
    VoiceRenderContext& render = s->render;
    if (slot >= render.fxSlotCount) {
        return;
    }
    if (render.fxSlotEffectRef[slot].type != effectType) {
        return;
    }
    InsertEffect& fx = render.fxSlots[slot];
    if (fx.process == nullptr) {
        return; // empty slot: nothing to parameterize
    }
    if (effectType == VN_EFFECT_THREE_BAND_TONE) {
        QueueVoiceTailSuffixRearm(render, slot);
        ToneApplyPreparedParam(fx.state, paramIndex, value, coeffs,
                               tailFrames);
    } else if (effectType == VN_EFFECT_FEEDBACK_DELAY &&
               (paramIndex == kDelayParamWet ||
                paramIndex == kDelayParamDry)) {
        QueueVoiceTailSuffixRearm(render, slot);
        DelayApplyPreparedParam(fx.state, paramIndex, value,
                                transitionFrames);
    } else if (effectType == VN_EFFECT_GAIN) {
        QueueVoiceTailSuffixRearm(render, slot);
        GainApplyPreparedParam(fx.state, coeffs.b0, transitionFrames);
    } else if (effectType == VN_EFFECT_LIMITER) {
        QueueVoiceTailSuffixRearm(render, slot);
        LimiterRequestPreparedParam(fx.state, paramIndex, coeffs.b0,
                                    transitionFrames);
    } else if (effectType == VN_EFFECT_COMPRESSOR) {
        // Compressor vectors require their dedicated nine-float transport.
        // A generic command is incomplete and therefore fails closed.
        return;
    } else {
        if (fx.setParam == nullptr) {
            return;
        }
        QueueVoiceTailSuffixRearm(render, slot);
        fx.setParam(fx.state, paramIndex, value);
    }
}

void VoicePool::ApplySetVoiceCompressorParameters(
    VnVoiceHandle handle, std::uint32_t slot,
    std::uint32_t effectType,
    const CompressorPreparedParameters& prepared,
    std::uint32_t transitionFrames) {
    if (effectType != VN_EFFECT_COMPRESSOR) {
        return;
    }
    Slot* s = SlotForCommandMetadata(handle);
    if (s == nullptr) {
        return;
    }
    VoiceRenderContext& render = s->render;
    if (slot >= render.fxSlotCount ||
        render.fxSlotEffectRef[slot].type != effectType) {
        return;
    }
    InsertEffect& fx = render.fxSlots[slot];
    if (fx.process == nullptr) {
        return;
    }
    QueueVoiceTailSuffixRearm(render, slot);
    CompressorRequestPreparedParameters(
        static_cast<CompressorState*>(fx.state), prepared,
        transitionFrames);
}

void VoicePool::ApplyBusTombstones(const std::uint8_t* alive,
                                   const std::uint32_t* parents,
                                   std::uint32_t slotCount,
                                   const std::uint32_t* generations,
                                   const std::uint32_t* migrationTargets) {
    if (alive == nullptr || parents == nullptr || slotCount == 0) {
        return; // no slot-aware plan installed yet: nothing tombstoned
    }
    // Nearest alive ancestor through the plan's parents. The master (index
    // 0) is never tombstoned, so the walk terminates; the guard bounds a
    // corrupt chain defensively at the master.
    const auto migrate = [alive, parents,
                          slotCount](std::uint32_t bus) -> std::uint32_t {
        std::uint32_t cursor = bus;
        for (std::uint32_t guard = 0; guard <= slotCount; ++guard) {
            if (cursor >= slotCount || alive[cursor] != 0) {
                return cursor < slotCount ? cursor : 0;
            }
            const std::uint32_t parent = parents[cursor];
            if (parent == cursor || parent >= slotCount) {
                return 0;
            }
            cursor = parent;
        }
        return 0;
    };
    for (Slot& slot : m_slots) {
        const SlotState state = slot.state.load(std::memory_order_relaxed);
        if (!IsAudioRenderingState(state) && state != SlotState::VariableStartWaiting) {
            // Claimed contexts remain logic-thread-owned until ApplyPlay
            // publishes them; touching one here would race its construction.
            continue;
        }
        // Rendering is entered by ApplyPlay on this audio thread and is never
        // written by the logic thread. The relaxed load is only this thread's
        // state observation; render-context access stays within its ownership.
        VoiceRenderContext& ctx = slot.render;
        if (ctx.busIndex < slotCount &&
            (alive[ctx.busIndex] == 0 ||
             (generations != nullptr &&
              ctx.busGeneration != generations[ctx.busIndex]))) {
            ctx.busIndex =
                migrationTargets != nullptr
                    ? migrationTargets[ctx.busIndex]
                    : migrate(ctx.busIndex);
            if (ctx.busIndex >= slotCount ||
                alive[ctx.busIndex] == 0) {
                ctx.busIndex = migrate(ctx.busIndex);
            }
            if (generations != nullptr) {
                ctx.busGeneration = generations[ctx.busIndex];
            }
        }
        for (std::uint32_t s = 0; s < kMaxVoiceSendSlots; ++s) {
            VoiceSendSlot& send = ctx.voiceSends[s];
            if (send.active && send.targetBusIndex < slotCount &&
                (alive[send.targetBusIndex] == 0 ||
                 (generations != nullptr &&
                  send.targetBusGeneration !=
                      generations[send.targetBusIndex]))) {
                send = VoiceSendSlot{};
                if (ctx.voiceSendActiveCount > 0) {
                    --ctx.voiceSendActiveCount;
                }
            }
        }
    }
}

void VoicePool::ApplySetVoiceSend(VnVoiceHandle handle, std::uint32_t sendSlot,
                                  std::uint32_t targetBusIndex,
                                  float levelDb,
                                  std::uint32_t targetBusGeneration) {
    Slot* slot = SlotForCommandMetadata(handle);
    if (slot == nullptr) {
        return; // stale in flight: benign no-op
    }
    if (sendSlot >= kMaxVoiceSendSlots) {
        return; // logic validated the slot; guard a corrupt record
    }
    VoiceSendSlot& send = slot->render.voiceSends[sendSlot];
    const bool wasActive = send.active;
    send = VoiceSendSlot{};
    send.active = true;
    send.targetBusIndex = targetBusIndex;
    send.targetBusGeneration = targetBusGeneration;
    // The initial level snaps (no fictitious prior value), exactly like a
    // voice's first gain at play.
    send.gain.SnapTo(DbToLinear(levelDb));
    if (!wasActive) {
        ++slot->render.voiceSendActiveCount;
    }
}

void VoicePool::ApplyClearVoiceSend(VnVoiceHandle handle,
                                    std::uint32_t sendSlot) {
    Slot* slot = SlotForCommandMetadata(handle);
    if (slot == nullptr) {
        return;
    }
    if (sendSlot >= kMaxVoiceSendSlots) {
        return;
    }
    VoiceSendSlot& send = slot->render.voiceSends[sendSlot];
    if (send.active) {
        send = VoiceSendSlot{};
        if (slot->render.voiceSendActiveCount > 0) {
            --slot->render.voiceSendActiveCount;
        }
    }
}

void VoicePool::ApplySetVoiceSendLevel(VnVoiceHandle handle,
                                       std::uint32_t sendSlot, float levelDb,
                                       std::uint32_t rampMs) {
    Slot* slot = SlotForCommandMetadata(handle);
    if (slot == nullptr) {
        return;
    }
    if (sendSlot >= kMaxVoiceSendSlots) {
        return;
    }
    VoiceSendSlot& send = slot->render.voiceSends[sendSlot];
    if (!send.active) {
        return; // no-op on an empty slot (the API refuses this logic-side)
    }
    const float linear = DbToLinear(levelDb);
    if (rampMs == 0) {
        send.gain.SnapTo(linear);
        send.ramping = false;
        return;
    }
    // Same multi-block ramp formulation as the voice/bus gain: whole-block
    // linear, landing exactly on a block boundary.
    const std::uint64_t rampFrames =
        (static_cast<std::uint64_t>(rampMs) * m_sampleRate + 999) / 1000;
    std::uint64_t blocks = (rampFrames + m_blockFrames - 1) / m_blockFrames;
    if (blocks == 0) {
        blocks = 1;
    }
    send.ramping = true;
    send.rampStartGain = send.gain.current;
    send.rampEndGain = linear;
    send.rampBlocksTotal = blocks;
    send.rampBlocksDone = 0;
}

VoicePool::RenderAllResult VoicePool::RenderAll(
    BlockArena& arena, float* const* busPlanes, std::uint32_t blockFrames,
    NotificationQueue& notifications, std::uint64_t blockIndex,
    std::uint64_t jobDeadlineAt, std::uint64_t sourceFrameBegin) {
    RenderAllResult outcome;
    const auto beginCompressors = [](VoiceRenderContext& render) {
        for (std::uint32_t slot = 0; slot < render.fxSlotCount; ++slot) {
            render.compressorParamTransitionThisBlock[slot] = false;
            render.compressorBypassTransitionThisBlock[slot] = false;
            if (render.fxSlotEffectRef[slot].type !=
                VN_EFFECT_COMPRESSOR) {
                continue;
            }
            auto* state = static_cast<CompressorState*>(
                render.fxSlots[slot].state);
            render.compressorParamTransitionThisBlock[slot] =
                state != nullptr &&
                state->parameterTransitionFramesRemaining != 0;
            render.compressorBypassTransitionThisBlock[slot] =
                render.fxBypassRamping[slot];
            CompressorBeginBlock(state);
        }
    };
    const auto publishCompressors =
        [&](std::uint32_t slotIndex, Slot& slot) {
            VoiceRenderContext& render = slot.render;
            const VnVoiceHandle renderHandle = MakeHandle(
                slotIndex,
                slot.generation.load(std::memory_order_relaxed));
            for (std::uint32_t effectSlot = 0;
                 effectSlot < render.fxSlotCount; ++effectSlot) {
                const EffectInstanceRef& ref =
                    render.fxSlotEffectRef[effectSlot];
                if (ref.type != VN_EFFECT_COMPRESSOR) {
                    continue;
                }
                const auto* state = static_cast<const CompressorState*>(
                    render.fxSlots[effectSlot].state);
                if (state == nullptr) {
                    continue;
                }
                CompressorRuntimePayload payload{};
                payload.hostIdentity = renderHandle;
                payload.blockEpoch = blockIndex;
                payload.detectorNonFiniteCount =
                    state->telemetry.detectorNonFiniteCount;
                payload.detectorSaturationCount =
                    state->telemetry.detectorSaturationCount;
                payload.hostScope = VN_EFFECT_HOST_SCOPE_VOICE;
                payload.hostDenseIndex = slotIndex;
                payload.effectSlot = effectSlot;
                const bool bypassed =
                    render.fxBypassRamping[effectSlot]
                        ? render.fxBypassTarget[effectSlot] >= 1.0f
                        : render.fxBypassMix[effectSlot] >= 1.0f;
                if (bypassed) {
                    payload.stateFlags |=
                        VN_COMPRESSOR_RUNTIME_BYPASSED;
                }
                if (render.compressorBypassTransitionThisBlock[
                        effectSlot]) {
                    payload.stateFlags |=
                        VN_COMPRESSOR_RUNTIME_BYPASS_TRANSITION;
                }
                if (render.compressorParamTransitionThisBlock[
                        effectSlot] ||
                    state->parameterTransitionFramesRemaining != 0) {
                    payload.stateFlags |=
                        VN_COMPRESSOR_RUNTIME_PARAM_TRANSITION;
                }
                if (state->telemetry.resetThisBlock) {
                    payload.stateFlags |=
                        VN_COMPRESSOR_RUNTIME_RESET_THIS_BLOCK;
                }
                if (state->telemetry.resetBridgeActive) {
                    payload.stateFlags |=
                        VN_COMPRESSOR_RUNTIME_RESET_BRIDGE;
                }
                payload.transitionFramesRemaining =
                    state->parameterTransitionFramesRemaining;
                payload.mainInputPeakLinear =
                    state->telemetry.mainInputPeak;
                payload.detectorPeakLinear =
                    state->telemetry.detectorPeak;
                payload.outputPeakLinear =
                    state->telemetry.outputPeak;
                payload.maxGainReductionDb =
                    state->telemetry.maxGainReductionDb;
                payload.endGainReductionDb =
                    state->telemetry.endGainReductionDb;
                m_effectBank.PublishCompressorRuntime(ref, payload);
            }
        };
    if (m_deferredJobCount == 0 && !m_workers.TryReserveBatch()) {
        // Serial in-callback path (renderWorkerCount = 0), unchanged from M4:
        // each rendering voice runs its full cascade and folds as it goes.
        // This keeps every existing baseline byte-untouched at the default.
        for (std::uint32_t i = 0; i < m_slots.size(); ++i) {
            Slot& slot = m_slots[i];
            // Relaxed: Rendering is entered and left only on this (audio)
            // thread, so the render loop always observes its own transitions.
            if (!IsAudioRenderingState(slot.state.load(std::memory_order_relaxed))) {
                continue;
            }
            PrepareFirstOutputTimelineWeights(slot);
            beginCompressors(slot.render);
            slot.render.monitorRenderSlot = i;
            slot.render.monitorRenderGeneration =
                slot.generation.load(std::memory_order_relaxed);
            slot.render.monitorPreChainFrames = 0;
            slot.render.dynamicVoiceTapActive =
                m_dynamicRoutes.HasVoiceTap(slot.render.dynamicSourceIdentity);
            // Pass the full bus-major plane base: the cascade folds into the
            // voice's own bus and scatters its sends into their target buses
            // from the same base.
            const StageResult result = RenderVoiceBlock(
                slot.render, arena, busPlanes, blockFrames, m_mixChannels,
                m_mixLayout, sourceFrameBegin, m_monoStereoMix);
            if (slot.firstOutputPhase == FirstOutputPhase::AwaitingFirstFold &&
                slot.render.cascade.runnable && slot.render.cascade.produced > 0)
                slot.firstOutputPhase = FirstOutputPhase::FirstFolded;
            if (result.phase != SourcePhase::Done) {
                m_dynamicRoutes.PublishVoiceTap(
                    slot.render.dynamicSourceIdentity,
                    slot.render.cascade.runnable
                        ? slot.render.cascade.scratch
                        : nullptr,
                    slot.render.cascade.runnable
                        ? slot.render.pcm.channelCount
                        : 0,
                    slot.render.cascade.runnable
                        ? slot.render.cascade.offset
                        : 0,
                    slot.render.cascade.runnable
                        ? slot.render.cascade.produced
                        : 0);
            }
            publishCompressors(i, slot);
            (void)StageCompletionProgress(
                slot, blockIndex,
                slot.render.logicalSourceOutputFramesThisBlock);
            PublishSourceTerminal(slot, blockIndex);
            HandleVoiceLifecycle(i, slot, result, notifications, blockIndex);
        }
        return outcome;
    }

    // Job-parallel path (renderWorkerCount > 0). Three phases realize the §8.2
    // determinism invariant:
    //   1. Serial pre-pass, slot order: reserve every rendering voice's cascade
    //      scratch from the one shared arena and collect the job list. The
    //      arena is a bump allocator, so doing all reservations here keeps
    //      dispatch allocation-free and the high-water mark identical to serial
    //      (same allocation set, order-independent peak).
    //   2. Parallel: run the independent per-voice cascades across the worker
    //      pool. Each cascade touches only its own context and scratch.
    //   3. Serial join, slot order: fold each voice into the bus planes and run
    //      its lifecycle. The float reduction into the shared planes happens in
    //      fixed slot order regardless of which worker produced which cascade,
    //      so the mix is byte-identical to the serial path at any worker count.
    std::uint32_t jobCount = m_deferredJobCount;
    if (jobCount == 0) {
        for (std::uint32_t i = 0; i < m_slots.size(); ++i) {
            Slot& slot = m_slots[i];
            if (!IsAudioRenderingState(slot.state.load(std::memory_order_relaxed))) {
                continue;
            }
            PrepareFirstOutputTimelineWeights(slot);
            beginCompressors(slot.render);
            slot.render.monitorRenderSlot = i;
            slot.render.monitorRenderGeneration =
                slot.generation.load(std::memory_order_relaxed);
            slot.render.monitorPreChainFrames = 0;
            slot.render.dynamicVoiceTapActive =
                m_dynamicRoutes.HasVoiceTap(slot.render.dynamicSourceIdentity);
            PrepareVoiceCascade(slot.render, arena, blockFrames, m_mixChannels);
            m_jobSlots[jobCount] = i;
            m_jobGenerations[jobCount] =
                slot.generation.load(std::memory_order_relaxed);
            ++jobCount;
        }

        if (jobCount > 0) {
            m_jobQuantumIndex = blockIndex;
            m_jobBlockFrames = blockFrames;
            m_jobSourceFrameBegin = sourceFrameBegin;
            const RenderWorkerPool::RunResult batch = m_workers.RunUntil(
                jobCount, &VoicePool::CascadeThunk, this, jobDeadlineAt);
            if (batch.busyPeakTicks > m_workerBusyPeakTicks) {
                m_workerBusyPeakTicks = batch.busyPeakTicks;
            }
            outcome.batchDeferred = batch.deferred && batch.unfinishedJobs != 0;
            outcome.overrunTicks = batch.overrunTicks;
            outcome.pendingVoices = batch.unfinishedJobs;
        } else {
            m_workers.CancelReservedBatch();
        }

    } else {
        outcome.batchDeferred = !m_workers.PendingJobsCompleted();
    }
    if (outcome.batchDeferred) {
        m_deferredJobCount = jobCount;
        return outcome;
    }
    m_deferredJobCount = 0;

    for (std::uint32_t j = 0; j < jobCount; ++j) {
        const std::uint32_t i = m_jobSlots[j];
        Slot& slot = m_slots[i];
        if (slot.render.lastResult.phase != SourcePhase::Done) {
            m_dynamicRoutes.PublishVoiceTap(
                slot.render.dynamicSourceIdentity,
                slot.render.cascade.runnable
                    ? slot.render.cascade.scratch
                    : nullptr,
                slot.render.cascade.runnable
                    ? slot.render.pcm.channelCount
                    : 0,
                slot.render.cascade.runnable
                    ? slot.render.cascade.offset
                    : 0,
                slot.render.cascade.runnable
                    ? slot.render.cascade.produced
                    : 0);
        }
        if (slot.render.cascade.runnable) {
            FoldVoiceIntoBuses(slot.render, busPlanes, blockFrames,
                               m_mixChannels, m_mixLayout, sourceFrameBegin, m_monoStereoMix);
            if (slot.firstOutputPhase == FirstOutputPhase::AwaitingFirstFold &&
                slot.render.cascade.produced > 0) slot.firstOutputPhase = FirstOutputPhase::FirstFolded;
        }
        publishCompressors(i, slot);
        (void)StageCompletionProgress(
            slot, blockIndex,
            slot.render.logicalSourceOutputFramesThisBlock);
        PublishSourceTerminal(slot, blockIndex);
        // A pre-pass that declined (gate closed or arena shortfall) still left
        // ctx.lastResult carrying {0, phase}, so the lifecycle reads uniformly.
        HandleVoiceLifecycle(i, slot, slot.render.lastResult, notifications,
                             blockIndex);
    }
    return outcome;
}

void VoicePool::QuarantineSlot(std::uint32_t slotIndex, Slot& slot,
                               NotificationQueue& notifications,
                               std::uint64_t blockIndex) {
    const VnVoiceHandle handle = MakeHandle(
        slotIndex, slot.generation.load(std::memory_order_relaxed));
    ClearFirstOutput(slot);
    // Deliberately not ReleaseSlot. Every step that call makes hands something
    // back to another thread while a writer may still be using it: dropping the
    // PCM use count lets the logic side unregister a buffer the cascade is
    // reading, retiring the stream ring lets the streaming thread detach and
    // reuse the slot, and pushing the effect refs onto the retire queue lets a
    // later voice receive an instance whose state is mid-write. The pooled
    // instances stay out of circulation for the rest of the session; the cost is
    // recorded rather than traded away.
    //
    // The generation still advances, so the public handle classifies as stale
    // exactly as it would for an ended voice, and no later command can reach
    // this render context.
    // A stranded writer keeps this true native installation loan until join.
    slot.originalBodyInstallationCompleted.store(false, std::memory_order_release);
    NoteMonitorVoiceRetired(
        slot.monitorPublicHandle.load(std::memory_order_relaxed));
    std::uint32_t next =
        slot.generation.load(std::memory_order_relaxed) + 1;
    if (next == 0) {
        next = 1;
    }
    slot.generation.store(next, std::memory_order_relaxed);
    slot.state.store(SlotState::Quarantined, std::memory_order_release);
    m_completed.fetch_add(1, std::memory_order_relaxed);
    // The host is told the voice ended, because from its side it did. Counting
    // it as completed keeps that count and the notification stream agreeing.
    notifications.Push(MakeVoiceNotification(VN_NOTIFY_VOICE_ENDED, handle,
                                             blockIndex));
}

void VoicePool::PublishLiveRouteSilence() {
    for (Slot& slot : m_slots) {
        if (!IsAudioRenderingState(slot.state.load(std::memory_order_relaxed)) ||
            slot.render.phase == SourcePhase::Done) {
            continue;
        }
        m_dynamicRoutes.PublishVoiceTap(
            slot.render.dynamicSourceIdentity, nullptr, 0, 0, 0);
    }
}

void VoicePool::RunCascadeJob(std::uint32_t jobIndex) {
    Slot& slot = m_slots[m_jobSlots[jobIndex]];
    if (slot.generation.load(std::memory_order_acquire) !=
        m_jobGenerations[jobIndex]) {
        return;
    }
    VoiceRenderContext& ctx = slot.render;
    if (ctx.timelineEvents.eventCount != 0 &&
        (ctx.timelineEvents.renderGeneration != m_jobGenerations[jobIndex] ||
         ctx.timelineEvents.quantumIndex != m_jobQuantumIndex)) {
        return;
    }
    if (ctx.cascade.runnable) {
        ctx.monitorRenderSlot = m_jobSlots[jobIndex];
        ctx.monitorRenderGeneration = m_jobGenerations[jobIndex];
        RunVoiceCascade(ctx, m_jobBlockFrames);
    }
}

void VoicePool::CascadeThunk(void* userData, std::uint32_t jobIndex) {
    static_cast<VoicePool*>(userData)->RunCascadeJob(jobIndex);
}

void VoicePool::HandleVoiceLifecycle(std::uint32_t slotIndex, Slot& slot,
                                     const StageResult& result,
                                     NotificationQueue& notifications,
                                     std::uint64_t blockIndex) {
    if ((result.phase == SourcePhase::Draining ||
         result.phase == SourcePhase::Done) &&
        m_timelineArms != nullptr &&
        slot.render.timelineArmSlot != UINT32_MAX) {
        const TimelineArmTable::Row* row =
            m_timelineArms->RowAt(slot.render.timelineArmSlot);
        const std::uint64_t identity =
            row == nullptr
                ? 0
                : row->identity.load(std::memory_order_acquire);
        if (static_cast<std::uint32_t>(identity) ==
                slot.render.timelineArmSlot &&
            static_cast<std::uint32_t>(identity >> 32) ==
                slot.render.timelineArmGeneration) {
            const TimelineArmToken token{
                slot.render.timelineArmSlot,
                slot.render.timelineArmGeneration, slotIndex,
                slot.generation.load(std::memory_order_relaxed)};
            const std::uint64_t completionSequence =
                blockIndex == UINT64_MAX ? UINT64_MAX : blockIndex + 1u;
            (void)m_timelineArms->AcknowledgeLogicalComplete(
                token, completionSequence);
        }
    }
    if (slot.render.streamParkActive) {
        if (!slot.render.cascade.runnable &&
            result.phase != SourcePhase::Done) {
            AdvanceStreamParkDeadline(slotIndex, slot, m_blockFrames);
            return;
        }
        if (result.phase == SourcePhase::Done) {
            FinishStreamPark(slotIndex, &slot,
                             StreamParkDisposition::ResumeReady);
            return;
        }
    }
    if (slot.render.streamRing != nullptr &&
        result.phase == SourcePhase::Active) {
        // Cooldown runs every rendered block (starved or not), and the
        // decrement happens BEFORE the emission check, so continuous
        // starvation emits notifications exactly m_starvationCooldownBlocks
        // blocks apart: emit at block N with the cooldown reloaded, reach zero
        // again at block N + cooldown, emit there. Recovery (a full block
        // produced) simply leaves the flag false; a later underrun emits as
        // soon as the cooldown has lapsed.
        if (slot.render.starveCooldownRemaining > 0) {
            --slot.render.starveCooldownRemaining;
        }
        if (slot.render.starvedThisBlock &&
            slot.render.starveCooldownRemaining == 0) {
            ++m_voiceStarved;
            slot.render.starveCooldownRemaining = m_starvationCooldownBlocks;
            notifications.Push(MakeVoiceNotification(
                VN_NOTIFY_VOICE_STARVED,
                MakeHandle(slotIndex,
                           slot.generation.load(std::memory_order_relaxed)),
                blockIndex));
        }
    }
    if (result.phase == SourcePhase::Done) {
        PublishPlaybackEnd(slot, blockIndex);
        if (m_timelineArms != nullptr &&
            slot.render.timelineArmSlot != UINT32_MAX) {
            const TimelineArmTable::Row* row =
                m_timelineArms->RowAt(slot.render.timelineArmSlot);
            const std::uint64_t identity =
                row == nullptr
                    ? 0
                    : row->identity.load(std::memory_order_acquire);
            if (static_cast<std::uint32_t>(identity) ==
                    slot.render.timelineArmSlot &&
                static_cast<std::uint32_t>(identity >> 32) ==
                    slot.render.timelineArmGeneration) {
                const TimelineArmToken token{
                    slot.render.timelineArmSlot,
                    slot.render.timelineArmGeneration, slotIndex,
                    slot.generation.load(std::memory_order_relaxed)};
                (void)m_timelineArms->Tombstone(token, blockIndex);
            }
        }
        // A fade ramp that reached zero reports stopped-by-fade and nothing
        // else; every other completion is a plain ended voice.
        const bool fadedOut =
            slot.render.fading &&
            slot.render.fadeDoneFrames >= slot.render.fadeTotalFrames;
        const VnVoiceHandle handle = MakeHandle(
            slotIndex, slot.generation.load(std::memory_order_relaxed));
        NoteMonitorVoiceRetired(slot.render.dynamicSourceIdentity);
        ReleaseSlot(slot, ReleaseThread::Audio);
        notifications.Push(MakeVoiceNotification(
            fadedOut ? VN_NOTIFY_VOICE_STOPPED_BY_FADE : VN_NOTIFY_VOICE_ENDED,
            handle, blockIndex));
    }
}

void VoicePool::FinishStreamPark(std::uint32_t slotIndex, Slot* slot,
                                 StreamParkDisposition disposition) {
    if (slotIndex >= m_streamParkMailboxes.size()) {
        return;
    }
    StreamParkMailbox& mailbox = m_streamParkMailboxes[slotIndex];
    const StreamParkMailboxState initialState =
        mailbox.state.load(std::memory_order_acquire);
    if (initialState != StreamParkMailboxState::Reserved &&
        initialState != StreamParkMailboxState::Cancelled) {
        return;
    }
    const bool cancelledAtEntry =
        initialState == StreamParkMailboxState::Cancelled;
    if (slot != nullptr) {
        const VnVoiceHandle renderHandle = MakeHandle(
            slotIndex,
            slot->generation.load(std::memory_order_relaxed));
        if (mailbox.outcome.renderHandle != renderHandle ||
            mailbox.outcome.publicHandle !=
                slot->render.streamParkPublicHandle ||
            mailbox.outcome.requestSequence !=
                slot->render.streamParkRequestSequence) {
            return;
        }
        if (disposition == StreamParkDisposition::ResumeReady &&
            !cancelledAtEntry) {
            StreamParkTokenSlot* tokenSlot = nullptr;
            if (mailbox.outcome.token.IsValid()) {
                if (m_streamParkCaptureBytesThisBlock >
                    kStreamParkCaptureBudgetBytes -
                        kStreamParkTokenBytes) {
                    return;
                }
                tokenSlot = ResolveStreamParkToken(
                    mailbox.outcome.token,
                    StreamParkTokenState::TransactionOwned);
                if (tokenSlot == nullptr)
                    return;
            }
            StreamTerminalReason terminal = StreamTerminalReason::None;
            if (!slot->render.streamRing->TryCommitPark(&terminal)) {
                disposition = ParkDispositionForTerminalReason(terminal);
            } else if (tokenSlot != nullptr) {
                StreamParkToken& token = tokenSlot->payload;
                token.channelCount =
                    slot->render.pcm.channelCount;
                token.profile =
                    slot->render.resampler.Profile();
                token.ringGeneration =
                    slot->render.streamRing->generation.load(
                        std::memory_order_relaxed);
                token.activationFrame =
                    slot->render.streamActivationStartFrame;
                token.activationLoopsRemaining =
                    slot->render.streamActivationLoopsRemaining;
                const std::uint64_t audibleAdvance =
                    slot->render.resampler.SourceFrame() >
                            kResampleHistoryPad
                        ? slot->render.resampler.SourceFrame() -
                              kResampleHistoryPad
                        : 0;
                MapLoopedSourceFrame(
                    token.activationFrame,
                    token.activationLoopsRemaining,
                    audibleAdvance,
                    slot->render.streamFrameCount,
                    slot->render.streamLoopStartFrame,
                    slot->render.streamLoopEndFrame,
                    &token.audibleFrame,
                    &token.audibleLoopsRemaining);
                const std::uint64_t feedAdvance =
                    slot->render.streamRing->readIndex.load(
                        std::memory_order_relaxed);
                MapLoopedSourceFrame(
                    slot->render.streamFeedActivationStartFrame,
                    slot->render.streamFeedActivationLoopsRemaining,
                    feedAdvance,
                    slot->render.streamFrameCount,
                    slot->render.streamLoopStartFrame,
                    slot->render.streamLoopEndFrame,
                    &token.feedFrame,
                    &token.feedLoopsRemaining);
                token.gateOpen = slot->render.streamGateOpen;
                if (!slot->render.resampler.CaptureParkState(
                        &token.resampler)) {
                    disposition =
                        StreamParkDisposition::IntegrityAbort;
                } else {
                    token.usedBytes = static_cast<std::uint32_t>(
                        sizeof(StreamParkToken) -
                        sizeof(ResamplerParkState) +
                        token.resampler.usedBytes);
                    mailbox.outcome.sourceFrame = token.feedFrame;
                    mailbox.outcome.loopsRemaining =
                        token.feedLoopsRemaining;
                    m_streamParkCaptureBytesThisBlock +=
                        kStreamParkTokenBytes;
                    m_streamParkCaptureBytesPeak = std::max(
                        m_streamParkCaptureBytesPeak,
                        m_streamParkCaptureBytesThisBlock);
                }
            } else {
                const std::uint64_t advance =
                    slot->render.resampler.SourceFrame() >
                            kResampleHistoryPad
                        ? slot->render.resampler.SourceFrame() -
                              kResampleHistoryPad
                        : 0;
                MapLoopedSourceFrame(
                    slot->render.streamActivationStartFrame,
                    slot->render.streamActivationLoopsRemaining,
                    advance, slot->render.streamFrameCount,
                    slot->render.streamLoopStartFrame,
                    slot->render.streamLoopEndFrame,
                    &mailbox.outcome.sourceFrame,
                    &mailbox.outcome.loopsRemaining);
            }
        }
        m_dynamicRoutes.InvalidateVoiceTap(
            slot->render.dynamicSourceIdentity);
        ReleaseSlot(*slot, ReleaseThread::Audio);
    }
    if (mailbox.outcome.token.IsValid()) {
        StreamParkTokenSlot* token = ResolveStreamParkToken(
            mailbox.outcome.token,
            StreamParkTokenState::TransactionOwned);
        if (token != nullptr) {
            token->state.store(StreamParkTokenState::Published,
                               std::memory_order_release);
        }
    }
    mailbox.outcome.disposition = disposition;
    mailbox.outcome.cancelled = false;
    StreamParkMailboxState expected = StreamParkMailboxState::Reserved;
    if (mailbox.state.compare_exchange_strong(
            expected, StreamParkMailboxState::Published,
            std::memory_order_release, std::memory_order_acquire)) {
        return;
    }
    if (expected == StreamParkMailboxState::Cancelled) {
        mailbox.outcome.cancelled = true;
        mailbox.state.store(StreamParkMailboxState::Published,
                            std::memory_order_release);
    }
}

void VoicePool::AdvanceStreamParkDeadline(std::uint32_t slotIndex, Slot& slot,
                                          std::uint32_t silentFrames) {
    VoiceRenderContext& render = slot.render;
    if (!render.streamParkActive || !render.fading) {
        return;
    }
    const std::uint32_t left =
        render.fadeTotalFrames - render.fadeDoneFrames;
    render.fadeDoneFrames += std::min(left, silentFrames);
    if (render.fadeDoneFrames >= render.fadeTotalFrames) {
        FinishStreamPark(slotIndex, &slot,
                         StreamParkDisposition::ResumeReady);
    }
}

void VoicePool::AdvanceArenaSilentStreamParks(std::uint32_t blockFrames) {
    for (std::uint32_t i = 0; i < m_slots.size(); ++i) {
        Slot& slot = m_slots[i];
        if (!IsAudioRenderingState(slot.state.load(std::memory_order_relaxed)) ||
            !slot.render.streamParkActive) {
            continue;
        }
        AdvanceStreamParkDeadline(i, slot, blockFrames);
    }
}

void VoicePool::GatherSnapshot(SnapshotPayload& out) const {
    std::uint32_t rendering = 0;
    const std::uint32_t reported =
        static_cast<std::uint32_t>(std::min<std::size_t>(
            m_slots.Capacity(), VN_SNAPSHOT_MAX_VOICES));
    for (std::uint32_t i = 0; i < m_slots.size(); ++i) {
        const Slot& slot = m_slots[i];
        // The snapshot describes the RENDERED block, so only Rendering
        // slots contribute: their render context is owned by the audio
            // side (ApplyPlay entered Rendering after the context was fully
            // built; ReleaseSlot leaves it on this same thread), which makes
        // the phase and tap reads race-free here. A Claimed slot still
        // belongs to the logic thread mid-play and rendered nothing this
        // block — it stays INACTIVE until its first block renders. The
        // acquire pairs with the play command's publication so a
        // Rendering observation also observes the initialized context.
        const bool isRendering =
            IsAudioRenderingState(slot.state.load(std::memory_order_acquire));
        if (isRendering) {
            ++rendering;
        }
        std::uint32_t observedState = VN_VOICE_STATE_INACTIVE;
        if (isRendering) {
            if (slot.render.phase == SourcePhase::Active) {
                observedState = slot.render.streamRing != nullptr && !slot.render.streamGateOpen
                    ? VN_VOICE_STATE_PREPARING : VN_VOICE_STATE_ACTIVE;
            } else if (slot.render.phase == SourcePhase::Draining) {
                observedState = VN_VOICE_STATE_DRAINING;
            }
        }
        const auto playbackState = isRendering && slot.render.timelineGated
            ? VN_VOICE_STATE_PREPARING : observedState;
        slot.renderedState.store(
            (static_cast<std::uint64_t>(slot.generation.load(std::memory_order_relaxed)) << 32)
                | playbackState, std::memory_order_release);
        if (i >= reported) {
            continue;
        }
        VnVoiceSnapshot& entry = out.voices[i];
        entry.handle = i;
        if (!isRendering) {
            entry.state = VN_VOICE_STATE_INACTIVE;
            entry.tapPostResample = 0.0f;
            entry.tapPostFx = 0.0f;
            entry.tapPostFilter = 0.0f;
            entry.tapPostGain = 0.0f;
            continue;
        }
        entry.state = observedState;
        entry.tapPostResample = slot.render.tapPostResample;
        entry.tapPostFx = slot.render.tapPostFx;
        entry.tapPostFilter = slot.render.tapPostFilter;
        entry.tapPostGain = slot.render.tapPostGain;
    }
    for (std::uint32_t i = m_slots.ConstructedCount(); i < reported; ++i) {
        out.voices[i] = VnVoiceSnapshot{};
        out.voices[i].handle = i;
        out.voices[i].state = VN_VOICE_STATE_INACTIVE;
    }
    for (std::uint32_t i = reported; i < VN_SNAPSHOT_MAX_VOICES; ++i) {
        out.voices[i] = VnVoiceSnapshot{};
    }
    // Rendering slots only: the count matches the block this payload
    // describes, and pending (Claimed) voices become visible exactly when
    // they first render.
    out.activeVoiceCount = rendering;
    // Audio-thread-owned running total; the payload carries it so the
    // logic-side capture never reads pool internals across threads.
    out.voiceStarvedCount = m_voiceStarved;
}

std::uint32_t VoicePool::RenderedState(VnVoiceHandle handle) const noexcept {
    const auto index = static_cast<std::uint32_t>(handle);
    if (handle == 0 || index >= m_slots.size()) return VN_VOICE_STATE_INACTIVE;
    const auto observed = m_slots[index].renderedState.load(std::memory_order_acquire);
    return (observed >> 32) == (handle >> 32)
        ? static_cast<std::uint32_t>(observed) : VN_VOICE_STATE_INACTIVE;
}

bool VoicePool::CapturePreparedRoutingV1(VnVoiceHandle handle,
                                         PreparedRoutingV1* out) const noexcept {
    const auto index = static_cast<std::uint32_t>(handle);
    const auto generation = static_cast<std::uint32_t>(handle >> 32);
    if (out == nullptr || handle == 0 || index >= m_slots.size()) return false;
    const auto& slot = m_slots[index];
    const auto state = slot.state.load(std::memory_order_acquire);
    if ((state != SlotState::Prepared && state != SlotState::PreparedCommitHeld) ||
        slot.generation.load(std::memory_order_acquire) != generation) return false;
    PreparedRoutingV1 captured{};
    captured.busIndex = slot.render.busIndex;
    captured.busGeneration = slot.render.busGeneration;
    std::uint32_t count = 0;
    for (std::uint32_t i = 0; i < kMaxVoiceSendSlots; ++i) {
        const auto& send = slot.render.voiceSends[i];
        captured.sends[i] = {send.active, send.targetBusIndex, send.targetBusGeneration};
        count += send.active ? 1u : 0u;
    }
    if (count != slot.render.voiceSendActiveCount) return false;
    *out = captured;
    return true;
}

bool VoicePool::CanCommitPreparedRoutingMigrationV1(VnVoiceHandle handle,
    const PreparedRoutingV1& expected, const PreparedRoutingV1& next) const noexcept {
    const auto index = static_cast<std::uint32_t>(handle);
    if (handle == 0 || index >= m_slots.size() ||
        m_slots[index].state.load(std::memory_order_acquire) != SlotState::Prepared) return false;
    PreparedRoutingV1 current{};
    if (!CapturePreparedRoutingV1(handle, &current) || !(current == expected) ||
        next.busIndex == UINT32_MAX || next.busGeneration == 0) return false;
    for (std::uint32_t i = 0; i < kMaxVoiceSendSlots; ++i) {
        if (next.sends[i].active || !expected.sends[i].active) {
            if (!(next.sends[i] == expected.sends[i])) return false;
        } else if (next.sends[i].busIndex != 0 || next.sends[i].busGeneration != 0) return false;
    }
    return true;
}

void VoicePool::CommitPreparedRoutingMigrationV1(VnVoiceHandle handle,
    const PreparedRoutingV1& expected, const PreparedRoutingV1& next) noexcept {
    if (!CanCommitPreparedRoutingMigrationV1(handle, expected, next)) return;
    assert(CanCommitPreparedRoutingMigrationV1(handle, expected, next));
    auto& render = m_slots[static_cast<std::uint32_t>(handle)].render;
    render.busIndex = next.busIndex;
    render.busGeneration = next.busGeneration;
    for (std::uint32_t i = 0; i < kMaxVoiceSendSlots; ++i) {
        if (expected.sends[i].active && !next.sends[i].active) {
            assert(render.voiceSendActiveCount != 0);
            render.voiceSends[i] = {};
            --render.voiceSendActiveCount;
        }
    }
}

VoiceRenderContext* VoicePool::Resolve(VnVoiceHandle handle) {
    Slot* slot = SlotFor(handle);
    return slot != nullptr ? &slot->render : nullptr;
}

std::uint32_t VoicePool::ActiveCount() const {
    std::uint32_t count = 0;
    for (const Slot& slot : m_slots) {
        // Relaxed: this is an observational counter (snapshot/high-water
        // accounting), not a synchronization point.
        if (slot.state.load(std::memory_order_relaxed) != SlotState::Free) {
            ++count;
        }
    }
    return count;
}

VoicePool::Slot* VoicePool::SlotFor(VnVoiceHandle handle) {
    // Out-of-range slot, freed slot, and generation mismatch are treated
    // identically: the handle is stale. Callers on the logic thread get
    // an ADVISORY answer — the voice can end on the audio thread right
    // after this returns; the acquire loads only guarantee a coherent
    // reading of the two liveness fields, and the check-then-act window
    // is closed by the audio side treating dead handles as no-ops.
    const std::uint32_t slotIndex = static_cast<std::uint32_t>(handle);
    const std::uint32_t generation =
        static_cast<std::uint32_t>(handle >> 32);
    if (slotIndex >= m_slots.size()) {
        return nullptr;
    }
    Slot& slot = m_slots[slotIndex];
    const SlotState state = slot.state.load(std::memory_order_acquire);
    // A quarantined slot is as unreachable as a freed one, and for a stronger
    // reason: its render context may have a writer this thread cannot see, so
    // resolving a handle to it would hand that context to a caller. The
    // generation bump at quarantine already makes every outstanding handle
    // mismatch; this keeps that true even across a generation wrap.
    if (state == SlotState::Free || state == SlotState::Quarantined ||
        state == SlotState::PreparedCommitHeld ||
        state == SlotState::CommandInstallHeld || state == SlotState::ReleaseHeld ||
        slot.generation.load(std::memory_order_acquire) != generation) {
        return nullptr;
    }
    return &slot;
}

VoicePool::Slot* VoicePool::SlotForCommandMetadata(VnVoiceHandle handle) {
    const auto* proof = m_commandMetadataProof;
    if (proof != nullptr && proof->Valid() && proof->pool_ == this && proof->begun_ &&
        proof->source_->Valid()) {
        const auto token = proof->source_->Token();
        if (static_cast<std::uint32_t>(handle) == token.renderSlot &&
            static_cast<std::uint32_t>(handle >> 32) == token.renderGeneration &&
            proof->slot_->state.load(std::memory_order_acquire) == SlotState::CommandInstallHeld)
            return proof->slot_;
    }
    return SlotFor(handle);
}

void VoicePool::NoteMonitorVoiceRetired(VnVoiceHandle publicHandle) noexcept {
    if (publicHandle == 0 || !harken::monitor::AudioLiveArmed()) {
        return;
    }
    const std::uint64_t publicGeneration =
        static_cast<std::uint64_t>(static_cast<std::uint32_t>(publicHandle >> 32));
    harken::monitor::NoteExactInstanceRetired(
        VN_MONITOR_TAP_VOICE_PRE_CHAIN, publicHandle, publicGeneration);
    harken::monitor::NoteExactInstanceRetired(
        VN_MONITOR_TAP_VOICE_POST_CHAIN, publicHandle, publicGeneration);
}

void VoicePool::ReleaseSlot(Slot& slot, ReleaseThread thread) {
    ClearFirstOutput(slot);
    const bool nativeFinished = FinishNativeVariableStart(slot, false);
    assert(nativeFinished);
    (void)nativeFinished;
    if (slot.state.load(std::memory_order_acquire) == SlotState::ReleaseHeld) {
        if (const auto hook = m_releaseHeldHookForTest.exchange(nullptr, std::memory_order_acq_rel))
            hook(m_releaseHeldContextForTest.load(std::memory_order_relaxed));
    }
    // Bypass paths have an explicit cancellation result, never inferred EOF.
    if (slot.render.sourceTerminal != nullptr &&
        slot.render.sourceTerminal->completedSequence.load(std::memory_order_relaxed) == 0) {
        slot.render.sourceTerminal->bypassResult.store(
            slot.render.streamParkActive ? SourceTerminalKind::Parked : SourceTerminalKind::Cancelled,
            std::memory_order_release);
    }
    slot.render.sourceTerminal = nullptr;
    slot.render.sourceTerminalToken = {};
    slot.monitorPublicHandle.store(0, std::memory_order_relaxed);
    if (slot.render.pcm.useCount != nullptr) {
        slot.render.pcm.useCount->fetch_sub(1, std::memory_order_release);
    }
    if (slot.render.streamRing != nullptr) {
        // Give the stream slot back without touching any logic-side
        // structure: Retired is the audio thread's entire contact surface.
        // The release store publishes this thread's final ring reads (the
        // consumed read index) to the streaming thread, which detaches the
        // slot and returns it to Free.
        slot.render.streamRing->state.store(StreamSlotState::Retired,
                                            std::memory_order_release);
        slot.render.streamRing = nullptr;
    }
    slot.streamRingToken.store(0, std::memory_order_release);
    // Retire every effect the voice still holds so the logic side reclaims the
    // pooled instances — otherwise a voice ending with effects installed would
    // leak them (the audio thread never frees). Cleared slots already read as
    // empty here, so only live installs are retired.
    for (std::uint32_t s = 0; s < slot.render.fxSlotCount; ++s) {
        if (slot.render.fxSlotEffectRef[s].type != kEffectTypeNone) {
            if (thread == ReleaseThread::Audio) {
                m_effectBank.RetireQueue().TryPush(slot.render.fxSlotEffectRef[s]);
            } else {
                // ReleaseHeld excludes readers of this slot. Logic must not
                // become a second producer of the audio retirement ring.
                m_effectBank.Recycle(slot.render.fxSlotEffectRef[s]);
            }
            slot.render.fxSlotEffectRef[s] = EffectInstanceRef{};
        }
    }
    // Generation bumps first, then Free publishes with release. A logic-
    // side reader between the two stores sees a generation mismatch;
    // after both, a Free slot — either way the handle classifies as
    // stale, so the store order is benign for liveness. The release on
    // the state store is what hands this slot's render memory back to
    // the logic thread (paired with the acquire in the free-slot scan).
    std::uint32_t next =
        slot.generation.load(std::memory_order_relaxed) + 1;
    if (next == 0) {
        // Generation 0 would let slot 0 mint handle 0; skipping it keeps
        // "handle 0 is never valid" structural across wraparound.
        next = 1;
    }
    PublishRelease(slot);
    slot.generation.store(next, std::memory_order_relaxed);
    CompleteOriginalSourceBodyDisposition(slot);
    slot.state.store(SlotState::Free, std::memory_order_release);
    m_completed.fetch_add(1, std::memory_order_relaxed);
}

} // namespace harken
