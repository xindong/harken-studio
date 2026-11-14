#include "harken/render/SignalRoutingTypes.h"
#include "harken/render/SignalPathBounds.h"
#include "harken/render/SignalRenderPlan.h"
#include "harken/render/BusTree.h"
#include "harken/render/BusActivityPlan.h"

#include "MonitorCaptureHub.h"

#include "harken/core/ChannelLayout.h"
#include "harken/core/SnapshotBuffer.h"
#include "harken/dsp/DownmixSum.h"
#include "harken/dsp/BiquadDormancy.h"
#include "harken/render/BuiltinEffects.h"
#include "harken/render/EffectChain.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <limits>

namespace harken {
std::uint32_t BusTree::RetainedSendCount() const noexcept {
    std::uint32_t result = 0;
    for (std::uint32_t i = 0; i < m_plans.Capacity(); ++i)
        if (m_plans.IsAcquired(i)) result = std::max(result, m_plans.View(i).sendCount);
    return result;
}

std::uint64_t BusTree::RetainedSignalScratchBytes(std::uint32_t dynamicCapacity,
    std::uint64_t extraEdges, std::uint32_t frames) const noexcept {
    std::uint64_t result = 0;
    const auto include = [&](const SignalRenderPlan* signals) {
        if (signals == nullptr || signals->NodeCount() == 0) return;
        const auto buses = signals->BusNodeCount();
        const auto extra = dynamicCapacity == 0 ? 0ull :
            static_cast<std::uint64_t>(buses) * (m_fxSlots + 2ull);
        const auto nodes = extra + signals->NodeCount();
        const auto edges = extra + buses + dynamicCapacity + extraEdges + signals->RelationCount();
        if (nodes > UINT32_MAX || edges > UINT32_MAX) { result = UINT64_MAX; return; }
        result = std::max(result, SignalRenderPlan::RoutingScratchBytes(
            static_cast<std::uint32_t>(nodes), buses, edges, frames));
    };
    for (const auto& activity : m_activities) include(activity->signals.get());
    // Direct offline fixtures can render installed topology without activities.
    if (m_activities.empty() && !HasAcceptedActivity())
        for (std::uint32_t i = 0; i < m_plans.Capacity(); ++i)
            if (m_plans.IsAcquired(i)) include(m_plans.View(i).signals);
    return result;
}

std::span<float*> BusTree::AudioPlanes() noexcept {
    return m_activeActivity != nullptr ? std::span<float*>(m_activeActivity->planes)
                                       : std::span<float*>(m_directPlanes);
}

BusTree::BusTree(const EngineConfig& config, TopologyPlanPool& plans,
                 EffectPoolBank& effects, DynamicRouteSystem& dynamicRoutes,
                 const BiquadTrigTable* biquadTrig)
    : m_plans(plans),
      m_effectBank(effects),
      m_dynamicRoutes(dynamicRoutes),
      m_logicParent(config.maxBuses, 0),
      m_logicAlive(config.maxBuses, 0),
      m_logicId(config.maxBuses),
      m_logicGeneration(config.maxBuses, 0),
      m_logicQuarantined(config.maxBuses, 0),
      m_logicRetireRevision(config.maxBuses, 0),
      m_capacity(config.maxBuses),
      m_blockFrames(config.blockFrames),
      m_sampleRate(config.mixRate.SampleRate()),
      m_fxSlots(config.fxSlotsPerBus),
      m_mixChannels(config.mixChannelCount),
      m_mixLayout(config.mixChannelLayout),
      m_deviceLayout(config.deviceChannelLayout),
      // Bus effect chains, the per-bus fold, and the send scatter all operate
      // at the mix width, so the dry scratch is sized to it (stereo at the
      // default).
      m_fxDryStorage(static_cast<std::size_t>(config.mixChannelCount) *
                     config.blockFrames),
      m_fxDry(config.mixChannelCount),
      m_outputScratchStorage(static_cast<std::size_t>(config.mixChannelCount) *
                             config.blockFrames),
      m_outputScratch(config.mixChannelCount),
      m_biquadTrig(biquadTrig) {
    if (!m_workSlots.ConstructThrough(m_capacity) ||
        !m_previewEffectStates.ConstructThrough(m_capacity)) throw std::bad_alloc{};
    m_directPlanes.resize(m_mixChannels);
    // Only Master owns an initial work row. Definitions acquire private rows
    // through activity preparation when actual work requires them.
    if (!m_workRows.ConstructThrough(1)) throw std::bad_alloc{};
    m_freeWorkRows.reserve(1);
    auto& master = m_workRows[0];
    master.slot = 0;
    master.generation = 1;
    master.references.store(1, std::memory_order_relaxed);
    master.phase.store(WorkPhase::Published, std::memory_order_relaxed);
    m_workSlots[0] = {0, 1};
    m_workRowsInUse = 1;
    // The master bus pre-exists at index 0 and is its own parent (the fold
    // never dereferences the master's parent). Its chain defaults to all
    // pre-fader, like every created bus.
    WorkState(0).id = VN_BUS_MASTER;
    WorkState(0).generation = 1;
    WorkState(0).parentIndex = 0;
    BindInitialWorkIdentity(0);
    m_logicParent[0] = 0;
    m_logicAlive[0] = 1;
    m_logicId[0] = VN_BUS_MASTER;
    m_logicGeneration[0] = 1;
    WorkState(0).faderAfterSlot = m_fxSlots;
    // The master's output fold is to the device layout, applied by the mixer at
    // the device boundary. Seed its default matrix (the Appendix A fold for the
    // resolved mix->device pair) so the pre-plan render — before any structural
    // change publishes a plan carrying the authoritative fold config — folds
    // correctly. At the stereo default mix == device, so this is the identity
    // and no fold runs.
    WorkState(0).outputLayout = m_deviceLayout;
    DefaultDownmixMatrix(m_mixLayout, m_deviceLayout, WorkState(0).downmixCoeffs);
    m_count = 1;
    m_activeCount = 1;
    // The master is audible from the first block; every later bus becomes
    // audible only when its plan swap applies (see ApplyTopologyPlan).
    m_audioCount = 1;
    // Per-channel base pointers into the preallocated dry scratch.
    for (std::uint32_t ch = 0; ch < m_mixChannels; ++ch) {
        m_fxDry[ch] = m_fxDryStorage.data() +
                      static_cast<std::size_t>(ch) * config.blockFrames;
        m_outputScratch[ch] =
            m_outputScratchStorage.data() +
            static_cast<std::size_t>(ch) * config.blockFrames;
    }
}

bool BusTree::PrepareDefinitionCapacity(std::uint32_t count) noexcept {
    if (count <= m_capacity) return true;
    try {
        const auto grow = [count](auto& rows) { rows.reserve(count); rows.resize(count); };
        grow(m_logicParent); grow(m_logicAlive); grow(m_logicId);
        grow(m_logicGeneration); grow(m_logicQuarantined); grow(m_logicRetireRevision);
        if (!m_workSlots.ConstructThrough(count) || !m_previewEffectStates.ConstructThrough(count))
            return false;
        m_capacity = count;
        return true;
    } catch (...) { return false; }
}

void BusTree::InitializeBusSlot(std::uint32_t busIndex, VnBusId id,
                                std::uint32_t parentIndex,
                                std::uint32_t generation) noexcept {
    assert(busIndex < m_workSlots.ConstructedCount());
    Bus& bus = WorkState(busIndex);
    bus = Bus{};
    bus.id = id;
    bus.busIndex = busIndex;
    bus.generation = generation;
    bus.parentIndex = parentIndex;
    bus.faderAfterSlot = m_fxSlots;
    bus.outputLayout = m_mixLayout;
    DefaultDownmixMatrix(m_mixLayout, m_mixLayout, bus.downmixCoeffs);
}

void BusTree::RetireBusSlotEffects(Bus& bus) noexcept {
    ReleasePreviewEffectChain(bus);
    for (std::uint32_t slot = 0; slot < m_fxSlots; ++slot) {
        if (bus.fxSlotEffectRef[slot].type != kEffectTypeNone) {
            const bool retired = m_effectBank.Retire(bus.fxSlotEffectRef[slot]);
            assert(retired && "effect retirement lane must hold every assigned row");
            (void)retired;
            bus.fxSlots[slot] = InsertEffect{};
            bus.fxSlotEffectRef[slot] = EffectInstanceRef{};
        }
    }
}

void BusTree::CommitLogicSlot(std::uint32_t busIndex, VnBusId id,
                              std::uint32_t parentIndex,
                              std::uint32_t generation) noexcept {
    const bool valid =
        busIndex < m_capacity && id != VnObjectId{} && generation != 0 &&
        parentIndex < m_capacity &&
        (busIndex >= m_count ||
         (m_logicAlive[busIndex] == 0 && !QuarantinedAt(busIndex)));
    assert(valid && "prepared Bus logic slot must be prevalidated");
    if (!valid) {
        return;
    }
    const bool reused =
        busIndex < m_count && m_logicGeneration[busIndex] != 0;
    m_logicId[busIndex] = id;
    m_logicParent[busIndex] = parentIndex;
    m_logicGeneration[busIndex] = generation;
    m_logicAlive[busIndex] = 1;
    m_logicQuarantined[busIndex] = 0;
    ++m_activeCount;
    if (reused) {
        ++m_slotReuseCount;
    }
    if (busIndex >= m_count) {
        m_count = busIndex + 1;
    }
}

VnResult BusTree::CreateBus(VnBusId id, VnBusId parentId) {
    if (id == VnObjectId{}) {
        // Zero stays out of the id space so zero-initialized descriptors
        // fail identity checks instead of silently routing.
        return VN_INVALID_ARG;
    }
    if (FindIndex(id) != kInvalidBusIndex) {
        return VN_IN_USE;
    }
    const std::uint32_t parentIndex = FindIndex(parentId);
    if (parentIndex == kInvalidBusIndex) {
        return VN_UNKNOWN_ID;
    }
    std::uint32_t busIndex = kInvalidBusIndex;
    std::uint32_t generation = 0;
    for (std::uint32_t i = 1; i < m_count; ++i) {
        if (m_logicAlive[i] != 0 || QuarantinedAt(i)) {
            continue;
        }
        generation = NextGenerationForSlot(i);
        if (generation != 0) {
            busIndex = i;
            break;
        }
        m_logicQuarantined[i] = 1;
        ++m_quarantinedCount;
    }
    if (busIndex == kInvalidBusIndex) {
        if (m_count >= m_capacity) {
            return VN_NO_BUS;
        }
        m_directPlanes.resize(static_cast<std::size_t>(m_count + 1) * m_mixChannels);
        busIndex = m_count;
        generation = 1;
        // The direct construction seam prepares only the requested hidden row.
        const auto work = PrepareWorkRow();
        if (!work.Valid()) return VN_DEVICE_ERROR;
        m_workSlots[busIndex] = work;
        InitializeBusSlot(busIndex, id, parentIndex, generation);
        if (!PublishWorkRow(work)) std::terminate();
    }
    CommitLogicSlot(busIndex, id, parentIndex, generation);
    return VN_OK;
}

BusTree::Bus* BusTree::BusState(std::uint32_t busIndex) {
    return busIndex < m_count && m_workSlots[busIndex].Valid() ? &WorkState(busIndex) : nullptr;
}

std::uint32_t BusTree::FindIndex(VnBusId id) const {
    for (std::uint32_t i = 0; i < m_count; ++i) {
        if (m_logicId[i] == id && m_logicAlive[i] != 0) {
            return i;
        }
    }
    return kInvalidBusIndex;
}

BusSlotRef BusTree::FindRef(VnBusId id) const {
    const std::uint32_t index = FindIndex(id);
    return index == kInvalidBusIndex ? BusSlotRef{}
                                     : BusSlotRef{index,
                                                  m_logicGeneration[index]};
}

BusSlotRef BusTree::RefAt(std::uint32_t busIndex) const {
    return busIndex < m_count && m_logicAlive[busIndex] != 0
               ? BusSlotRef{busIndex, m_logicGeneration[busIndex]}
               : BusSlotRef{};
}

std::uint32_t BusTree::GenerationAt(std::uint32_t busIndex) const {
    return busIndex < m_count ? m_logicGeneration[busIndex] : 0;
}

std::uint32_t BusTree::NextGenerationForSlot(
    std::uint32_t busIndex) const {
    if (busIndex >= m_capacity || QuarantinedAt(busIndex)) {
        return 0;
    }
    const std::uint32_t current = m_logicGeneration[busIndex];
    if (current == 0) {
        return 1;
    }
    return current == (std::numeric_limits<std::uint32_t>::max)()
               ? 0
               : current + 1;
}

bool BusTree::LogicRefMatches(BusSlotRef ref) const {
    return ref.Valid() && ref.index < m_count &&
           m_logicAlive[ref.index] != 0 &&
           m_logicGeneration[ref.index] == ref.generation;
}

bool BusTree::AudioRefMatches(BusSlotRef ref) const {
    if (!ref.Valid() || ref.index >= m_audioCount ||
        AudioGenerationAt(ref.index) != ref.generation) {
        return false;
    }
    const std::uint8_t* alive = InstalledAlive();
    return alive == nullptr ? ref.index == 0 : alive[ref.index] != 0;
}

std::uint32_t BusTree::AudioGenerationAt(std::uint32_t index) const {
    if (index >= m_audioCount) return 0;
    const auto plan = m_plans.View(m_currentPlanIndex);
    if (plan.slotCount != 0 && plan.generations != nullptr && plan.generations[index] != 0)
        return plan.generations[index];
    return HasWorkRow(index) ? WorkState(index).generation : 0;
}

std::uint32_t BusTree::FreeBusSlotCount() const {
    std::uint32_t free = m_capacity - m_count;
    for (std::uint32_t i = 1; i < m_count; ++i) {
        if (m_logicAlive[i] == 0 && !QuarantinedAt(i) && NextGenerationForSlot(i) != 0) {
            ++free;
        }
    }
    return free;
}

bool BusTree::HoldSlotReuse(BusSlotRef ref, std::uint64_t revision) noexcept {
    if (!ref.Valid() || ref.index >= m_count ||
        m_logicGeneration[ref.index] != ref.generation ||
        revision == 0 || revision <= InstalledGraphRevision()) {
        return false;
    }
    m_logicRetireRevision[ref.index] = std::max(m_logicRetireRevision[ref.index], revision);
    return true;
}

bool BusTree::HoldRetiredSlot(BusSlotRef ref, std::uint64_t revision) noexcept {
    if (ref.index == 0 || !HoldSlotReuse(ref, revision)) {
        return false;
    }
    MarkLogicDead(ref.index);
    return true;
}

bool BusTree::ReleaseRetiredSlot(BusSlotRef ref, std::uint64_t revision) noexcept {
    if (!ref.Valid() || ref.index >= m_count ||
        m_logicGeneration[ref.index] != ref.generation || revision == 0 ||
        m_logicRetireRevision[ref.index] != revision || InstalledGraphRevision() < revision) {
        return false;
    }
    m_logicRetireRevision[ref.index] = 0;
    return true;
}

const std::uint8_t* BusTree::InstalledAlive() const {
    const TopologyPlanView plan = m_plans.View(m_currentPlanIndex);
    return plan.valid && plan.slotCount != 0 ? plan.alive : nullptr;
}

const std::uint32_t* BusTree::InstalledParents() const {
    const TopologyPlanView plan = m_plans.View(m_currentPlanIndex);
    return plan.valid && plan.slotCount != 0 ? plan.parents : nullptr;
}

const std::uint32_t* BusTree::InstalledGenerations() const {
    const TopologyPlanView plan = m_plans.View(m_currentPlanIndex);
    return plan.valid && plan.slotCount != 0 ? plan.generations : nullptr;
}

const std::uint32_t* BusTree::InstalledMigrationTargets() const {
    const TopologyPlanView plan = m_plans.View(m_currentPlanIndex);
    return plan.valid && plan.slotCount != 0 ? plan.migrationTargets
                                             : nullptr;
}

void BusTree::ArmRamp(GainRamp& gain, bool& ramping, float& rampStartGain,
                      float& rampEndGain, std::uint64_t& rampBlocksTotal,
                      std::uint64_t& rampBlocksDone, float targetLinear,
                      std::uint32_t rampMs) {
    if (!gain.started || rampMs == 0) {
        // First-ever value snaps (no fictitious prior value to ramp from);
        // rampMs == 0 snaps at this block boundary by request.
        gain.SnapTo(targetLinear);
        ramping = false;
        return;
    }
    // Frames round up so a short positive ramp is never zero-length, then
    // blocks round up so the ramp completes exactly on a block boundary.
    const std::uint64_t rampFrames =
        (static_cast<std::uint64_t>(rampMs) * m_sampleRate + 999) / 1000;
    std::uint64_t blocks = (rampFrames + m_blockFrames - 1) / m_blockFrames;
    if (blocks == 0) {
        blocks = 1;
    }
    // A re-set during an active ramp restarts from the value in effect at
    // this block boundary: deterministic, no queued targets.
    ramping = true;
    rampStartGain = gain.current;
    rampEndGain = targetLinear;
    rampBlocksTotal = blocks;
    rampBlocksDone = 0;
}

namespace {

enum class BusToneKind : std::uint8_t { Lowpass, Highpass };

double ProcessBusToneSection(BusTree::Bus::ToneSection& section,
                           BusToneKind kind, const BiquadTrigTable* trig,
                           std::uint32_t sampleRate, float* const* channels,
                           std::uint32_t channelCount, std::uint32_t frames,
                           std::span<SignalBiquadSegment> prepare = {}) {
    if (!section.enabled && !section.transition.transitioning && !section.glide.active) {
        section.signalSchedule = {};
        return 1;
    }
    if (section.transition.transitioning || !section.glide.active || trig == nullptr) {
        const auto bound = section.signalBound.Advance(section.enabled, section.coeffs,
            section.transition, frames);
        section.signalHighWater = std::max(section.signalHighWater, bound);
        BiquadProcessSmoothed(section.transition, section.enabled, section.coeffs,
            section.state, section.tailFrames, channels, channelCount, 0, frames);
        section.signalSchedule = {};
        return bound;
    }
    constexpr std::uint32_t kControlFrames = 16;
    std::uint32_t consumed = 0, segment = 0;
    double bound = 1;
    while (consumed < frames) {
        if (!section.glide.active) {
            // A glide can finish between control ticks. Its progress then
            // stops, so consume the fixed-coefficient remainder once.
            const auto remaining = frames - consumed;
            bound = std::max(bound, section.signalBound.Advance(section.enabled,
                section.coeffs, section.transition, remaining));
            BiquadProcessSmoothed(section.transition, section.enabled, section.coeffs,
                section.state, section.tailFrames, channels, channelCount, consumed, remaining);
            break;
        }
        const std::uint32_t controlOffset = section.glide.progress % kControlFrames;
        const std::uint32_t count = std::min(kControlFrames - controlOffset, frames - consumed);
        bound = std::max(bound, section.signalBound.Advance(section.enabled,
            section.coeffs, section.transition, count, true));
        BiquadProcessSmoothed(section.transition, section.enabled, section.coeffs,
            section.state, section.tailFrames, channels, channelCount, consumed, count);
        consumed += count;
        section.glide.Advance(count);
        if (!section.signalSchedule.empty()) {
            // The certificate and audio consume exactly the same prepared tuples.
            assert(segment < section.signalSchedule.size());
            const auto& next = section.signalSchedule[segment];
            assert(next.frames == count);
            section.coeffs = next.nextCoefficients;
            section.tailFrames = next.nextTailFrames;
        } else {
            const auto point = trig->Lookup(static_cast<float>(sampleRate), section.glide.FrequencyHz());
            const auto candidate = kind == BusToneKind::Lowpass
                ? MakeLowpassFromTrig(point, section.glide.Q()) : MakeHighpassFromTrig(point, section.glide.Q());
            if (IsBiquadStable(candidate)) {
                section.coeffs = candidate;
                section.tailFrames = EstimateBiquadTailFrames(candidate);
            }
            if (!section.glide.active && IsBiquadStable(section.transition.targetCoeffs)) {
                section.coeffs = section.transition.targetCoeffs;
                section.tailFrames = section.transition.targetTailFrames;
            }
        }
        if (!prepare.empty()) {
            assert(segment < prepare.size());
            prepare[segment] = {section.coeffs, section.tailFrames, count};
        }
        ++segment;
    }
    section.signalSchedule = {};
    section.signalHighWater = std::max(section.signalHighWater, bound);
    return bound;
}

bool ToneSectionActive(const BusTree::Bus::ToneSection& section) {
    return section.enabled || section.transition.transitioning ||
           section.glide.active;
}

} // namespace

bool BusTree::CaptureDormantToneSection(const Bus::ToneSection& section,
    std::uint32_t channels, DormantToneSection& out) noexcept {
    if (channels == 0 || channels > kMaxChannels || section.transition.transitioning ||
        section.transition.pendingValid || section.glide.active ||
        !IsBiquadStable(section.coeffs) || std::isnan(section.signalHighWater) ||
        !std::isfinite(section.glide.currentLog2Hz) ||
        !std::isfinite(section.glide.currentLog2Q) ||
        !std::isfinite(section.glide.currentGainDb)) return false;
    DormantToneSection next;
    if (section.enabled) {
        if (!CaptureBiquadZeroState(section.state, section.transition.lastInput, next.zeroSigns)) return false;
        auto trial = section;
        float zero[kMaxChannels]{};
        float* planes[kMaxChannels];
        for (std::uint32_t ch = 0; ch < channels; ++ch) planes[ch] = &zero[ch];
        // Kind/rate are unused in the settled path; use the actual processing
        // seam so both filter state and feedback certificates must be stable.
        ProcessBusToneSection(trial, BusToneKind::Lowpass, nullptr, 0, planes, channels, 1);
        std::uint32_t afterSigns = 0;
        if (!CaptureBiquadZeroState(trial.state, trial.transition.lastInput, afterSigns) ||
            next.zeroSigns != afterSigns ||
            !section.signalBound.CaptureSettled().SameState(trial.signalBound.CaptureSettled()) ||
            std::bit_cast<std::uint64_t>(section.signalHighWater) !=
                std::bit_cast<std::uint64_t>(trial.signalHighWater)) return false;
        for (std::uint32_t ch = 0; ch < channels; ++ch)
            if (std::bit_cast<std::uint32_t>(zero[ch]) != 0) return false;
    }
    // Disabled sections freeze lastInput. The next enable seeds its new
    // branch from it, while old inactive z history is never read again.
    std::copy_n(section.transition.lastInput, kMaxChannels, next.lastInput);
    next.enabled = section.enabled;
    next.coefficients = section.coeffs;
    next.tailFrames = section.tailFrames;
    next.log2Hz = section.glide.currentLog2Hz;
    next.log2Q = section.glide.currentLog2Q;
    next.gainDb = section.glide.currentGainDb;
    next.signalBound = section.signalBound.CaptureSettled();
    next.signalHighWater = section.signalHighWater;
    out = next;
    return true;
}

void BusTree::RestoreDormantToneSection(Bus::ToneSection& section,
    const DormantToneSection& saved) noexcept {
    section = {};
    section.enabled = saved.enabled;
    section.coeffs = saved.coefficients;
    section.tailFrames = saved.tailFrames;
    section.glide.currentLog2Hz = saved.log2Hz;
    section.glide.currentLog2Q = saved.log2Q;
    section.glide.currentGainDb = saved.gainDb;
    section.signalBound.RestoreSettled(saved.signalBound);
    section.signalHighWater = saved.signalHighWater;
    if (saved.enabled) RestoreBiquadZeroState(section.state, section.transition.lastInput, saved.zeroSigns);
    else std::copy_n(saved.lastInput, kMaxChannels, section.transition.lastInput);
}

void BusTree::ApplySetBusFilter(std::uint32_t busIndex,
                                std::uint32_t filterKind, bool enable,
                                float cutoffHz, float q,
                                const BiquadCoeffs& coeffs,
                                std::uint32_t tailFrames,
                                std::uint32_t transitionFrames, bool outputOnly,
                                std::uint32_t busGeneration) {
    if (!HasWorkRow(busIndex) ||
        (busGeneration != 0 &&
         !AudioRefMatches({busIndex, busGeneration}))) {
        return;
    }
    const bool lowpass = filterKind == kFilterKindLowpass;
    if (!lowpass && filterKind != kFilterKindHighpass) {
        return;
    }
    Bus& bus = WorkState(busIndex);
    Bus::ToneSection& section =
        outputOnly ? (lowpass ? bus.outputLpf : bus.outputHpf)
                   : (lowpass ? bus.busLpf : bus.busHpf);
    NoteWorkMutation(bus);
    // Buses are already mixing: Master from init, others after Create/Finalize.
    // Default bypass is a live section. First enable/retune/disable — including
    // rampMs==0 — follows Voice post-firstBlock. Zero-length ramps fall back to
    // BiquadSmoothingFrames; bypass→enable is a dry→wet RequestBiquadTransition.
    // Do not treat "no prior filter command" as Voice firstBlock.
    const std::uint32_t effectiveFrames =
        transitionFrames == 0 ? BiquadSmoothingFrames(m_sampleRate)
                              : transitionFrames;
    if (transitionFrames != 0 && !section.transition.transitioning &&
        section.enabled && enable) {
        section.transition.targetCoeffs = coeffs;
        section.transition.targetTailFrames = tailFrames;
        section.glide.Retarget(cutoffHz, q, 0.0f, transitionFrames);
        return;
    }
    if (enable) {
        section.glide.Snap(cutoffHz, q, 0.0f);
    } else {
        section.glide.active = false;
    }
    RequestBiquadTransition(section.transition, section.enabled,
                            section.coeffs, tailFrames, enable, coeffs,
                            effectiveFrames);
}

void BusTree::ApplyVolume(std::uint32_t busIndex, float volumeDb,
                          std::uint32_t rampMs,
                          std::uint32_t busGeneration, bool outputOnly) {
    if (!HasWorkRow(busIndex) ||
        (busGeneration != 0 &&
         !AudioRefMatches({busIndex, busGeneration}))) {
        // A volume command for a just-created bus drains after that bus's
        // plan-swap command (command order), so by here m_audioCount already
        // covers it; the bound is defensive, not a live case.
        return;
    }
    Bus& bus = WorkState(busIndex);
    float linear = 0.0f;
    if (!TryVolumeGainFromDb(volumeDb, &linear)) {
        ++m_invalidVolumeApplies;
        return;
    }
    const std::uint64_t rampFrames =
        (static_cast<std::uint64_t>(rampMs) * m_sampleRate + 999) / 1000;
    NoteWorkMutation(bus);
    if (outputOnly) {
        bus.outputGain.Set(linear, rampFrames);
        return;
    }
    // First volume on a bus snaps even if rampMs != 0. Later sets use
    // N = ceil(rampMs * rate / 1000) mix frames; rampMs 0 is N = 0.
    if (!bus.gainStarted) {
        bus.gain.Set(linear, 0);
        bus.gainStarted = true;
        return;
    }
    bus.gain.Set(linear, rampFrames);
}

BusTree::SendEdge* BusTree::FindEdge(std::uint32_t fromBus,
                                     std::uint32_t toBus) {
    const auto plan = m_plans.View(m_currentPlanIndex);
    if (!plan.valid || fromBus >= m_audioCount || plan.sendStates == nullptr)
        return nullptr;
    const auto range = plan.Sends(fromBus);
    for (std::uint32_t s = 0; s < range.count; ++s)
        if (plan.sendTargets[range.first + s] == toBus)
            return &plan.sendStates[range.first + s];
    return nullptr;
}

void BusTree::ApplyTopologyPlan(std::uint32_t planIndex,
                                std::uint32_t connectFrom,
                                std::uint32_t connectTo, float connectLevelDb,
                                std::uint64_t acceptedGraphRevision) {
    const TopologyPlanView plan = m_plans.View(planIndex);
    if (!plan.valid) return;
    const auto previousIndex = m_currentPlanIndex;
    if (previousIndex == planIndex) return;
    const auto previous = m_plans.View(previousIndex);
    auto* previousSignals = m_activeActivity == nullptr ? InstalledSignals() : nullptr;
    if (plan.signals != nullptr && previousSignals != nullptr && plan.signals != previousSignals)
        plan.signals->AdoptContinuityFrom(*previousSignals);
    // Swap: retire whatever plan was installed (the logic side reclaims it) and
    // adopt the new one. A single command record is inherently atomic, so this
    // whole apply is the block-boundary swap.
    m_currentPlanIndex = planIndex;
    // The swap is the one point where the audio thread learns of new buses:
    // adopt the committed dense-slot bound the plan carries. Any bus the
    // logic thread appended since the previous swap becomes indexable now,
    // and never before — its entry (parent link, gain slot) was fully written
    // before this command was enqueued, so the command queue's release/acquire
    // publishes it to us here. A plan with no slot info (slotCount 0, e.g.
    // one a test built directly) keeps the pre-amendment meaning where the
    // slot bound and the live order coincide.
    m_audioCount = plan.slotCount != 0 ? plan.slotCount : plan.orderCount;
    // A generation transition is the audio-side slot initialization point.
    // The logic thread may reuse a still-rendered dense index in the same
    // transaction, so it never writes that Bus render state directly.
    if (plan.generations != nullptr && plan.busIds != nullptr &&
        plan.alive != nullptr && plan.slotCount != 0) {
        for (std::uint32_t i = 0; i < m_audioCount; ++i) {
            const std::uint32_t generation = plan.generations[i];
            if (plan.alive[i] == 0 || generation == 0) {
                continue;
            }
            if (HasWorkRow(i) && WorkState(i).generation != generation) {
                if (m_activeActivity != nullptr) {
                    // Topology publication must already have adopted a fresh
                    // incarnation. Never reset a leased published work row.
                    assert(false && "topology activity must prepare the new Bus incarnation");
                    continue;
                }
                RetireBusSlotEffects(WorkState(i));
                const std::uint32_t parent =
                    plan.parents != nullptr &&
                            plan.parents[i] != kPlanParentUnset
                        ? plan.parents[i]
                        : 0;
                InitializeBusSlot(i, plan.busIds[i], parent, generation);
            }
        }
    }
    // Install the plan-carried tree parents (the zero-new-opcode delivery
    // path for a live reparent, mirroring the fold config below). An unset
    // entry leaves the audio-side parent untouched.
    if (plan.parents != nullptr) {
        for (std::uint32_t i = 0; i < m_audioCount; ++i) {
            if (HasWorkRow(i) && plan.parents[i] != kPlanParentUnset) {
                NoteWorkMutation(WorkState(i));
                WorkState(i).parentIndex = plan.parents[i];
            }
        }
    }
    // Deleted Preview hosts first adopt their prepared empty chain, transferring
    // current instances to the same non-lossy retirement owner as live edits.
    for (std::uint32_t i = 0; i < plan.effectUpdateCount; ++i) {
        const auto& adoption = plan.effectUpdates[i];
        const auto host = BusSlotRef::FromPacked(adoption.host);
        if (plan.alive != nullptr && host.index < m_audioCount && plan.alive[host.index] == 0)
            ApplyPreviewEffectChain(adoption);
    }
    // Tombstoned slots leave the graph at this same swap: retire any
    // installed effect instances back to their pools (the logic thread
    // reclaims them like every clear — a bus is a persistent node, so the
    // tails are cut, not drained) and zero all meter taps so the snapshot's
    // dense-indexed busPeaks read 0 for a destroyed slot. Idempotent across
    // republishes: an already-scrubbed slot holds no instances and a zero
    // peak.
    if (plan.alive != nullptr && plan.slotCount != 0) {
        for (std::uint32_t i = 0; i < m_audioCount; ++i) {
            if (plan.alive[i] != 0 || !HasWorkRow(i)) {
                continue;
            }
            Bus& bus = WorkState(i);
            RetireBusSlotEffects(bus);
            bus.id = {};
            bus.preFaderPeak = 0.0f;
            bus.postFaderPeak = 0.0f;
            bus.outputPeak = 0.0f;
            std::fill_n(bus.outputChannelPeaks, kMaxChannels, 0.0f);
        }
    }
    // Install the per-bus output fold config the plan carries (the logic side
    // is authoritative and republishes the full fold config on every structural
    // change, so this reconciles the audio state to it). This is the zero-new-
    // opcode delivery path for Vn_SetBusOutputLayout / Vn_SetBusDownmixMatrix /
    // Vn_ResetBusDownmixMatrix: those calls update the logic-side config and
    // publish a plan, which the block-boundary swap installs here. A plan with
    // no fold config (a fold layout of 0, e.g. one a test built directly) leaves
    // the bus at "no fold". The master's entry carries its device matrix.
    if (plan.outputLayouts != nullptr) {
        for (std::uint32_t i = 0; i < m_audioCount; ++i) {
            if (!HasWorkRow(i)) continue;
            WorkState(i).outputLayout = plan.outputLayouts[i];
            plan.CopyDownmix(i, WorkState(i).downmixCoeffs);
        }
    }
    if (plan.busVolumeApply != nullptr &&
        plan.preparedBusVolumeDb != nullptr &&
        plan.preparedBusVolumeRampMs != nullptr) {
        for (std::uint32_t i = 0; i < m_audioCount; ++i) {
            const std::uint32_t generation =
                plan.generations != nullptr ? plan.generations[i] : 0;
            if ((plan.busVolumeApply[i] & kApplyBusNeutralProperties) != 0) {
                if (plan.alive == nullptr || plan.alive[i] == 0 ||
                    generation == 0 || !HasWorkRow(i) || !AudioRefMatches({i, generation})) {
                    continue;
                }
                Bus& bus = WorkState(i);
                if ((plan.busVolumeApply[i] & kApplyBusPreserveEffects) == 0)
                for (std::uint32_t slot = 0; slot < m_fxSlots; ++slot) {
                    ApplyClearBusEffect(i, slot, generation);
                    bus.fxBypassMix[slot] = 0.0f;
                    bus.fxBypassTarget[slot] = 0.0f;
                    bus.fxBypassRamping[slot] = false;
                }
                bus.faderAfterSlot = m_fxSlots;
            }
            if ((plan.busVolumeApply[i] & kApplyBusVolume) != 0) {
                ApplyVolume(i, plan.preparedBusVolumeDb[i],
                            plan.preparedBusVolumeRampMs[i], generation);
            }
            if ((plan.busVolumeApply[i] & kApplyBusOutputVolume) != 0) {
                ApplyVolume(i, plan.preparedBusOutputVolumeDb[i],
                            plan.preparedBusVolumeRampMs[i], generation, true);
            }
        }
    }
    for (std::uint32_t i = 0; i < plan.effectUpdateCount; ++i)
        ApplyPreviewEffectChain(plan.effectUpdates[i]);
    // Only audio reads the predecessor's live ramps. Both plans remain owned
    // until this copy completes; publishing retirement earlier would race with
    // logic reclamation. Source and target generations fence dense-slot reuse.
    for (std::uint32_t from = 0; from < m_audioCount; ++from) {
        const auto range = plan.Sends(from);
        const auto previousCount = previous.slotCount != 0 ? previous.slotCount : previous.orderCount;
        const bool sameSource = previous.valid && from < previousCount &&
            (plan.generations == nullptr || previous.generations == nullptr ||
             plan.generations[from] == previous.generations[from]);
        for (std::uint32_t s = 0; s < range.count; ++s) {
            const auto planSlot = range.first + s;
            const auto target = plan.sendTargets[planSlot];
            SendEdge& edge = plan.sendStates[planSlot];
            edge = SendEdge{};
            if (target == kEmptySendTarget) {
                continue;
            }
            const std::uint32_t targetGeneration =
                plan.generations != nullptr && target < m_audioCount
                    ? plan.generations[target]
                    : 0;
            const auto priorSlot = sameSource ? previous.SendOffset(from, plan.LogicalSendSlot(planSlot))
                                              : UINT32_MAX;
            const bool retained = priorSlot != UINT32_MAX && previous.sendStates != nullptr &&
                (plan.sendGenerations == nullptr || previous.sendGenerations == nullptr ||
                 plan.sendGenerations[planSlot] == previous.sendGenerations[priorSlot]) &&
                previous.sendStates[priorSlot].toIndex == target &&
                (targetGeneration == 0 || previous.sendStates[priorSlot].toGeneration == targetGeneration);
            if (retained) edge = previous.sendStates[priorSlot];
            edge.toIndex = target;
            edge.toGeneration = targetGeneration;
            if (plan.sendLevelApply != nullptr &&
                plan.preparedSendLevels != nullptr &&
                plan.sendLevelApply[planSlot] != 0) {
                const float linear = plan.preparedSendLevels[planSlot];
                const std::uint32_t rampMs =
                    plan.sendLevelRampMs != nullptr
                        ? plan.sendLevelRampMs[planSlot]
                        : 0;
                if (retained && rampMs != 0) {
                    ArmRamp(edge.gain, edge.ramping,
                            edge.rampStartGain, edge.rampEndGain,
                            edge.rampBlocksTotal, edge.rampBlocksDone,
                            linear, rampMs);
                } else {
                    edge.gain.SnapTo(linear);
                    edge.ramping = false;
                    edge.rampStartGain = linear;
                    edge.rampEndGain = linear;
                    edge.rampBlocksTotal = 0;
                    edge.rampBlocksDone = 0;
                }
            }
        }
    }
    // A connect carries its new edge's initial level, applied atomically with
    // the swap so the edge sounds correct on its first block. Storage was
    // prepared for every membership record before publishing this plan.
    if (connectFrom != kInvalidBusIndex) {
        SendEdge* edge = FindEdge(connectFrom, connectTo);
        if (edge != nullptr) {
            *edge = SendEdge{};
            edge->toIndex = connectTo;
            edge->toGeneration =
                plan.generations != nullptr && connectTo < m_audioCount
                    ? plan.generations[connectTo]
                    : 0;
            edge->gain.SnapTo(std::pow(10.0f, connectLevelDb / 20.0f));
        }
    }
    if (previousIndex != kInvalidTopologyPlanIndex && previousIndex != planIndex)
        m_plans.Retire(previousIndex);
    if (acceptedGraphRevision != 0) {
        m_installedGraphRevision.store(acceptedGraphRevision,
                                       std::memory_order_release);
    }
}

void BusTree::ApplySetBusSendLevel(std::uint32_t fromBus, std::uint32_t toBus,
                                   float levelDb, std::uint32_t rampMs,
                                   std::uint32_t fromGeneration,
                                   std::uint32_t toGeneration,
                                   std::uint64_t sendGeneration) {
    if ((fromGeneration != 0 &&
         !AudioRefMatches({fromBus, fromGeneration})) ||
        (toGeneration != 0 &&
         !AudioRefMatches({toBus, toGeneration}))) {
        return;
    }
    SendEdge* edge = FindEdge(fromBus, toBus);
    if (edge != nullptr && sendGeneration != 0) {
        const auto plan = m_plans.View(m_currentPlanIndex);
        const auto index = static_cast<std::size_t>(edge - plan.sendStates);
        if (plan.sendGenerations == nullptr || plan.sendGenerations[index] != sendGeneration) return;
    }
    if (edge != nullptr && toGeneration != 0 &&
        edge->toGeneration != toGeneration) {
        return;
    }
    if (edge == nullptr) return;
    if (HasWorkRow(fromBus)) NoteWorkMutation(WorkState(fromBus));
    const float linear = std::pow(10.0f, levelDb / 20.0f);
    ArmRamp(edge->gain, edge->ramping, edge->rampStartGain, edge->rampEndGain,
            edge->rampBlocksTotal, edge->rampBlocksDone, linear, rampMs);
}

void BusTree::AudioDiscardLimiterGainReduction(std::uint64_t epoch) {
    const auto order = WorkingOrder();
    const auto count = order.empty() ? m_audioCount : static_cast<std::uint32_t>(order.size());
    for (std::uint32_t k = 0; k < count; ++k) {
        const auto i = order.empty() ? k : order[k];
        if (!HasWorkRow(i)) continue;
        auto& bus = WorkState(i);
        if (!AudioRefMatches({i, bus.generation})) continue;
        for (std::uint32_t s = 0; s < m_fxSlots; ++s) {
            if (bus.fxSlotEffectRef[s].type != VN_EFFECT_LIMITER ||
                bus.fxSlots[s].state == nullptr) continue;
            auto& state = *static_cast<LimiterState*>(bus.fxSlots[s].state);
            if (state.gainReduction.renderEpoch == epoch) LimiterFinishRender(state, 0, 0);
        }
    }
}

bool BusTree::AudioReadLimiterGainReduction(BusSlotRef ref, std::uint32_t slot,
    const EffectInstanceRef& expected, std::uint64_t epoch,
    std::uint64_t sourceFrameBegin, LimiterGainReduction& out) {
    out = {};
    if (!AudioRefMatches(ref) || !HasWorkRow(ref.index) || slot >= m_fxSlots) return false;
    const auto& bus = WorkState(ref.index);
    return CopyLimiterGainReduction(bus.fxSlots[slot], bus.fxSlotEffectRef[slot],
                                   expected, epoch, sourceFrameBegin, out);
}

void BusTree::FinalizeBus(Bus& bus, float* const* planes,
                          std::uint32_t blockFrames,
                          std::uint64_t blockEpoch,
                          std::uint64_t sourceFrameBegin) {
    // §7.3 bus block signal flow. The input sum is already accumulated in
    // `planes` (feeders folded in before this bus is finalized). The fader
    // split is clamped defensively; the logic side validates it <= m_fxSlots.
    float* const* dry = m_fxDry.data();
    const std::uint32_t fader =
        (bus.faderAfterSlot <= m_fxSlots) ? bus.faderAfterSlot : m_fxSlots;
    const std::uint32_t busIndex =
        bus.busIndex;
    const auto processSlot = [&](std::uint32_t slot) {
        EffectDynamicInputView dynamicInput{};
        const EffectDynamicInputView* input = nullptr;
        DynamicDetectorStatus detectorStatus{};
        CompressorState* compressorState = nullptr;
        const bool bypassTransitionThisBlock =
            bus.fxBypassRamping[slot];
        const bool bypassedThisBlock =
            bus.fxBypassRamping[slot]
                ? bus.fxBypassTarget[slot] >= 1.0f
                : bus.fxBypassMix[slot] >= 1.0f;
        bool parameterTransitionThisBlock = false;
        if (bus.fxSlotEffectRef[slot].type == VN_EFFECT_COMPRESSOR) {
            compressorState =
                static_cast<CompressorState*>(bus.fxSlots[slot].state);
            CompressorBeginBlock(compressorState);
            parameterTransitionThisBlock =
                compressorState->parameterTransitionFramesRemaining != 0;
            dynamicInput.detectorMagnitude = m_dynamicRoutes.BuildDetector(
                busIndex, slot, blockFrames, &detectorStatus);
            dynamicInput.frameCount = blockFrames;
            dynamicInput.signalForm = VN_DYNAMIC_SIGNAL_RAW_AUDIO;
            if (detectorStatus.authored) {
                input = &dynamicInput;
            }
        }
        const std::uint64_t effectHandle =
            PackedMonitorEffectHandle(busIndex, slot);
        const std::uint64_t effectGeneration = bus.fxSlotEffectRef[slot].generation;
        if (bus.fxSlots[slot].process != nullptr &&
            harken::monitor::AudioLiveArmed()) {
            harken::monitor::PublishPlanarAudioChunk(
                VN_MONITOR_TAP_EFFECT_SLOT_INPUT, effectHandle, effectGeneration,
                slot, planes, blockFrames, m_mixChannels, 0, m_mixLayout,
                m_sampleRate, sourceFrameBegin);
        }
        auto* limiter = bus.fxSlotEffectRef[slot].type == VN_EFFECT_LIMITER
            ? static_cast<LimiterState*>(bus.fxSlots[slot].state) : nullptr;
        if (limiter != nullptr) LimiterBeginRender(*limiter, blockEpoch, sourceFrameBegin);
        auto* transition = bus.fxChainTransition;
        ProcessChainSlot(bus.fxSlots[slot], bus.fxBypassRamping[slot],
            bus.fxBypassMix[slot], bus.fxBypassTarget[slot],
            planes, dry, m_mixChannels, 0, blockFrames, input, limiter);
        if (limiter != nullptr) {
            LimiterFinishRender(*limiter, 0, blockFrames);
            if (transition != nullptr)
                limiter->gainReduction.status = LimiterGainReductionStatus::ChainTransition;
        }
        if (bus.fxSlots[slot].process != nullptr &&
            harken::monitor::AudioLiveArmed()) {
            harken::monitor::PublishPlanarAudioChunk(
                VN_MONITOR_TAP_EFFECT_SLOT_OUTPUT, effectHandle, effectGeneration,
                slot, planes, blockFrames, m_mixChannels, 0, m_mixLayout,
                m_sampleRate, sourceFrameBegin);
        }
        if (compressorState != nullptr) {
            bus.activityDetector = bus.activityDetector ||
                compressorState->telemetry.detectorPeak != 0.0f ||
                detectorStatus.nonFiniteCount != 0;
            compressorState->telemetry.detectorNonFiniteCount +=
                detectorStatus.nonFiniteCount;
            compressorState->telemetry.detectorSaturationCount +=
                detectorStatus.saturationCount;
            CompressorRuntimePayload payload{};
            payload.hostObjectId = bus.id;
            payload.blockEpoch = blockEpoch;
            payload.detectorNonFiniteCount =
                compressorState->telemetry.detectorNonFiniteCount;
            payload.detectorSaturationCount =
                compressorState->telemetry.detectorSaturationCount;
            payload.hostScope = VN_EFFECT_HOST_SCOPE_BUS;
            payload.hostDenseIndex = busIndex;
            payload.effectSlot = slot;
            if (bypassedThisBlock) {
                payload.stateFlags |= VN_COMPRESSOR_RUNTIME_BYPASSED;
            }
            if (bypassTransitionThisBlock) {
                payload.stateFlags |=
                    VN_COMPRESSOR_RUNTIME_BYPASS_TRANSITION;
            }
            if (parameterTransitionThisBlock ||
                compressorState->parameterTransitionFramesRemaining != 0) {
                payload.stateFlags |=
                    VN_COMPRESSOR_RUNTIME_PARAM_TRANSITION;
            }
            if (detectorStatus.authored) {
                payload.stateFlags |=
                    VN_COMPRESSOR_RUNTIME_EXTERNAL_AUTHORED;
            }
            if (detectorStatus.active) {
                payload.stateFlags |=
                    VN_COMPRESSOR_RUNTIME_EXTERNAL_ACTIVE;
            }
            if (compressorState->telemetry.resetThisBlock) {
                payload.stateFlags |=
                    VN_COMPRESSOR_RUNTIME_RESET_THIS_BLOCK;
            }
            if (compressorState->telemetry.resetBridgeActive) {
                payload.stateFlags |=
                    VN_COMPRESSOR_RUNTIME_RESET_BRIDGE;
            }
            payload.transitionFramesRemaining =
                compressorState->parameterTransitionFramesRemaining;
            payload.staleRouteCount = detectorStatus.staleRouteCount;
            payload.mainInputPeakLinear =
                compressorState->telemetry.mainInputPeak;
            payload.detectorPeakLinear =
                compressorState->telemetry.detectorPeak;
            payload.outputPeakLinear =
                compressorState->telemetry.outputPeak;
            payload.maxGainReductionDb =
                compressorState->telemetry.maxGainReductionDb;
            payload.endGainReductionDb =
                compressorState->telemetry.endGainReductionDb;
            m_effectBank.PublishCompressorRuntime(
                bus.fxSlotEffectRef[slot], payload);
        }
    };
    const auto processRange = [&](std::uint32_t first, std::uint32_t end) {
        for (auto slot = first; slot < end; ++slot) {
            auto* transition = bus.fxChainTransition;
            auto next = slot + 1;
            if (transition == nullptr || (transition->changedMask & (1u << slot)) == 0) {
                processSlot(slot);
                continue;
            }
            next = end;
            std::array<float, kMaxFxSlotsPerBus> starts{};
            for (auto s = slot; s < next; ++s)
                starts[s] = bus.fxBypassRamping[s] ? bus.fxBypassMix[s] : -1.0f;
            const auto tap = [&](std::uint32_t s, float* const* branch, bool output) {
                auto* limiter = bus.fxSlotEffectRef[s].type == VN_EFFECT_LIMITER
                    ? static_cast<LimiterState*>(bus.fxSlots[s].state) : nullptr;
                if (limiter != nullptr) {
                    if (!output) LimiterBeginRender(*limiter, blockEpoch, sourceFrameBegin);
                    else {
                        LimiterFinishRender(*limiter, 0, blockFrames);
                        limiter->gainReduction.status = LimiterGainReductionStatus::ChainTransition;
                    }
                }
                // Passive observations describe the new branch's actual slot
                // I/O; arming a monitor must not change the audible transition.
                if (bus.fxSlots[s].process != nullptr && harken::monitor::AudioLiveArmed())
                    harken::monitor::PublishPlanarAudioChunk(
                        output ? VN_MONITOR_TAP_EFFECT_SLOT_OUTPUT : VN_MONITOR_TAP_EFFECT_SLOT_INPUT,
                        PackedMonitorEffectHandle(busIndex, s), bus.fxSlotEffectRef[s].generation,
                        s, branch, blockFrames, m_mixChannels, 0, m_mixLayout,
                        m_sampleRate, sourceFrameBegin);
            };
            ProcessTransitionChainRange(transition->oldEffects.data(), transition->oldBypass.data(),
                bus.fxSlots, bus.fxBypassTarget, starts.data(), slot, next,
                planes, transition->dry.data(), transition->alternate.data(), m_mixChannels,
                0, blockFrames, transition->done, transition->totalFrames,
                [&](auto s, auto branch) { tap(s, branch, false); },
                [&](auto s, auto branch) { tap(s, branch, true); });
            for (auto s = slot; s < next; ++s) {
                bus.fxBypassMix[s] = bus.fxBypassTarget[s];
                bus.fxBypassRamping[s] = false;
            }
            slot = next - 1;
        }
    };
    if (!bus.signalBoundsPrepared)
        (void)SignalBusBounds(bus.busIndex, blockFrames, {});
    bus.signalBoundsPrepared = false;
    // Pre-fader chain: fx slots [0, faderAfterSlot) before the gain.
    processRange(0, fader);
    m_dynamicRoutes.CaptureBus(busIndex, VN_DYNAMIC_TAP_BUS_PRE_FADER,
                               planes, m_mixChannels, blockFrames);
    const bool unchangedFaderSamples = bus.gain.remainingFrames == 0 &&
        bus.gain.target == 1.0f && !ToneSectionActive(bus.busLpf) &&
        !ToneSectionActive(bus.busHpf);
    float unchangedChannelPeaks[kMaxChannels];
    bus.preFaderPeak = StageTapPeak(planes, m_mixChannels, 0, blockFrames,
        unchangedFaderSamples && fader == m_fxSlots ? unchangedChannelPeaks : nullptr);
    if (harken::monitor::AudioLiveArmed()) {
        harken::monitor::PublishPlanarAudioChunk(
            VN_MONITOR_TAP_BUS_PRE_FADER, busIndex + 1u, bus.generation, 0, planes,
            blockFrames, m_mixChannels, 0, m_mixLayout, m_sampleRate,
            sourceFrameBegin);
    }
    // BusVolume — frame-accurate OutputGain. Silent mix frames still advance.
    bus.gain.Apply(planes, m_mixChannels, 0, blockFrames);
    ProcessBusToneSection(bus.busLpf, BusToneKind::Lowpass, m_biquadTrig,
                          m_sampleRate, planes, m_mixChannels, blockFrames);
    ProcessBusToneSection(bus.busHpf, BusToneKind::Highpass, m_biquadTrig,
                          m_sampleRate, planes, m_mixChannels, blockFrames);
    m_dynamicRoutes.CaptureBus(busIndex, VN_DYNAMIC_TAP_BUS_POST_FADER,
                               planes, m_mixChannels, blockFrames);
    if (unchangedFaderSamples) {
        bus.postFaderPeak = bus.preFaderPeak;
        if (fader == m_fxSlots)
            std::copy_n(unchangedChannelPeaks, m_mixChannels, bus.outputChannelPeaks);
    } else {
        bus.postFaderPeak = StageTapPeak(planes, m_mixChannels, 0, blockFrames,
            fader == m_fxSlots ? bus.outputChannelPeaks : nullptr);
    }
    if (harken::monitor::AudioLiveArmed()) {
        harken::monitor::PublishPlanarAudioChunk(
            VN_MONITOR_TAP_BUS_POST_FADER, busIndex + 1u, bus.generation, 0, planes,
            blockFrames, m_mixChannels, 0, m_mixLayout, m_sampleRate,
            sourceFrameBegin);
    }
    // Post-fader chain: fx slots [faderAfterSlot, m_fxSlots) after the gain.
    processRange(fader, m_fxSlots);
    FinishPreviewEffectChain(bus, blockFrames);
    // Post-everything output tap (after post-fader fx): by the time this bus is
    // finalized every feeder has folded in and the whole chain has run, so the
    // tap covers the bus's whole subtree; at index 0 it is the final mix. Sends
    // scatter from this same final output. A bus that folds to a narrower
    // output layout re-takes this tap post-fold (FoldBusOutput), per §6.4.
    // Compiled into ALL builds (the section-11.7 declared behavior change):
    // the scan feeds the meter Variable transport, a runtime feature, so it no
    // longer rides the stage-tap gate — Release snapshots read honest peaks.
    bus.outputPeak =
        fader == m_fxSlots
            ? bus.postFaderPeak
            : StageTapPeak(planes, m_mixChannels, 0, blockFrames,
                bus.outputChannelPeaks);
    if (harken::monitor::AudioLiveArmed()) {
        harken::monitor::PublishPlanarAudioChunk(
            VN_MONITOR_TAP_BUS_POST_EFFECTS, busIndex + 1u, bus.generation, 0, planes,
            blockFrames, m_mixChannels, 0, m_mixLayout, m_sampleRate,
            sourceFrameBegin);
    }
}

void BusTree::FoldBusOutput(Bus& bus, float* const* planes,
                            std::uint32_t blockFrames) {
    // The bus rendered its chain at the mix width; fold those planes to its
    // narrower output layout through its own matrix, then name-place the folded
    // channels back into the mix-width planes so the parent sum and sends see a
    // mix-width buffer carrying only the placed channels. src = the mix layout,
    // dst = the bus's output layout (a concrete layout narrower than the mix).
    const std::uint32_t dstCount = LayoutChannelCount(bus.outputLayout);
    // 1. Fold into the reusable dry scratch (m_fxDry, mix-width, so it holds the
    //    dstCount folded channels). FinalizeBus already ran, so the scratch is
    //    free to reuse this block.
    for (std::uint32_t r = 0; r < dstCount; ++r) {
        float* out = m_fxDry[r];
        for (std::uint32_t n = 0; n < blockFrames; ++n) {
            out[n] = 0.0f;
        }
        for (std::uint32_t c = 0; c < m_mixChannels; ++c) {
            const float g = bus.downmixCoeffs[r * 8 + c];
            if (g == 0.0f) {
                continue;
            }
            const float* s = planes[c];
            for (std::uint32_t n = 0; n < blockFrames; ++n) {
                out[n] += s[n] * g;
            }
        }
        for (std::uint32_t n = 0; n < blockFrames; ++n) {
            out[n] = RecoverFiniteDownmixSum(out[n], bus.downmixCoeffs + r * 8,
                                            planes, m_mixChannels, n);
        }
    }
    const std::uint32_t busIndex =
        bus.busIndex;
    m_dynamicRoutes.CaptureBus(busIndex, VN_DYNAMIC_TAP_BUS_POST_EFFECTS,
                               m_fxDry.data(), dstCount, blockFrames);
    // 2. Clear every mix-width plane, then name-place the folded channels: the
    //    role of output channel r goes to the mix channel of the same role;
    //    unmatched channels stay silent (no upmix synthesis) — a mono fold's
    //    center is dropped when the mix layout has no center channel.
    for (std::uint32_t c = 0; c < m_mixChannels; ++c) {
        std::memset(planes[c], 0,
                    static_cast<std::size_t>(blockFrames) * sizeof(float));
    }
    ChannelRole roles[kMaxChannels];
    LayoutRoles(bus.outputLayout, roles);
    for (std::uint32_t r = 0; r < dstCount; ++r) {
        // Pure name placement: a folded channel lands in the mix channel of the
        // same role, and a role the mix layout lacks contributes nothing (no
        // upmix synthesis). A mono output (role FC) folded into a mix without a
        // center is therefore silent — the spec's placement rule, not a
        // fallback.
        const int mixIdx = ChannelOfRole(m_mixLayout, roles[r]);
        if (mixIdx >= 0) {
            std::memcpy(planes[static_cast<std::uint32_t>(mixIdx)], m_fxDry[r],
                        static_cast<std::size_t>(blockFrames) * sizeof(float));
        }
    }
    // §6.4: the bus meter is the peak across all channels of the final output,
    // post the per-bus fold. All builds, like the finalize tap above
    // (section 11.7).
    bus.outputPeak = StageTapPeak(planes, m_mixChannels, 0, blockFrames,
        bus.outputChannelPeaks);
}

void BusTree::ScatterSend(const float* const* src, float* const* dst,
                          std::uint32_t blockFrames, SendEdge& edge) {
    if (edge.ramping) {
        edge.gain.SetBlockTarget(edge.rampStartGain, edge.rampEndGain,
                                 edge.rampBlocksDone, edge.rampBlocksTotal);
    }
    edge.gain.MixAddBlock(src, dst, m_mixChannels, 0, blockFrames);
    if (edge.ramping) {
        ++edge.rampBlocksDone;
        if (edge.rampBlocksDone >= edge.rampBlocksTotal) {
            edge.ramping = false;
        }
    }
}

SignalBusRoutingInfo BusTree::SignalBusBounds(std::uint32_t index, std::uint32_t frames,
    std::span<SignalBiquadSegment> schedules) noexcept {
    auto& bus = WorkState(index);
    SignalBusRoutingInfo result{};
    result.id = bus.id;
    result.generation = bus.generation;
    result.outputGainBound = SignalOutputGainBound(bus.outputGain, frames);
    bus.signalGainHistory = std::max(bus.signalGainHistory, SignalOutputGainBound(bus.gain, frames));
    bus.signalOutputHistory = std::max(bus.signalOutputHistory, result.outputGainBound);
    double input = bus.signalGainHistory;
    for (std::uint32_t slot = 0; slot < m_fxSlots; ++slot) {
        const auto& effect = bus.fxSlots[slot];
        double value = 1;
        if (effect.process != nullptr && effect.state != nullptr) {
            switch (bus.fxSlotEffectRef[slot].type) {
            case VN_EFFECT_GAIN: {
                const auto& state = *static_cast<const GainState*>(effect.state);
                value = SignalIncrementBound(state.gainLinear, state.targetGainLinear,
                    state.gainIncrement, state.transitionFramesRemaining, frames);
                break;
            }
            case VN_EFFECT_FEEDBACK_DELAY: {
                const auto& state = *static_cast<const DelayEffectState*>(effect.state);
                const double wet = SignalIncrementBound(state.wet, state.targetWet,
                    state.wetIncrement, state.transitionFramesRemaining, frames);
                const double dry = SignalIncrementBound(state.dry, state.targetDry,
                    state.dryIncrement, state.transitionFramesRemaining, frames);
                value = dry + wet / (1.0 - std::max(state.feedback, state.maxFeedbackSinceReset));
                break;
            }
            case VN_EFFECT_THREE_BAND_TONE: {
                const auto& state = *static_cast<const ToneEffectState*>(effect.state);
                for (std::size_t band = 0; band < 3; ++band) {
                    auto& history = bus.signalToneHistory[slot][band];
                    history = std::max({1.0, history, state.signalBandHistory[band]});
                    value = SignalBoundProduct(value, history);
                }
                break;
            }
            case VN_EFFECT_LIMITER: value = 2; break;
            case VN_EFFECT_COMPRESSOR: {
                const auto& state = *static_cast<const CompressorState*>(effect.state);
                value = 2 * std::max({1.0, state.resetBridgeStartGain,
                    SignalIncrementBound(state.parameters.makeupLinear, state.targetParameters.makeupLinear,
                        state.parameterIncrements.makeupLinear, state.parameterTransitionFramesRemaining, frames)});
                break;
            }
            default: value = std::numeric_limits<double>::infinity(); break;
            }
            if (!bus.fxBypassRamping[slot] && bus.fxBypassMix[slot] >= 1) value = 1;
            else if (bus.fxBypassRamping[slot] || bus.fxBypassMix[slot] > 0) value = std::max(1.0, value);
        }
        // A replaced upstream effect does not reset surviving downstream memory.
        bus.signalSlotHistory[slot] = std::max({1.0, bus.signalSlotHistory[slot], value});
        input = SignalBoundProduct(input, bus.signalSlotHistory[slot]);
    }
    const auto sectionBound = [&](Bus::ToneSection& section, BusToneKind kind, std::size_t slot) {
        section.signalSchedule = {};
        if (!schedules.empty()) {
            auto storage = schedules.subspan(slot * (frames / 16u + 2u), frames / 16u + 2u);
            auto copy = section;
            const auto bound = ProcessBusToneSection(copy, kind, m_biquadTrig,
                m_sampleRate, nullptr, 0, frames, storage);
            section.signalHighWater = std::max(section.signalHighWater, bound);
            if (section.glide.active && !section.transition.transitioning && m_biquadTrig != nullptr)
                section.signalSchedule = storage;
        }
        return section.signalHighWater;
    };
    input = SignalBoundProduct(input, sectionBound(bus.busLpf, BusToneKind::Lowpass, 0));
    input = SignalBoundProduct(input, sectionBound(bus.busHpf, BusToneKind::Highpass, 1));
    result.outputFilterBound = SignalBoundProduct(
        sectionBound(bus.outputLpf, BusToneKind::Lowpass, 2),
        sectionBound(bus.outputHpf, BusToneKind::Highpass, 3));
    double layout = 1;
    if (index != 0 && IsConcreteLayout(bus.outputLayout) && LayoutChannelCount(bus.outputLayout) < m_mixChannels) {
        layout = 0;
        ChannelRole roles[kMaxChannels];
        LayoutRoles(bus.outputLayout, roles);
        for (std::uint32_t row = 0; row < LayoutChannelCount(bus.outputLayout); ++row) {
            if (ChannelOfRole(m_mixLayout, roles[row]) < 0) continue;
            double sum = 0;
            for (std::uint32_t ch = 0; ch < m_mixChannels; ++ch)
                sum += std::abs(static_cast<double>(bus.downmixCoeffs[row * 8 + ch]));
            layout = std::max(layout, sum);
        }
    }
    bus.signalLayoutHistory = std::max(bus.signalLayoutHistory, layout);
    result.inputBound = SignalBoundProduct(SignalBoundProduct(input, bus.signalLayoutHistory), bus.signalOutputHistory);
    return result;
}

std::size_t BusTree::SignalRoutingExtraNodes(SignalBusMapping mapping) const noexcept {
    const auto plan = m_dynamicRoutes.InstalledPlan();
    for (std::uint32_t i = 0; i < plan.rowCount; ++i) {
        const auto& row = plan.rows[i];
        if (row.sourceKind == VN_DYNAMIC_SOURCE_BUS &&
            mapping.Ordinal(row.sourceIndex) != UINT32_MAX &&
            mapping.Ordinal(row.targetBusIndex) != UINT32_MAX)
            return static_cast<std::size_t>(mapping.Count()) * (m_fxSlots + 2ull);
    }
    return 0;
}

std::size_t BusTree::SignalRoutingEdgeCapacity(SignalBusMapping mapping) const noexcept {
    const auto plan = m_plans.View(m_currentPlanIndex);
    auto result = static_cast<std::size_t>(mapping.Count()) + SignalRoutingExtraNodes(mapping) +
        m_dynamicRoutes.InstalledPlan().rowCount;
    if (plan.valid)
        for (std::uint32_t i = 0; i < mapping.Count(); ++i)
            if (mapping.Bus(i) < m_audioCount) result += plan.Sends(mapping.Bus(i)).count;
    return result;
}

std::uint32_t BusTree::SignalRoutingSnapshot(std::span<SignalBusRoutingInfo> info,
    std::span<SignalFeedbackEdge> edges, std::uint32_t frames,
    std::span<SignalBiquadSegment> schedules, std::uint32_t nodeOffset,
    SignalBusMapping mapping) noexcept {
    const auto count = mapping.Count();
    const auto perBus = static_cast<std::size_t>(4) * (frames / 16u + 2u);
    const auto extraNodes = SignalRoutingExtraNodes(mapping);
    if (info.size() < count || edges.size() < SignalRoutingEdgeCapacity(mapping) ||
        schedules.size() < count * perBus || extraNodes > UINT32_MAX -
            static_cast<std::uint64_t>(nodeOffset)) return UINT32_MAX;
    const auto plan = m_plans.View(m_currentPlanIndex);
    const bool segmented = extraNodes != 0;
    const auto inputNode = [&](std::uint32_t ordinal) {
        return segmented ? nodeOffset + ordinal * (m_fxSlots + 2) : mapping.Node(ordinal);
    };
    for (std::uint32_t i = 0; i < count; ++i) {
        info[i] = {};
        const auto bus = mapping.Bus(i);
        if (mapping.Node(i) >= nodeOffset) return UINT32_MAX;
        // A prepared world can retain empty definition slots beyond the
        // installed topology. They have no routing state to sample.
        if (bus >= m_audioCount || !HasWorkRow(bus)) continue;
        if (plan.valid && plan.alive != nullptr && plan.alive[bus] == 0) continue;
        info[i] = SignalBusBounds(bus, frames, schedules.subspan(i * perBus, perBus));
        info[i].inputNode = inputNode(i);
        WorkState(bus).signalBoundsPrepared = true;
    }
    std::uint32_t used = 0;
    if (segmented) {
        for (std::uint32_t i = 0; i < count; ++i) {
            if (info[i].id == VnBusId{}) continue;
            const auto bus = mapping.Bus(i), base = inputNode(i);
            const auto fader = std::min(WorkState(bus).faderAfterSlot, m_fxSlots);
            auto previous = base;
            for (std::uint32_t slot = 0; slot <= m_fxSlots; ++slot) {
                if (slot == fader) {
                    edges[used++] = {previous, base + m_fxSlots + 1, 1, false};
                    previous = base + m_fxSlots + 1;
                }
                if (slot < m_fxSlots) {
                    edges[used++] = {previous, base + slot + 1, 1, false};
                    previous = base + slot + 1;
                }
            }
            edges[used++] = {previous, mapping.Node(i), info[i].inputBound, false};
        }
        const auto routes = m_dynamicRoutes.InstalledPlan();
        for (std::uint32_t i = 0; i < routes.rowCount; ++i) {
            const auto& row = routes.rows[i];
            const auto from = mapping.Ordinal(row.sourceIndex), to = mapping.Ordinal(row.targetBusIndex);
            if (!m_dynamicRoutes.InstalledBusTapMatches(row) || from == UINT32_MAX || to == UINT32_MAX ||
                row.effectSlot >= m_fxSlots || info[from].id == VnBusId{} || info[to].id == VnBusId{} ||
                WorkState(row.targetBusIndex).fxSlotEffectRef[row.effectSlot].type != VN_EFFECT_COMPRESSOR) continue;
            const auto base = inputNode(from);
            const auto fader = std::min(WorkState(row.sourceIndex).faderAfterSlot, m_fxSlots);
            std::uint32_t source = 0;
            switch (row.tap) {
            case VN_DYNAMIC_TAP_BUS_PRE_FADER: source = base + fader; break;
            case VN_DYNAMIC_TAP_BUS_POST_FADER: source = base + m_fxSlots + 1; break;
            case VN_DYNAMIC_TAP_BUS_POST_EFFECTS:
                source = base + m_fxSlots + (fader == m_fxSlots ? 1u : 0u); break;
            default: return UINT32_MAX;
            }
            edges[used++] = {source, inputNode(to) + row.effectSlot + 1,
                row.levelLinear == 0 ? 0 : std::numeric_limits<double>::infinity(), false};
        }
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        if (info[i].id == VnBusId{}) continue;
        const auto from = mapping.Bus(i);
        const auto& bus = WorkState(from);
        const auto parent = mapping.Ordinal(bus.parentIndex);
        if (from != 0 && parent != UINT32_MAX && info[parent].id != VnBusId{})
            edges[used++] = {mapping.Node(i), inputNode(parent),
                SignalBoundProduct(info[i].outputFilterBound, segmented ? 1 : info[parent].inputBound), false};
        if (!plan.valid) continue;
        const auto range = plan.Sends(from);
        for (std::uint32_t slot = 0; slot < range.count; ++slot) {
            const auto to = mapping.Ordinal(plan.sendTargets[range.first + slot]);
            if (to == UINT32_MAX || info[to].id == VnBusId{}) continue;
            const auto* edge = &plan.sendStates[range.first + slot];
            auto gain = edge->gain;
            if (edge->ramping) gain.SetBlockTarget(edge->rampStartGain, edge->rampEndGain,
                edge->rampBlocksDone, edge->rampBlocksTotal);
            const double level = std::max(std::abs(static_cast<double>(gain.current)),
                std::abs(static_cast<double>(gain.target)));
            edges[used++] = {mapping.Node(i), inputNode(to),
                SignalBoundProduct(level, segmented ? 1 : info[to].inputBound), false};
        }
    }
    return used;
}

bool BusTree::SignalReceiversCannotReachSources(std::span<std::uint8_t> roles,
    SignalBusMapping mapping) const noexcept {
    // The full-tree shortcut is optional. A compact/reordered projection uses
    // the exact active graph below, never a scan of unrelated definitions.
    if (mapping.Count() != m_audioCount || roles.size() < mapping.Count() ||
        SignalRoutingExtraNodes(mapping) != 0) return false;
    for (std::uint32_t i = 0; i < mapping.Count(); ++i)
        if (mapping.Bus(i) != i || mapping.Node(i) != i) return false;
    const auto plan = m_plans.View(m_currentPlanIndex);
    const auto count = plan.valid ? plan.orderCount : m_audioCount;
    // Reverse the existing feeds-first ordinary order. Bit 1 propagates from
    // sources to every input that can reach them; bit 2 identifies receivers.
    for (std::uint32_t k = count; k != 0; --k) {
        const auto from = plan.valid ? plan.order[k - 1] : count - k;
        if (from >= m_audioCount) return false;
        if (!HasWorkRow(from)) continue;
        const auto parent = WorkState(from).parentIndex;
        if (from != 0 && parent < m_audioCount) roles[from] |= roles[parent] & 1u;
        if (plan.valid) {
            const auto range = plan.Sends(from);
            for (std::uint32_t slot = 0; slot < range.count; ++slot) {
                const auto target = plan.sendTargets[range.first + slot];
                if (target < m_audioCount) roles[from] |= roles[target] & 1u;
            }
        }
        if ((roles[from] & 3u) == 3u) return false;
    }
    return true;
}

bool BusTree::MixDown(const BusRenderView& busPlanes, std::uint32_t blockFrames,
                      std::uint64_t blockEpoch, std::uint64_t sourceFrameBegin,
                      SignalRenderPlan* signals) {
    const TopologyPlanView plan = m_plans.View(m_currentPlanIndex);
    // Feeds-first order: with a plan, the published order (every bus after all
    // buses that feed it — tree children and send sources — master last).
    // Without a plan (before the first structural change), the descending-index
    // fold: on a pure tree a child's index exceeds its parent's, so high->low
    // finalizes every feeder before its receiver. The no-plan/no-send path is
    // thus bit-identical to the prior fold.
    // The audio-visible slot bound advances only with the plan swap, in
    // lockstep with the plan installed there, so a valid plan's order always
    // stays within these slots (equal until a destroy tombstones a slot out
    // of the live order). A longer order would mean bus structure leaked
    // across the swap boundary — an impossible state now that CreateBus never
    // advances the audio count; assert it in Debug rather than tolerating it.
    const bool usePlan = plan.valid;
    assert((!usePlan || plan.orderCount <= m_audioCount) &&
           "installed plan order must stay within the audio-visible slots");
    const auto order = WorkingOrder();
    const std::uint32_t n = order.empty() ? m_audioCount : static_cast<std::uint32_t>(order.size());
    for (std::uint32_t k = 0; k < n; ++k) {
        const std::uint32_t i = order.empty() ? (n - 1 - k) : order[k];
        Bus& bus = WorkState(i);
        float* const* planes = busPlanes.ForBus(i, m_mixChannels, bus.generation);
        for (std::uint32_t ch = 0; ch < m_mixChannels && !bus.activityInput; ++ch)
            for (std::uint32_t frame = 0; frame < blockFrames; ++frame)
                if (planes[ch][frame] != 0.0f) { bus.activityInput = true; break; }
        if (signals == nullptr || !signals->NeedsBlockRouting()) bus.signalBoundsPrepared = false;
        FinalizeBus(bus, planes, blockFrames, blockEpoch, sourceFrameBegin);
        const auto signalNode = signals == nullptr ? UINT32_MAX : signals->NodeForBus(i, bus.id, bus.generation);
        // Per-bus output fold (non-master): a bus configured to present a
        // narrower output layout than the mix folds through its own matrix and
        // name-places the result back into these mix-width planes, so the
        // parent sum and sends below consume the folded output. The master's
        // output fold is to the device layout and is applied downstream by the
        // mixer, so it is skipped here. Inert when the output layout equals the
        // mix layout (every bus at the stereo default).
        const bool folded =
            i != 0 && IsConcreteLayout(bus.outputLayout) &&
            LayoutChannelCount(bus.outputLayout) < m_mixChannels;
        // Existing Compressor taps end at the ordinary effects/layout stage.
        // Formal signal sources are captured below after receiving and Out Vol.
        if (folded) {
            FoldBusOutput(bus, planes, blockFrames);
        } else {
            m_dynamicRoutes.CaptureBus(
                i, VN_DYNAMIC_TAP_BUS_POST_EFFECTS, planes, m_mixChannels,
                blockFrames);
        }
        if (signalNode != UINT32_MAX) {
            if (!signals->ProcessBus(signalNode, planes, bus.outputGain)) return false;
            bus.outputPeak = StageTapPeak(planes, m_mixChannels, 0, blockFrames,
                bus.outputChannelPeaks);
        }
        if (i != 0) {
            float* const* parent = busPlanes.ForBus(bus.parentIndex, m_mixChannels);
            const bool outputFilter =
                ToneSectionActive(bus.outputLpf) ||
                ToneSectionActive(bus.outputHpf);
            const bool captureSignal = signalNode != UINT32_MAX &&
                                       signals->RequiresSourceCapture(signalNode);
            if (!outputFilter && !captureSignal) {
                for (std::uint32_t ch = 0; ch < m_mixChannels; ++ch) {
                    bus.outputGain.MixAdd(planes[ch], parent[ch], blockFrames,
                                          0);
                }
            } else {
                // Output Volume, then Output Bus LPF/HPF, then into parent.
                // Scratch keeps the shared planes intact for Send.
                for (std::uint32_t ch = 0; ch < m_mixChannels; ++ch) {
                    std::memset(
                        m_outputScratch[ch], 0,
                        static_cast<std::size_t>(blockFrames) * sizeof(float));
                    bus.outputGain.MixAdd(planes[ch], m_outputScratch[ch],
                                          blockFrames, 0);
                }
                ProcessBusToneSection(bus.outputLpf, BusToneKind::Lowpass,
                                      m_biquadTrig, m_sampleRate,
                                      m_outputScratch.data(), m_mixChannels,
                                      blockFrames);
                ProcessBusToneSection(bus.outputHpf, BusToneKind::Highpass,
                                      m_biquadTrig, m_sampleRate,
                                      m_outputScratch.data(), m_mixChannels,
                                      blockFrames);
                if (signalNode != UINT32_MAX && !signals->CaptureBus(signalNode,
                    {m_outputScratch.data(), m_mixChannels})) return false;
                for (std::uint32_t ch = 0; ch < m_mixChannels; ++ch) {
                    float* dst = parent[ch];
                    const float* src = m_outputScratch[ch];
                    for (std::uint32_t n = 0; n < blockFrames; ++n) {
                        dst[n] += src[n];
                    }
                }
            }
        }
        // Send edges: scatter this bus's finalized output into each send target
        // at the target's level. Membership comes from the plan; the level from
        // the reconciled table. Only reachable once a plan is in force, so
        // pre-plan renders skip this entirely. Send reads post-S2 planes and
        // does not take Output Volume or Output Bus filter.
        if (usePlan) {
            const auto range = plan.Sends(i);
            for (std::uint32_t s = 0; s < range.count; ++s) {
                const auto to = plan.sendTargets[range.first + s];
                if (to == kEmptySendTarget) {
                    continue;
                }
                SendEdge* edge = &plan.sendStates[range.first + s];
                if (edge != nullptr) {
                    ScatterSend(planes, busPlanes.ForBus(to, m_mixChannels, edge->toGeneration),
                                blockFrames, *edge);
                }
            }
        }
        if (i == 0) {
            bus.outputGain.Apply(planes, m_mixChannels, 0, blockFrames);
            ProcessBusToneSection(bus.outputLpf, BusToneKind::Lowpass,
                                  m_biquadTrig, m_sampleRate, planes,
                                  m_mixChannels, blockFrames);
            ProcessBusToneSection(bus.outputHpf, BusToneKind::Highpass,
                                  m_biquadTrig, m_sampleRate, planes,
                                  m_mixChannels, blockFrames);
            if (signalNode != UINT32_MAX && !signals->CaptureBus(signalNode,
                {planes, m_mixChannels})) return false;
        } else {
            bus.outputGain.Advance(blockFrames);
        }
    }
    return true;
}

// The bus insert-effect apply methods below mirror VoicePool's effect apply
// path exactly (pure reuse): the logic thread acquires and configures a pooled
// instance and hands (type, poolIndex) across by command; the audio thread only
// installs it and, when a slot gives an instance up (replace or clear), names it
// on the shared retire queue for the logic thread to reclaim. The audio thread
// never constructs or frees an effect.

void BusTree::ApplySetBusEffect(std::uint32_t busIndex, std::uint32_t slot,
                                std::uint32_t effectType,
                                std::uint32_t poolIndex,
                                std::uint64_t poolGeneration,
                                std::uint32_t busGeneration) {
    const EffectInstanceRef incoming{
        effectType, poolIndex, poolGeneration};
    void* incomingState = m_effectBank.LiveStateFor(incoming);
    if (incomingState == nullptr) { return; }
    if (busIndex >= m_workSlots.ConstructedCount() || !m_workSlots[busIndex].Valid() || WorkState(busIndex).id == VnObjectId{} ||
        (busGeneration != 0 &&
         !AudioRefMatches({busIndex, busGeneration})) ||
        slot >= m_fxSlots) {
        // Not audio-visible yet (never reached in practice; a bus's effect
        // command drains after its create swap) or out of range: retire the
        // unused instance rather than leak it (the audio thread never frees).
        const bool retired = m_effectBank.Retire(incoming);
        assert(retired && "effect retirement lane must hold every assigned row");
        (void)retired;
        return;
    }
    Bus& bus = WorkState(busIndex);
    // In-place replace: whatever this slot held is retired for reclamation.
    NoteWorkMutation(bus);
    if (bus.fxSlotEffectRef[slot].type != kEffectTypeNone) {
        if (harken::monitor::AudioLiveArmed()) {
            const std::uint64_t effectHandle =
                PackedMonitorEffectHandle(busIndex, slot);
            const std::uint64_t effectGeneration =
                bus.fxSlotEffectRef[slot].generation;
            harken::monitor::NoteExactInstanceRetired(
                VN_MONITOR_TAP_EFFECT_SLOT_INPUT, effectHandle, effectGeneration);
            harken::monitor::NoteExactInstanceRetired(
                VN_MONITOR_TAP_EFFECT_SLOT_OUTPUT, effectHandle, effectGeneration);
        }
        const bool retired = m_effectBank.Retire(bus.fxSlotEffectRef[slot]);
        assert(retired && "effect retirement lane must hold every assigned row");
        (void)retired;
    }
    bus.fxSlots[slot] =
        BuildEffect(effectType, incomingState);
    bus.fxSlotEffectRef[slot] = incoming;
    // The slot keeps its settled bypass mix: installing into a slot bypassed
    // while empty (mix 1) yields a dry output from the first block, matching the
    // empty-slot settle-at-apply rule.
}

void BusTree::ApplyClearBusEffect(std::uint32_t busIndex, std::uint32_t slot,
                                  std::uint32_t busGeneration) {
    if (busIndex >= m_workSlots.ConstructedCount() || !m_workSlots[busIndex].Valid() || WorkState(busIndex).id == VnObjectId{} ||
        (busGeneration != 0 &&
         !AudioRefMatches({busIndex, busGeneration})) ||
        slot >= m_fxSlots) {
        return;
    }
    Bus& bus = WorkState(busIndex);
    NoteWorkMutation(bus);
    if (bus.fxSlotEffectRef[slot].type != kEffectTypeNone) {
        // Retire the instance and empty the slot; its tail is cut here (a bus is
        // a persistent node with no drain, so a cleared effect simply stops).
        if (harken::monitor::AudioLiveArmed()) {
            const std::uint64_t effectHandle =
                PackedMonitorEffectHandle(busIndex, slot);
            const std::uint64_t effectGeneration =
                bus.fxSlotEffectRef[slot].generation;
            harken::monitor::NoteExactInstanceRetired(
                VN_MONITOR_TAP_EFFECT_SLOT_INPUT, effectHandle, effectGeneration);
            harken::monitor::NoteExactInstanceRetired(
                VN_MONITOR_TAP_EFFECT_SLOT_OUTPUT, effectHandle, effectGeneration);
        }
        const bool retired = m_effectBank.Retire(bus.fxSlotEffectRef[slot]);
        assert(retired && "effect retirement lane must hold every assigned row");
        (void)retired;
        bus.fxSlots[slot] = InsertEffect{};
        bus.fxSlotEffectRef[slot] = EffectInstanceRef{};
    }
    // Settle any in-progress bypass ramp to its target and keep the target, so
    // the cleared slot behaves exactly like a never-occupied one (empty-slot
    // settle-at-apply rule).
    bus.fxBypassMix[slot] = bus.fxBypassTarget[slot];
    bus.fxBypassRamping[slot] = false;
}

void BusTree::ApplySetBusEffectParam(std::uint32_t busIndex, std::uint32_t slot,
                                     std::uint32_t paramIndex, float value,
                                     std::uint32_t effectType,
                                     const BiquadCoeffs& coeffs,
                                     std::uint32_t tailFrames,
                                     std::uint32_t transitionFrames,
                                     std::uint32_t busGeneration) {
    if (busIndex >= m_workSlots.ConstructedCount() || !m_workSlots[busIndex].Valid() || WorkState(busIndex).id == VnObjectId{} ||
        (busGeneration != 0 &&
         !AudioRefMatches({busIndex, busGeneration})) ||
        slot >= m_fxSlots) {
        return;
    }
    Bus& bus = WorkState(busIndex);
    if (bus.fxSlotEffectRef[slot].type != effectType) {
        return;
    }
    InsertEffect& fx = bus.fxSlots[slot];
    if (fx.process == nullptr) {
        return; // empty slot: nothing to parameterize
    }
    NoteWorkMutation(bus);
    if (effectType == VN_EFFECT_THREE_BAND_TONE) {
        ToneApplyPreparedParam(fx.state, paramIndex, value, coeffs,
                               tailFrames);
    } else if (effectType == VN_EFFECT_FEEDBACK_DELAY &&
               (paramIndex == kDelayParamWet ||
                paramIndex == kDelayParamDry)) {
        DelayApplyPreparedParam(fx.state, paramIndex, value,
                                transitionFrames);
    } else if (effectType == VN_EFFECT_GAIN) {
        GainApplyPreparedParam(fx.state, coeffs.b0, transitionFrames);
    } else if (effectType == VN_EFFECT_LIMITER) {
        LimiterRequestPreparedParam(fx.state, paramIndex, coeffs.b0,
                                    transitionFrames);
    } else if (effectType == VN_EFFECT_COMPRESSOR) {
        // Compressor vectors require their dedicated nine-float transport.
        // A generic command is incomplete and therefore fails closed.
        return;
    } else if (fx.setParam != nullptr) {
        fx.setParam(fx.state, paramIndex, value);
    }
}

void BusTree::ApplySetBusCompressorParameters(
    std::uint32_t busIndex, std::uint32_t slot,
    std::uint32_t effectType,
    const CompressorPreparedParameters& prepared,
    std::uint32_t transitionFrames,
    std::uint32_t busGeneration) {
    if (effectType != VN_EFFECT_COMPRESSOR ||
        busIndex >= m_workSlots.ConstructedCount() || !m_workSlots[busIndex].Valid() || WorkState(busIndex).id == VnObjectId{} ||
        (busGeneration != 0 &&
         !AudioRefMatches({busIndex, busGeneration})) ||
        slot >= m_fxSlots) {
        return;
    }
    Bus& bus = WorkState(busIndex);
    if (bus.fxSlotEffectRef[slot].type != effectType) {
        return;
    }
    InsertEffect& fx = bus.fxSlots[slot];
    if (fx.process == nullptr) {
        return;
    }
    NoteWorkMutation(bus);
    CompressorRequestPreparedParameters(
        static_cast<CompressorState*>(fx.state), prepared,
        transitionFrames);
}

void BusTree::ApplySetBusEffectBypass(std::uint32_t busIndex, std::uint32_t slot,
                                      std::uint32_t bypass,
                                      std::uint32_t action,
                                      std::uint32_t expectedEffectType,
                                      std::uint32_t busGeneration) {
    if (busIndex >= m_workSlots.ConstructedCount() || !m_workSlots[busIndex].Valid() || WorkState(busIndex).id == VnObjectId{} ||
        (busGeneration != 0 &&
         !AudioRefMatches({busIndex, busGeneration})) ||
        slot >= m_fxSlots) {
        return;
    }
    Bus& bus = WorkState(busIndex);
    if (action == kEffectActionReset) {
        NoteWorkMutation(bus);
        if (bus.fxSlotEffectRef[slot].type == expectedEffectType &&
            expectedEffectType == VN_EFFECT_COMPRESSOR &&
            bus.fxSlots[slot].reset != nullptr) {
            bus.fxSlots[slot].reset(bus.fxSlots[slot].state);
        }
        return;
    }
    // Target 1 = fully dry (bypassed), 0 = fully wet (active).
    NoteWorkMutation(bus);
    const float target = (bypass != 0) ? 1.0f : 0.0f;
    const bool enablingLimiter =
        target == 0.0f && bus.fxBypassTarget[slot] == 1.0f &&
        bus.fxSlotEffectRef[slot].type == VN_EFFECT_LIMITER;
    bus.fxBypassTarget[slot] = target;
    if (bus.fxSlots[slot].process == nullptr) {
        // An empty slot is skipped in the pipeline before any ramp could
        // settle, so settle it here at the moment the command applies:
        // otherwise the ramp would linger and, once an effect is installed
        // later, leak a wet->dry crossfade on that first block. Matches the
        // voice-side empty-slot settle-at-apply rule.
        bus.fxBypassMix[slot] = target;
        bus.fxBypassRamping[slot] = false;
        return;
    }
    if (enablingLimiter && bus.fxSlots[slot].reset != nullptr) {
        // The frozen Limiter contract starts bypass-to-enable with clean gain
        // reduction while retaining its parameters.
        bus.fxSlots[slot].reset(bus.fxSlots[slot].state);
    }
    // Occupied slot: arm a one-block crossfade only when the target differs from
    // the settled mix, so setting the state a slot already holds never restarts
    // a ramp.
    bus.fxBypassRamping[slot] = (target != bus.fxBypassMix[slot]);
}

void BusTree::ApplySetBusFaderPoint(std::uint32_t busIndex,
                                    std::uint32_t faderAfterSlot,
                                    std::uint32_t busGeneration) {
    if (busIndex >= m_workSlots.ConstructedCount() || !m_workSlots[busIndex].Valid() || WorkState(busIndex).id == VnObjectId{} ||
        (busGeneration != 0 &&
         !AudioRefMatches({busIndex, busGeneration}))) {
        return;
    }
    NoteWorkMutation(WorkState(busIndex));
    // A plain block-boundary swap of the split index. The logic side validated
    // faderAfterSlot <= fxSlotsPerBus; clamp defensively. FinalizeBus reads the
    // new split next block; see Vn_SetBusFaderPoint's public doc for the click
    // discipline.
    WorkState(busIndex).faderAfterSlot =
        (faderAfterSlot <= m_fxSlots) ? faderAfterSlot : m_fxSlots;
}

} // namespace harken
