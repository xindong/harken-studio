#include "harken/render/BusTree.h"

#include "MonitorCaptureHub.h"

#include "harken/core/ChannelLayout.h"
#include "harken/core/SnapshotBuffer.h"
#include "harken/render/BuiltinEffects.h"
#include "harken/render/EffectChain.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <limits>

namespace harken {

BusTree::BusTree(const EngineConfig& config, TopologyPlanPool& plans,
                 EffectPoolBank& effects, DynamicRouteSystem& dynamicRoutes,
                 const BiquadTrigTable* biquadTrig)
    : m_plans(plans),
      m_effectBank(effects),
      m_dynamicRoutes(dynamicRoutes),
      m_buses(config.maxBuses),
      m_logicParent(config.maxBuses, 0),
      m_logicAlive(config.maxBuses, 0),
      m_logicId(config.maxBuses),
      m_logicGeneration(config.maxBuses, 0),
      m_logicQuarantined(config.maxBuses, 0),
      m_logicRetireRevision(config.maxBuses, 0),
      m_sends(static_cast<std::size_t>(config.maxBuses) *
              config.busSendSlotsPerBus),
      m_sendSlots(config.busSendSlotsPerBus),
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
    // The master bus pre-exists at index 0 and is its own parent (the fold
    // never dereferences the master's parent). Its chain defaults to all
    // pre-fader, like every created bus.
    m_buses[0].id = VN_BUS_MASTER;
    m_buses[0].generation = 1;
    m_buses[0].parentIndex = 0;
    m_logicParent[0] = 0;
    m_logicAlive[0] = 1;
    m_logicId[0] = VN_BUS_MASTER;
    m_logicGeneration[0] = 1;
    m_buses[0].faderAfterSlot = m_fxSlots;
    // The master's output fold is to the device layout, applied by the mixer at
    // the device boundary. Seed its default matrix (the Appendix A fold for the
    // resolved mix->device pair) so the pre-plan render — before any structural
    // change publishes a plan carrying the authoritative fold config — folds
    // correctly. At the stereo default mix == device, so this is the identity
    // and no fold runs.
    m_buses[0].outputLayout = m_deviceLayout;
    DefaultDownmixMatrix(m_mixLayout, m_deviceLayout, m_buses[0].downmixCoeffs);
    m_count = 1;
    m_activeCount = 1;
    m_materializedCount = 1;
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

bool BusTree::CanMaterializePreparedBusSlots(
    std::uint32_t firstIndex, std::uint32_t slotCount) const noexcept {
    return slotCount != 0 && m_materializedCount == m_count &&
           firstIndex == m_count && firstIndex <= m_capacity &&
           slotCount <= m_capacity - firstIndex;
}

void BusTree::InitializeBusSlot(std::uint32_t busIndex, VnBusId id,
                                std::uint32_t parentIndex,
                                std::uint32_t generation) noexcept {
    assert(busIndex < m_capacity);
    Bus& bus = m_buses[busIndex];
    bus = Bus{};
    bus.id = id;
    bus.generation = generation;
    bus.parentIndex = parentIndex;
    bus.faderAfterSlot = m_fxSlots;
    bus.outputLayout = m_mixLayout;
    DefaultDownmixMatrix(m_mixLayout, m_mixLayout, bus.downmixCoeffs);
    if (m_sendSlots != 0) {
        SendEdge* row =
            &m_sends[static_cast<std::size_t>(busIndex) * m_sendSlots];
        for (std::uint32_t slot = 0; slot < m_sendSlots; ++slot) {
            row[slot] = SendEdge{};
        }
    }
}

void BusTree::RetireBusSlotEffects(Bus& bus) noexcept {
    for (std::uint32_t slot = 0; slot < m_fxSlots; ++slot) {
        if (bus.fxSlotEffectRef[slot].type != kEffectTypeNone) {
            m_effectBank.RetireQueue().TryPush(
                bus.fxSlotEffectRef[slot]);
            bus.fxSlots[slot] = InsertEffect{};
            bus.fxSlotEffectRef[slot] = EffectInstanceRef{};
        }
    }
}

void BusTree::MaterializePreparedBusSlots(
    std::uint32_t firstIndex, const PreparedBusSlot* slots,
    std::uint32_t slotCount) noexcept {
    const bool valid =
        slots != nullptr &&
        CanMaterializePreparedBusSlots(firstIndex, slotCount);
    assert(valid && "prepared Bus slots must be one validated append batch");
    if (!valid) {
        return;
    }
    for (std::uint32_t offset = 0; offset < slotCount; ++offset) {
        const std::uint32_t busIndex = firstIndex + offset;
        assert(!IsNull(slots[offset].id));
        assert(slots[offset].parentIndex < busIndex);
        const std::uint32_t generation =
            slots[offset].generation != 0 ? slots[offset].generation : 1u;
        InitializeBusSlot(busIndex, slots[offset].id,
                          slots[offset].parentIndex, generation);
    }
    m_materializedCount = firstIndex + slotCount;
}

void BusTree::CommitPreparedBusVisibility(std::uint32_t firstIndex,
                                          std::uint32_t slotCount) noexcept {
    const bool valid =
        slotCount != 0 && firstIndex == m_count &&
        firstIndex <= m_capacity && slotCount <= m_capacity - firstIndex &&
        m_materializedCount == firstIndex + slotCount;
    assert(valid && "visibility commit must match the materialized Bus batch");
    if (!valid) {
        return;
    }
    for (std::uint32_t busIndex = firstIndex;
         busIndex < firstIndex + slotCount; ++busIndex) {
        m_logicParent[busIndex] = m_buses[busIndex].parentIndex;
        m_logicAlive[busIndex] = 1;
        m_logicId[busIndex] = m_buses[busIndex].id;
        m_logicGeneration[busIndex] = m_buses[busIndex].generation;
        ++m_activeCount;
    }
    m_count = firstIndex + slotCount;
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
        m_materializedCount = m_count;
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
    // Appending into preallocated storage: no heap traffic, and existing
    // indices stay stable. Creation is a logic-side structural operation;
    // the audio side observes the new bus only through the plan a create
    // publishes, swapped at a block boundary.
    assert(m_materializedCount == m_count &&
           "ordinary creation cannot overlap a prepared Bus batch");
    if (m_materializedCount != m_count) {
        return VN_NO_BUS;
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
        busIndex = m_count;
        generation = 1;
        // An appended slot is not audio-visible yet, so initializing its
        // render state on the logic thread is race-free.
        InitializeBusSlot(busIndex, id, parentIndex, generation);
    }
    CommitLogicSlot(busIndex, id, parentIndex, generation);
    return VN_OK;
}

BusTree::Bus* BusTree::BusState(std::uint32_t busIndex) {
    return busIndex < m_count ? &m_buses[busIndex] : nullptr;
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
        m_buses[ref.index].generation != ref.generation) {
        return false;
    }
    const std::uint8_t* alive = InstalledAlive();
    return alive == nullptr ? ref.index == 0 : alive[ref.index] != 0;
}

std::uint32_t BusTree::FreeBusSlotCount() const {
    std::uint32_t free = m_capacity - m_count;
    for (std::uint32_t i = 1; i < m_count; ++i) {
        if (m_logicAlive[i] == 0 && !QuarantinedAt(i)) {
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

void ProcessBusToneSection(BusTree::Bus::ToneSection& section,
                           BusToneKind kind, const BiquadTrigTable* trig,
                           std::uint32_t sampleRate, float* const* channels,
                           std::uint32_t channelCount,
                           std::uint32_t frames) {
    if (!section.enabled && !section.transition.transitioning &&
        !section.glide.active) {
        return;
    }
    if (section.transition.transitioning || !section.glide.active ||
        trig == nullptr) {
        BiquadProcessSmoothed(section.transition, section.enabled,
                              section.coeffs, section.state,
                              section.tailFrames, channels, channelCount, 0,
                              frames);
        return;
    }
    constexpr std::uint32_t kControlFrames = 16;
    std::uint32_t consumed = 0;
    while (consumed < frames) {
        const std::uint32_t controlOffset =
            section.glide.progress % kControlFrames;
        const std::uint32_t count =
            std::min(kControlFrames - controlOffset, frames - consumed);
        BiquadProcessSmoothed(section.transition, section.enabled,
                              section.coeffs, section.state,
                              section.tailFrames, channels, channelCount,
                              consumed, count);
        consumed += count;
        section.glide.Advance(count);
        const BiquadTrigPoint point =
            trig->Lookup(static_cast<float>(sampleRate),
                         section.glide.FrequencyHz());
        const BiquadCoeffs candidate =
            kind == BusToneKind::Lowpass
                ? MakeLowpassFromTrig(point, section.glide.Q())
                : MakeHighpassFromTrig(point, section.glide.Q());
        if (IsBiquadStable(candidate)) {
            section.coeffs = candidate;
            section.tailFrames = EstimateBiquadTailFrames(candidate);
        }
        if (!section.glide.active &&
            IsBiquadStable(section.transition.targetCoeffs)) {
            section.coeffs = section.transition.targetCoeffs;
            section.tailFrames = section.transition.targetTailFrames;
        }
    }
}

bool ToneSectionActive(const BusTree::Bus::ToneSection& section) {
    return section.enabled || section.transition.transitioning ||
           section.glide.active;
}

} // namespace

void BusTree::ApplySetBusFilter(std::uint32_t busIndex,
                                std::uint32_t filterKind, bool enable,
                                float cutoffHz, float q,
                                const BiquadCoeffs& coeffs,
                                std::uint32_t tailFrames,
                                std::uint32_t transitionFrames, bool outputOnly,
                                std::uint32_t busGeneration) {
    if (busIndex >= m_audioCount ||
        (busGeneration != 0 &&
         !AudioRefMatches({busIndex, busGeneration}))) {
        return;
    }
    const bool lowpass = filterKind == kFilterKindLowpass;
    if (!lowpass && filterKind != kFilterKindHighpass) {
        return;
    }
    Bus& bus = m_buses[busIndex];
    Bus::ToneSection& section =
        outputOnly ? (lowpass ? bus.outputLpf : bus.outputHpf)
                   : (lowpass ? bus.busLpf : bus.busHpf);
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
    if (busIndex >= m_audioCount ||
        (busGeneration != 0 &&
         !AudioRefMatches({busIndex, busGeneration}))) {
        // A volume command for a just-created bus drains after that bus's
        // plan-swap command (command order), so by here m_audioCount already
        // covers it; the bound is defensive, not a live case.
        return;
    }
    Bus& bus = m_buses[busIndex];
    float linear = 0.0f;
    if (!TryVolumeGainFromDb(volumeDb, &linear)) {
        ++m_invalidVolumeApplies;
        return;
    }
    const std::uint64_t rampFrames =
        (static_cast<std::uint64_t>(rampMs) * m_sampleRate + 999) / 1000;
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

BusTree::SendEdge* BusTree::SendRow(std::uint32_t fromBus) {
    if (fromBus >= m_audioCount || m_sendSlots == 0) {
        return nullptr;
    }
    return &m_sends[static_cast<std::size_t>(fromBus) * m_sendSlots];
}

BusTree::SendEdge* BusTree::FindEdge(std::uint32_t fromBus,
                                     std::uint32_t toBus) {
    SendEdge* row = SendRow(fromBus);
    if (row == nullptr) {
        return nullptr;
    }
    for (std::uint32_t s = 0; s < m_sendSlots; ++s) {
        if (row[s].toIndex == toBus) {
            return &row[s];
        }
    }
    return nullptr;
}

void BusTree::ApplyTopologyPlan(std::uint32_t planIndex,
                                std::uint32_t connectFrom,
                                std::uint32_t connectTo, float connectLevelDb,
                                std::uint64_t acceptedGraphRevision) {
    // Swap: retire whatever plan was installed (the logic side reclaims it) and
    // adopt the new one. A single command record is inherently atomic, so this
    // whole apply is the block-boundary swap.
    if (m_currentPlanIndex != kInvalidTopologyPlanIndex) {
        m_plans.Retire(m_currentPlanIndex);
    }
    m_currentPlanIndex = planIndex;
    const TopologyPlanView plan = m_plans.View(planIndex);
    if (!plan.valid) {
        return;
    }
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
            if (m_buses[i].generation != generation) {
                RetireBusSlotEffects(m_buses[i]);
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
            if (plan.parents[i] != kPlanParentUnset) {
                m_buses[i].parentIndex = plan.parents[i];
            }
        }
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
            if (plan.alive[i] != 0) {
                continue;
            }
            Bus& bus = m_buses[i];
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
    if (plan.outputLayouts != nullptr && plan.downmixCoeffs != nullptr) {
        for (std::uint32_t i = 0; i < m_audioCount; ++i) {
            m_buses[i].outputLayout = plan.outputLayouts[i];
            std::memcpy(m_buses[i].downmixCoeffs,
                        plan.downmixCoeffs + static_cast<std::size_t>(i) * 64,
                        64 * sizeof(float));
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
                    generation == 0 || !AudioRefMatches({i, generation})) {
                    continue;
                }
                Bus& bus = m_buses[i];
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
    // Reconcile each audio edge directly against its authoritative plan slot.
    // The logic-side adjacency keeps stable slots across republishes, so equal
    // slot+target preserves the live gain and ramp, an empty slot clears it,
    // and a changed target starts with fresh level state. This is one bounded
    // pass over the fixed rows rather than searching each target in the row.
    const std::uint32_t membershipSlots =
        plan.slotsPerBus < m_sendSlots ? plan.slotsPerBus : m_sendSlots;
    for (std::uint32_t from = 0; from < m_audioCount; ++from) {
        const std::uint32_t* membership =
            plan.sendTargets + static_cast<std::size_t>(from) * plan.slotsPerBus;
        SendEdge* row = &m_sends[static_cast<std::size_t>(from) * m_sendSlots];
        for (std::uint32_t s = 0; s < m_sendSlots; ++s) {
            const std::uint32_t target =
                s < membershipSlots ? membership[s] : kEmptySendTarget;
            if (target == kEmptySendTarget) {
                row[s] = SendEdge{};
                continue;
            }
            SendEdge& edge = row[s];
            const std::uint32_t targetGeneration =
                plan.generations != nullptr && target < m_audioCount
                    ? plan.generations[target]
                    : 0;
            const bool retained =
                edge.toIndex == target &&
                (targetGeneration == 0 ||
                 edge.toGeneration == targetGeneration);
            if (!retained) {
                edge = SendEdge{};
                edge.toIndex = target;
                edge.toGeneration = targetGeneration;
            }
            const std::size_t planSlot =
                static_cast<std::size_t>(from) * plan.slotsPerBus + s;
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
    // the swap so the edge sounds correct on its first block. Find its level
    // slot (a reconnect may not have one after reconcile) or take a free slot;
    // the logic side verified capacity, so a free slot exists.
    if (connectFrom != kInvalidBusIndex) {
        SendEdge* edge = FindEdge(connectFrom, connectTo);
        if (edge == nullptr) {
            SendEdge* row = SendRow(connectFrom);
            if (row != nullptr) {
                for (std::uint32_t s = 0; s < m_sendSlots; ++s) {
                    if (row[s].toIndex == kInvalidBusIndex) {
                        edge = &row[s];
                        break;
                    }
                }
            }
        }
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
    if (acceptedGraphRevision != 0) {
        m_installedGraphRevision.store(acceptedGraphRevision,
                                       std::memory_order_release);
    }
}

void BusTree::ApplySetBusSendLevel(std::uint32_t fromBus, std::uint32_t toBus,
                                   float levelDb, std::uint32_t rampMs,
                                   std::uint32_t fromGeneration,
                                   std::uint32_t toGeneration) {
    if ((fromGeneration != 0 &&
         !AudioRefMatches({fromBus, fromGeneration})) ||
        (toGeneration != 0 &&
         !AudioRefMatches({toBus, toGeneration}))) {
        return;
    }
    SendEdge* edge = FindEdge(fromBus, toBus);
    if (edge != nullptr && toGeneration != 0 &&
        edge->toGeneration != toGeneration) {
        return;
    }
    if (edge == nullptr) {
        // An edge in the CURRENT plan's membership without a level slot:
        // it entered by a whole-graph publish (a pack load's single plan,
        // which carries no per-edge identity) rather than by a connect
        // (whose swap seeds the slot). Seed the slot here and snap — the
        // seeding command rides the same drain as the plan swap, so the
        // edge never renders slot-less. A connect-built edge always finds
        // its slot above, keeping every existing path byte-identical.
        const TopologyPlanView plan = m_plans.View(m_currentPlanIndex);
        bool member = false;
        if (plan.valid && fromBus < m_audioCount) {
            const std::uint32_t* membership =
                plan.sendTargets +
                static_cast<std::size_t>(fromBus) * plan.slotsPerBus;
            for (std::uint32_t s = 0; s < plan.slotsPerBus; ++s) {
                if (membership[s] == toBus) {
                    member = true;
                    break;
                }
            }
        }
        if (!member) {
            return; // not in the membership; nothing to level
        }
        SendEdge* row = SendRow(fromBus);
        if (row == nullptr) {
            return;
        }
        for (std::uint32_t s = 0; s < m_sendSlots; ++s) {
            if (row[s].toIndex == kInvalidBusIndex) {
                edge = &row[s];
                break;
            }
        }
        if (edge == nullptr) {
            return; // no free slot: unreachable, capacity was staged
        }
        *edge = SendEdge{};
        edge->toIndex = toBus;
        edge->toGeneration = toGeneration;
        edge->gain.SnapTo(std::pow(10.0f, levelDb / 20.0f));
        return;
    }
    const float linear = std::pow(10.0f, levelDb / 20.0f);
    ArmRamp(edge->gain, edge->ramping, edge->rampStartGain, edge->rampEndGain,
            edge->rampBlocksTotal, edge->rampBlocksDone, linear, rampMs);
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
        static_cast<std::uint32_t>(&bus - m_buses.data());
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
        ProcessChainSlot(bus.fxSlots[slot], bus.fxBypassRamping[slot],
                         bus.fxBypassMix[slot], bus.fxBypassTarget[slot],
                         planes, dry, m_mixChannels, 0, blockFrames, input);
        if (bus.fxSlots[slot].process != nullptr &&
            harken::monitor::AudioLiveArmed()) {
            harken::monitor::PublishPlanarAudioChunk(
                VN_MONITOR_TAP_EFFECT_SLOT_OUTPUT, effectHandle, effectGeneration,
                slot, planes, blockFrames, m_mixChannels, 0, m_mixLayout,
                m_sampleRate, sourceFrameBegin);
        }
        if (compressorState != nullptr) {
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
    // Pre-fader chain: fx slots [0, faderAfterSlot) before the gain.
    for (std::uint32_t s = 0; s < fader; ++s) {
        processSlot(s);
    }
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
    for (std::uint32_t s = fader; s < m_fxSlots; ++s) {
        processSlot(s);
    }
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
    }
    const std::uint32_t busIndex =
        static_cast<std::uint32_t>(&bus - m_buses.data());
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

void BusTree::MixDown(float* const* busPlanes, std::uint32_t blockFrames,
                      std::uint64_t blockEpoch, std::uint64_t sourceFrameBegin) {
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
    const std::uint32_t n = usePlan ? plan.orderCount : m_audioCount;
    for (std::uint32_t k = 0; k < n; ++k) {
        const std::uint32_t i = usePlan ? plan.order[k] : (n - 1 - k);
        Bus& bus = m_buses[i];
        float* const* planes = busPlanes + i * m_mixChannels;
        FinalizeBus(bus, planes, blockFrames, blockEpoch, sourceFrameBegin);
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
        if (folded) {
            FoldBusOutput(bus, planes, blockFrames);
        } else {
            m_dynamicRoutes.CaptureBus(
                i, VN_DYNAMIC_TAP_BUS_POST_EFFECTS, planes, m_mixChannels,
                blockFrames);
        }
        if (i != 0) {
            float* const* parent = busPlanes + bus.parentIndex * m_mixChannels;
            const bool outputFilter =
                ToneSectionActive(bus.outputLpf) ||
                ToneSectionActive(bus.outputHpf);
            if (!outputFilter) {
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
            const std::uint32_t* membership =
                plan.sendTargets + static_cast<std::size_t>(i) * plan.slotsPerBus;
            for (std::uint32_t s = 0; s < plan.slotsPerBus; ++s) {
                const std::uint32_t to = membership[s];
                if (to == kEmptySendTarget) {
                    continue;
                }
                SendEdge* edge = FindEdge(i, to);
                if (edge != nullptr) {
                    ScatterSend(planes, busPlanes + to * m_mixChannels,
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
        } else {
            bus.outputGain.Advance(blockFrames);
        }
    }
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
    if (busIndex >= m_capacity || m_buses[busIndex].id == VnObjectId{} ||
        (busGeneration != 0 &&
         !AudioRefMatches({busIndex, busGeneration})) ||
        slot >= m_fxSlots) {
        // Not audio-visible yet (never reached in practice; a bus's effect
        // command drains after its create swap) or out of range: retire the
        // unused instance rather than leak it (the audio thread never frees).
        m_effectBank.RetireQueue().TryPush(incoming);
        return;
    }
    Bus& bus = m_buses[busIndex];
    // In-place replace: whatever this slot held is retired for reclamation.
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
        m_effectBank.RetireQueue().TryPush(bus.fxSlotEffectRef[slot]);
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
    if (busIndex >= m_capacity || m_buses[busIndex].id == VnObjectId{} ||
        (busGeneration != 0 &&
         !AudioRefMatches({busIndex, busGeneration})) ||
        slot >= m_fxSlots) {
        return;
    }
    Bus& bus = m_buses[busIndex];
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
        m_effectBank.RetireQueue().TryPush(bus.fxSlotEffectRef[slot]);
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
    if (busIndex >= m_capacity || m_buses[busIndex].id == VnObjectId{} ||
        (busGeneration != 0 &&
         !AudioRefMatches({busIndex, busGeneration})) ||
        slot >= m_fxSlots) {
        return;
    }
    Bus& bus = m_buses[busIndex];
    if (bus.fxSlotEffectRef[slot].type != effectType) {
        return;
    }
    InsertEffect& fx = bus.fxSlots[slot];
    if (fx.process == nullptr) {
        return; // empty slot: nothing to parameterize
    }
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
        busIndex >= m_capacity || m_buses[busIndex].id == VnObjectId{} ||
        (busGeneration != 0 &&
         !AudioRefMatches({busIndex, busGeneration})) ||
        slot >= m_fxSlots) {
        return;
    }
    Bus& bus = m_buses[busIndex];
    if (bus.fxSlotEffectRef[slot].type != effectType) {
        return;
    }
    InsertEffect& fx = bus.fxSlots[slot];
    if (fx.process == nullptr) {
        return;
    }
    CompressorRequestPreparedParameters(
        static_cast<CompressorState*>(fx.state), prepared,
        transitionFrames);
}

void BusTree::ApplySetBusEffectBypass(std::uint32_t busIndex, std::uint32_t slot,
                                      std::uint32_t bypass,
                                      std::uint32_t action,
                                      std::uint32_t expectedEffectType,
                                      std::uint32_t busGeneration) {
    if (busIndex >= m_capacity || m_buses[busIndex].id == VnObjectId{} ||
        (busGeneration != 0 &&
         !AudioRefMatches({busIndex, busGeneration})) ||
        slot >= m_fxSlots) {
        return;
    }
    Bus& bus = m_buses[busIndex];
    if (action == kEffectActionReset) {
        if (bus.fxSlotEffectRef[slot].type == expectedEffectType &&
            expectedEffectType == VN_EFFECT_COMPRESSOR &&
            bus.fxSlots[slot].reset != nullptr) {
            bus.fxSlots[slot].reset(bus.fxSlots[slot].state);
        }
        return;
    }
    // Target 1 = fully dry (bypassed), 0 = fully wet (active).
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
    if (busIndex >= m_capacity || m_buses[busIndex].id == VnObjectId{} ||
        (busGeneration != 0 &&
         !AudioRefMatches({busIndex, busGeneration}))) {
        return;
    }
    // A plain block-boundary swap of the split index. The logic side validated
    // faderAfterSlot <= fxSlotsPerBus; clamp defensively. FinalizeBus reads the
    // new split next block; see Vn_SetBusFaderPoint's public doc for the click
    // discipline.
    m_buses[busIndex].faderAfterSlot =
        (faderAfterSlot <= m_fxSlots) ? faderAfterSlot : m_fxSlots;
}

} // namespace harken
